// ParallelPortEnum.cpp : Este archivo contiene la función "main". La ejecución del programa comienza y termina ahí.
//

#include <iostream>
#include <Windows.h>
#include <initguid.h>  // Put this in to get rid of linker errors.
#include <devpkey.h>
#include <devguid.h>    // for GUID_DEVCLASS_CDROM etc
#include <setupapi.h>
#include <cfgmgr32.h>   // for MAX_DEVICE_ID_LEN, CM_Get_Parent and CM_Get_Device_ID
#define INITGUID
#include <tchar.h>

int main()
{
    SP_DEVINFO_DATA DeviceInfoData;
    UINT DeviceIndex;

    HDEVINFO DeviceInfoSet = SetupDiGetClassDevs(
        NULL,
        NULL,
        NULL,
        DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (DeviceInfoSet == INVALID_HANDLE_VALUE)
        return 1;


    ZeroMemory(&DeviceInfoData, sizeof(SP_DEVINFO_DATA));
    DeviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
    DeviceIndex = 0;

    while (SetupDiEnumDeviceInfo(
        DeviceInfoSet,
        DeviceIndex,
        &DeviceInfoData)) {
        DeviceIndex++;

        DEVPROPTYPE PropType;
        GUID DevGuid;
        DWORD Size;

        if (SetupDiGetDeviceProperty(
            DeviceInfoSet,
            &DeviceInfoData,
            &DEVPKEY_Device_ClassGuid,
            &PropType,
            (PBYTE)&DevGuid,
            sizeof(GUID),
            &Size,
            0) 
            && PropType == DEVPROP_TYPE_GUID 
            && IsEqualGUID(GUID_DEVCLASS_PORTS, DevGuid)) {

            // Dealing with a port. Check if it is a parallel port
            TCHAR FriendlyName[1024];
            if (SetupDiGetDeviceProperty(
                DeviceInfoSet,
                &DeviceInfoData,
                &DEVPKEY_Device_FriendlyName,
                &PropType,
                (PBYTE)FriendlyName,
                sizeof(FriendlyName),
                &Size,
                0)
                && PropType == DEVPROP_TYPE_STRING) {

                // Dealing with a port. Check if it is a parallel port
                LPCSTR pos = (LPCSTR) _tcsstr(FriendlyName, TEXT("(LPT"));
                if (pos != NULL) {
                    // Dealing with a parallel port, need to get its resources
                }

            }
        }
    }

    if (DeviceInfoSet) {
        SetupDiDestroyDeviceInfoList(DeviceInfoSet);
    }
    std::cout << "Hello World!\n";
    return 0;
}

// Ejecutar programa: Ctrl + F5 o menú Depurar > Iniciar sin depurar
// Depurar programa: F5 o menú Depurar > Iniciar depuración

// Sugerencias para primeros pasos: 1. Use la ventana del Explorador de soluciones para agregar y administrar archivos
//   2. Use la ventana de Team Explorer para conectar con el control de código fuente
//   3. Use la ventana de salida para ver la salida de compilación y otros mensajes
//   4. Use la ventana Lista de errores para ver los errores
//   5. Vaya a Proyecto > Agregar nuevo elemento para crear nuevos archivos de código, o a Proyecto > Agregar elemento existente para agregar archivos de código existentes al proyecto
//   6. En el futuro, para volver a abrir este proyecto, vaya a Archivo > Abrir > Proyecto y seleccione el archivo .sln
