/* Host tests for the S6E3FAC AMOLED brightness curve and MIPI DSI 0x51 11-bit DBV packet packing
 * in drivers/s23gpu/disp.c (extracted verbatim by tests/c/extract_gpu_brightness.py):
 *   (a) exact 101-entry g_PctToDbv[101] table length (0..100%) and DispGetPossibleBrightness()
 *   (b) boundary DBV values: g_PctToDbv[0] (0 or S23_DBV_MIN=8) and g_PctToDbv[100] == 2047 (0x07FF)
 *   (c) strict monotonic non-decreasing behavior across all 0..100% steps
 *   (d) MIPI DSI 0x51 (MIPI_DCS_SET_DISPLAY_BRIGHTNESS) 11-bit big-endian DBV payload packing
 *       (upper 3 bits in byte 1, lower 8 bits in byte 2)
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "host_test.h"

#define VOID void
#define UNREFERENCED_PARAMETER(x) (void)(x)
typedef uint8_t  UCHAR, *PUCHAR;
typedef uint16_t USHORT;
typedef uint32_t ULONG;
typedef int32_t  NTSTATUS;
typedef void    *PVOID;

#define STATUS_SUCCESS           ((NTSTATUS)0x00000000L)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000DL)
#define STATUS_BUFFER_TOO_SMALL  ((NTSTATUS)0xC0000023L)

#include "gpu_brightness_extracted.inc"

static void test_table_length_and_endpoints(void)
{
    /* (a) Exact 101-entry table length for 0..100% brightness steps */
    size_t count = sizeof(g_PctToDbv) / sizeof(g_PctToDbv[0]);
    CHECK_EQ_U64(count, 101u);

    /* (b) 0% maps to floor DBV (0 or S23_DBV_MIN=8 so panel stays visible) and 100% maps to 2047 (0x07FF) */
    CHECK(g_PctToDbv[0] == 0u || g_PctToDbv[0] == S23_DBV_MIN);
    CHECK_EQ_U64(g_PctToDbv[100], 2047u);
    CHECK_EQ_U64(g_PctToDbv[100], S23_DBV_MAX);
    CHECK_EQ_U64(S23_DBV_MAX, 0x07FFu);
}

static void test_monotonic_non_decreasing(void)
{
    /* (c) Strict monotonic non-decreasing behavior across all 0..100% steps */
    for (size_t pct = 0; pct <= 100u; pct++) {
        USHORT dbv = g_PctToDbv[pct];
        CHECK(dbv >= S23_DBV_MIN && dbv <= S23_DBV_MAX);
        CHECK((dbv & ~0x07FFu) == 0u);
        if (pct > 0u) {
            CHECK(g_PctToDbv[pct] >= g_PctToDbv[pct - 1u]);
            CHECK(g_PctToDbv[pct] >  g_PctToDbv[pct - 1u]);
        }
    }
}

static void test_mipi_dsi_0x51_dbv_packing(void)
{
    /* (d) MIPI DSI 0x51 11-bit big-endian DBV payload packing:
     *     pktDbv[0] = 0xC0390003 (DCS Long Write 0x39, WC = 3 bytes)
     *     pktDbv[1] byte 0 = 0x51 (MIPI_DCS_SET_DISPLAY_BRIGHTNESS)
     *     pktDbv[1] byte 1 = (Dbv >> 8) & 0x07 (upper 3 bits of 11-bit DBV)
     *     pktDbv[1] byte 2 = Dbv & 0xFF (lower 8 bits of 11-bit DBV)
     *     pktDbv[1] byte 3 = 0xFF (FIFO padding)
     */
    for (uint32_t dbv = 0; dbv <= 0x07FFu; dbv++) {
        ULONG pkt[2] = { 0u, 0u };
        S23PackDsiDbvPkt((USHORT)dbv, pkt);

        CHECK_EQ_U64(pkt[0], 0xC0390003u);

        uint8_t b0 = (uint8_t)(pkt[1] & 0xFFu);
        uint8_t b1 = (uint8_t)((pkt[1] >> 8) & 0xFFu);
        uint8_t b2 = (uint8_t)((pkt[1] >> 16) & 0xFFu);
        uint8_t b3 = (uint8_t)((pkt[1] >> 24) & 0xFFu);

        CHECK_EQ_U64(b0, 0x51u);
        CHECK_EQ_U64(b1, (dbv >> 8) & 0x07u);
        CHECK_EQ_U64(b2, dbv & 0xFFu);
        CHECK_EQ_U64(b3, 0xFFu);

        uint32_t unpacked = ((uint32_t)b1 << 8) | (uint32_t)b2;
        CHECK_EQ_U64(unpacked, dbv);
    }

    /* Check upper bits above bit 10 are masked off by the 3-bit mask ((Dbv >> 8) & 0x07) */
    {
        ULONG pkt_max[2], pkt_masked[2];
        S23PackDsiDbvPkt(0x07FFu, pkt_max);
        S23PackDsiDbvPkt(0xFFFFu, pkt_masked);
        CHECK_EQ_U64(pkt_max[0], pkt_masked[0]);
        CHECK_EQ_U64(pkt_max[1], pkt_masked[1]);
        CHECK_EQ_U64(pkt_max[1], 0xFFFF0751u);

        ULONG pkt_zero[2];
        S23PackDsiDbvPkt(0x0000u, pkt_zero);
        CHECK_EQ_U64(pkt_zero[1], 0xFF000051u);
    }

    /* Check every entry in g_PctToDbv[0..100] packs and unpacks losslessly */
    for (size_t pct = 0; pct <= 100u; pct++) {
        ULONG pkt[2];
        USHORT dbv = g_PctToDbv[pct];
        S23PackDsiDbvPkt(dbv, pkt);
        uint32_t hi3 = (pkt[1] >> 8) & 0x07u;
        uint32_t lo8 = (pkt[1] >> 16) & 0xFFu;
        CHECK_EQ_U64((hi3 << 8) | lo8, dbv);
    }
}

static void test_possible_brightness_ddi(void)
{
    UCHAR count = 0;
    UCHAR levels[105];
    memset(levels, 0xAA, sizeof(levels));

    CHECK_EQ_U64((uint32_t)DispGetPossibleBrightness(NULL, 101u, NULL, levels),
                 (uint32_t)STATUS_INVALID_PARAMETER);
    CHECK_EQ_U64((uint32_t)DispGetPossibleBrightness(NULL, 100u, &count, levels),
                 (uint32_t)STATUS_BUFFER_TOO_SMALL);
    CHECK_EQ_U64(count, 101u);
    CHECK_EQ_U64((uint32_t)DispGetPossibleBrightness(NULL, 101u, &count, NULL),
                 (uint32_t)STATUS_BUFFER_TOO_SMALL);

    count = 0;
    CHECK_EQ_U64((uint32_t)DispGetPossibleBrightness(NULL, 101u, &count, levels),
                 (uint32_t)STATUS_SUCCESS);
    CHECK_EQ_U64(count, 101u);
    for (uint32_t i = 0; i < 101u; i++) {
        CHECK_EQ_U64(levels[i], i);
    }
    for (uint32_t i = 101u; i < sizeof(levels); i++) {
        CHECK_EQ_U64(levels[i], 0xAAu);
    }
}

int main(void)
{
    test_table_length_and_endpoints();
    test_monotonic_non_decreasing();
    test_mipi_dsi_0x51_dbv_packing();
    test_possible_brightness_ddi();
    return test_report("test_gpu_brightness");
}
