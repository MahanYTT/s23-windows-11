/* Host tests for the s23batt MAX77705 fuel-gauge decoding & fast-charge configuration
 * (drivers/s23batt/s23batt.c, extracted verbatim by tests/c/extract_batt.py; the driver itself
 * is not modified):
 *   S23BattConfigureFastCharge  CNFG_06 unlock (0xBD=0x6D), abnormalOff (0x08/0x0B) 0x04->0x05
 *                               recovery toggle on CNFG_00 (0xB7), normal mode preservation,
 *                               4400 mV float (0xBB=0x18), 2500 mA CC (0xB9=0x32), 3000 mA CHGIN
 *                               (0xC0=0xF7), CHGIN/WCIN select (0xC3=0x68), I2C write failure handling
 *   S23BattReadHardware         register -> SOC / mV / mA / tenths-Kelvin / mWh / Windows power state
 *   S23BattDumpStatus, S23LogHex4, Append* (log formatting, buffer bounds)
 * The bit-bang I2C layer is replaced by a fake register file (FG 0x36 words, CHG 0x69 bytes).
 *
 * Scale factors exercised here are the ones the driver implements; they match the public
 * MAX1720x/MAX17055 ModelGauge m5 register LSBs (as used by Linux
 * drivers/power/supply/max17042_battery.c) with one sense resistor value for both current and
 * capacity: VCell 78.125 uV, Current 1.5625 uV / Rsense, Capacity 5.0 uVh / Rsense, Temp 1/256 C.
 * Driver: current 0.78125 mA and capacity 2.5 mAh per LSB, i.e. Rsense = 2 mOhm (not verified
 * against the S23 schematic). The CHG_DTLS -> power state table is locked as implemented; it is a
 * regression guard, not a datasheet verification. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "host_test.h"

/* ---- user-mode shims for the kernel types/calls used by the extracted code ---- */
#define VOID void
#define TRUE 1
#define FALSE 0
typedef uint8_t BOOLEAN;
typedef uint32_t ULONG;
typedef int32_t LONG, NTSTATUS;
typedef uint64_t ULONGLONG;
typedef int64_t LONGLONG;
typedef wchar_t WCHAR;
typedef void *PVOID, *HANDLE;
typedef struct { uint16_t Length, MaximumLength; const WCHAR *Buffer; } UNICODE_STRING;
typedef struct { ULONG Length; HANDLE RootDirectory; UNICODE_STRING *ObjectName; ULONG Attributes; PVOID Sd, Sqos; } OBJECT_ATTRIBUTES;
typedef struct { NTSTATUS Status; uint64_t Information; } IO_STATUS_BLOCK;
typedef struct { LONGLONG QuadPart; } LARGE_INTEGER;
typedef struct { uint64_t q[3]; } KEVENT, *PKEVENT;
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
#define OBJ_CASE_INSENSITIVE 0x40u
#define OBJ_KERNEL_HANDLE 0x200u
#define FILE_WRITE_DATA 0x2u
#define SYNCHRONIZE 0x00100000u
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define FILE_SHARE_READ 1u
#define FILE_SHARE_WRITE 2u
#define FILE_OPEN_IF 3u
#define FILE_SYNCHRONOUS_IO_NONALERT 0x20u
#define FILE_WRITE_THROUGH 0x2u

#include "batt_types.inc"

typedef struct _S23BATT_EXT {
    volatile uint8_t *Tlmm;
    KEVENT I2cLock;
    ULONGLONG NextSampleSeq;
} S23BATT_EXT, *PS23BATT_EXT;

static int g_locked;
static void LockAcquire(PKEVENT ev) { (void)ev; CHECK(g_locked == 0); g_locked++; }
static void LockRelease(PKEVENT ev) { (void)ev; CHECK(g_locked == 1); g_locked--; }
static void MemZero(void *dst, size_t n) { memset(dst, 0, n); }

/* fake MAX77705: fuel gauge words (0x36) and charger bytes (0x69) */
static uint16_t g_fg[256];
static uint8_t g_chg[256];
static int g_failReg = -1;
static int g_failChgWrite = 0;
static int g_fastChargeCalls;
static uint32_t g_stallUsTotal;

