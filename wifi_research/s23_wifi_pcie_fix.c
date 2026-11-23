/*
 * s23_wifi_pcie_fix.c
 *
 * Qualcomm Snapdragon 8 Gen 2 (SM8550 / Kalama) - Samsung Galaxy S23 (dm1q)
 * Native Windows 11 ARM64 Bring-Up: PCIe0 Root Complex + QCA6490 Wi-Fi Fixes
 *
 * Resolves the 5 hardware register defects discovered during live probe vs.
 * Linux kernel (pci-msm.c) / UEFI (G289Pkg) auditing that held the PCIe0 LTSSM
 * frozen in state 0x00 (S_DETECT_QUIET):
 *
 *   1. PMIC SMPS rails (smpe4, smpg2, smpg4, smpg5, smpg6) left in Retention
 *      mode (set_mode=0 -> SPMI STATUS1=0x47, VREG_OK bit 7 = 0) instead of
 *      active NPM/PWM mode (set_mode=1, mode=7 -> STATUS1=0x87/0x8F).
 *   2. PARF_DEVICE_TYPE (PARF+0x1000) written as 0x0 (Endpoint) instead of
 *      0x4 (Root Complex) prior to PHY initialization.
 *   3. Canonical Qualcomm PARF registers overwritten instead of matching
 *      pci-msm.c: PARF+0x00 (SYS_CTRL = 0x365E), PARF+0x350 (DBI_BASE_ADDR = 0),
 *      and PARF+0x358 (SLV_ADDR_SPACE_SIZE = 0x04000000 / 64 MiB).
 *   4. DBI+0x0A0 (PCI_EXP_LNKCTL2) Target Link Speed forced to Gen 2 (0x2)
 *      before initial link training instead of PCIe-spec Gen 1 (0x1).
 *   5. PARF trapped in Flush Mode (PARF_STATUS 0x230 = 0x00000B00:
 *      FLUSH_COMPLETED bit 8 + FLUSH_MODE bit 11), holding the Sub-MAC in
 *      reset until SW_CLR_FLUSH_MODE (BIT(10) = 0x400) is written alongside
 *      LTSSM_EN (BIT(8) = 0x100) to PARF+0x1B0.
 *
 * Expected virtual MMIO base mappings passed to Configure_S23_PCIe0_QCA6490():
 *   parf     -> SM8550 PCIe0 PARF          (phys 0x01C00000, size 0x3000)
 *   dbi      -> SM8550 PCIe0 DBI / DWC     (phys 0x60000000, size 0x2000)
 *   tcs_base -> APPS_RSC DRV2 TCS0 block   (phys 0x17A20D00 = 0x17A20000 + 0xD00)
 */

#include <stdint.h>
#include <stddef.h>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

/* ============================================================================
 * ARM64 Strongly Ordered MMIO Access Macros
 * ============================================================================ */
#ifndef MMIO_DMB_SY
#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
#define MMIO_DMB_SY() __asm__ volatile("dmb sy" ::: "memory")
#elif defined(_M_ARM64) && defined(_MSC_VER)
#define MMIO_DMB_SY() __dmb(_ARM64_BARRIER_SY)
#elif defined(__GNUC__) || defined(__clang__)
#define MMIO_DMB_SY() __asm__ volatile("" ::: "memory")
#else
#define MMIO_DMB_SY() ((void)0)
#endif
#endif

#ifndef mmio_r32
#if defined(__GNUC__) || defined(__clang__)
#define mmio_r32(base, off)                                                    \
    __extension__ ({                                                           \
        MMIO_DMB_SY();                                                         \
        uint32_t _mmio_val =                                                   \
            *(const volatile uint32_t *)((const volatile uint8_t *)(base) +    \
                                         (uint32_t)(off));                     \
        MMIO_DMB_SY();                                                         \
        _mmio_val;                                                             \
    })
