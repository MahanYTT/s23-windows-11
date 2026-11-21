/*
 * s23aclsvc.c - Native ARM64 LocalSystem one-shot NTFS ACL recovery service for S23 (dm1q)
 *
 * Runs directly under services.exe (LocalSystem, zero RPC needed), repairs the chkdsk-wiped
 * NTFS Security Descriptors using icacls.exe with pure numeric *S-1-... SIDs, logs each step
 * write-through to C:\Users\Public\s23_aclsvc.log, disables itself (Start=4), flushes HKLM,
 * and reboots cleanly via ntdll!NtShutdownSystem(ShutdownReboot).
 */

#include <stdint.h>
#include <stddef.h>

typedef uint16_t WCHAR;
typedef uint32_t DWORD;
typedef uint32_t ULONG;
typedef int32_t  LONG;
typedef int32_t  BOOL;
typedef void    *HANDLE;
typedef void    *LPVOID;
typedef void    *HKEY;
typedef uintptr_t SERVICE_STATUS_HANDLE;

#define TRUE  1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INFINITE             0xFFFFFFFFU

#define GENERIC_WRITE        0x40000000U
#define FILE_SHARE_READ      0x00000001U
#define FILE_SHARE_WRITE     0x00000002U
#define CREATE_ALWAYS        2U
#define OPEN_EXISTING        3U
#define FILE_ATTRIBUTE_NORMAL   0x00000080U
#define FILE_FLAG_WRITE_THROUGH 0x80000000U

#define CREATE_NO_WINDOW     0x08000000U
#define SERVICE_WIN32_OWN_PROCESS 0x00000010U
#define SERVICE_START_PENDING     0x00000002U
#define SERVICE_RUNNING           0x00000004U
#define SERVICE_STOPPED           0x00000001U
#define SERVICE_CONTROL_STOP      0x00000001U
#define SERVICE_CONTROL_SHUTDOWN  0x00000005U
#define SERVICE_ACCEPT_STOP       0x00000001U
#define SERVICE_ACCEPT_SHUTDOWN   0x00000004U

#define HKEY_LOCAL_MACHINE   ((HKEY)(uintptr_t)0x80000002ULL)
#define KEY_SET_VALUE        0x0002U
#define REG_DWORD            4U
#define TOKEN_ADJUST_PRIVILEGES 0x0020U
#define TOKEN_QUERY             0x0008U
#define SE_PRIVILEGE_ENABLED    0x00000002U

typedef struct _STARTUPINFOW {
    DWORD cb;
    WCHAR *lpReserved;
    WCHAR *lpDesktop;
    WCHAR *lpTitle;
    DWORD dwX, dwY, dwXSize, dwYSize;
    DWORD dwXCountChars, dwYCountChars;
    DWORD dwFillAttribute;
    DWORD dwFlags;
    uint16_t wShowWindow;
    uint16_t cbReserved2;
    uint8_t *lpReserved2;
    HANDLE hStdInput;
    HANDLE hStdOutput;
    HANDLE hStdError;
} STARTUPINFOW;

typedef struct _PROCESS_INFORMATION {
    HANDLE hProcess;
    HANDLE hThread;
    DWORD dwProcessId;
    DWORD dwThreadId;
} PROCESS_INFORMATION;

typedef struct _SERVICE_STATUS {
    DWORD dwServiceType;
    DWORD dwCurrentState;
    DWORD dwControlsAccepted;
    DWORD dwWin32ExitCode;
    DWORD dwServiceSpecificExitCode;
    DWORD dwCheckPoint;
    DWORD dwWaitHint;
} SERVICE_STATUS;

typedef void (*LPSERVICE_MAIN_FUNCTIONW)(DWORD argc, WCHAR **argv);
typedef void (*LPHANDLER_FUNCTION)(DWORD dwControl);