typedef struct {
    uint8_t reg;
    uint8_t val;
} chg_write_op;

static chg_write_op g_chgWrites[64];
static size_t g_chgWriteCount;

static void KeStallExecutionProcessor(ULONG us)
{
    g_stallUsTotal += us;
}
static uint32_t Rd32(volatile uint8_t *base, uint32_t off) { (void)base; return 0xA5000000u | off; }
static void Wr32(volatile uint8_t *base, uint32_t off, uint32_t v) { (void)base; CHECK(v == (0xA5000000u | off)); }
static void I2cBusRecover(volatile uint8_t *t, uint32_t sda, uint32_t scl) { (void)t; (void)sda; (void)scl; }
static int I2cRegRead16LE(volatile uint8_t *t, uint32_t sda, uint32_t scl, uint8_t addr7, uint8_t reg, uint16_t *out)
{
    (void)t; (void)sda; (void)scl;
    CHECK(addr7 == MAX77705_FG_ADDR);
    if ((int)reg == g_failReg) return -1;
    *out = g_fg[reg];
    return 0;
}
static int I2cRegRead8(volatile uint8_t *t, uint32_t sda, uint32_t scl, uint8_t addr7, uint8_t reg, uint8_t *out)
{
    (void)t; (void)sda; (void)scl;
    CHECK(addr7 == MAX77705_CHG_ADDR);
    *out = g_chg[reg];
    return 0;
}
static int I2cRegWrite8(volatile uint8_t *t, uint32_t sda, uint32_t scl, uint8_t addr7, uint8_t reg, uint8_t val)
{
    (void)t; (void)sda; (void)scl;
    CHECK(addr7 == MAX77705_CHG_ADDR);
    if (g_chgWriteCount == 0u) {
        g_fastChargeCalls++;
    }
    if (g_chgWriteCount < sizeof(g_chgWrites) / sizeof(g_chgWrites[0])) {
        g_chgWrites[g_chgWriteCount].reg = reg;
        g_chgWrites[g_chgWriteCount].val = val;
    }
    g_chgWriteCount++;
    if (g_failChgWrite) {
        return -1;
    }
    g_chg[reg] = val;
    return 0;
}

/* captured log output (S23LogRaw) and status file (ZwWriteFile from S23BattDumpStatus) */
static char g_log[4096];
static size_t g_logLen;
static char g_status[600];
static size_t g_statusLen;
static void S23LogRaw(const char *msg, ULONG len)
{
    CHECK(len > 0U && len < 512U);
    if (g_logLen + len < sizeof(g_log)) { memcpy(g_log + g_logLen, msg, len); g_logLen += len; g_log[g_logLen] = 0; }
}
static void RtlInitUnicodeString(UNICODE_STRING *us, const WCHAR *s)
{
    us->Buffer = s;
    us->Length = (uint16_t)(wcslen(s) * 2U);
    us->MaximumLength = (uint16_t)(us->Length + 2U);
}
static NTSTATUS ZwCreateFile(HANDLE *h, ULONG acc, OBJECT_ATTRIBUTES *oa, IO_STATUS_BLOCK *io, PVOID alloc, ULONG attr,
                             ULONG share, ULONG disp, ULONG opts, PVOID ea, ULONG ealen)
{
    (void)acc; (void)io; (void)alloc; (void)attr; (void)share; (void)disp; (void)opts; (void)ea; (void)ealen;
    CHECK(oa->ObjectName && wcscmp(oa->ObjectName->Buffer, L"\\??\\C:\\Users\\Public\\s23batt_status.txt") == 0);
    *h = (HANDLE)g_status;
    return 0;
}
static NTSTATUS ZwWriteFile(HANDLE h, HANDLE ev, PVOID apc, PVOID ctx, IO_STATUS_BLOCK *io, PVOID buf, ULONG len,
                            LARGE_INTEGER *off, PVOID key)
{
    (void)h; (void)ev; (void)apc; (void)ctx; (void)io; (void)key;
    CHECK(off->QuadPart == 0);
    CHECK(len <= 512U);
    if (len < sizeof(g_status)) { memcpy(g_status, buf, len); g_statusLen = len; g_status[len] = 0; }
    return 0;
}
static NTSTATUS ZwClose(HANDLE h) { (void)h; return 0; }

