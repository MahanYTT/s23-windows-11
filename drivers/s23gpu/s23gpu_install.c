/*
 * s23gpu_install.c - Idempotent ARM64 Root Device + Driver Installer for ROOT\S23GPU
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <stdio.h>

static const WCHAR kS23GpuHwIds[] =
    L"ROOT\\S23GPU\0"
    L"PCI\\VEN_5143&DEV_0740&SUBSYS_07405143&REV_01\0"
    L"PCI\\VEN_5143&DEV_0740\0";

static int EnsureRootS23GpuHwIds(void)
{
    HDEVINFO dis = SetupDiGetClassDevsW(NULL, L"ROOT", NULL, DIGCF_ALLCLASSES);
    if (dis == INVALID_HANDLE_VALUE) return 0;

    SP_DEVINFO_DATA did;
    did.cbSize = sizeof(did);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(dis, i, &did); i++) {
        WCHAR hwid[256] = {0};
        if (SetupDiGetDeviceRegistryPropertyW(dis, &did, SPDRP_HARDWAREID, NULL,
                                              (PBYTE)hwid, sizeof(hwid) - sizeof(WCHAR), NULL)) {
            if (_wcsicmp(hwid, L"ROOT\\S23GPU") == 0) {
                SetupDiSetDeviceRegistryPropertyW(dis, &did, SPDRP_HARDWAREID,
                                                  (const BYTE *)kS23GpuHwIds, sizeof(kS23GpuHwIds));
                SetupDiDestroyDeviceInfoList(dis);
                return 1;
            }
        }
    }
    SetupDiDestroyDeviceInfoList(dis);
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2) {
        wprintf(L"Usage: s23gpu_install.exe <path-to-s23gpu.inf>\n");
        return 2;
    }
    LPCWSTR infPath = argv[1];

    if (!EnsureRootS23GpuHwIds()) {
        HDEVINFO dis = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_DISPLAY, NULL);
        if (dis == INVALID_HANDLE_VALUE) {
            wprintf(L"[ERR] SetupDiCreateDeviceInfoList err=%lu\n", GetLastError());
            return 1;
        }
        SP_DEVINFO_DATA did;
        did.cbSize = sizeof(did);
        if (!SetupDiCreateDeviceInfoW(dis, L"DISPLAY", &GUID_DEVCLASS_DISPLAY,
                                      L"Qualcomm Adreno 740 GPU (S23)",
                                      NULL, DICD_GENERATE_ID, &did)) {
            wprintf(L"[ERR] SetupDiCreateDeviceInfoW err=%lu\n", GetLastError());
            SetupDiDestroyDeviceInfoList(dis);
            return 1;
        }
        if (!SetupDiSetDeviceRegistryPropertyW(dis, &did, SPDRP_HARDWAREID,
                                               (const BYTE *)kS23GpuHwIds, sizeof(kS23GpuHwIds))) {
            wprintf(L"[ERR] SetDeviceRegistryProperty err=%lu\n", GetLastError());
            SetupDiDestroyDeviceInfoList(dis);
            return 1;
        }
        if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, dis, &did)) {
            wprintf(L"[ERR] DIF_REGISTERDEVICE err=%lu\n", GetLastError());
            SetupDiDestroyDeviceInfoList(dis);
            return 1;
        }
        SetupDiDestroyDeviceInfoList(dis);
        wprintf(L"Created ROOT\\S23GPU root device node (VEN_5143&DEV_0740)\n");
    } else {
        wprintf(L"ROOT\\S23GPU device node already exists (updated HardwareIDs to VEN_5143&DEV_0740)\n");
    }

    BOOL reboot = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(NULL, L"ROOT\\S23GPU", infPath, INSTALLFLAG_FORCE, &reboot)) {
        wprintf(L"[ERR] UpdateDriverForPlugAndPlayDevicesW err=0x%08lx\n", GetLastError());
        return 1;
    }
    wprintf(L"Bound driver %ls to ROOT\\S23GPU (reboot=%d)\n", infPath, reboot ? 1 : 0);
    return 0;
}