typedef struct _SERVICE_TABLE_ENTRYW {
    WCHAR *lpServiceName;
    LPSERVICE_MAIN_FUNCTIONW lpServiceProc;
} SERVICE_TABLE_ENTRYW;

typedef struct _LUID {
    DWORD LowPart;
    LONG  HighPart;
} LUID;

typedef struct _LUID_AND_ATTRIBUTES {
    LUID  Luid;
    DWORD Attributes;
} LUID_AND_ATTRIBUTES;

typedef struct _TOKEN_PRIVILEGES {
    DWORD PrivilegeCount;
    LUID_AND_ATTRIBUTES Privileges[1];
} TOKEN_PRIVILEGES;

/* kernel32.dll */
__declspec(dllimport) HANDLE CreateFileW(const WCHAR *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
__declspec(dllimport) BOOL   WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
__declspec(dllimport) BOOL   FlushFileBuffers(HANDLE);
__declspec(dllimport) BOOL   CloseHandle(HANDLE);
__declspec(dllimport) BOOL   CreateProcessW(const WCHAR *, WCHAR *, void *, void *, BOOL, DWORD, void *, const WCHAR *, STARTUPINFOW *, PROCESS_INFORMATION *);
__declspec(dllimport) DWORD  WaitForSingleObject(HANDLE, DWORD);
__declspec(dllimport) BOOL   GetExitCodeProcess(HANDLE, DWORD *);
__declspec(dllimport) DWORD  GetLastError(void);
__declspec(dllimport) DWORD  GetTickCount(void);
__declspec(dllimport) HANDLE CreateThread(void *, size_t, DWORD (*)(LPVOID), LPVOID, DWORD, DWORD *);
__declspec(dllimport) HANDLE GetCurrentProcess(void);
__declspec(dllimport) void   Sleep(DWORD);
__declspec(dllimport) void   ExitProcess(DWORD);

/* advapi32.dll */
__declspec(dllimport) BOOL                  StartServiceCtrlDispatcherW(const SERVICE_TABLE_ENTRYW *);
__declspec(dllimport) SERVICE_STATUS_HANDLE RegisterServiceCtrlHandlerW(const WCHAR *, LPHANDLER_FUNCTION);
__declspec(dllimport) BOOL                  SetServiceStatus(SERVICE_STATUS_HANDLE, SERVICE_STATUS *);
__declspec(dllimport) LONG                  RegOpenKeyExW(HKEY, const WCHAR *, DWORD, DWORD, HKEY *);
__declspec(dllimport) LONG                  RegSetValueExW(HKEY, const WCHAR *, DWORD, DWORD, const uint8_t *, DWORD);
__declspec(dllimport) LONG                  RegFlushKey(HKEY);
__declspec(dllimport) LONG                  RegCloseKey(HKEY);
__declspec(dllimport) BOOL                  OpenProcessToken(HANDLE, DWORD, HANDLE *);
__declspec(dllimport) BOOL                  AdjustTokenPrivileges(HANDLE, BOOL, TOKEN_PRIVILEGES *, DWORD, void *, void *);

/* ntdll.dll */
__declspec(dllimport) LONG NtShutdownSystem(ULONG Action);

static HANDLE g_log = INVALID_HANDLE_VALUE;
static SERVICE_STATUS_HANDLE g_ssh = 0;

static void LogOpen(void) {
    g_log = CreateFileW(
        L"C:\\Users\\Public\\s23_aclsvc.log",
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        NULL);
}

static void LogRaw(const char *s) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    DWORD len = 0;
    while (s[len]) len++;
    DWORD wr = 0;
    WriteFile(g_log, s, len, &wr, NULL);
    FlushFileBuffers(g_log);
}

static void LogU32(DWORD v) {
    char buf[16];
    int n = 0;
    if (v == 0) {
        buf[n++] = '0';
    } else {
        char tmp[16];
        int t = 0;
        while (v > 0) { tmp[t++] = (char)('0' + (v % 10U)); v /= 10U; }
        while (t > 0) buf[n++] = tmp[--t];
    }
    buf[n] = 0;
    LogRaw(buf);
}