#include "batt_funcs.inc"

/* ---- helpers ---- */
static S23BATT_EXT g_ext;
static uint8_t g_fakeTlmm[16];

static void regs_nominal(void)
{
    memset(g_fg, 0, sizeof(g_fg));
    memset(g_chg, 0, sizeof(g_chg));
    g_failReg = -1;
    g_failChgWrite = 0;
    g_fastChargeCalls = 0;
    g_chgWriteCount = 0;
    g_stallUsTotal = 0;
    g_fg[0x00] = 0x0000;          /* status: battery present (bit 3 clear) */
    g_fg[0x06] = 0x3200;          /* RepSOC 50.00 % */
    g_fg[0x08] = 0x1900;          /* Temp 25.0 C */
    g_fg[0x09] = 0xC800;          /* VCell 51200 * 78.125 uV = 4000 mV */
    g_fg[0x0A] = 0xFF00;          /* Current -256 LSB = -200 mA */
    g_fg[0x0B] = 0xFF80;          /* AvgCurrent -128 LSB = -100 mA */
    g_fg[0x10] = 1520;            /* FullCap 3800 mAh */
    g_fg[0x17] = 123;             /* Cycles */
    g_fg[0x18] = 1560;            /* DesignCap 3900 mAh */
    g_fg[0x35] = 1520;            /* FullCapRep 3800 mAh */
}

static int run(S23_BATT_STATE *s)
{
    memset(s, 0xCC, sizeof(*s));
    g_ext.Tlmm = g_fakeTlmm;
    int rc = S23BattReadHardware(&g_ext, s, FALSE);
    CHECK(g_locked == 0);
    return rc;
}

/* ---- tests ---- */
static void test_nominal_discharge(void)
{
    S23_BATT_STATE s;
    regs_nominal();
    g_statusLen = 0;
    ULONGLONG seq0 = g_ext.NextSampleSeq;
    CHECK_EQ_U64(run(&s), 0);
    CHECK(s.Valid);
    CHECK_EQ_U64(s.Tag, 1);
    CHECK_EQ_U64(s.SocX100, 5000);
    CHECK_EQ_U64(s.VoltageMv, 4000);
    CHECK_EQ_U64((uint32_t)s.CurrentMa, (uint32_t)-200);
    CHECK_EQ_U64((uint32_t)s.AvgCurrentMa, (uint32_t)-100);
    CHECK_EQ_U64(s.TempTenthsK, 2981);                 /* 25.0 C */
    CHECK_EQ_U64(s.DesignedCapMwh, 15132);             /* 3900 mAh * 3.88 V */
    CHECK_EQ_U64(s.FullChargedCapMwh, 14744);          /* 3800 mAh * 3.88 V */
    CHECK_EQ_U64(s.RemainingMwh, 7372);
    CHECK_EQ_U64(s.CycleCount, 123);
    CHECK_EQ_U64(s.PowerState, BATTERY_DISCHARGING);
    CHECK_EQ_U64((uint32_t)s.RateMw, (uint32_t)-800);
    CHECK_EQ_U64(s.Seq, seq0 + 1U);
    CHECK_EQ_U64(g_fastChargeCalls, 0);
    CHECK(strstr(g_status, "BATT: soc=50.0% v=4000mV i=-200mA rate=-800mW pwr=0x02 ") == g_status);
    CHECK(g_statusLen >= 2U && g_status[g_statusLen - 2] == '\r' && g_status[g_statusLen - 1] == '\n');
}

