; ARM64 SMCCC & Gunyah-safe Single-Instruction MMIO primitives (armasm64.exe)
; VOID      S23SmcCall(ULONGLONG Regs[8])
; ULONG     S23MmioRd32(const volatile VOID *Addr)
; VOID      S23MmioWr32(volatile VOID *Addr, ULONG Val)
; VOID      S23MmioWr64(volatile VOID *Addr, ULONGLONG Val)

    AREA    |.text|, CODE, READONLY, ALIGN=4
    EXPORT  S23SmcCall
    EXPORT  S23MmioRd32
    EXPORT  S23MmioWr32
    EXPORT  S23MmioWr64

S23SmcCall PROC
    stp     x19, lr, [sp, #-16]!
    mov     x19, x0
    ldp     x0, x1, [x19, #0]
    ldp     x2, x3, [x19, #16]
    ldp     x4, x5, [x19, #32]
    ldp     x6, x7, [x19, #48]
    smc     #0
    stp     x0, x1, [x19, #0]
    stp     x2, x3, [x19, #16]
    stp     x4, x5, [x19, #32]
    stp     x6, x7, [x19, #48]
    ldp     x19, lr, [sp], #16
    ret
    ENDP

; Single 32-bit LDR (never LDP, ISV=1 for Gunyah EL2 Stage-2 vSMMU traps)
S23MmioRd32 PROC
    dmb     sy
    ldr     w0, [x0]
    dmb     sy
    ret
    ENDP

; Single 32-bit STR from w1 (never STP, never WZR/r31 for Gunyah EL2 vSMMU traps)
S23MmioWr32 PROC
    dmb     sy
    str     w1, [x0]
    dmb     sy
    ret
    ENDP

; Single 64-bit STR from x1 (never STP, never XZR/r31)
S23MmioWr64 PROC
    dmb     sy
    str     x1, [x0]
    dmb     sy
    ret
    ENDP

    END
