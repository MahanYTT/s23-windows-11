/*
 * s23disp_only.c - Switch active display output solely to Qualcomm Adreno 740 (SAM0911)
 *                  and disconnect Microsoft Basic Display Driver via CCD SetDisplayConfig.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wtsapi32.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static int has_substr_i(const WCHAR *hay, const WCHAR *needle)
{
    size_t hlen, nlen, i;

    if (!hay || !needle) return 0;
    hlen = wcslen(hay);
    nlen = wcslen(needle);
    if (nlen == 0) return 1;
    if (hlen < nlen) return 0;
    for (i = 0; i <= hlen - nlen; i++) {
        if (_wcsnicmp(hay + i, needle, nlen) == 0) {
            return 1;
        }
    }
    return 0;
}

static LONG query_paths(UINT32 flags, DISPLAYCONFIG_PATH_INFO **outPaths, UINT32 *outNPath,
                        DISPLAYCONFIG_MODE_INFO **outModes, UINT32 *outNMode)
{
    LONG rc;
    UINT32 nPath = 0, nMode = 0;
    DISPLAYCONFIG_PATH_INFO *paths = NULL;
    DISPLAYCONFIG_MODE_INFO *modes = NULL;

    for (;;) {
        rc = GetDisplayConfigBufferSizes(flags, &nPath, &nMode);
        if (rc != ERROR_SUCCESS) {
            return rc;
        }
        free(paths);
        free(modes);
        paths = (DISPLAYCONFIG_PATH_INFO *)calloc(nPath ? nPath : 1, sizeof(*paths));
        modes = (DISPLAYCONFIG_MODE_INFO *)calloc(nMode ? nMode : 1, sizeof(*modes));
        if (!paths || !modes) {
            free(paths);
            free(modes);
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        rc = QueryDisplayConfig(flags, &nPath, paths, &nMode, modes, NULL);
        if (rc == ERROR_SUCCESS) {
            *outPaths = paths;
            *outNPath = nPath;
            *outModes = modes;
            *outNMode = nMode;
            return ERROR_SUCCESS;
        }
        if (rc != ERROR_INSUFFICIENT_BUFFER) {
            free(paths);
            free(modes);
            return rc;
        }
    }
}

static void get_names(LUID adapterId, UINT32 targetId,
                      DISPLAYCONFIG_TARGET_DEVICE_NAME *tname,
                      DISPLAYCONFIG_ADAPTER_NAME *aname)
{
    ZeroMemory(tname, sizeof(*tname));
    tname->header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    tname->header.size = sizeof(*tname);
    tname->header.adapterId = adapterId;
    tname->header.id = targetId;
    if (DisplayConfigGetDeviceInfo(&tname->header) != ERROR_SUCCESS) {
        ZeroMemory(tname, sizeof(*tname));
    }

    ZeroMemory(aname, sizeof(*aname));
    aname->header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME;
    aname->header.size = sizeof(*aname);
    aname->header.adapterId = adapterId;
    aname->header.id = 0;
    if (DisplayConfigGetDeviceInfo(&aname->header) != ERROR_SUCCESS) {
        ZeroMemory(aname, sizeof(*aname));
    }
}

static int run_worker(void)
{
    FILE *logf = _wfopen(L"C:\\Users\\Public\\s23disp_only.log", L"w");
    DISPLAYCONFIG_PATH_INFO *paths = NULL;
    DISPLAYCONFIG_MODE_INFO *modes = NULL;
    UINT32 nPath = 0, nMode = 0, i;
    int samIdx = -1;
    DWORD sessId = 0;
    LONG rc;

    if (!logf) {
        logf = stdout;
    } else {
        setvbuf(logf, NULL, _IONBF, 0);
    }

    ProcessIdToSessionId(GetCurrentProcessId(), &sessId);
    fprintf(logf, "s23disp_only worker pid=%lu session=%lu\n",
            (unsigned long)GetCurrentProcessId(), (unsigned long)sessId);

    rc = query_paths(QDC_ALL_PATHS, &paths, &nPath, &modes, &nMode);
    if (rc != ERROR_SUCCESS) {
        fprintf(logf, "QueryDisplayConfig(QDC_ALL_PATHS) failed: %ld\n", rc);
        if (logf != stdout) fclose(logf);
        return 1;
    }

    fprintf(logf, "QDC_ALL_PATHS: paths=%u modes=%u\n", nPath, nMode);
    for (i = 0; i < nPath; i++) {
        DISPLAYCONFIG_TARGET_DEVICE_NAME tname;
        DISPLAYCONFIG_ADAPTER_NAME aname;
        int active = (paths[i].flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
        int avail = paths[i].targetInfo.targetAvailable ? 1 : 0;

        get_names(paths[i].targetInfo.adapterId, paths[i].targetInfo.id, &tname, &aname);
        fprintf(logf, "  PATH[%u]: active=%d avail=%d adp=%08lX:%08lX src=%u tgt=%u name=\"%ls\" mon=\"%ls\" drv=\"%ls\"\n",
                i, active, avail,
                (unsigned long)paths[i].targetInfo.adapterId.HighPart,
                (unsigned long)paths[i].targetInfo.adapterId.LowPart,
                paths[i].sourceInfo.id, paths[i].targetInfo.id,
                tname.monitorFriendlyDeviceName, tname.monitorDevicePath, aname.adapterDevicePath);

        if (avail &&
            (has_substr_i(tname.monitorDevicePath, L"SAM0911") ||
             has_substr_i(tname.monitorFriendlyDeviceName, L"S23 AMOLED"))) {
            if (samIdx < 0 || (paths[i].sourceInfo.id == 0 && active)) {
                samIdx = (int)i;
            }
        }
    }

    if (samIdx < 0) {
        fprintf(logf, "ERROR: Target SAM0911 / S23 AMOLED not found in available paths!\n");
        free(paths);
        free(modes);
        if (logf != stdout) fclose(logf);
        return 2;
    }

    {
        DISPLAYCONFIG_PATH_INFO p = paths[samIdx];
        p.flags = DISPLAYCONFIG_PATH_ACTIVE;
        p.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        p.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        p.targetInfo.rotation = DISPLAYCONFIG_ROTATION_IDENTITY;
        p.targetInfo.scaling = DISPLAYCONFIG_SCALING_IDENTITY;
        p.targetInfo.refreshRate.Numerator = 120;
        p.targetInfo.refreshRate.Denominator = 1;
        p.targetInfo.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;

        rc = SetDisplayConfig(1, &p, 0, NULL,
                              SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
        fprintf(logf, "SetDisplayConfig(120Hz, path=%d) -> %ld (0x%08lX)\n", samIdx, rc, (unsigned long)rc);
        if (rc != ERROR_SUCCESS) {
            p.targetInfo.refreshRate.Numerator = 0;
            p.targetInfo.refreshRate.Denominator = 0;
            rc = SetDisplayConfig(1, &p, 0, NULL,
                                  SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
            fprintf(logf, "SetDisplayConfig(auto, path=%d) -> %ld (0x%08lX)\n", samIdx, rc, (unsigned long)rc);
        }
    }

    free(paths);
    free(modes);
    paths = NULL;
    modes = NULL;

    Sleep(1000);

    {
        LONG qrc = query_paths(QDC_ONLY_ACTIVE_PATHS, &paths, &nPath, &modes, &nMode);
        if (qrc != ERROR_SUCCESS) {
            fprintf(logf, "QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS) failed: %ld\n", qrc);
        } else {
            fprintf(logf, "QDC_ONLY_ACTIVE_PATHS: paths=%u modes=%u\n", nPath, nMode);
            for (i = 0; i < nPath; i++) {
                DISPLAYCONFIG_TARGET_DEVICE_NAME tname;
                DISPLAYCONFIG_ADAPTER_NAME aname;
                UINT w = 0, h = 0, num = 0, den = 0;
                LONG px = 0, py = 0;
                UINT32 sm = paths[i].sourceInfo.modeInfoIdx;
                UINT32 tm = paths[i].targetInfo.modeInfoIdx;

                get_names(paths[i].targetInfo.adapterId, paths[i].targetInfo.id, &tname, &aname);
                if (sm < nMode && modes[sm].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
                    w = modes[sm].sourceMode.width;
                    h = modes[sm].sourceMode.height;
                    px = modes[sm].sourceMode.position.x;
                    py = modes[sm].sourceMode.position.y;
                }
                if (tm < nMode && modes[tm].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) {
                    num = modes[tm].targetMode.targetVideoSignalInfo.vSyncFreq.Numerator;
                    den = modes[tm].targetMode.targetVideoSignalInfo.vSyncFreq.Denominator;
                }
                fprintf(logf, "  ACTIVE[%u]: name=\"%ls\" mon=\"%ls\" %ux%u @ (%ld,%ld) %u/%u Hz drv=\"%ls\"\n",
                        i, tname.monitorFriendlyDeviceName, tname.monitorDevicePath,
                        w, h, px, py, num, den, aname.adapterDevicePath);
            }
        }
        free(paths);
        free(modes);
    }

    if (logf != stdout) fclose(logf);
    return (rc == ERROR_SUCCESS) ? 0 : 1;
}

int wmain(int argc, wchar_t **argv)
{
    int isWorker = 0;
    DWORD sessId = 0;

    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--worker") == 0) {
            isWorker = 1;
        }
    }

    ProcessIdToSessionId(GetCurrentProcessId(), &sessId);
    if (sessId == 0 && !isWorker) {
        DWORD consoleSess = WTSGetActiveConsoleSessionId();
        HANDLE hTok = NULL, hDup = NULL;
        WCHAR exePath[MAX_PATH] = {0};
        WCHAR cmdLine[MAX_PATH + 32] = {0};
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        DWORD exitCode = 1;

        if (consoleSess == 0xFFFFFFFF || !WTSQueryUserToken(consoleSess, &hTok)) {
            fwprintf(stderr, L"[ERR] WTSQueryUserToken(consoleSess=%lu) failed err=%lu\n",
                     (unsigned long)consoleSess, (unsigned long)GetLastError());
            return 10;
        }
        if (!DuplicateTokenEx(hTok, MAXIMUM_ALLOWED, NULL, SecurityIdentification, TokenPrimary, &hDup)) {
            fwprintf(stderr, L"[ERR] DuplicateTokenEx failed err=%lu\n", (unsigned long)GetLastError());
            CloseHandle(hTok);
            return 11;
        }
        CloseHandle(hTok);

        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        swprintf(cmdLine, MAX_PATH + 32, L"\"%ls\" --worker", exePath);

        ZeroMemory(&si, sizeof(si));
        ZeroMemory(&pi, sizeof(pi));
        si.cb = sizeof(si);
        si.lpDesktop = L"winsta0\\default";

        DeleteFileW(L"C:\\Users\\Public\\s23disp_only.log");
        if (!CreateProcessAsUserW(hDup, exePath, cmdLine, NULL, NULL, FALSE,
                                  CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            fwprintf(stderr, L"[ERR] CreateProcessAsUserW failed err=%lu\n", (unsigned long)GetLastError());
            CloseHandle(hDup);
            return 12;
        }
        CloseHandle(hDup);

        if (WaitForSingleObject(pi.hProcess, 15000) == WAIT_TIMEOUT) {
            TerminateProcess(pi.hProcess, 13);
            exitCode = 13;
        } else {
            GetExitCodeProcess(pi.hProcess, &exitCode);
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        FILE *rf = _wfopen(L"C:\\Users\\Public\\s23disp_only.log", L"r");
        if (rf) {
            char buf[1024];
            while (fgets(buf, sizeof(buf), rf)) {
                fputs(buf, stdout);
            }
            fclose(rf);
        } else {
            fprintf(stderr, "[WARN] C:\\Users\\Public\\s23disp_only.log missing (worker rc=%lu)\n",
                    (unsigned long)exitCode);
        }
        return (int)exitCode;
    }

    return run_worker();
}