static void test_invalid_samples_leave_output_untouched(void)
{
    S23_BATT_STATE s;
    regs_nominal();
    g_fg[0x09] = 0xFFFF;                               /* I2C bus floating high */
    CHECK(run(&s) == -2);
    CHECK_EQ_U64(s.Valid, 0xCC);
    g_fg[0x09] = 0x0000;
    CHECK(run(&s) == -2);
    CHECK_EQ_U64(s.Valid, 0xCC);

    regs_nominal();
    g_failReg = 0x35;                                  /* any NAK fails the whole sample */
    CHECK(run(&s) != 0);
    CHECK_EQ_U64(s.Valid, 0xCC);

    S23BATT_EXT noMap;
    memset(&noMap, 0, sizeof(noMap));
    CHECK(S23BattReadHardware(&noMap, &s, FALSE) == -1);
}

static void test_soc(void)
{
    S23_BATT_STATE s;
    static const struct { uint16_t raw; uint32_t x100; } v[] = {
        { 0x0000, 0 }, { 0x0001, 0 }, { 0x0003, 1 }, { 0x0080, 50 }, { 0x00FF, 99 },
        { 0x0100, 100 }, { 0x3280, 5050 }, { 0x6400, 10000 }, { 0x64FF, 10000 }, { 0x6500, 10000 },
        { 0xFFFF, 10000 },
    };
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        regs_nominal();
        g_fg[0x06] = v[i].raw;
        CHECK_EQ_U64(run(&s), 0);
        CHECK_EQ_U64(s.SocX100, v[i].x100);
        CHECK(s.RemainingMwh <= s.FullChargedCapMwh);
    }
}

static void test_voltage_current_full_sweep(void)
{
    /* VCell: 78.125 uV/LSB == 5/64 mV; Current: 0.78125 mA/LSB == 25/32 mA, truncated toward 0 */
    S23_BATT_STATE s;
    regs_nominal();
    for (uint32_t raw = 1; raw < 0xFFFFu; raw += 7u) {
        g_fg[0x09] = (uint16_t)raw;
        g_fg[0x0A] = (uint16_t)raw;
        CHECK_EQ_U64(run(&s), 0);
        CHECK_EQ_U64(s.VoltageMv, (raw * 5u) / 64u);
        int32_t sraw = (raw >= 0x8000u) ? (int32_t)raw - 65536 : (int32_t)raw;
        CHECK_EQ_U64((uint32_t)s.CurrentMa, (uint32_t)((sraw * 25) / 32));
    }
    regs_nominal();
    g_fg[0x0A] = 0x7FFF;
    run(&s);
    CHECK_EQ_U64((uint32_t)s.CurrentMa, 25599u);
    g_fg[0x0A] = 0x8000;
    run(&s);
    CHECK_EQ_U64((uint32_t)s.CurrentMa, (uint32_t)-25600);
    g_fg[0x0A] = 0xFFFF;                               /* -0.78 mA truncates to 0 */
    run(&s);
    CHECK_EQ_U64((uint32_t)s.CurrentMa, 0);
}

static void test_temperature(void)
{
    S23_BATT_STATE s;
    static const struct { uint16_t st, raw; uint32_t tenthsK; } v[] = {
        { 0x0000, 0x1900, 2981 },                      /* 25.0 C */
        { 0x0000, 0x0000, 2731 },                      /* 0.0 C */
        { 0x0000, 0xFFFF, 2731 },                      /* -1/256 C truncates to 0 */
        { 0x0000, 0xE200, 2431 },                      /* -30.0 C: lower limit, kept */
        { 0x0000, 0xE1E6, 2981 },                      /* -30.1 C: out of range -> 25.0 C */
        { 0x0000, 0x5500, 3581 },                      /* 85.0 C: upper limit, kept */
        { 0x0000, 0x551A, 2981 },                      /* 85.1 C: out of range -> 25.0 C */
        { 0x0000, 0x8000, 2981 },                      /* -128 C */
        { 0x0000, 0x7FFF, 2981 },                      /* +127.99 C */
        { 0x0008, 0x0000, 2981 },                      /* status bit 3 (Bst): no thermistor -> 25 C */
        { 0x0008, 0x5000, 2981 },
    };
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        regs_nominal();
        g_fg[0x00] = v[i].st;
        g_fg[0x08] = v[i].raw;
        CHECK_EQ_U64(run(&s), 0);
        CHECK_EQ_U64(s.TempTenthsK, v[i].tenthsK);
    }
    /* never outside -30.0 .. +85.0 C for any raw value */
    regs_nominal();
    for (uint32_t raw = 0; raw <= 0xFFFFu; raw += 3u) {
        g_fg[0x08] = (uint16_t)raw;
        run(&s);
        CHECK(s.TempTenthsK >= 2431u && s.TempTenthsK <= 3581u);
    }
}

