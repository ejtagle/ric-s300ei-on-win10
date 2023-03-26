// dllmain.cpp : Define el punto de entrada de la aplicación DLL.
#include "pch.h"
#include <stdint.h>

// NOTE: The CONTEXT structure contains processor-specific 
// information. This sample was designed for X86 processors.

//addVectoredExceptionHandler constants:
//CALL_FIRST means call this exception handler first;
//CALL_LAST means call this exception handler last
#define CALL_FIRST 1  
#define CALL_LAST 0 EXCEPTION_SINGLE_STEP


static PVOID hOldVectorHandler = NULL;
static HINSTANCE hRIC32Dll = NULL;
static HINSTANCE hInpOutDll = NULL;

//DLLPortIO function support
typedef BOOL(__stdcall* lpIsInpOutDriverOpen)(void);
typedef UCHAR   (_stdcall *lpDlPortReadPortUchar)(USHORT port);
typedef void    (_stdcall *lpDlPortWritePortUchar)(USHORT port, UCHAR Value);

lpIsInpOutDriverOpen IsInpOutDriverOpen = NULL;
lpDlPortReadPortUchar DlPortReadPortUchar = NULL;
lpDlPortWritePortUchar DlPortWritePortUchar = NULL;

// Twain 
typedef uint16_t (__stdcall* lpDS_Entry)(void* pOrigin, uint32_t DG, uint16_t DAT, uint16_t MSG, void* pData);
lpDS_Entry orgDS_Entry = NULL;

LARGE_INTEGER TimerFreq;
LARGE_INTEGER StartTimerValue;
USHORT PITValue = 0;
int rdStep = 0;
int rdCount = 0;
BYTE emulatedPort[4] = { 0 };
BYTE emulatedPortHI[4] = { 0 };

