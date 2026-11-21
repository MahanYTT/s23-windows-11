#include <ntddk.h>
#include <wdf.h>
#include <bthxddi.h>
#include "s23bthx.h"

// HCI In-Band Sleep (IBS) parser and filter

#define IBS_WAKE_ACK 0xFC
#define IBS_WAKE_IND 0xFD
#define IBS_SLEEP_IND 0xFE

NTSTATUS
S23BthxProcessHciRx(
    _In_ PDEVICE_CONTEXT DevCtx,
    _In_ PUCHAR Buffer,
    _In_ size_t Length,
    _Out_ size_t *BytesConsumed
    )
{
    size_t i;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(DevCtx);
    
    // Filter IBS bytes from the stream.
    // If we see 0xFD (WAKE_IND), we must immediately send 0xFC (WAKE_ACK)
    // so the Bluetooth controller doesn't block waiting.
    
    for (i = 0; i < Length; i++) {
        UCHAR b = Buffer[i];
        
        if (b == IBS_WAKE_IND) {
            // Need to transmit IBS_WAKE_ACK back to controller over UART
            // WdfIoTargetSendWriteSynchronously(DevCtx->UartTarget, ...)
        } else if (b == IBS_SLEEP_IND) {
            // Controller is going to sleep
        } else if (b == IBS_WAKE_ACK) {
            // Controller acknowledged our wake request
        } else {
            // Real HCI data byte
        }
    }
    
    *BytesConsumed = Length;
    return status;
}