static void test_capacity_fallbacks(void)
{
    S23_BATT_STATE s;
    CHECK_EQ_U64(MahToMwh(0), 0);
    CHECK_EQ_U64(MahToMwh(1), 4);                      /* 3.88 rounds to 4 */
    CHECK_EQ_U64(MahToMwh(3900), 15132);
    CHECK_EQ_U64(MahToMwh(6000), 23280);              /* largest capacity the decoder accepts */

    regs_nominal();
    g_fg[0x18] = 0;                                    /* DesignCap invalid -> 3900 mAh fallback */
    run(&s);
    CHECK_EQ_U64(s.DesignedCapMwh, 15132);
    g_fg[0x18] = 0xFFFF;
    run(&s);
    CHECK_EQ_U64(s.DesignedCapMwh, 15132);
    g_fg[0x18] = 800;                                  /* 2000 mAh: lower limit, kept */
    run(&s);
    CHECK_EQ_U64(s.DesignedCapMwh, 7760);
    g_fg[0x18] = 2400;                                 /* 6000 mAh: upper limit, kept */
    run(&s);
    CHECK_EQ_U64(s.DesignedCapMwh, 23280);
    g_fg[0x18] = 2401;                                 /* 6002 mAh: rejected */
    run(&s);
    CHECK_EQ_U64(s.DesignedCapMwh, 15132);

    regs_nominal();
    g_fg[0x35] = 0xFFFF;                               /* FullCapRep invalid -> FullCap (0x10) */
    g_fg[0x10] = 1400;                                 /* 3500 mAh */
    run(&s);
    CHECK_EQ_U64(s.FullChargedCapMwh, 13580);
    g_fg[0x10] = 0;                                    /* both invalid -> design capacity */
    run(&s);
    CHECK_EQ_U64(s.FullChargedCapMwh, s.DesignedCapMwh);
    g_fg[0x35] = 600;                                  /* 1500 mAh: lower limit, kept */
    run(&s);
    CHECK_EQ_U64(s.FullChargedCapMwh, 5820);
    g_fg[0x06] = 0xFFFF;
    run(&s);
    CHECK_EQ_U64(s.RemainingMwh, s.FullChargedCapMwh);
}

static void test_power_state_table(void)
{
    S23_BATT_STATE s;
    const ULONG ON = BATTERY_POWER_ON_LINE, CHG = BATTERY_CHARGING, DIS = BATTERY_DISCHARGING;
    /* expected state for CHG_DTLS 0..15 while on line, with +500 mA and with -200 mA */
    static const ULONG wantPos[16] = { 5, 5, 5, 1, 1, 1, 1, 1, 3, 1, 3, 3, 1, 1, 1, 1 };
    static const ULONG wantNeg[16] = { 5, 5, 5, 1, 1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3 };
    for (uint32_t online = 0; online < 3; online++) {
        for (uint32_t d = 0; d < 16; d++) {
            for (int neg = 0; neg < 2; neg++) {
                regs_nominal();
                g_chg[0xB2] = (uint8_t)(online == 0 ? 0x9F : online == 1 ? 0x40 : 0x20); /* CHGIN / WCIN */
                g_chg[0xB4] = (uint8_t)(0xF0u | d);
                g_fg[0x0A] = neg ? 0xFF00 : 0x0280;    /* -200 mA / +500 mA */
                CHECK_EQ_U64(run(&s), 0);
                if (online == 0) {
                    CHECK_EQ_U64(s.PowerState, DIS);
                    CHECK_EQ_U64(g_fastChargeCalls, 0);
                    CHECK_EQ_U64((uint32_t)s.RateMw, neg ? (uint32_t)-800 : 0u);
                    continue;
                }
                CHECK_EQ_U64(g_fastChargeCalls, 1);
                ULONG want = neg ? wantNeg[d] : wantPos[d];
                CHECK_EQ_U64(s.PowerState, want);
                CHECK((s.PowerState & (CHG | DIS)) != (CHG | DIS));
                CHECK(s.PowerState & ON);
                if (s.PowerState & DIS) CHECK(s.RateMw <= 0);
                else CHECK(s.RateMw >= 0);
                if (s.PowerState & CHG) CHECK_EQ_U64((uint32_t)s.RateMw, neg ? 0u : 2000u);
            }
        }
    }
    /* -25 mA is still "idle" in the 5..7 (charger not charging) states; -25.8 mA is discharging */
    regs_nominal();
    g_chg[0xB2] = 0x40;
    g_chg[0xB4] = 0x05;
    g_fg[0x0A] = (uint16_t)-32;                         /* -25 mA */
    run(&s);
    CHECK_EQ_U64(s.PowerState, ON);
    CHECK_EQ_U64(s.RateMw, 0);
    g_fg[0x0A] = (uint16_t)-33;                         /* -25.78 -> -25 mA (truncated) */
    run(&s);
    CHECK_EQ_U64(s.PowerState, ON);
    g_fg[0x0A] = (uint16_t)-34;                         /* -26.56 -> -26 mA */
    run(&s);
    CHECK_EQ_U64(s.PowerState, ON | DIS);
}