static void LogHex(DWORD v) {
    static const char hx[] = "0123456789ABCDEF";
    char buf[11];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; i++) buf[2 + i] = hx[(v >> ((7 - i) * 4)) & 0xFU];
    buf[10] = 0;
    LogRaw(buf);
}

static void RunIcacls(const char *tag, const WCHAR *args, DWORD timeoutMs) {
    static WCHAR cmd[1024];
    const WCHAR *prefix = L"C:\\Windows\\System32\\icacls.exe ";
    size_t p = 0;
    while (prefix[p] && p < 1000) { cmd[p] = prefix[p]; p++; }
    size_t q = 0;
    while (args[q] && p < 1020) { cmd[p++] = args[q++]; }
    cmd[p] = 0;

    STARTUPINFOW si;
    uint8_t *sib = (uint8_t *)&si;
    for (size_t i = 0; i < sizeof(si); i++) sib[i] = 0;
    si.cb = sizeof(si);

    PROCESS_INFORMATION pi;
    pi.hProcess = NULL; pi.hThread = NULL; pi.dwProcessId = 0; pi.dwThreadId = 0;

    DWORD t0 = GetTickCount();
    LogRaw("[icacls] "); LogRaw(tag); LogRaw(" ... ");
    BOOL ok = CreateProcessW(L"C:\\Windows\\System32\\icacls.exe", cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (!ok) {
        LogRaw("CREATE_FAIL err="); LogU32(GetLastError()); LogRaw("\r\n");
        return;
    }
    DWORD wr = WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD ec = 0xFFFFFFFFU;
    GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    DWORD dt = GetTickCount() - t0;
    LogRaw("wait="); LogU32(wr); LogRaw(" rc="); LogHex(ec); LogRaw(" ms="); LogU32(dt); LogRaw("\r\n");
}

static void DisableSelfAndCleanSetup(void) {
    HKEY hk = NULL;
    DWORD start4 = 4U;
    LONG rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\S23AclFix", 0, KEY_SET_VALUE, &hk);
    if (rc == 0 && hk) {
        rc = RegSetValueExW(hk, L"Start", 0, REG_DWORD, (const uint8_t *)&start4, sizeof(start4));
        RegFlushKey(hk);
        RegCloseKey(hk);
        LogRaw("[reg] S23AclFix Start=4 rc="); LogU32((DWORD)rc); LogRaw("\r\n");
    } else {
        LogRaw("[reg] Open S23AclFix failed rc="); LogU32((DWORD)rc); LogRaw("\r\n");
    }

    hk = NULL;
    DWORD zero = 0U;
    WCHAR empty[1] = { 0 };
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\Setup", 0, KEY_SET_VALUE, &hk) == 0 && hk) {
        RegSetValueExW(hk, L"SetupType", 0, REG_DWORD, (const uint8_t *)&zero, sizeof(zero));
        RegSetValueExW(hk, L"SystemSetupInProgress", 0, REG_DWORD, (const uint8_t *)&zero, sizeof(zero));
        RegSetValueExW(hk, L"CmdLine", 0, 1U /* REG_SZ */, (const uint8_t *)empty, sizeof(empty));
        RegFlushKey(hk);
        RegCloseKey(hk);
    }
    RegFlushKey(HKEY_LOCAL_MACHINE);
}

