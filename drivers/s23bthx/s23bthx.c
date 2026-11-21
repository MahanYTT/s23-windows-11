#include <ntddk.h>
#include <wdf.h>
#include <bthxddi.h>
#include "s23bthx.h"

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD S23BthxDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP S23BthxEvtDriverContextCleanup;

EVT_WDF_DEVICE_PREPARE_HARDWARE S23BthxEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE S23BthxEvtDeviceReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY S23BthxEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT S23BthxEvtDeviceD0Exit;

EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL S23BthxEvtIoInternalDeviceControl;

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS status;
    WDF_DRIVER_CONFIG config;

    WDF_DRIVER_CONFIG_INIT(&config, S23BthxDeviceAdd);
    config.EvtDriverUnload = NULL;

    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);

    return status;
}

NTSTATUS
S23BthxDeviceAdd(
    _In_ WDFDRIVER       Driver,
    _In_ PWDFDEVICE_INIT DeviceInit
    )
{
    NTSTATUS status;
    WDFDEVICE device;
    PDEVICE_CONTEXT devCtx;
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFQUEUE queue;

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDevicePrepareHardware = S23BthxEvtDevicePrepareHardware;
    pnpPowerCallbacks.EvtDeviceReleaseHardware = S23BthxEvtDeviceReleaseHardware;
    pnpPowerCallbacks.EvtDeviceD0Entry = S23BthxEvtDeviceD0Entry;
    pnpPowerCallbacks.EvtDeviceD0Exit = S23BthxEvtDeviceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);

    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    devCtx = DeviceGetContext(device);
    devCtx->Device = device;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.EvtIoInternalDeviceControl = S23BthxEvtIoInternalDeviceControl;

    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);

    return status;
}

NTSTATUS
S23BthxEvtDevicePrepareHardware(
    _In_ WDFDEVICE    Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    return STATUS_SUCCESS;
}

NTSTATUS
S23BthxEvtDeviceReleaseHardware(
    _In_ WDFDEVICE    Device,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    return STATUS_SUCCESS;
}

NTSTATUS
S23BthxEvtDeviceD0Entry(
    _In_ WDFDEVICE              Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
    )
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(PreviousState);
    return STATUS_SUCCESS;
}

NTSTATUS
S23BthxEvtDeviceD0Exit(
    _In_ WDFDEVICE              Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState
    )
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(TargetState);
    return STATUS_SUCCESS;
}

VOID
S23BthxEvtIoInternalDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode
    )
{
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    PDEVICE_CONTEXT devCtx = DeviceGetContext(WdfIoQueueGetDevice(Queue));
    size_t length;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(devCtx);

    switch (IoControlCode) {
    case IOCTL_BTHX_GET_VERSION:
        if (OutputBufferLength >= sizeof(BTHX_VERSION)) {
            PBTHX_VERSION ver;
            status = WdfRequestRetrieveOutputBuffer(Request, sizeof(BTHX_VERSION), (PVOID*)&ver, &length);
            if (NT_SUCCESS(status)) {
                ver->Version = BTHX_DDI_VERSION_1;
                WdfRequestSetInformation(Request, sizeof(BTHX_VERSION));
            }
        } else {
            status = STATUS_BUFFER_TOO_SMALL;
        }
        break;

    case IOCTL_BTHX_SET_VERSION:
        status = STATUS_SUCCESS;
        break;

    case IOCTL_BTHX_QUERY_CAPABILITIES:
        if (OutputBufferLength >= sizeof(BTHX_CAPABILITIES)) {
            PBTHX_CAPABILITIES caps;
            status = WdfRequestRetrieveOutputBuffer(Request, sizeof(BTHX_CAPABILITIES), (PVOID*)&caps, &length);
            if (NT_SUCCESS(status)) {
                caps->MaxAclTransferInSize = 1021;
                caps->ScoSupport = ScoSupportNone;
                caps->MaxScoChannels = 0;
                caps->IsDeviceIdleCapable = TRUE;
                caps->IsDeviceWakeCapable = FALSE;
                WdfRequestSetInformation(Request, sizeof(BTHX_CAPABILITIES));
            }
        } else {
            status = STATUS_BUFFER_TOO_SMALL;
        }
        break;

    case IOCTL_BTHX_WRITE_HCI:
        status = STATUS_SUCCESS;
        break;

    case IOCTL_BTHX_READ_HCI:
        status = STATUS_SUCCESS;
        break;
    }

    WdfRequestComplete(Request, status);
}