static void test_critical(void)
{
    S23_BATT_STATE s;
    static const struct { uint16_t soc, vcell; uint8_t b2; int crit; } v[] = {
        { 0x0200, 0x9C40, 0x00, 1 },                   /* 2.00 %, 3125 mV, off line */
        { 0x0203, 0x9C40, 0x00, 0 },                   /* 2.01 % */
        { 0x0000, 0x9C40, 0x40, 0 },                   /* on line */
        { 0x0100, 0xA280, 0x00, 0 },                   /* 3250 mV: not below limit */
        { 0x0100, 0xA27F, 0x00, 1 },                   /* 3249 mV */
        { 0x0100, 0x7D00, 0x00, 0 },                   /* 2500 mV: implausible, not critical */
        { 0x0100, 0x7D0D, 0x00, 1 },                   /* 2501 mV */
    };
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        regs_nominal();
        g_fg[0x06] = v[i].soc;
        g_fg[0x09] = v[i].vcell;
        g_chg[0xB2] = v[i].b2;
        run(&s);
        CHECK_EQ_U64((s.PowerState & BATTERY_CRITICAL) != 0, v[i].crit);
    }
}

static void test_logging_bounds(void)
{
    S23_BATT_STATE s;
    regs_nominal();
    g_fg[0x0A] = 0x8000;
    g_fg[0x09] = 0xFFFE;
    g_logLen = 0;
    memset(&s, 0, sizeof(s));
    g_ext.Tlmm = g_fakeTlmm;
    CHECK_EQ_U64(S23BattReadHardware(&g_ext, &s, TRUE), 0);
    CHECK(strstr(g_log, "MAX77705 FG0 (st00, rem05, soc06, temp08): 00000000 00000000 00003200 00001900\r\n") != NULL);
    CHECK(strstr(g_log, "BATT decoded: soc=50.0% v=5119mV i=-25600mA (avg=-100mA) rate=-131046mW") != NULL);

    /* Append* never write past cap - 1, AppendSigned handles INT32_MIN */
    char b[8];
    size_t p = 0;
    memset(b, 'x', sizeof(b));
    AppendSigned(b, &p, sizeof(b), INT32_MIN);
    CHECK_EQ_U64(p, 7);
    CHECK(memcmp(b, "-214748", 7) == 0 && b[7] == 'x');
    p = 0;
    AppendStr(b, &p, 4, "abcdef");
    CHECK_EQ_U64(p, 3);
    AppendHex(b, &p, 4, 0xFFFFFFFFu, 8);
    CHECK_EQ_U64(p, 3);
    char big[600];
    p = 0;
    AppendSigned(big, &p, sizeof(big), INT32_MIN);
    big[p] = 0;
    CHECK(strcmp(big, "-2147483648") == 0);
    p = 0;
    AppendSigned(big, &p, sizeof(big), 0);
    big[p] = 0;
    CHECK(strcmp(big, "0") == 0);

    /* worst-case status line stays inside the 512-byte buffer */
    memset(&s, 0xFF, sizeof(s));
    s.CurrentMa = INT32_MIN;
    s.RateMw = INT32_MIN;
    S23BattDumpStatus(&s, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF);
    CHECK(g_statusLen > 0U && g_statusLen <= 512U);
}