#else
#define mmio_r32(base, off)                                                    \
    (MMIO_DMB_SY(),                                                            \
     *(const volatile uint32_t *)((const volatile uint8_t *)(base) +           \
                                  (uint32_t)(off)))
#endif
#endif

#ifndef mmio_w32
#define mmio_w32(base, off, val)                                               \
    do {                                                                       \
        MMIO_DMB_SY();                                                         \
        *(volatile uint32_t *)((volatile uint8_t *)(base) + (uint32_t)(off)) = \
            (uint32_t)(val);                                                   \
        MMIO_DMB_SY();                                                         \
    } while (0)
#endif

#ifndef mmio_rmw32
#define mmio_rmw32(base, off, clear_bits, set_bits)                            \
    do {                                                                       \
        uint32_t _mmio_cur = mmio_r32((base), (off));                          \
        uint32_t _mmio_new =                                                   \
            (_mmio_cur & ~(uint32_t)(clear_bits)) | (uint32_t)(set_bits);      \
        mmio_w32((base), (off), _mmio_new);                                    \
    } while (0)
#endif

/* ============================================================================
 * SM8550 RPMh APPS_RSC DRV2 TCS0 Register Map (relative to tcs_base = 0x17A20D00)
 * ============================================================================ */
#define RSC_DRV_IRQ_ENABLE          0x00U       /* Bit 0: TCS0 IRQ enable */
#define RSC_DRV_IRQ_STATUS          0x04U       /* Bit 0: TCS0 completion status */
#define RSC_DRV_IRQ_CLEAR           0x08U       /* Bit 0: TCS0 completion clear */
#define TCS0_CTRL                   0x24U       /* AMC_ENABLE / AMC_TRIGGER */
#define TCS0_STATUS                 0x28U       /* Bit 0: 1 == TCS0 idle */
#define TCS0_CMD_ENABLE             0x2CU       /* Bitmask of active cmd slots */
#define TCS0_CMD0_MSGID             0x34U       /* Slot 0 message header */
#define TCS0_CMD0_ADDR              0x38U       /* Slot 0 20-bit CMD_DB address */
#define TCS0_CMD0_DATA              0x3CU       /* Slot 0 32-bit payload */
#define TCS0_CMD0_STATUS            0x40U       /* Slot 0 tx/completion status */

#define TCS0_IRQ_BIT                (1U << 0)
#define TCS0_STATUS_IDLE            (1U << 0)
#define TCS_AMC_ENABLE              (1U << 16)
#define TCS_AMC_TRIGGER             (1U << 24)

/* RSC v3 MSGID: LEN=8 (0x08) | RESP_REQ (0x100) | WRITE (0x10000) */
#define RPMH_MSGID_WRITE_RESP       (8U | 0x100U | 0x10000U)

/* RPMh VRM per-regulator sub-register offsets */
#define VRM_REG_VOLTAGE             0x00U       /* Target voltage in mV */
#define VRM_REG_ENABLE              0x04U       /* 1 = enable, 0 = disable */
#define VRM_REG_MODE                0x08U       /* PMIC5 operating mode */

/* PMIC5 FTSMPS525 active Normal Power Mode (NPM / forced PWM) */
#define VRM_MODE_NPM_PWM            7U
#define VRM_ENABLE_ON               1U

/* Live S23 (dm1q) CMD_DB (0x81C60000) SLV4 VRM base addresses for QCA6490 rails */
#define VRM_ADDR_SMPE4              0x40200U    /* PM8550VS_E S4: vdd-wlan-dig  (966 mV)  */
#define VRM_ADDR_SMPG2              0x40400U    /* PM8550VS_G S2: vdd-wlan-aon  (1012 mV) */
#define VRM_ADDR_SMPG4              0x40600U    /* PM8550VS_G S4: vdd-wlan-rfa2 (1350 mV) */
#define VRM_ADDR_SMPG5              0x44100U    /* PM8550VS_G S5: vdd-wlan      (966 mV)  */
#define VRM_ADDR_SMPG6              0x40700U    /* PM8550VS_G S6: vdd-wlan-rfa1 (1900 mV) */

