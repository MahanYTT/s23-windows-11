/*
 * install_root_mbb.c - Idempotent ARM64 Root Net Device & MBBCx Driver Installer for ROOT\S23MBB
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <stdio.h>
#include <wchar.h>

static const WCHAR kTargetHwId[] = L"ROOT\\S23MBB";
static const WCHAR kS23MbbHwIds[] =
    L"ROOT\\S23MBB\0"
    L"ACPI\\S23MBB01\0";

static BOOL MultiSzContainsI(const WCHAR *msz, const WCHAR *needle)
{
    for (const WCHAR *p = msz; p && *p; p += wcslen(p) + 1) {
        if (_wcsicmp(p, needle) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static int UninstallRootS23Mbb(void)
{
    HDEVINFO dis = SetupDiGetClassDevsW(NULL, L"ROOT", NULL, DIGCF_ALLCLASSES);
    if (dis == INVALID_HANDLE_VALUE) {
        wprintf(L"[ERR] SetupDiGetClassDevsW err=%lu\n", GetLastError());
        return -1;
    }

    int removed = 0;
    SP_DEVINFO_DATA did;
    did.cbSize = sizeof(did);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(dis, i, &did); i++) {
        WCHAR hwid[512] = {0};
        if (!SetupDiGetDeviceRegistryPropertyW(dis, &did, SPDRP_HARDWAREID, NULL,
                                               (PBYTE)hwid, sizeof(hwid) - 2 * sizeof(WCHAR), NULL)) {
            continue;
        }
        if (!MultiSzContainsI(hwid, kTargetHwId)) {
            continue;
        }
        WCHAR instId[256] = {0};
        SetupDiGetDeviceInstanceIdW(dis, &did, instId, ARRAYSIZE(instId), NULL);
        BOOL reboot = FALSE;
        if (DiUninstallDevice(NULL, dis, &did, 0, &reboot)) {
            wprintf(L"Removed %ls via DiUninstallDevice (reboot=%d)\n", instId, reboot ? 1 : 0);
            removed++;
        } else if (SetupDiCallClassInstaller(DIF_REMOVE, dis, &did)) {
            wprintf(L"Removed %ls via DIF_REMOVE\n", instId);
            removed++;
        } else {
            wprintf(L"[ERR] Failed to remove %ls err=%lu\n", instId, GetLastError());
            SetupDiDestroyDeviceInfoList(dis);
            return -1;
        }
    }
    SetupDiDestroyDeviceInfoList(dis);
    wprintf(L"Uninstall complete: %d node(s) removed\n", removed);
    return removed;
}

static int EnsureRootS23MbbHwIds(void)
{
    HDEVINFO dis = SetupDiGetClassDevsW(NULL, L"ROOT", NULL, DIGCF_ALLCLASSES);
    if (dis == INVALID_HANDLE_VALUE) return 0;

    SP_DEVINFO_DATA did;
    did.cbSize = sizeof(did);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(dis, i, &did); i++) {
        WCHAR hwid[512] = {0};
        if (SetupDiGetDeviceRegistryPropertyW(dis, &did, SPDRP_HARDWAREID, NULL,
                                              (PBYTE)hwid, sizeof(hwid) - 2 * sizeof(WCHAR), NULL)) {
            if (MultiSzContainsI(hwid, kTargetHwId)) {
                SetupDiSetDeviceRegistryPropertyW(dis, &did, SPDRP_HARDWAREID,
                                                  (const BYTE *)kS23MbbHwIds, sizeof(kS23MbbHwIds));
                SetupDiDestroyDeviceInfoList(dis);
                return 1;
            }
        }
    }
    SetupDiDestroyDeviceInfoList(dis);
    return 0;
}

static void PrintDevNodeStatus(void)
{
    HDEVINFO dis = SetupDiGetClassDevsW(&GUID_DEVCLASS_NET, L"ROOT", NULL, DIGCF_PRESENT);
    if (dis == INVALID_HANDLE_VALUE) return;

    SP_DEVINFO_DATA did;
    did.cbSize = sizeof(did);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(dis, i, &did); i++) {
        WCHAR hwid[512] = {0};
        if (SetupDiGetDeviceRegistryPropertyW(dis, &did, SPDRP_HARDWAREID, NULL,
                                              (PBYTE)hwid, sizeof(hwid) - 2 * sizeof(WCHAR), NULL) &&
            MultiSzContainsI(hwid, kTargetHwId)) {
            WCHAR instId[256] = {0};
            ULONG status = 0, problem = 0;
            SetupDiGetDeviceInstanceIdW(dis, &did, instId, ARRAYSIZE(instId), NULL);
            CONFIGRET cr = CM_Get_DevNode_Status(&status, &problem, did.DevInst, 0);
            wprintf(L"DevNode %ls: cr=0x%lx status=0x%08lx (started=%d) problem=%lu\n",
                    instId, (unsigned long)cr, status, (status & DN_STARTED) ? 1 : 0, problem);
        }
    }
    SetupDiDestroyDeviceInfoList(dis);
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2) {
        wprintf(L"Usage:\n"
                L"  install_root_mbb.exe <path-to-s23mbb.inf>\n"
                L"  install_root_mbb.exe --reinstall <path-to-s23mbb.inf>\n"
                L"  install_root_mbb.exe --uninstall\n");
        return 2;
    }

    if (_wcsicmp(argv[1], L"--uninstall") == 0) {
        return UninstallRootS23Mbb() < 0 ? 1 : 0;
    }

    LPCWSTR rawInf = argv[1];
    if (_wcsicmp(argv[1], L"--reinstall") == 0) {
        if (argc < 3) {
            wprintf(L"[ERR] --reinstall requires <path-to-s23mbb.inf>\n");
            return 2;
        }
        if (UninstallRootS23Mbb() < 0) return 1;
        rawInf = argv[2];
    }

    WCHAR fullInf[MAX_PATH] = {0};
    DWORD n = GetFullPathNameW(rawInf, ARRAYSIZE(fullInf), fullInf, NULL);
    if (n == 0 || n >= ARRAYSIZE(fullInf) || GetFileAttributesW(fullInf) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"[ERR] INF not found: %ls (err=%lu)\n", rawInf, GetLastError());
        return 1;
    }

    if (!EnsureRootS23MbbHwIds()) {
        HDEVINFO dis = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_NET, NULL);
        if (dis == INVALID_HANDLE_VALUE) {
            wprintf(L"[ERR] SetupDiCreateDeviceInfoList err=%lu\n", GetLastError());
            return 1;
        }
        SP_DEVINFO_DATA did;
        did.cbSize = sizeof(did);
        if (!SetupDiCreateDeviceInfoW(dis, L"Net", &GUID_DEVCLASS_NET,
                                      L"Samsung Galaxy S23 Snapdragon X70 5G Modem",
                                      NULL, DICD_GENERATE_ID, &did)) {
            wprintf(L"[ERR] SetupDiCreateDeviceInfoW err=%lu\n", GetLastError());
            SetupDiDestroyDeviceInfoList(dis);
            return 1;
        }
        if (!SetupDiSetDeviceRegistryPropertyW(dis, &did, SPDRP_HARDWAREID,
                                               (const BYTE *)kS23MbbHwIds, sizeof(kS23MbbHwIds))) {
            wprintf(L"[ERR] SetupDiSetDeviceRegistryPropertyW err=%lu\n", GetLastError());
            SetupDiDestroyDeviceInfoList(dis);
            return 1;
        }
        if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, dis, &did)) {
            wprintf(L"[ERR] DIF_REGISTERDEVICE err=%lu\n", GetLastError());
            SetupDiDestroyDeviceInfoList(dis);
            return 1;
        }
        SetupDiDestroyDeviceInfoList(dis);
        wprintf(L"Created ROOT\\S23MBB root Net device node (HWIDs: ROOT\\S23MBB, ACPI\\S23MBB01)\n");
    } else {
        wprintf(L"ROOT\\S23MBB device node already exists (updated HardwareIDs)\n");
    }

    BOOL reboot = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(NULL, kTargetHwId, fullInf, INSTALLFLAG_FORCE, &reboot)) {
        wprintf(L"[ERR] UpdateDriverForPlugAndPlayDevicesW err=0x%08lx\n", GetLastError());
        return 1;
    }
    wprintf(L"Bound driver %ls to %ls (reboot=%d)\n", fullInf, kTargetHwId, reboot ? 1 : 0);
    PrintDevNodeStatus();
    return 0;
}
