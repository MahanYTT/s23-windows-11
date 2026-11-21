#include <ntddk.h>
#include <wdf.h>
#include <bthxddi.h>
#include "s23bthx.h"

// GENI UART and ACPI UART target connection setup

NTSTATUS
S23BthxConnectUart(
    _In_ PDEVICE_CONTEXT DevCtx
    )
{
    NTSTATUS status;
    WDF_IO_TARGET_OPEN_PARAMS openParams;
    UNICODE_STRING uartName;
    
    // ACPI\QCOM0831 is our BT node. We need to open the UART connection via the resource hub.
    // The UART resource string should be generated based on the ConnectionResource.
    // For now we use the standardized RH path format.
    
    RtlInitUnicodeString(&uartName, L"\\??\\RESOURCE_HUB\\0000000000000000"); // TODO: resolve exact ID
    
    status = WdfIoTargetCreate(DevCtx->Device, WDF_NO_OBJECT_ATTRIBUTES, &DevCtx->UartTarget);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(
        &openParams,
        &uartName,
        FILE_GENERIC_READ | FILE_GENERIC_WRITE
    );
    
    openParams.ShareAccess = 0;
    openParams.CreateOptions = FILE_SYNCHRONOUS_IO_NONALERT;
    
    status = WdfIoTargetOpen(DevCtx->UartTarget, &openParams);
    
    return status;
}