/* ============================================================================
 * SM8550 PCIe0 PARF (0x01C00000) & Synopsys DBI (0x60000000) Registers
 * ============================================================================ */
#define PARF_SYS_CTRL               0x0000U     /* System / clock-gating / wakeup control */
#define PARF_LTSSM                  0x01B0U     /* LTSSM state, enable & flush clear */
#define PARF_STATUS                 0x0230U     /* Hardware status (FLUSH_MODE / FLUSH_COMPLETED) */
#define PARF_DBI_BASE_ADDR          0x0350U     /* DBI internal base address (0x0 on SM8550) */
#define PARF_SLV_ADDR_SPACE_SIZE    0x0358U     /* Outbound slave aperture size (64 MiB) */
#define PARF_DEVICE_TYPE            0x1000U     /* Synopsys DWC device_type[3:0] strap */

#define PARF_DEVICE_TYPE_RC         0x00000004U /* 0x4 = Root Complex (Root Port); 0x0 = Endpoint */
#define PARF_SYS_CTRL_CANONICAL     0x0000365EU /* Canonical pci-msm.c SYS_CTRL configuration */
#define PARF_SLV_ADDR_SPACE_64M     0x04000000U /* 64 MiB window: 0x60000000..0x63FFFFFF */

#define PARF_LTSSM_EN               (1U << 8)   /* 0x100: Enable Synopsys LTSSM state machine */
#define PARF_LTSSM_SW_CLR_FLUSH     (1U << 10)  /* 0x400: SW_CLR_FLUSH_MODE releases Sub-MAC */

#define PARF_STATUS_FLUSH_COMPLETED (1U << 8)   /* 0x100: Pending AXI/TLP flush completed */
#define PARF_STATUS_FLUSH_MODE      (1U << 11)  /* 0x800: PARF holding Sub-MAC in flush reset */

#define DBI_PCI_EXP_LNKCTL2         0x00A0U     /* PCIe Cap (0x70) + 0x30: Link Control 2 */
#define PCI_EXP_LNKCTL2_TLS_MASK    0x0000000FU /* Bits [3:0]: Target Link Speed */
#define PCI_EXP_LNKCTL2_TLS_GEN1    0x00000001U /* 0x1 = Gen 1 (2.5 GT/s) initial training speed */

#define RPMH_POLL_LIMIT             100000U
#define FLUSH_CLEAR_POLL_LIMIT      10000U

/*
 * Configure_S23_PCIe0_QCA6490
 *
 * Applies all 5 critical SM8550 PCIe0 / QCA6490 bring-up fixes in hardware order:
 *   - Fix 1 (RPMh TCS0): Promote all 5 Wi-Fi SMPS rails from Retention to NPM/PWM.
 *   - Fix 2 (PARF):      Strap Synopsys DWC core as Root Complex (0x4) before PHY init.
 *   - Fix 3 (PARF):      Restore canonical SYS_CTRL, DBI_BASE_ADDR, SLV_ADDR_SPACE_SIZE.
 *   - Fix 4 (DBI):       Set PCI_EXP_LNKCTL2 Target Link Speed to Gen 1 (0x1).
 *   - Fix 5 (PARF):      Clear latched Flush Mode (BIT(10)) alongside LTSSM_EN (BIT(8)).
 *
 * Integration note for full cold-boot driver bring-up:
 *   On SM8550, QMP PHY register programming (0x01C06000), gcc_pcie_0_pipe_clk
 *   enable + PCS_STATUS (0x01C06214 bit 6 == 0) poll, and PERST# (GPIO94)
 *   deassertion occur between PARF pre-PHY configuration (Fixes 2-3) and the
 *   first DBI / LTSSM accesses (Fixes 4-5). Ensure PIPE clock and PHY are ready
 *   before passing a live `dbi` pointer into Fix 4 to avoid an AXI SError.
 */