static void test_configure_fast_charge(void)
{
    /* Off-line: S23BattConfigureFastCharge must be a no-op */
    regs_nominal();
    S23BattConfigureFastCharge(g_fakeTlmm, TLMM_GPIO206, TLMM_GPIO207, FALSE, 0x08u);
    CHECK_EQ_U64(g_chgWriteCount, 0u);

    /* (a) CNFG_06 (0xBD) unlock write 0x6D (bits [3:2] = 0x3, bit 0 = WDT kick) &
     * (d) Float voltage 4400 mV (CNFG_04 0xBB = 0x18), 2500 mA fast-charge current
     *     (CNFG_02 0xB9 = 0x32), 3000 mA CHGIN input limit (CNFG_09 0xC0 = 0xF7),
     *     and wireless/CHGIN config (CNFG_12 0xC3 = 0x68) from cold/unconfigured state */
    regs_nominal();
    g_logLen = 0;
    S23BattConfigureFastCharge(g_fakeTlmm, TLMM_GPIO206, TLMM_GPIO207, TRUE, 0x01u);

    /* 1 initial WDT kick + 9 programming writes = 10 writes total */
    CHECK_EQ_U64(g_chgWriteCount, 10u);
    CHECK_EQ_U64(g_chgWrites[0].reg, 0xBDu);
    CHECK_EQ_U64(g_chgWrites[0].val, 0x6Du);
    CHECK_EQ_U64((g_chgWrites[0].val >> 2) & 0x03u, 0x03u);

    CHECK_EQ_U64(g_chgWrites[1].reg, 0xBDu);
    CHECK_EQ_U64(g_chgWrites[1].val, 0x6Du);
    CHECK_EQ_U64(g_chgWrites[2].reg, 0xC3u);
    CHECK_EQ_U64(g_chgWrites[2].val, 0x68u); /* CNFG_12: CHGINSEL | WCINSEL | 4.6V */
    CHECK_EQ_U64(g_chgWrites[3].reg, 0xB7u);
    CHECK_EQ_U64(g_chgWrites[3].val, 0x04u); /* CNFG_00: Buck ON, Charger OFF toggle */
    CHECK_EQ_U64(g_chgWrites[4].reg, 0xBBu);
    CHECK_EQ_U64(g_chgWrites[4].val, 0x18u); /* CNFG_04: 4400 mV float voltage */
    CHECK_EQ_U64(g_chgWrites[5].reg, 0xB8u);
    CHECK_EQ_U64(g_chgWrites[5].val, 0x38u); /* CNFG_01: timer/restart disabled */
    CHECK_EQ_U64(g_chgWrites[6].reg, 0xB9u);
    CHECK_EQ_U64(g_chgWrites[6].val, 0x32u); /* CNFG_02: 2500 mA fast-charge current */
    CHECK_EQ_U64(g_chgWrites[7].reg, 0xC0u);
    CHECK_EQ_U64(g_chgWrites[7].val, 0xF7u); /* CNFG_09: 3000 mA CHGIN limit | CHG_EN */
    CHECK_EQ_U64(g_chgWrites[8].reg, 0xC1u);
    CHECK_EQ_U64(g_chgWrites[8].val, 0x3Fu); /* CNFG_10: 1600 mA WCIN limit */
    CHECK_EQ_U64(g_chgWrites[9].reg, 0xB7u);
    CHECK_EQ_U64(g_chgWrites[9].val, 0x05u); /* CNFG_00: Buck ON, Charger ON */
    CHECK_EQ_U64(g_stallUsTotal, 60000u);

    CHECK_EQ_U64(g_chg[0xBD], 0x6Du);
    CHECK_EQ_U64(g_chg[0xC3], 0x68u);
    CHECK_EQ_U64(g_chg[0xBB], 0x18u);
    CHECK_EQ_U64(g_chg[0xB8], 0x38u);
    CHECK_EQ_U64(g_chg[0xB9], 0x32u);
    CHECK_EQ_U64(g_chg[0xC0], 0xF7u);
    CHECK_EQ_U64(g_chg[0xC1], 0x3Fu);
    CHECK_EQ_U64(g_chg[0xB7], 0x05u);

    /* (c) Normal mode preservation on CNFG_00 (0x05):
     *     When registers are already configured and chgDtls01 is normal (e.g. 0x01 CC / 0x02 CV),
     *     only the WDT/unlock write (0xBD = 0x6D) is issued and CNFG_00 stays 0x05 without toggling to 0x04 */
    g_chgWriteCount = 0;
    g_stallUsTotal = 0;
    S23BattConfigureFastCharge(g_fakeTlmm, TLMM_GPIO206, TLMM_GPIO207, TRUE, 0x01u);
    CHECK_EQ_U64(g_chgWriteCount, 1u);
    CHECK_EQ_U64(g_chgWrites[0].reg, 0xBDu);
    CHECK_EQ_U64(g_chgWrites[0].val, 0x6Du);
    CHECK_EQ_U64(g_chg[0xB7], 0x05u);
    CHECK_EQ_U64(g_stallUsTotal, 0u);

    /* (b) abnormalOff (0x08 / 0x0B) recovery toggle to 0x04 before 0x05 on CNFG_00 (0xB7),
     *     even when all config registers already hold their fast-charge values */
    static const uint8_t abnormal_dtls[2] = { 0x08u, 0x0Bu };
    for (size_t k = 0; k < 2u; k++) {
        g_chgWriteCount = 0;
        g_stallUsTotal = 0;
        S23BattConfigureFastCharge(g_fakeTlmm, TLMM_GPIO206, TLMM_GPIO207, TRUE,
                                   (uint8_t)(0xA0u | abnormal_dtls[k]));
        CHECK_EQ_U64(g_chgWriteCount, 10u);

        int idx_04 = -1, idx_05 = -1;
        for (size_t i = 0; i < g_chgWriteCount; i++) {
            if (g_chgWrites[i].reg == 0xB7u && g_chgWrites[i].val == 0x04u && idx_04 < 0) {
                idx_04 = (int)i;
            }
            if (g_chgWrites[i].reg == 0xB7u && g_chgWrites[i].val == 0x05u) {
                idx_05 = (int)i;
            }
        }
        CHECK(idx_04 >= 0 && idx_05 > idx_04);
        CHECK_EQ_U64(g_chg[0xB7], 0x05u);
        CHECK_EQ_U64(g_stallUsTotal, 60000u);
    }

    /* (d) Graceful handling of I2C write failure:
     *     When I2cRegWrite8 fails (-1), S23BattConfigureFastCharge and S23BattReadHardware
     *     must not crash, hang, or corrupt fuel-gauge telemetry */
    regs_nominal();
    g_chg[0xB2] = 0x40u; /* CHGIN online */
    g_chg[0xB4] = 0x08u; /* abnormalOff */
    g_failChgWrite = 1;
    S23_BATT_STATE s;
    CHECK_EQ_U64(run(&s), 0);
    CHECK(s.Valid);
    CHECK_EQ_U64(s.SocX100, 5000u);
    CHECK_EQ_U64(s.VoltageMv, 4000u);
    CHECK_EQ_U64(g_chgWriteCount, 10u);
    CHECK_EQ_U64(g_chg[0xB7], 0x00u); /* unchanged because writes failed */
}

int main(void)
{
    test_nominal_discharge();
    test_invalid_samples_leave_output_untouched();
    test_soc();
    test_voltage_current_full_sweep();
    test_temperature();
    test_capacity_fallbacks();
    test_power_state_table();
    test_critical();
    test_logging_bounds();
    test_configure_fast_charge();
    return test_report("test_batt_fg");
}
