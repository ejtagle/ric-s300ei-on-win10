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
    
    if (instr == 0xFA || instr == 0xFB) {
        Context->Rip++;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    USHORT addr = (instr == 0xEE || instr == 0xEC) 
        ? (USHORT)(Context->Rdx & 0xFFFF) 
        : p[1];
    
    // Do not emulate timer and interrupt controller
    if (addr == 0x40 || addr == 0x43 || addr == 0x21 || addr == 0x20) {
        // Simulate no available parallel ports here
        if (instr == 0xE4 || instr == 0xEC) {

            Context->Eax &= 0xFFFFFF00;
        }
        Context->Rip += (instr == 0xEE || instr == 0xEC) ? 1 : 2;

        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // The following ones do NOT exist
    if ((addr >= 0x278 && addr <= 0x27F) ||
        (addr >= 0x678 && addr <= 0x67F) ||
        (addr >= 0x3BC && addr <= 0x3BF) || addr == 0x20
        ) {

        if (instr == 0xE4 || instr == 0xEC) {

            //  EC in  al, dx
            Context->Eax &= 0xFFFFFF00;
        }

        Context->Rip += (instr == 0xEE || instr == 0xEC) ? 1 : 2;

        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if ((addr >= 0x378 && addr <= 0x37F) ||
        (addr >= 0x778 && addr <= 0x77F)) {

        // Emulate instructions
        if (instr == 0xEE) {
            //  EE out dx, al
            DlPortWritePortUchar(addr, (BYTE)Context->Rax);
}
        else {
            //  EC in  al, dx
            Context->Rax &= 0xFFFFFF00;
            Context->Rax |= DlPortReadPortUchar(addr);
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
#pragma comment(linker, "/EXPORT:DS_Entry=_DS_Entry@20")
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