void Configure_S23_PCIe0_QCA6490(volatile uint8_t* parf,
                                 volatile uint8_t* dbi,
                                 volatile uint8_t* tcs_base)
{
    /* ========================================================================
     * FIX 1: Promote QCA6490 PMIC SMPS Rails out of Retention into NPM/PWM Mode
     * ------------------------------------------------------------------------
     * Root cause:
     *   The live probe previously voted the 5 QCA6490 SMPS rails (smpe4,
     *   smpg2, smpg4, smpg5, smpg6) with set_mode=0, writing only VRM_VOLTAGE
     *   (+0x00) and VRM_ENABLE (+0x04) without writing VRM_MODE (+0x08).
     *   Without an active mode vote, PM8550VS_E/G left SMPS rails in ultra-low-
     *   power Retention mode (SPMI MODE_CTL=0x03). In particular, smpg5
     *   (PPID 0x6A7, vdd-wlan) stalled at SPMI STATUS1=0x47 (VREG_OK bit 7 = 0),
     *   unable to supply active QCA6490 digital/PCIe SerDes inrush current.
     *
     * Resolution:
     *   Issue RPMh APPS_RSC_DRV2 TCS0 AMC commands for all 5 Wi-Fi SMPS rails
     *   with set_mode=1 and mode=7 (NPM/PWM active mode):
     *     1) vrm_base + 0x00 <- target voltage (mV)
     *     2) vrm_base + 0x08 <- 0x7 (NPM/PWM active mode)
     *     3) vrm_base + 0x04 <- 0x1 (regulator enable)
     *   This transitions all 5 rails to SPMI STATUS1 = 0x87 or 0x8F (VREG_OK=1)
     *   with MODE_CTL = 0x07 so the QCA6490 powers up cleanly and presents
     *   receiver termination on the PCIe0 lanes.
     * ======================================================================== */
    if (tcs_base != NULL) {
        static const struct {
            uint32_t vrm_base;
            uint32_t mv;
            uint32_t set_mode;
            uint32_t mode;
        } wifi_rails[5] = {
            /* smpe4 (PM8550VS_E S4, vdd-wlan-dig,  PPID 0x4A4):  966 mV, set_mode=1, mode=7 */
            { VRM_ADDR_SMPE4,  966U, 1U, VRM_MODE_NPM_PWM },
            /* smpg2 (PM8550VS_G S2, vdd-wlan-aon,  PPID 0x69E): 1012 mV, set_mode=1, mode=7 */
            { VRM_ADDR_SMPG2, 1012U, 1U, VRM_MODE_NPM_PWM },
            /* smpg4 (PM8550VS_G S4, vdd-wlan-rfa2, PPID 0x6A4): 1350 mV, set_mode=1, mode=7 */
            { VRM_ADDR_SMPG4, 1350U, 1U, VRM_MODE_NPM_PWM },
            /* smpg5 (PM8550VS_G S5, vdd-wlan,      PPID 0x6A7):  966 mV, set_mode=1, mode=7 */
            { VRM_ADDR_SMPG5,  966U, 1U, VRM_MODE_NPM_PWM },
            /* smpg6 (PM8550VS_G S6, vdd-wlan-rfa1, PPID 0x6AA): 1900 mV, set_mode=1, mode=7 */
            { VRM_ADDR_SMPG6, 1900U, 1U, VRM_MODE_NPM_PWM }
        };

        /* Preserve TCS0 state so existing UEFI/Windows RSC configuration stays intact. */
        uint32_t saved_irq_en = mmio_r32(tcs_base, RSC_DRV_IRQ_ENABLE);
        uint32_t saved_cmd_en = mmio_r32(tcs_base, TCS0_CMD_ENABLE);
        uint32_t saved_msgid  = mmio_r32(tcs_base, TCS0_CMD0_MSGID);
        uint32_t saved_addr   = mmio_r32(tcs_base, TCS0_CMD0_ADDR);
        uint32_t saved_data   = mmio_r32(tcs_base, TCS0_CMD0_DATA);

        /* Ensure TCS0 completion status latches in RSC_DRV_IRQ_STATUS bit 0. */
        mmio_rmw32(tcs_base, RSC_DRV_IRQ_ENABLE, 0U, TCS0_IRQ_BIT);

        for (uint32_t r = 0U; r < 5U; r++) {
            uint32_t cmd_addrs[3];
            uint32_t cmd_datas[3];
            uint32_t cmd_count = 0U;

            /* Step 1a: Program target voltage (VRM_VOLTAGE = vrm_base + 0x00). */
            cmd_addrs[cmd_count] = wifi_rails[r].vrm_base + VRM_REG_VOLTAGE;
            cmd_datas[cmd_count] = wifi_rails[r].mv;
            cmd_count++;

            /* Step 1b: Force active NPM/PWM mode (VRM_MODE = vrm_base + 0x08 <- 7) when set_mode=1. */
            if (wifi_rails[r].set_mode != 0U) {
                cmd_addrs[cmd_count] = wifi_rails[r].vrm_base + VRM_REG_MODE;
                cmd_datas[cmd_count] = wifi_rails[r].mode;
                cmd_count++;
            }

            /* Step 1c: Assert regulator enable (VRM_ENABLE = vrm_base + 0x04 <- 1). */
            cmd_addrs[cmd_count] = wifi_rails[r].vrm_base + VRM_REG_ENABLE;
            cmd_datas[cmd_count] = VRM_ENABLE_ON;
            cmd_count++;

            for (uint32_t c = 0U; c < cmd_count; c++) {
                /* Wait for TCS0 controller to report IDLE before mutating slot 0. */
                for (uint32_t poll = 0U; poll < RPMH_POLL_LIMIT; poll++) {
                    if ((mmio_r32(tcs_base, TCS0_STATUS) & TCS0_STATUS_IDLE) != 0U) {
                        break;
                    }
                }

                /* Clear any stale TCS0 completion latch and disarm all command slots. */
                mmio_w32(tcs_base, RSC_DRV_IRQ_CLEAR, TCS0_IRQ_BIT);
                mmio_w32(tcs_base, TCS0_CMD_ENABLE, 0U);

                /* Load single-slot AMC write command with completion response requested. */
                mmio_w32(tcs_base, TCS0_CMD0_MSGID, RPMH_MSGID_WRITE_RESP);
                mmio_w32(tcs_base, TCS0_CMD0_ADDR,  cmd_addrs[c]);
                mmio_w32(tcs_base, TCS0_CMD0_DATA,  cmd_datas[c]);
                mmio_w32(tcs_base, TCS0_CMD_ENABLE, 1U);

                /*
                 * Qualcomm RSC v3 AMC trigger handshake:
                 * Explicitly transition TRIGGER=0 -> ENABLE=0 -> ENABLE=1 -> TRIGGER=1
                 * with readbacks between edges so the hardware detects a clean rising
                 * edge and dispatches the AMC request to the RPMh VRM sequencer.
                 */
                mmio_rmw32(tcs_base, TCS0_CTRL, TCS_AMC_TRIGGER, 0U);
                (void)mmio_r32(tcs_base, TCS0_CTRL);
                mmio_rmw32(tcs_base, TCS0_CTRL, TCS_AMC_ENABLE, 0U);
                (void)mmio_r32(tcs_base, TCS0_CTRL);
                mmio_rmw32(tcs_base, TCS0_CTRL, 0U, TCS_AMC_ENABLE);
                (void)mmio_r32(tcs_base, TCS0_CTRL);
                mmio_rmw32(tcs_base, TCS0_CTRL, 0U, TCS_AMC_TRIGGER);

                /* Poll RSC_DRV_IRQ_STATUS bit 0 until RPMh VRM acknowledges completion. */
                for (uint32_t poll = 0U; poll < RPMH_POLL_LIMIT; poll++) {
                    if ((mmio_r32(tcs_base, RSC_DRV_IRQ_STATUS) & TCS0_IRQ_BIT) != 0U) {
                        break;
                    }
                }

                /* Read command completion status and disarm AMC trigger/enable cleanly. */
                (void)mmio_r32(tcs_base, TCS0_CMD0_STATUS);
                mmio_rmw32(tcs_base, TCS0_CTRL, TCS_AMC_TRIGGER, 0U);
                mmio_rmw32(tcs_base, TCS0_CTRL, TCS_AMC_ENABLE, 0U);
                mmio_w32(tcs_base, TCS0_CMD_ENABLE, 0U);
                mmio_w32(tcs_base, RSC_DRV_IRQ_CLEAR, TCS0_IRQ_BIT);
            }
        }

        /* Restore TCS0 slot 0 registers and IRQ enable state. */
        mmio_w32(tcs_base, TCS0_CMD0_MSGID, saved_msgid);
        mmio_w32(tcs_base, TCS0_CMD0_ADDR,  saved_addr);
        mmio_w32(tcs_base, TCS0_CMD0_DATA,  saved_data);
        mmio_w32(tcs_base, TCS0_CMD_ENABLE, saved_cmd_en);
        mmio_w32(tcs_base, RSC_DRV_IRQ_ENABLE, saved_irq_en);
    }

    if (parf != NULL) {
        /* ====================================================================
         * FIX 2: Strap Synopsys DesignWare Core as Root Complex (RC)
         * --------------------------------------------------------------------
         * Root cause:
         *   PARF+0x1000 (PARF_DEVICE_TYPE) was previously written as 0x0
         *   (PCIe Endpoint). With both the SM8550 host controller and the
         *   QCA6490 Wi-Fi silicon strapped as Endpoints, neither side acts as
         *   a Root Port to initiate downstream training or expose a Type-1
         *   PCIe bridge header.
         *
         * Resolution:
         *   Write 0x4 (Root Complex / Root Port) to PARF+0x1000 first, before
         *   QMP PHY programming, matching pci-msm.c (line 6088) and UEFI
         *   g289_rc0.c.
         * ==================================================================== */
        mmio_w32(parf, PARF_DEVICE_TYPE, PARF_DEVICE_TYPE_RC);

        /* ====================================================================
         * FIX 3: Restore Canonical Qualcomm PARF Configuration Registers
         * --------------------------------------------------------------------
         * Root cause:
         *   Overwriting PARF_SYS_CTRL, PARF_DBI_BASE_ADDR, and
         *   PARF_SLV_ADDR_SPACE_SIZE with non-canonical values broke internal
         *   AXI/DBI address decoding and allowed core/AXI clock auto-gating
         *   while the controller was idle in S_DETECT_QUIET.
         *
         * Resolution (exact canonical pci-msm.c / g289_rc0.c sequence):
         *   - PARF+0x0350 (DBI_BASE_ADDR) = 0x0:
         *       On SM8550 PARF v2.7+/v2.9, internal DBI base stays 0 (writing
         *       physical 0x60000000 misroutes DBI/ELBI/iATU transactions).
         *   - PARF+0x0000 (SYS_CTRL) = 0x365E:
         *       Transitions from POR default 0x2000361C to canonical 0x365E,
         *       asserting CORE_CLK_CGC_DIS (BIT(6)), SLV/MSTR_ACLK_CGC_DIS
         *       (BIT(9)|BIT(10)), L1/L23_CLK_RMV_DIS (BIT(1)|BIT(2)),
         *       AUX_PWR_DET (BIT(4)), and SLV/MST_WAKEUP_EN (BIT(12)|BIT(13))
         *       so core and AXI clocks cannot auto-gate during training.
         *   - PARF+0x0358 (SLV_ADDR_SPACE_SIZE) = 0x04000000 (64 MiB):
         *       Opens the full 64 MiB outbound PCIe0 slave window covering
         *       0x60000000..0x63FFFFFF (DBI, ECAM, I/O, and MEM32 BAR space).
         * ==================================================================== */
        mmio_w32(parf, PARF_DBI_BASE_ADDR,       0x00000000U);
        mmio_w32(parf, PARF_SYS_CTRL,            PARF_SYS_CTRL_CANONICAL);
        mmio_w32(parf, PARF_SLV_ADDR_SPACE_SIZE, PARF_SLV_ADDR_SPACE_64M);
    }

    /*
     * [Full cold-boot integration boundary]
     * QMP PHY 115-triplet init (0x01C06000), gcc_pcie_0_pipe_clk enable,
     * PHY PCS_STATUS (0x01C06214 bit 6 == 0) poll, and PERST# (GPIO94 HIGH)
     * release must have completed before touching DBI (0x60000000) below.
     */

    if (dbi != NULL) {
        /* ====================================================================
         * FIX 4: Start Link Training at Gen 1 (2.5 GT/s) in PCI_EXP_LNKCTL2
         * --------------------------------------------------------------------
         * Root cause:
         *   DBI+0x0A0 (PCI_EXP_LNKCTL2, bits [3:0] Target Link Speed) was
         *   programmed to Gen 2 (0x2 / 5.0 GT/s) prior to initial training.
         *   Per the PCIe Base Specification, initial Detect -> Polling ->
         *   Configuration -> L0 negotiation must begin at Gen 1 (2.5 GT/s)
         *   before up-training to Gen 2 via Recovery.Speed once L0 is reached.
         *
         * Resolution:
         *   Read-modify-write DBI+0x0A0 to clear bits [3:0] and set 0x1
         *   (Gen 1 / 2.5 GT/s) while preserving upper LNKCTL2 control bits.
         * ==================================================================== */
        mmio_rmw32(dbi,
                   DBI_PCI_EXP_LNKCTL2,
                   PCI_EXP_LNKCTL2_TLS_MASK,
                   PCI_EXP_LNKCTL2_TLS_GEN1);
    }

    if (parf != NULL) {
        /* ====================================================================
         * FIX 5: Clear Latched PARF Flush Mode and Enable LTSSM
         * --------------------------------------------------------------------
         * Root cause:
         *   Live audit of PARF_STATUS (PARF+0x230) read 0x00000B00, showing
         *   both FLUSH_COMPLETED (BIT(8) = 0x100) and FLUSH_MODE (BIT(11) =
         *   0x800) latched high. While FLUSH_MODE is asserted, PARF holds the
         *   Synopsys Sub-MAC in reset/flush barrier: setting LTSSM_EN alone
         *   (PARF+0x1B0 = 0x100) leaves the state machine permanently frozen
         *   in state 0x00 (S_DETECT_QUIET, DBI+0x72C DBG1 = 0x08200000).
         *
         * Resolution:
         *   Write SW_CLR_FLUSH_MODE (BIT(10) = 0x400) alongside LTSSM_EN
         *   (BIT(8) = 0x100) to PARF+0x1B0 (PARF_LTSSM) to release the
         *   Sub-MAC out of flush reset and start link training toward L0,
         *   then poll PARF_STATUS (0x230) until FLUSH_MODE (BIT(11)) clears.
         * ==================================================================== */
        mmio_rmw32(parf,
                   PARF_LTSSM,
                   0U,
                   PARF_LTSSM_SW_CLR_FLUSH | PARF_LTSSM_EN);

        for (uint32_t poll = 0U; poll < FLUSH_CLEAR_POLL_LIMIT; poll++) {
            if ((mmio_r32(parf, PARF_STATUS) & PARF_STATUS_FLUSH_MODE) == 0U) {
                break;
            }
        }
    }
}