static void RebootNow(void) {
    HANDLE hTok = NULL;
    BOOL tokOk = OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hTok);
    BOOL adjOk = FALSE;
    if (tokOk && hTok) {
        TOKEN_PRIVILEGES tp;
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid.LowPart = 19U; /* SE_SHUTDOWN_PRIVILEGE = 19 */
        tp.Privileges[0].Luid.HighPart = 0;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        adjOk = AdjustTokenPrivileges(hTok, FALSE, &tp, 0, NULL, NULL);
        CloseHandle(hTok);
    }
    LogRaw("[reboot] tok="); LogU32((DWORD)tokOk); LogRaw(" adj="); LogU32((DWORD)adjOk);
    LogRaw(" err="); LogU32(GetLastError()); LogRaw("\r\n");

    HANDLE hVol = CreateFileW(L"\\\\.\\C:", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);
    if (hVol != INVALID_HANDLE_VALUE) {
        BOOL fl = FlushFileBuffers(hVol);
        CloseHandle(hVol);
        LogRaw("[reboot] FlushVolume C: ok="); LogU32((DWORD)fl); LogRaw("\r\n");
    }

    LogRaw("[reboot] Calling NtShutdownSystem(1 /* ShutdownReboot */)...\r\n");
    if (g_log != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_log);
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
    }
    Sleep(1000);
    LONG st = NtShutdownSystem(1U /* ShutdownReboot */);
    /* Only reached if NtShutdownSystem returned an error */
    LogOpen();
    LogRaw("[reboot] NtShutdownSystem returned "); LogHex((DWORD)st); LogRaw("\r\n");
    Sleep(5000);
}

#define RX_TOP  L"/grant \"*S-1-5-32-545:RX\" /grant \"*S-1-5-11:RX\" /grant \"*S-1-5-19:RX\" /grant \"*S-1-5-20:RX\" /grant \"*S-1-1-0:RX\" /grant \"*S-1-15-2-1:RX\" /grant \"*S-1-15-2-2:RX\""
#define RX_ALL  L"/grant \"*S-1-5-32-545:(OI)(CI)RX\" /grant \"*S-1-5-11:(OI)(CI)RX\" /grant \"*S-1-5-19:(OI)(CI)RX\" /grant \"*S-1-5-20:(OI)(CI)RX\" /grant \"*S-1-1-0:(OI)(CI)RX\" /grant \"*S-1-15-2-1:(OI)(CI)RX\" /grant \"*S-1-15-2-2:(OI)(CI)RX\""
#define SYS_ADM L"/grant \"*S-1-5-18:(OI)(CI)F\" /grant \"*S-1-5-32-544:(OI)(CI)F\""
#define SYS_TOP L"/grant \"*S-1-5-18:F\" /grant \"*S-1-5-32-544:F\""

