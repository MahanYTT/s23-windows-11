; ARM64 SMCCC, Gunyah-safe Single-Instruction MMIO, and D-Cache Clean primitives (armasm64.exe)
; VOID      S23SmcCall(ULONGLONG Regs[8])
; ULONG     S23MmioRd32(const volatile VOID *Addr)
; VOID      S23MmioWr32(volatile VOID *Addr, ULONG Val)
; VOID      S23MmioWr64(volatile VOID *Addr, ULONGLONG Val)
; VOID      S23CleanCacheRange(const VOID *Addr, SIZE_T Len)

    AREA    |.text|, CODE, READONLY, ALIGN=4
    EXPORT  S23SmcCall
    EXPORT  S23MmioRd32
    EXPORT  S23MmioWr32
    EXPORT  S23MmioWr64
    EXPORT  S23CleanCacheRange

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

; Single 32-bit LDR (ISV=1 for Gunyah EL2 Stage-2 vSMMU traps)
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

; Clean data cache to Point of Coherency (PoC) for non-coherent ADSP DMA reads
S23CleanCacheRange PROC
    cbz     x1, clean_done
    add     x1, x0, x1
    bic     x0, x0, #63
clean_loop
    dc      cvac, x0
    add     x0, x0, #64
    cmp     x0, x1
    blo     clean_loop
clean_done
    dsb     sy
    ret
    ENDP

    END
