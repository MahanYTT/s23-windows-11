; ARM64 SMCCC trampoline for Qualcomm SCM calls (armasm64.exe)
; VOID S23SmcCall(ULONGLONG Regs[8])

    AREA    |.text|, CODE, READONLY, ALIGN=4
    EXPORT  S23SmcCall

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

    END