static DWORD RepairWorker(LPVOID unused) {
    (void)unused;
    LogOpen();
    LogRaw("=== s23aclsvc.exe v1.1 started ===\r\n");

    /* Phase 1: Instant top-level root & Windows directory traversal (no OI/CI -> zero ntmarta full-disk walk) */
    RunIcacls("C:\\ root",     L"C:\\ " SYS_TOP L" " RX_TOP L" /C /Q", 30000U);
    RunIcacls("C:\\s23_diag",  L"C:\\s23_diag.ps1 /grant \"*S-1-5-32-545:RX\" /grant \"*S-1-1-0:RX\" /C /Q", 30000U);
    RunIcacls("C:\\s23_sys",   L"C:\\s23_sys_install.ps1 /grant \"*S-1-5-32-545:RX\" /grant \"*S-1-1-0:RX\" /C /Q", 30000U);
    RunIcacls("C:\\s23_agent", L"C:\\s23_usb_agent.ps1 /grant \"*S-1-5-32-545:RX\" /grant \"*S-1-1-0:RX\" /C /Q", 30000U);
    RunIcacls("C:\\s23_touch", L"C:\\s23_touch_pkg /grant \"*S-1-5-32-545:(OI)(CI)RX\" /grant \"*S-1-1-0:(OI)(CI)RX\" /T /C /Q", 30000U);
    RunIcacls("C:\\s23_batt",  L"C:\\s23_batt_pkg /grant \"*S-1-5-32-545:(OI)(CI)RX\" /grant \"*S-1-1-0:(OI)(CI)RX\" /T /C /Q", 30000U);
    RunIcacls("C:\\s23_wifi",  L"C:\\s23_wifi_pkg /grant \"*S-1-5-32-545:(OI)(CI)RX\" /grant \"*S-1-1-0:(OI)(CI)RX\" /T /C /Q", 30000U);
    RunIcacls("C:\\Windows top", L"C:\\Windows " RX_TOP L" /C /Q", 30000U);

    /* Phase 2: ServiceProfiles (LocalService + NetworkService) — fixes Security ID 0 */
    RunIcacls("ServiceProfiles",  L"C:\\Windows\\ServiceProfiles " SYS_TOP L" /grant \"*S-1-5-19:RX\" /grant \"*S-1-5-20:RX\" /grant \"*S-1-5-32-545:RX\" /grant \"*S-1-1-0:RX\" /C /Q", 30000U);
    RunIcacls("LocalService /T",  L"C:\\Windows\\ServiceProfiles\\LocalService " SYS_ADM L" /grant \"*S-1-5-19:(OI)(CI)F\" /T /C /Q", 60000U);
    RunIcacls("NetworkService /T",L"C:\\Windows\\ServiceProfiles\\NetworkService " SYS_ADM L" /grant \"*S-1-5-20:(OI)(CI)F\" /T /C /Q", 60000U);

    /* Phase 3: Critical Windows runtime writable folders */
    RunIcacls("Windows\\Temp /T",  L"C:\\Windows\\Temp " SYS_ADM L" /grant \"*S-1-5-32-545:(OI)(CI)M\" /grant \"*S-1-5-19:(OI)(CI)F\" /grant \"*S-1-5-20:(OI)(CI)F\" /grant \"*S-1-1-0:(OI)(CI)M\" /T /C /Q", 60000U);
    RunIcacls("winevt /T",         L"C:\\Windows\\System32\\winevt " SYS_ADM L" /grant \"*S-1-5-19:(OI)(CI)F\" /grant \"*S-1-5-20:(OI)(CI)RX\" /grant \"*S-1-5-32-545:(OI)(CI)RX\" /grant \"*S-1-1-0:(OI)(CI)RX\" /T /C /Q", 60000U);
    RunIcacls("LogFiles /T",       L"C:\\Windows\\System32\\LogFiles " SYS_ADM L" /grant \"*S-1-5-19:(OI)(CI)F\" /grant \"*S-1-5-20:(OI)(CI)F\" /grant \"*S-1-5-32-545:(OI)(CI)RX\" /T /C /Q", 60000U);
    RunIcacls("Tasks /T",          L"C:\\Windows\\System32\\Tasks " SYS_ADM L" /grant \"*S-1-5-19:(OI)(CI)RX\" /grant \"*S-1-5-20:(OI)(CI)RX\" /grant \"*S-1-5-32-545:(OI)(CI)RX\" /T /C /Q", 60000U);
    RunIcacls("catroot2 /T",       L"C:\\Windows\\System32\\catroot2 " SYS_ADM L" /grant \"*S-1-5-19:(OI)(CI)F\" /grant \"*S-1-5-20:(OI)(CI)F\" /grant \"*S-1-5-32-545:(OI)(CI)RX\" /T /C /Q", 60000U);
    RunIcacls("spool /T",          L"C:\\Windows\\System32\\spool " SYS_ADM L" /grant \"*S-1-5-19:(OI)(CI)F\" /grant \"*S-1-5-20:(OI)(CI)F\" /grant \"*S-1-5-32-545:(OI)(CI)RX\" /T /C /Q", 60000U);
    RunIcacls("sru /T",            L"C:\\Windows\\System32\\sru " SYS_ADM L" /grant \"*S-1-5-19:(OI)(CI)F\" /grant \"*S-1-5-20:(OI)(CI)F\" /T /C /Q", 60000U);

    /* Phase 4: C:\Users (Public, Default, s23) & C:\ProgramData */
    RunIcacls("C:\\Users top",     L"C:\\Users " SYS_TOP L" /grant \"*S-1-5-32-545:RX\" /grant \"*S-1-5-11:RX\" /grant \"*S-1-1-0:RX\" /grant \"*S-1-15-2-1:RX\" /grant \"*S-1-15-2-2:RX\" /C /Q", 30000U);
    RunIcacls("Users\\Public /T",  L"C:\\Users\\Public " SYS_ADM L" /grant \"*S-1-5-32-545:(OI)(CI)F\" /grant \"*S-1-1-0:(OI)(CI)F\" /grant \"*S-1-5-19:(OI)(CI)F\" /grant \"*S-1-5-20:(OI)(CI)F\" /T /C /Q", 60000U);
    RunIcacls("Users\\Default /T", L"C:\\Users\\Default " SYS_ADM L" /grant \"*S-1-5-32-545:(OI)(CI)RX\" /grant \"*S-1-1-0:(OI)(CI)RX\" /T /C /Q", 60000U);
    RunIcacls("Users\\s23 /T",     L"C:\\Users\\s23 " SYS_ADM L" /grant \"*S-1-5-21-90484302-2165722077-966411361-1000:(OI)(CI)F\" /grant \"*S-1-15-2-1:(OI)(CI)RX\" /grant \"*S-1-15-2-2:(OI)(CI)RX\" /T /C /Q", 120000U);
    RunIcacls("s23 Packages /T",   L"C:\\Users\\s23\\AppData\\Local\\Packages /grant \"*S-1-15-2-1:(OI)(CI)F\" /grant \"*S-1-15-2-2:(OI)(CI)F\" /T /C /Q", 60000U);
    RunIcacls("ProgramData /T",    L"C:\\ProgramData " SYS_ADM L" /grant \"*S-1-5-32-545:(OI)(CI)M\" /grant \"*S-1-5-11:(OI)(CI)M\" /grant \"*S-1-5-19:(OI)(CI)F\" /grant \"*S-1-5-20:(OI)(CI)F\" /grant \"*S-1-1-0:(OI)(CI)RX\" /grant \"*S-1-15-2-1:(OI)(CI)RX\" /grant \"*S-1-15-2-2:(OI)(CI)RX\" /T /C /Q", 120000U);

    /* Phase 5: Windows executable/UI/SxS subtrees */
    RunIcacls("System32 /T",       L"C:\\Windows\\System32 " RX_ALL L" /T /C /Q", 180000U);
    RunIcacls("SysArm32 /T",       L"C:\\Windows\\SysArm32 " RX_ALL L" /T /C /Q", 60000U);
    RunIcacls("SysWOW64 /T",       L"C:\\Windows\\SysWOW64 " RX_ALL L" /T /C /Q", 120000U);
    RunIcacls("SystemApps /T",     L"C:\\Windows\\SystemApps " RX_ALL L" /T /C /Q", 120000U);
    RunIcacls("ImmersiveCP /T",    L"C:\\Windows\\ImmersiveControlPanel " RX_ALL L" /T /C /Q", 60000U);
    RunIcacls("ShellExp /T",       L"C:\\Windows\\ShellExperiences " RX_ALL L" /T /C /Q", 60000U);
    RunIcacls("ShellComp /T",      L"C:\\Windows\\ShellComponents " RX_ALL L" /T /C /Q", 60000U);
    RunIcacls("SystemRes /T",      L"C:\\Windows\\SystemResources " RX_ALL L" /T /C /Q", 60000U);
    RunIcacls("Resources /T",      L"C:\\Windows\\Resources " RX_ALL L" /T /C /Q", 60000U);
    RunIcacls("Fonts /T",          L"C:\\Windows\\Fonts " RX_ALL L" /T /C /Q", 60000U);
    RunIcacls("Globalization /T",  L"C:\\Windows\\Globalization " RX_ALL L" /T /C /Q", 60000U);
    RunIcacls("INF /T",            L"C:\\Windows\\INF " RX_ALL L" /T /C /Q", 60000U);
    RunIcacls("Registration /T",   L"C:\\Windows\\Registration " RX_ALL L" /T /C /Q", 60000U);
    RunIcacls("PolicyDefs /T",     L"C:\\Windows\\PolicyDefinitions " RX_ALL L" /T /C /Q", 60000U);
    RunIcacls("Security /T",       L"C:\\Windows\\Security " RX_ALL L" /T /C /Q", 60000U);
    RunIcacls("Logs /T",           L"C:\\Windows\\Logs " RX_ALL L" /T /C /Q", 60000U);
    RunIcacls("Web /T",            L"C:\\Windows\\Web " RX_ALL L" /T /C /Q", 60000U);
    RunIcacls("Microsoft.NET /T",  L"C:\\Windows\\Microsoft.NET " RX_ALL L" /T /C /Q", 120000U);
    RunIcacls("assembly /T",       L"C:\\Windows\\assembly " RX_ALL L" /T /C /Q", 60000U);
    RunIcacls("ProgramFiles /T",   L"\"C:\\Program Files\" " RX_ALL L" /T /C /Q", 120000U);
    RunIcacls("ProgramFiles86 /T", L"\"C:\\Program Files (x86)\" " RX_ALL L" /T /C /Q", 120000U);
    RunIcacls("WinSxS /T",         L"C:\\Windows\\WinSxS " RX_ALL L" /T /C /Q", 300000U);

    DisableSelfAndCleanSetup();
    LogRaw("=== s23aclsvc.exe ALL PHASES COMPLETE ===\r\n");
    RebootNow();
    return 0;
}

