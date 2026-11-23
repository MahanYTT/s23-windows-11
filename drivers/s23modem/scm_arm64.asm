; ARM64 SMCCC, Gunyah-safe Single-Instruction MMIO, and D-Cache Clean primitives (armasm64.exe)
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

S23MmioRd32 PROC
    dmb     sy
    ldr     w0, [x0]
    dmb     sy
    ret
    ENDP

S23MmioWr32 PROC
    dmb     sy
    str     w1, [x0]
    dmb     sy
    ret
    ENDP

S23MmioWr64 PROC
    dmb     sy
    str     x1, [x0]
    dmb     sy
    ret
    ENDP

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
