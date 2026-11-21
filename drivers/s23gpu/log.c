/* Write-through kernel log to C:\Users\Public\s23gpu.log */
#include "s23gpu.h"
#include <stdarg.h>

#define S23GPU_LOG_MAX_BYTES (2u * 1024u * 1024u)

static HANDLE g_File = NULL;
static KMUTEX g_LogLock;
static ULONGLONG g_LogBytes = 0;

VOID LogOpen(VOID)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;

    KeInitializeMutex(&g_LogLock, 0);
    g_LogBytes = 0;
    RtlInitUnicodeString(&name, L"\\??\\C:\\Users\\Public\\s23gpu.log");
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&g_File,
                                 FILE_GENERIC_WRITE | SYNCHRONIZE,
                                 &oa, &iosb, NULL,
                                 FILE_ATTRIBUTE_NORMAL,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 FILE_OVERWRITE_IF,
                                 FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE | FILE_WRITE_THROUGH,
                                 NULL, 0))) {
        g_File = NULL;
    }
}

VOID LogClose(VOID)
{
    KeWaitForSingleObject(&g_LogLock, Executive, KernelMode, FALSE, NULL);
    if (g_File != NULL) {
        ZwClose(g_File);
        g_File = NULL;
    }
    KeReleaseMutex(&g_LogLock, FALSE);
}

VOID LogPrint(_In_z_ _Printf_format_string_ PCSTR Fmt, ...)
{
    va_list args;
    CHAR text[512];
    SIZE_T length = 0;
    IO_STATUS_BLOCK iosb;
    NTSTATUS st;

    va_start(args, Fmt);
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        va_end(args);
        return;
    }
    st = RtlStringCbVPrintfA(text, sizeof(text), Fmt, args);
    va_end(args);
    if (!NT_SUCCESS(st) && st != STATUS_BUFFER_OVERFLOW) {
        return;
    }
    if (!NT_SUCCESS(RtlStringCbLengthA(text, sizeof(text), &length)) || length == 0) {
        return;
    }
    KeWaitForSingleObject(&g_LogLock, Executive, KernelMode, FALSE, NULL);
    if (g_File != NULL && g_LogBytes + length <= S23GPU_LOG_MAX_BYTES) {
        LARGE_INTEGER off;
        off.QuadPart = (LONGLONG)g_LogBytes;
        if (NT_SUCCESS(ZwWriteFile(g_File, NULL, NULL, NULL, &iosb, text, (ULONG)length, &off, NULL))) {
            g_LogBytes += iosb.Information;
        }
    }
    KeReleaseMutex(&g_LogLock, FALSE);
}