static void SvcCtrlHandler(DWORD ctrl) {
    if (!g_ssh) return;
    if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
        SERVICE_STATUS st;
        st.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        st.dwCurrentState = SERVICE_STOPPED;
        st.dwControlsAccepted = 0;
        st.dwWin32ExitCode = 0;
        st.dwServiceSpecificExitCode = 0;
        st.dwCheckPoint = 0;
        st.dwWaitHint = 0;
        SetServiceStatus(g_ssh, &st);
    }
}

static void SvcMain(DWORD argc, WCHAR **argv) {
    (void)argc; (void)argv;
    g_ssh = RegisterServiceCtrlHandlerW(L"S23AclFix", SvcCtrlHandler);
    if (g_ssh) {
        SERVICE_STATUS st;
        st.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        st.dwCurrentState = SERVICE_RUNNING;
        st.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
        st.dwWin32ExitCode = 0;
        st.dwServiceSpecificExitCode = 0;
        st.dwCheckPoint = 0;
        st.dwWaitHint = 0;
        SetServiceStatus(g_ssh, &st);
    }
    /* Keep service alive while RepairWorker runs and reboots */
    for (;;) {
        Sleep(60000U);
    }
}

/* Force non-empty .reloc for ARM64 /DYNAMICBASE */
static const void * volatile gRelocAnchors[2] = {
    (const void *)SvcMain,
    (const void *)RepairWorker,
};

void S23Entry(void) {
    if (!gRelocAnchors[0] || !gRelocAnchors[1]) ExitProcess(1);
    /* Start repair worker IMMEDIATELY so even if SCM pipe fails we still run & log */
    HANDLE hW = CreateThread(NULL, 0, RepairWorker, NULL, 0, NULL);

    static WCHAR svcName[] = L"S23AclFix";
    SERVICE_TABLE_ENTRYW tbl[2];
    tbl[0].lpServiceName = svcName;
    tbl[0].lpServiceProc = SvcMain;
    tbl[1].lpServiceName = NULL;
    tbl[1].lpServiceProc = NULL;
    StartServiceCtrlDispatcherW(tbl);

    if (hW) {
        WaitForSingleObject(hW, INFINITE);
        CloseHandle(hW);
    }
    ExitProcess(0);
}
