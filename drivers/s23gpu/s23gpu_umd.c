/* Minimal ARM64 stub UMD for Phase 2 KMD verification (replaced by Mesa UMD in Phase 3) */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

__declspec(dllexport) HRESULT WINAPI OpenAdapter10(void *pOpenData)
{
    (void)pOpenData;
    return E_FAIL;
}

__declspec(dllexport) HRESULT WINAPI OpenAdapter10_2(void *pOpenData)
{
    (void)pOpenData;
    return E_FAIL;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)hinst;
    (void)reason;
    (void)reserved;
    return TRUE;
}