static LONG WINAPI
VectoredHandler(
    struct _EXCEPTION_POINTERS* ExceptionInfo
)
{
    PCONTEXT Context;

    if (ExceptionInfo->ExceptionRecord->ExceptionCode != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;

    // Get the context
    Context = ExceptionInfo->ContextRecord;

    /*
    EE out dx, al
    EC in  al, dx
    E6 43                                         out     43h, al  // 40 43 21
    E4 21                                         in      al, 21h
    FA                                            cli
    FB                                            sti
    */
#ifdef _AMD64_
    PBYTE p = (PBYTE)Context->Rip;

    BYTE instr = p[0];
    if (instr != 0xEE && instr != 0xEC && instr != 0xE6 && instr != 0xE4 && instr != 0xFA && instr != 0xFB)
        return EXCEPTION_CONTINUE_SEARCH;

    // -----------Capture acceleration
    if (instr == 0xFA && (Context->Rip & 0x1FFFF) == 0x4DD3) {
        // Start of timer readback

        // Plug in the value to return
        Context->Rax &= 0xFFFF0000;
        Context->Rax |= PITValue ^ 0xFFFF;

        rdCount++;
        if (rdCount > 2) { // The first 2 reads will return 0xFFFF, just to avoid problems
            LARGE_INTEGER currTimerValue;
            QueryPerformanceCounter((LARGE_INTEGER*)&currTimerValue);
            uint64_t ctr = ((currTimerValue.QuadPart - StartTimerValue.QuadPart) / TimerFreq.QuadPart);
            PITValue = (ctr > 65535ULL) ? 0 : (USHORT)(65535ULL - ctr);
        }

        // Skip routine
        Context->Rip += 0x4E08 - 0x4DD3;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (instr == 0xE6 && (Context->Rip & 0x1FFFF) == 0x4DB2) {
        // Start of timer reset

        // Starting timer
        QueryPerformanceFrequency(&TimerFreq);
        TimerFreq.QuadPart /= 1000000ULL; // Convert to MHZ
        if (!TimerFreq.QuadPart) TimerFreq.QuadPart = 1;
        QueryPerformanceCounter(&StartTimerValue);
        rdStep = 0;
        rdCount = 0;
        PITValue = 0xFFFF;

        // Skip routine
        Context->Rip += 0x4DC6 - 0x4DB2;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (instr == 0xEC && (Context->Rip & 0x1FFFF) == 0x58A1) {
        // Start of scan line readback

        DWORD64 count = Context->Rcx;
        PBYTE dst = (PBYTE)Context->Rdi;
        USHORT addr = Context->Rdx & 0xFFFF;
        do {
            *dst++ = DlPortReadPortUchar(addr);
        } while (--count);
        Context->Rdi = (DWORD64)dst;
        Context->Rcx = 0;

        // Skip routine
        Context->Rip += 0x58A5 - 0x58A1;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    //--------------------

    // CLI/STI
    if (instr == 0xFA || instr == 0xFB) {
        Context->Rip++;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    USHORT addr = (instr == 0xEE || instr == 0xEC)
        ? (USHORT)(Context->Rdx & 0xFFFF)
        : p[1];

    // Emulate timer and interrupt controller, as it is used to get
    //  microsecond timer resolutions 
    if (addr == 0x40 || addr == 0x43 || addr == 0x21 || addr == 0x20) {
        // 34=>43 means start PIT
        // 00=>43 means read LO/HI decreasing
        // 1.19318 MHz

        // By default, clean AL
        if (instr == 0xE4 || instr == 0xEC) {
            Context->Rax &= 0xFFFFFF00;
        }

        if ((instr == 0xE6 || instr == 0xEE) && addr == 0x43)
        {
            if ((Context->Rax & 0xFF) == 0x34) {
                // Starting timer
                QueryPerformanceFrequency(&TimerFreq);
                TimerFreq.QuadPart /= 1000000ULL; // Convert to MHZ
                if (!TimerFreq.QuadPart) TimerFreq.QuadPart = 1;
                QueryPerformanceCounter(&StartTimerValue);
                rdStep = 0;
                rdCount = 0;
                PITValue = 0xFFFF;
            }

            if ((Context->Rax & 0xFF) == 0x00) {
                rdCount++;
                if (rdCount > 2) { // The first 2 reads will return 0xFFFF, just to avoid problems
                    LARGE_INTEGER currTimerValue;
                    QueryPerformanceCounter((LARGE_INTEGER*)&currTimerValue);
                    uint64_t ctr = ((currTimerValue.QuadPart - StartTimerValue.QuadPart) / TimerFreq.QuadPart);
                    PITValue = (ctr > 65535ULL) ? 0 : (USHORT)(65535ULL - ctr);
                }
                rdStep = 0;
            }
        }

        if ((instr == 0xEC || instr == 0xE4) && addr == 0x40) {
            Context->Rax |= ((rdStep) ? PITValue >> 8 : PITValue) & 0xFF;
            rdStep ^= 1;
        }

        Context->Rip += (instr == 0xEE || instr == 0xEC) ? 1 : 2;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // The following LPT is never emulated
    if ((addr >= 0x278 && addr <= 0x27F) ||
        (addr >= 0x678 && addr <= 0x67F)
        ) {

        if (instr == 0xE4 || instr == 0xEC) {
            Context->Rax &= 0xFFFFFF00;
        }

        Context->Rip += (instr == 0xEE || instr == 0xEC) ? 1 : 2;

        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // This one will always be emulated, to avoid crashes...
    if (addr >= 0x3BC && addr <= 0x3BF) {

        if (instr == 0xE4 || instr == 0xEC) {
            Context->Rax &= 0xFFFFFF00;
            Context->Rax |= emulatedPort[addr - 0x3BC];
        }
        else {
            emulatedPort[addr - 0x3BC] = (BYTE)Context->Rax;
        }

        Context->Rip += (instr == 0xEE || instr == 0xEC) ? 1 : 2;

        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (addr >= 0x7BC && addr <= 0x7BF) {

        if (instr == 0xE4 || instr == 0xEC) {
            Context->Rax &= 0xFFFFFF00;
            Context->Rax |= emulatedPortHI[addr - 0x7BC];
        }
        else {
            emulatedPortHI[addr - 0x7BC] = (BYTE)Context->Rax;
        }

        Context->Rip += (instr == 0xEE || instr == 0xEC) ? 1 : 2;

        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if ((addr >= 0x378 && addr <= 0x37F) ||
        (addr >= 0x778 && addr <= 0x77F)) {

        // Emulate instructions
        if (instr == 0xE4 || instr == 0xEC) {
            Context->Rax &= 0xFFFFFF00;
            Context->Rax |= DlPortReadPortUchar(addr);
        }
        else {
            DlPortWritePortUchar(addr, (BYTE)Context->Rax);
        }

        Context->Rip += (instr == 0xEE || instr == 0xEC) ? 1 : 2;

        return EXCEPTION_CONTINUE_EXECUTION;
    }
    else
        return EXCEPTION_CONTINUE_SEARCH;
#else
    PBYTE p = (PBYTE)Context->Eip;

    BYTE instr = p[0];
    if (instr != 0xEE && instr != 0xEC && instr != 0xE6 && instr != 0xE4 && instr != 0xFA && instr != 0xFB)
        return EXCEPTION_CONTINUE_SEARCH;

    // -----------Capture acceleration
    if (instr == 0xEC && (Context->Eip & 0x1FFFF) == 0x499E) {
        // ControlCTRLLines acceleration

        USHORT addr = Context->Edx & 0xFFFF;
        BYTE v = DlPortReadPortUchar(addr);
        if ((Context->Ebx & 0xFF00) == 0) {
            // Clear bits
            v &= ~Context->Ebx & 0xFF;
        }
        else {
            // Set bits
            v |= Context->Ebx & 0xFF;
        }
        DlPortWritePortUchar(addr,v);
        Context->Eax &= 0xFFFFFF00;
        Context->Eax |= v;
        Context->Eip += 0x49B7 - 0x499E;

        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (instr == 0xEC && (Context->Eip & 0x1FFFF) == 0x58C0) {
        // Start of scan line readback (PS2)

    /*
                                                ; int ReadLinePS2(void)
.text:100058AC                                   ReadLinePS2     proc near               ; CODE XREF: ReadLine+14↓p
.text:100058AC 000 53                                            push    ebx
.text:100058AD 004 57                                            push    edi
.text:100058AE 008 66 8B 15 9C 60 02 10                          mov     dx, LPTBaseAddress
.text:100058B5 008 57                                            push    edi
.text:100058B6 00C 66 53                                         push    bx
.text:100058B8 00E 66 BB 20 FF                                   mov     bx, 0FF20h
.text:100058BC 00E 66 83 C2 02                                   add     dx, 2           ; Add
.text:100058C0 00E EC                                            in      al, dx
.text:100058C1 00E 0A C3                                         or      al, bl          ; Logical Inclusive OR
.text:100058C3 00E EE                                            out     dx, al
.text:100058C4 00E EC                                            in      al, dx
.text:100058C5 00E 66 83 EA 02                                   sub     dx, 2           ; Integer Subtraction
.text:100058C9 00E A2 34 61 02 10                                mov     byte_10026134, al
.text:100058CE
.text:100058CE                                   loc_100058CE:                           ; CODE XREF: ReadLinePS2+52↓j
.text:100058CE 00E 66 BB 02 FF                                   mov     bx, 0FF02h
.text:100058D2 00E 66 83 C2 02                                   add     dx, 2           ; Add
.text:100058D6 00E A0 34 61 02 10                                mov     al, byte_10026134
.text:100058DB 00E 0A C3                                         or      al, bl          ; Logical Inclusive OR
.text:100058DD 00E EE                                            out     dx, al
.text:100058DE 00E 66 83 EA 02                                   sub     dx, 2           ; Integer Subtraction
.text:100058E2 00E EC                                            in      al, dx
.text:100058E3 00E 66 50                                         push    ax
.text:100058E5 010 66 BB 02 00                                   mov     bx, 2
.text:100058E9 010 66 83 C2 02                                   add     dx, 2           ; Add
.text:100058ED 010 F6 D3                                         not     bl              ; One's Complement Negation
.text:100058EF 010 A0 34 61 02 10                                mov     al, byte_10026134
.text:100058F4 010 22 C3                                         and     al, bl          ; Logical AND
.text:100058F6 010 EE                                            out     dx, al
.text:100058F7 010 66 83 EA 02                                   sub     dx, 2           ; Integer Subtraction
.text:100058FB 010 66 58                                         pop     ax
.text:100058FD 00E AA                                            stosb                   ; Store String
.text:100058FE 00E E2 CE                                         loop    loc_100058CE    ; Loop while CX != 0
.text:10005900 00E 66 BB 20 00                                   mov     bx, 20h ; ' '
.text:10005904 00E 66 83 C2 02                                   add     dx, 2           ; Add
.text:10005908 00E EC                                            in      al, dx
.text:10005909 00E F6 D3                                         not     bl              ; One's Complement Negation
.text:1000590B 00E 22 C3                                         and     al, bl          ; Logical AND
.text:1000590D 00E EE                                            out     dx, al
.text:1000590E 00E 66 83 EA 02                                   sub     dx, 2           ; Integer Subtraction
.text:10005912 00E 66 5B                                         pop     bx
.text:10005914 00C 58                                            pop     eax
.text:10005915 008 2B F8                                         sub     edi, eax        ; Integer Subtraction
.text:10005917 008 8B C7                                         mov     eax, edi
.text:10005919 008 5F                                            pop     edi
.text:1000591A 004 5B                                            pop     ebx
.text:1000591B 000 C3                                            retn                    ; Return Near from Procedure
.text:1000591B
     */
        USHORT addr = Context->Edx & 0xFFFF;
        addr -= 2;

        // Enable input mode
        BYTE v = DlPortReadPortUchar(addr + 2);
        DlPortWritePortUchar(addr + 2, v | 0x20);

        // Read each byte
        BYTE orgCtl = DlPortReadPortUchar(addr + 2);
        DWORD count = Context->Ecx;
        PBYTE dst = (PBYTE)Context->Edi;
        do {
            DlPortWritePortUchar(addr + 2, orgCtl | 0x02);
            *dst++ = DlPortReadPortUchar(addr);
            DlPortWritePortUchar(addr + 2, orgCtl & (~0x02));
        } while (--count);

        // Disable input mode
        v = DlPortReadPortUchar(addr + 2);
        DlPortWritePortUchar(addr + 2, v & (~0x20));
        
        // Resume execution
        Context->Edi = (DWORD)dst;
        Context->Ecx = 0;

        // Skip routine
        Context->Eip += 0x590E - 0x58C0;
        return EXCEPTION_CONTINUE_EXECUTION;

    }

    if (instr == 0xEE && (Context->Eip & 0x1FFFF) == 0x5927) {
        // Start of scan line readback (SPP)

       /*
.text:1000591C                                   ; int ReadLineSPP(void)
.text:1000591C                                   ReadLineSPP     proc near               ; CODE XREF: ReadLine:loc_100059A4↓p
.text:1000591C 000 53                                            push    ebx
.text:1000591D 004 57                                            push    edi
.text:1000591E 008 66 8B 15 9C 60 02 10                          mov     dx, LPTBaseAddress
.text:10005925 008 B0 FF                                         mov     al, 0FFh
.text:10005927 008 EE                                            out     dx, al
.text:10005928 008 66 57                                         push    di
.text:1000592A
.text:1000592A                                   loc_1000592A:                           ; CODE XREF: ReadLineSPP+60↓j
.text:1000592A 00A 66 53                                         push    bx
.text:1000592C 00C 66 BB 02 FF                                   mov     bx, 0FF02h
.text:10005930 00C 66 83 C2 02                                   add     dx, 2           ; Add
.text:10005934 00C EC                                            in      al, dx
.text:10005935 00C 0A C3                                         or      al, bl          ; Logical Inclusive OR
.text:10005937 00C 66 BB 08 FF                                   mov     bx, 0FF08h
.text:1000593B 00C 0A C3                                         or      al, bl          ; Logical Inclusive OR
.text:1000593D 00C EE                                            out     dx, al
.text:1000593E 00C 66 83 C2 FF                                   add     dx, 0FFFFh      ; Add
.text:10005942 00C EC                                            in      al, dx
.text:10005943 00C 66 50                                         push    ax
.text:10005945 00E 66 BB 08 00                                   mov     bx, 8
.text:10005949 00E 66 83 C2 01                                   add     dx, 1           ; Add
.text:1000594D 00E EC                                            in      al, dx
.text:1000594E 00E F6 D3                                         not     bl              ; One's Complement Negation
.text:10005950 00E 22 C3                                         and     al, bl          ; Logical AND
.text:10005952 00E EE                                            out     dx, al
.text:10005953 00E 66 83 C2 FF                                   add     dx, 0FFFFh      ; Add
.text:10005957 00E EC                                            in      al, dx
.text:10005958 00E 66 50                                         push    ax
.text:1000595A 010 66 BB 02 00                                   mov     bx, 2
.text:1000595E 010 66 83 C2 01                                   add     dx, 1           ; Add
.text:10005962 010 EC                                            in      al, dx
.text:10005963 010 F6 D3                                         not     bl              ; One's Complement Negation
.text:10005965 010 22 C3                                         and     al, bl          ; Logical AND
.text:10005967 010 EE                                            out     dx, al
.text:10005968 010 66 83 EA 02                                   sub     dx, 2           ; Integer Subtraction
.text:1000596C 010 66 58                                         pop     ax
.text:1000596E 00E 66 5B                                         pop     bx
.text:10005970 00C F8                                            clc                     ; Clear Carry Flag
.text:10005971 00C C0 E8 04                                      shr     al, 4           ; Shift Logical Right
.text:10005974 00C 80 E3 F0                                      and     bl, 0F0h        ; Logical AND
.text:10005977 00C 0A C3                                         or      al, bl          ; Logical Inclusive OR
.text:10005979 00C 66 5B                                         pop     bx
.text:1000597B 00A AA                                            stosb                   ; Store String
.text:1000597C 00A E2 AC                                         loop    loc_1000592A    ; Loop while CX != 0
.text:1000597E 00A 66 58                                         pop     ax
.text:10005980 008 66 2B F8                                      sub     di, ax          ; Integer Subtraction
.text:10005983 008 66 8B C7                                      mov     ax, di
.text:10005986 008 5F                                            pop     edi
.text:10005987 004 5B                                            pop     ebx
.text:10005988 000 C3                                            retn                    ; Return Near from Procedure
.text:10005988                                   ReadLineSPP     endp
        */

        USHORT addr = Context->Edx & 0xFFFF;
        DlPortWritePortUchar(addr, 0xFF);

        DWORD count = Context->Ecx;
        PBYTE dst = (PBYTE)Context->Edi;

        BYTE orgCtl = DlPortReadPortUchar(addr + 2);
        do {
            DlPortWritePortUchar(addr + 2, orgCtl | (0x2 | 0x8));
            BYTE rd1 = DlPortReadPortUchar(addr + 1);
            DlPortWritePortUchar(addr + 2, orgCtl & (~0x8));
            BYTE rd2 = DlPortReadPortUchar(addr + 1);
            DlPortWritePortUchar(addr + 2, orgCtl & (~0x2));
            *dst++ = (rd2 >> 4) | (rd1 & 0xF0);
        } while (--count);

        // Resume execution
        Context->Edi = (DWORD)dst;
        Context->Eax = Context->Ecx;
        Context->Ecx = 0;

        // Skip routine
        Context->Eip += 0x5986 - 0x5927;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (instr == 0xEC && (Context->Eip & 0x1FFFF) == 0x58A1) {
        // Start of scan line readback (EPP)

/*
text:10005894                                   ; int ReadLineEPP(void)
.text:10005894                                   ReadLineEPP     proc near               ; CODE XREF: ReadLine+9↓p
.text:10005894 000 57                                            push    edi
.text:10005895 004 66 8B 15 9C 60 02 10                          mov     dx, LPTBaseAddress
.text:1000589C 004 66 83 C2 04                                   add     dx, 4           ; Add
.text:100058A0 004 57                                            push    edi
.text:100058A1
.text:100058A1                                   loc_100058A1:                           ; CODE XREF: ReadLineEPP+F↓j
.text:100058A1 008 EC                                            in      al, dx
.text:100058A2 008 AA                                            stosb                   ; Store String
.text:100058A3 008 E2 FC                                         loop    loc_100058A1    ; Loop while CX != 0
.text:100058A5 008 58                                            pop     eax
.text:100058A6 004 2B F8                                         sub     edi, eax        ; Integer Subtraction
.text:100058A8 004 8B C7                                         mov     eax, edi
.text:100058AA 004 5F                                            pop     edi
.text:100058AB 000 C3                                            retn                    ; Return Near from Procedure
*/
        DWORD count = Context->Ecx;
        PBYTE dst = (PBYTE)Context->Edi;
        USHORT addr = Context->Edx & 0xFFFF;
        do {
            *dst++ = DlPortReadPortUchar(addr);
        } while (--count);
        Context->Edi = (DWORD)dst;
        Context->Ecx = 0;

        // Skip routine
        Context->Eip += 0x58A5 - 0x58A1;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (instr == 0xFA && (Context->Eip & 0x1FFFF) == 0x4DD3) {
        // Start of timer readback

        // Plug in the value to return
        Context->Eax &= 0xFFFF0000;
        Context->Eax |= PITValue ^ 0xFFFF;

        rdCount++;
        if (rdCount > 2) { // The first 2 reads will return 0xFFFF, just to avoid problems
            LARGE_INTEGER currTimerValue;
            QueryPerformanceCounter((LARGE_INTEGER*)&currTimerValue);
            uint64_t ctr = ((currTimerValue.QuadPart - StartTimerValue.QuadPart) / TimerFreq.QuadPart);
            PITValue = (ctr > 65535ULL) ? 0 : (USHORT)(65535ULL - ctr);
        }

        // Skip routine
        Context->Eip += 0x4E08 - 0x4DD3;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (instr == 0xE6 && (Context->Eip & 0x1FFFF) == 0x4DB2) {
        // Start of timer reset

        // Starting timer
        QueryPerformanceFrequency(&TimerFreq);
        TimerFreq.QuadPart /= 1000000ULL; // Convert to MHZ
        if (!TimerFreq.QuadPart) TimerFreq.QuadPart = 1;
        QueryPerformanceCounter(&StartTimerValue);
        rdStep = 0;
        rdCount = 0;
        PITValue = 0xFFFF;

        // Skip routine
        Context->Eip += 0x4DC6 - 0x4DB2;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    //--------------------
    
    // CLI/STI
    if (instr == 0xFA || instr == 0xFB) {
        Context->Eip++;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    USHORT addr = (instr == 0xEE || instr == 0xEC) 
        ? (USHORT) (Context->Edx & 0xFFFF) 
        : p[1];
    
    // Emulate timer and interrupt controller, as it is used to get
    //  microsecond timer resolutions 
    if (addr == 0x40 || addr == 0x43 || addr == 0x21 || addr == 0x20) {
        // 34=>43 means start PIT
        // 00=>43 means read LO/HI decreasing
        // 1.19318 MHz

        // By default, clean AL
        if (instr == 0xE4 || instr == 0xEC) {
            Context->Eax &= 0xFFFFFF00;
        }

        if ((instr == 0xE6 || instr == 0xEE) && addr == 0x43)
        {
            if ((Context->Eax & 0xFF) == 0x34) {
                // Starting timer
                QueryPerformanceFrequency(&TimerFreq);
                TimerFreq.QuadPart /= 1000000ULL; // Convert to MHZ
                if (!TimerFreq.QuadPart) TimerFreq.QuadPart = 1;
                QueryPerformanceCounter(&StartTimerValue);
                rdStep = 0;
                rdCount = 0;
                PITValue = 0xFFFF;
            }

            if ((Context->Eax & 0xFF) == 0x00) {
                rdCount++;
                if (rdCount > 2) { // The first 2 reads will return 0xFFFF, just to avoid problems
                    LARGE_INTEGER currTimerValue;
                    QueryPerformanceCounter((LARGE_INTEGER*)&currTimerValue);
                    uint64_t ctr = ((currTimerValue.QuadPart - StartTimerValue.QuadPart) / TimerFreq.QuadPart);
                    PITValue = (ctr > 65535ULL) ? 0 : (USHORT)(65535ULL - ctr);
                }
                rdStep = 0;
            }
        }

        if ((instr == 0xEC || instr == 0xE4) && addr == 0x40) {
            Context->Eax |= ((rdStep) ? PITValue >> 8 : PITValue) & 0xFF;
            rdStep ^= 1;
        }

        Context->Eip += (instr == 0xEE || instr == 0xEC) ? 1 : 2;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // The following LPT is never emulated
    if ((addr >= 0x278 && addr <= 0x27F) ||
        (addr >= 0x678 && addr <= 0x67F)
        ) {

        if (instr == 0xE4 || instr == 0xEC) {
            Context->Eax &= 0xFFFFFF00;
        }

        Context->Eip += (instr == 0xEE || instr == 0xEC) ? 1 : 2;

        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // This one will always be emulated, to avoid crashes...
    if (addr >= 0x3BC && addr <= 0x3BF) {

        if (instr == 0xE4 || instr == 0xEC) {
            Context->Eax &= 0xFFFFFF00;
            Context->Eax |= emulatedPort[addr - 0x3BC];
        }
        else {
            emulatedPort[addr - 0x3BC] = (BYTE)Context->Eax;
        }

        Context->Eip += (instr == 0xEE || instr == 0xEC) ? 1 : 2;

        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (addr >= 0x7BC && addr <= 0x7BF) {

        if (instr == 0xE4 || instr == 0xEC) {
            Context->Eax &= 0xFFFFFF00;
            Context->Eax |= emulatedPortHI[addr - 0x7BC];
        }
        else {
            emulatedPortHI[addr - 0x7BC] = (BYTE)Context->Eax;
        }

        Context->Eip += (instr == 0xEE || instr == 0xEC) ? 1 : 2;

        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if ((addr >= 0x378 && addr <= 0x37F) ||
        (addr >= 0x778 && addr <= 0x77F)) {

        // Emulate instructions
        if (instr == 0xE4 || instr == 0xEC) {
            Context->Eax &= 0xFFFFFF00;
            Context->Eax |= DlPortReadPortUchar(addr);
        }
        else {
            DlPortWritePortUchar(addr, (BYTE)Context->Eax);
        }

        Context->Eip += (instr == 0xEE || instr == 0xEC) ? 1 : 2;

        return EXCEPTION_CONTINUE_EXECUTION;
    }
    else
        return EXCEPTION_CONTINUE_SEARCH;

#endif    
}

static void End(void)
{
    if (hOldVectorHandler) {
        RemoveVectoredExceptionHandler(hOldVectorHandler);
        hOldVectorHandler = NULL;
    }
    if (hRIC32Dll) {
        FreeLibrary(hRIC32Dll);
        hRIC32Dll = NULL;
    }
    if (hInpOutDll) {
        FreeLibrary(hInpOutDll);
        hInpOutDll = NULL;
    }

    IsInpOutDriverOpen = NULL;
    DlPortReadPortUchar = NULL;
    DlPortWritePortUchar = NULL;
    orgDS_Entry = NULL;
}


static void Init(void)
{
    hOldVectorHandler = AddVectoredExceptionHandler(CALL_FIRST, VectoredHandler);
    hRIC32Dll = LoadLibraryA("RIC32.ds_");
    hInpOutDll = LoadLibraryA("inpout32.dll");
    if (!hInpOutDll) {
        MessageBoxA(NULL, "Unable to open inout DLL", "ERROR", MB_OK | MB_ICONERROR);
        End();
        return;
    }
    if (!hRIC32Dll) {
        MessageBoxA(NULL, "Unable to open RIC32 DLL", "ERROR", MB_OK | MB_ICONERROR);
        End();
        return;
    }


    // Get the IO functions we need to communicate with the scanner.
    IsInpOutDriverOpen = (lpIsInpOutDriverOpen)GetProcAddress(hInpOutDll, "IsInpOutDriverOpen");
    DlPortReadPortUchar = (lpDlPortReadPortUchar)GetProcAddress(hInpOutDll, "DlPortReadPortUchar");
    DlPortWritePortUchar = (lpDlPortWritePortUchar)GetProcAddress(hInpOutDll, "DlPortWritePortUchar");

    // Get the DS_Entry 
    orgDS_Entry = (lpDS_Entry)GetProcAddress(hRIC32Dll, "DS_Entry");

    if (!IsInpOutDriverOpen ||
        !DlPortReadPortUchar ||
        !DlPortWritePortUchar ||
        !orgDS_Entry) {
        MessageBoxA(NULL, "Unable to resolve entry points", "ERROR", MB_OK | MB_ICONERROR);
        End();
        return;
    }

    if (!IsInpOutDriverOpen()) {
        MessageBoxA(NULL, "Unable to initialize IO driver (must execute as Admin)", "ERROR", MB_OK | MB_ICONERROR);
        End();
        return;
    }
}



// Forward to the loaded Dll
#ifndef _AMD64_
#pragma comment(linker, "/EXPORT:DS_Entry=_DS_Entry@20")
#endif
extern "C" uint16_t __stdcall DS_Entry(void* pOrigin, uint32_t DG, uint16_t DAT, uint16_t MSG, void* pData)
{
    if (!orgDS_Entry)
        return 0;
    return orgDS_Entry(pOrigin, DG, DAT, MSG, pData);
}

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        Init();
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    case DLL_PROCESS_DETACH:
        End();
        break;
    }
    return TRUE;
}

