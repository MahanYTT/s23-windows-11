/*
 * s23gpuprobe.c - Samsung Galaxy S23 (dm1q / SM8550-AC) Adreno 740 Kernel Probe v1.0.0.4
 *
 * Pure-C ARM64 WDM Demand-Start (Start = 3) diagnostic & opt-in bring-up driver.
 * Uses FILE_OPEN_IF on pre-allocated 64 KB C:\Users\Public\s23gpuprobe.log with
 * FILE_WRITE_THROUGH | FILE_SYNCHRONOUS_IO_NONALERT so every LogNL() flushes
 * directly to UFS before the next MMIO instruction executes.
 *
 * Registry control:
 *   HKLM\SYSTEM\CurrentControlSet\Services\s23gpuprobe\Parameters\StageMask (REG_DWORD)
 *   Safe default if missing: StageMask = 0x01 (read-only telemetry ONLY!)
 *
 * StageMask bits:
 *   0x01 : Stage 1 (Safe Read-Only Telemetry - DEFAULT):
 *          - CMD_DB (0x81C60000) lookup & AUX VLVL dump for cx/mx/mxc/gfx/gx/gmxc/mmcx/xo.lvl
 *          - ChipInfo (0x81CF4000) header read
 *          - GCC (0x00100000) GPLL0 + GCC_GPU_* CBCR/BCR dump
 *          - Strictly gated GPUCC (0x03D90000) dump (ONLY if GCC_GPU_BCR==0 & CFG_AHB active)
 *          - Strictly AXI-gated read-only GMU/KGSL probe (NEVER touches 0x03D00000/0x03D6A000
 *            unless CX_GDSCR PWR_ON==1 & AHB ready, and GX_GDSCR PWR_ON==1 for GX regs!)
 *   0x02 : Stage 2 (Opt-In Clock/Power Bring-Up & Live Adreno 740 ID/Status Read):
 *          - Direct APPS_RSC DRV2 TCS0 (0x17A20000) RPMh ARC votes (cx, mx, mxc, gfx)
 *          - GCC_GPU_CFG_AHB + GPLL0 vote + GPUCC_CX_GDSC + SMMU/MEMNOC/DVM/DDRSS clocks
 *          - GPUCC HUB (200 MHz), FF (200 MHz), AHB/CRC_AHB/HUB_AON/HUB_CX_INT/CX_FF
 *          - GPU_CC_PLL0 (0x03D90000) & GPU_CC_PLL1 (0x03D91000, 440 MHz) Lucid OLE lock
 *          - GPUCC GMU RCG (220 MHz) + CXO_AON/CXO/CX_GMU + GX_GDSC + GX_GFX3D/GX_GMU/GX_FF
 *          - Re-verifies CX & GX AXI gates, then reads live GMU_CX, GMU_AO, RBBM_STATUS,
 *            RBBM_STATUS3, GBIF, CP_SQE_CNTL, SECVID/TSB registers
 *   0x04 : Stage 3 (Opt-In Non-Destructive Scratch R/W & GX Power-Collapse Recovery Test):
 *          - CX scratch R/W/restore on GEN7_GMU_GENERAL_0 (0x03D7E714) while CM3_SYSRESET==1
 *          - GX scratch R/W/restore on GEN7_RBBM_SP_HYST_CNT (0x03D00108) &
 *            GEN7_RBBM_RAC_THRESHOLD_CNT (0x03D00110) while CP_SQE_CNTL==0
 *          - GX GDSC SW_COLLAPSE=1 -> CFG[15]==1 -> SW_COLLAPSE=0 -> GDSCR[31]==1 recovery test
 *   0x08 : Stage 4 (Read-Only GMU Cold-Preflight Inspection when CX Gate is OPEN):
 *          - GMU ITCM (0x03D6D000, 16 KB) & DTCM (0x03D71000, 16 KB + G190_DTCM_VERSION 0x03D74FE0)
 *          - GPU CX_MISC (0x03D9E000, TCM_RET_CNTL 0x03D9E0E4) & GMU cache/HFI/RSCC bridge regs
 *          - G482_RSCC (0x03D50000: STATUS0, PDC_SEQ, SEQ_BUSY, SEQ_MEM_0..4, TCS0..3_STATUS)
 *          - GMU_PDC (0x0B280000: ENABLE_PDC, MATCH_LO/HI, SEQ_START_ADDR, STATUS_1024)
 *          - GPUCC GX / ACD / MEMNOC / MND1X / SLEEP / DEMET detailed clock & BCR state
 */

#include <stdint.h>
#include <stddef.h>

typedef int32_t  NTSTATUS;
typedef uint16_t WCHAR;
typedef uint32_t ULONG;
typedef uint8_t  BOOLEAN;
typedef void    *PVOID;
typedef void    *HANDLE;

#define STATUS_SUCCESS               ((NTSTATUS)0x00000000L)
#define STATUS_UNSUCCESSFUL          ((NTSTATUS)0xC0000001L)
#define NT_SUCCESS(st)               (((NTSTATUS)(st)) >= 0)

#define OBJ_CASE_INSENSITIVE         0x00000040UL
#define OBJ_KERNEL_HANDLE            0x00000200UL
#define FILE_GENERIC_WRITE           0x00120116UL
#define FILE_SHARE_READ              0x00000001UL
#define FILE_SHARE_WRITE             0x00000002UL
#define FILE_OPEN_IF                 0x00000003UL
#define FILE_WRITE_THROUGH           0x00000002UL
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020UL
#define FILE_ATTRIBUTE_NORMAL        0x00000080UL
#define KEY_READ                     0x00020019UL
#define KeyValuePartialInformation   2U
#define MmNonCached                  0U

typedef union _LARGE_INTEGER {
    struct { uint32_t LowPart; int32_t HighPart; };
    int64_t QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER, PHYSICAL_ADDRESS;

typedef struct _UNICODE_STRING {
    uint16_t Length;
    uint16_t MaximumLength;
    WCHAR   *Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

typedef struct _OBJECT_ATTRIBUTES {
    uint32_t        Length;
    uint32_t        Pad0;
    HANDLE          RootDirectory;
    PUNICODE_STRING ObjectName;
    uint32_t        Attributes;
    uint32_t        Pad1;
    PVOID           SecurityDescriptor;
    PVOID           SecurityQualityOfService;
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;

typedef struct _IO_STATUS_BLOCK {
    union { NTSTATUS Status; PVOID Pointer; };
    uintptr_t Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

typedef struct _KEY_VALUE_PARTIAL_INFORMATION {
    uint32_t TitleIndex;
    uint32_t Type;
    uint32_t DataLength;
    uint8_t  Data[16];
} KEY_VALUE_PARTIAL_INFORMATION;

struct _DRIVER_OBJECT;
typedef void (*PDRIVER_UNLOAD)(struct _DRIVER_OBJECT *DriverObject);

typedef struct _DRIVER_OBJECT {
    int16_t         Type;
    int16_t         Size;
    uint32_t        Pad0;
    PVOID           DeviceObject;
    uint32_t        Flags;
    uint32_t        Pad1;
    PVOID           DriverStart;
    uint32_t        DriverSize;
    uint32_t        Pad2;
    PVOID           DriverSection;
    PVOID           DriverExtension;
    UNICODE_STRING  DriverName;
    PUNICODE_STRING HardwareDatabase;
    PVOID           FastIoDispatch;
    PVOID           DriverInit;
    PVOID           DriverStartIo;
    PDRIVER_UNLOAD  DriverUnload;
    PVOID           MajorFunction[28];
} DRIVER_OBJECT, *PDRIVER_OBJECT;

__declspec(dllimport) PVOID    MmMapIoSpace(PHYSICAL_ADDRESS PhysicalAddress, size_t NumberOfBytes, uint32_t CacheType);
__declspec(dllimport) void     MmUnmapIoSpace(PVOID BaseAddress, size_t NumberOfBytes);
__declspec(dllimport) void     KeStallExecutionProcessor(uint32_t MicroSeconds);
__declspec(dllimport) NTSTATUS KeDelayExecutionThread(int8_t WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Interval);
__declspec(dllimport) void     RtlInitUnicodeString(PUNICODE_STRING DestinationString, const WCHAR *SourceString);
__declspec(dllimport) NTSTATUS ZwCreateFile(
    HANDLE *FileHandle, uint32_t DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
    PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER AllocationSize, uint32_t FileAttributes,
    uint32_t ShareAccess, uint32_t CreateDisposition, uint32_t CreateOptions,
    PVOID EaBuffer, uint32_t EaLength);
__declspec(dllimport) NTSTATUS ZwReadFile(
    HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, uint32_t Length,
    PLARGE_INTEGER ByteOffset, uint32_t *Key);
__declspec(dllimport) NTSTATUS ZwWriteFile(
    HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock, const void *Buffer, uint32_t Length,
    PLARGE_INTEGER ByteOffset, uint32_t *Key);
__declspec(dllimport) NTSTATUS ZwOpenKey(HANDLE *KeyHandle, uint32_t DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes);
__declspec(dllimport) NTSTATUS ZwQueryValueKey(
    HANDLE KeyHandle, PUNICODE_STRING ValueName, uint32_t KeyValueInformationClass,
    PVOID KeyValueInformation, uint32_t Length, uint32_t *ResultLength);
__declspec(dllimport) NTSTATUS ZwClose(HANDLE Handle);
__declspec(dllimport) PVOID    MmAllocateContiguousMemorySpecifyCache(
    size_t NumberOfBytes,
    PHYSICAL_ADDRESS LowestAcceptableAddress,
    PHYSICAL_ADDRESS HighestAcceptableAddress,
    PHYSICAL_ADDRESS BoundaryAddressMultiple,
    uint32_t CacheType);
__declspec(dllimport) void     MmFreeContiguousMemorySpecifyCache(
    PVOID BaseAddress, size_t NumberOfBytes, uint32_t CacheType);
__declspec(dllimport) PHYSICAL_ADDRESS MmGetPhysicalAddress(PVOID BaseAddress);

#ifndef FILE_GENERIC_READ
#define FILE_GENERIC_READ 0x00120089U
#endif
#ifndef FILE_OPEN
#define FILE_OPEN 0x00000001U
#endif
#ifndef STATUS_END_OF_FILE
#define STATUS_END_OF_FILE ((NTSTATUS)0xC0000011L)
#endif

/* ============================================================================
 * Physical Bases & Verified Register Offsets (S23 dm1q / SM8550-AC)
 * ============================================================================ */
#define PA_GCC            0x00100000ULL
#define SZ_GCC            0x001F5000UL

#define PA_KGSL           0x03D00000ULL
#define SZ_KGSL           0x00040000UL

#define PA_RSCC           0x03D50000ULL  /* G482_RSCC / s23_live.dts reg-names="rscc" */
#define SZ_RSCC           0x00010000UL

#define PA_GMU_CORE       0x03D6A000ULL  /* KGSL + 0x1A800*4; covers ITCM/DTCM/CM3/HFI/GENERAL/GMU_AO */
#define SZ_GMU_CORE       0x00026000UL   /* 0x03D6A000..0x03D8FFFF */

#define PA_GPUCC          0x03D90000ULL
#define SZ_GPUCC          0x0000A000UL

#define PA_CX_MISC        0x03D9E000ULL  /* G482_GMU_HIGH+0x4000 / s23_live.dts reg-names="cx_misc" */
#define SZ_CX_MISC        0x00001000UL

#define PA_KGSL_SMMU      0x03DA0000ULL  /* s23_live.dts kgsl-smmu@3da0000 */
#define SZ_KGSL_SMMU      0x00040000UL

#define PA_GMU_PDC        0x0B280000ULL  /* G482_PDC / s23_live.dts reg-names="gmu_pdc" */
#define SZ_GMU_PDC        0x00010000UL

#define PA_APPS_RSC_DRV2  0x17A20000ULL
#define SZ_APPS_RSC_DRV2  0x00002000UL

#define PA_CMD_DB         0x81C60000ULL
#define SZ_CMD_DB         0x00020000UL

#define PA_CHIPINFO       0x81CF4000ULL
#define SZ_CHIPINFO       0x00001000UL

/* GCC offsets (gcc-kalama.c) */
#define GCC_GPLL0_MODE             0x00000U
#define GCC_GPLL0_L_VAL            0x00004U
#define GCC_GPLL0_USER_CTL         0x00010U
#define GCC_APCS_CLK_BR_ENA_VOTE   0x52000U  /* BIT(15) = gcc_gpu_gpll0_clk_src */
#define GCC_APCS_GPLL_ENA_VOTE     0x52018U  /* BIT(0)  = GPLL0 */
#define GCC_GPU_BCR                0x71000U
#define GCC_GPU_CFG_AHB_CBCR       0x71004U
#define GCC_QMIP_GPU_AHB_CBCR      0x71008U
#define GCC_GPU_MEMNOC_GFX_CBCR    0x71010U
#define GCC_GPU_SNOC_DVM_GFX_CBCR  0x71018U
#define GCC_DDRSS_GPU_AXI_CBCR     0x71154U

/* GPUCC offsets (gpucc-kalama.c + s23_live.dts) */
#define GPUCC_PLL0_BASE            0x0000U
#define GPUCC_PLL1_BASE            0x1000U
#define PLL_OFF_MODE               0x00U
#define PLL_OFF_OPMODE             0x04U
#define PLL_OFF_STATE              0x08U
#define PLL_OFF_STATUS             0x0CU
#define PLL_OFF_L_VAL              0x10U
#define PLL_OFF_ALPHA_VAL          0x14U
#define PLL_OFF_USER_CTL           0x18U
#define PLL_OFF_USER_CTL_U         0x1CU
#define PLL_OFF_CONFIG_CTL         0x20U
#define PLL_OFF_CONFIG_CTL_U       0x24U
#define PLL_OFF_CONFIG_CTL_U1      0x28U
#define PLL_OFF_TEST_CTL           0x2CU
#define PLL_OFF_TEST_CTL_U         0x30U
#define PLL_OFF_TEST_CTL_U1        0x34U
#define PLL_OFF_TEST_CTL_U2        0x38U

#define GPUCC_HLOS1_VOTE_SMMU_CBCR 0x7000U
#define GPUCC_XO_BCR               0x9000U
#define GPUCC_CXO_AON_CBCR         0x9004U
#define GPUCC_FREQ_MEAS_CBCR       0x9008U
#define GPUCC_DEMET_CBCR           0x900CU
#define GPUCC_GX_BCR               0x9058U
#define GPUCC_GX_GDSCR             0x905CU
#define GPUCC_GX_CFG_GDSCR         0x9060U
#define GPUCC_GX_GFX3D_CBCR        0x90A8U
#define GPUCC_GX_VSENSE_CBCR       0x90B0U
#define GPUCC_GX_GMU_CBCR          0x90BCU
#define GPUCC_GX_FF_CBCR           0x90C0U
#define GPUCC_GX_GFX3D_RDVM_CBCR   0x90C8U
#define GPUCC_CX_BCR               0x9104U
#define GPUCC_CX_GDSCR             0x9108U
#define GPUCC_AHB_CBCR             0x911CU
#define GPUCC_CRC_AHB_CBCR         0x9120U
#define GPUCC_SLEEP_CBCR           0x9134U
#define GPUCC_CX_GMU_CBCR          0x913CU
#define GPUCC_CXO_CBCR             0x9144U
#define GPUCC_HUB_CX_INT_CBCR      0x9148U
#define GPUCC_CX_FF_CBCR           0x914CU
#define GPUCC_MEMNOC_GFX_CBCR      0x9150U
#define GPUCC_GFX3D_AON_BCR        0x9198U
#define GPUCC_MND1X_0_CBCR         0x9288U
#define GPUCC_MND1X_1_CBCR         0x928CU
#define GPUCC_GMU_BCR              0x9314U
#define GPUCC_GMU_RCG_CMD          0x9318U
#define GPUCC_GMU_RCG_CFG          0x931CU
#define GPUCC_ACD_BCR              0x9358U
#define GPUCC_HUB_AON_CBCR         0x93E8U
#define GPUCC_HUB_RCG_CMD          0x93ECU
#define GPUCC_HUB_RCG_CFG          0x93F0U
#define GPUCC_FF_RCG_CMD           0x9474U
#define GPUCC_FF_RCG_CFG           0x9478U
#define GPUCC_GX_DOMAIN_MISC3      0x9504U
#define GPUCC_CX_GDSC_HW_CTRL      0x953CU

/* GMU_CORE (0x03D6A000) byte offsets = (gen7_dword - 0x1A800) * 4 */
#define GMU_ITCM_OFF               0x03000U  /* 0x03D6D000 (0x1B400), 16 KB */
#define GMU_DTCM_OFF               0x07000U  /* 0x03D71000 (0x1C400), 16 KB */
#define GMU_DTCM_VERSION_OFF       0x0AFE0U  /* 0x03D74FE0 (G190_DTCM_VERSION) */
#define GMU_ICACHE_CONFIG          0x13000U  /* 0x03D7D000 */
#define GMU_DCACHE_CONFIG          0x13004U  /* 0x03D7D004 */
#define GMU_SYS_BUS_CONFIG         0x1303CU  /* 0x03D7D03C */
#define GMU_CM3_SYSRESET           0x14000U  /* 0x03D7E000 (0x1F800) */
#define GMU_CM3_BOOT_CONFIG        0x14004U  /* 0x03D7E004 (0x1F801) */
#define GMU_CM3_FW_BUSY            0x14068U  /* 0x03D7E068 (0x1F81A) */
#define GMU_CM3_FW_INIT_RESULT     0x14070U  /* 0x03D7E070 (0x1F81C) */
#define GMU_CM3_CFG                0x140B4U  /* 0x03D7E0B4 (0x1F82D) */
#define GMU_ALWAYS_ON_CNT_L        0x14220U  /* 0x03D7E220 (0x1F888) */
#define GMU_ALWAYS_ON_CNT_H        0x14224U  /* 0x03D7E224 (0x1F889) */
#define GMU_GFX_PWR_CLK_STATUS     0x14340U  /* 0x03D7E340 (0x1F8D0) */
#define GMU_RPMH_POWER_STATE       0x143B0U  /* 0x03D7E3B0 (0x1F8EC) */
#define GMU_RPMH_HYST_CTRL         0x143C0U  /* 0x03D7E3C0 */
#define GMU_RPMH_CTRL              0x143C4U  /* 0x03D7E3C4 */
#define GMU_HFI_CTRL_STATUS        0x14600U  /* 0x03D7E600 (0x1F980) */
#define GMU_HFI_QTBL_INFO          0x14610U  /* 0x03D7E610 (0x1F984) */
#define GMU_HFI_QTBL_ADDR          0x14614U  /* 0x03D7E614 (0x1F985) */
#define GMU_HFI_INIT               0x14618U  /* 0x03D7E618 (0x1F986) */
#define GMU_G2H_INTR_CLEAR         0x14644U  /* 0x03D7E644 (0x1F991) */
#define GMU_G2H_INTR_INFO          0x14648U  /* 0x03D7E648 (0x1F992) */
#define GMU_G2H_INTR_MASK          0x1464CU  /* 0x03D7E64C (0x1F993) */
#define GMU_H2G_SET                0x14650U  /* 0x03D7E650 (0x1F994) */
#define GMU_GENERAL_0              0x14714U  /* 0x03D7E714 (0x1F9C5) */
#define GMU_GENERAL_1              0x14718U  /* 0x03D7E718 (0x1F9C6) */
#define GMU_GENERAL_8              0x14734U  /* 0x03D7E734 (log IOVA) */
#define GMU_GENERAL_9              0x14738U  /* 0x03D7E738 */
#define GMU_GENERAL_10             0x1473CU  /* 0x03D7E73C (0x1F9CF) */
#define GMU_AO_HOST_INTR_CLR       0x24C10U  /* 0x03D8EC10 (0x23B04) */
#define GMU_AO_HOST_INTR_STATUS    0x24C14U  /* 0x03D8EC14 (0x23B05) */
#define GMU_AO_HOST_INTR_MASK      0x24C18U  /* 0x03D8EC18 (0x23B06) */
#define GMU_RSCC_CONTROL_REQ       0x24C1CU  /* 0x03D8EC1C (0x23B07) */
#define GMU_RSCC_CONTROL_ACK       0x24C20U  /* 0x03D8EC20 (0x23B08) */
#define GMU_AO_GPU_CX_BUSY_STATUS  0x24C30U  /* 0x03D8EC30 (0x23B0C) */
#define GMU_AO_GPU_CX_BUSY_STATUS2 0x24C34U  /* 0x03D8EC34 (0x23B0D) */
#define GMU_AO_AHB_FENCE_CTRL      0x24C40U  /* 0x03D8EC40 (0x23B10) */
#define GMU_AO_AHB_FENCE_RANGE_0   0x24C44U  /* 0x03D8EC44 (0x23B11) */
#define GMU_AO_AHB_FENCE_STATUS    0x24C4CU  /* 0x03D8EC4C (0x23B13) */
#define GMU_AO_SPARE_CNTL          0x24C58U  /* 0x03D8EC58 (0x23B16) */

/* CX_MISC (0x03D9E000), G482_RSCC (0x03D50000), GMU_PDC (0x0B280000) */
#define CX_MISC_TCM_RET_CNTL       0x00E4U   /* 0x03D9E0E4 (GEN7_GPU_CX_MISC_TCM_RET_CNTL) */

#define RSCC_RSC_STATUS0_DRV0      0x0010U   /* 0x03D50010 */
#define RSCC_PDC_SEQ_START_ADDR    0x0020U   /* 0x03D50020 */
#define RSCC_PDC_MATCH_VALUE_LO    0x0024U   /* 0x03D50024 */
#define RSCC_PDC_MATCH_VALUE_HI    0x0028U   /* 0x03D50028 */
#define RSCC_PDC_SLAVE_ID_DRV0     0x002CU   /* 0x03D5002C */
#define RSCC_OVERRIDE_START_ADDR   0x0400U   /* 0x03D50400 */
#define RSCC_SEQ_BUSY_DRV0         0x0404U   /* 0x03D50404 */
#define RSCC_SEQ_MEM_0_DRV0        0x0550U   /* 0x03D50550..0x03D50560 (GEN7_2_0_RSCC_SEQ_MEM_0_DRV0) */
#define RSCC_TCS0_DRV0_STATUS      0x0D28U   /* 0x03D50D28 */
#define RSCC_TCS1_DRV0_STATUS      0x0FC8U   /* 0x03D50FC8 */
#define RSCC_TCS2_DRV0_STATUS      0x1268U   /* 0x03D51268 */
#define RSCC_TCS3_DRV0_STATUS      0x1508U   /* 0x03D51508 */

#define PDC_GPU_STATUS_1024        0x1024U   /* 0x0B281024 */
#define PDC_GPU_ENABLE_PDC         0x4500U   /* 0x0B284500 */
#define PDC_GPU_MATCH_VALUE_LO     0x4510U   /* 0x0B284510 */
#define PDC_GPU_MATCH_VALUE_HI     0x4514U   /* 0x0B284514 */
#define PDC_GPU_SEQ_START_ADDR     0x4520U   /* 0x0B284520 */

/* KGSL (0x03D00000) byte offsets = gen7_dword * 4 (GX-domain) */
#define KGSL_RBBM_GBIF_HALT        (0x00016U * 4U)  /* 0x00058 */
#define KGSL_RBBM_GBIF_HALT_ACK    (0x00017U * 4U)  /* 0x0005C */
#define KGSL_RBBM_SP_HYST_CNT      (0x00042U * 4U)  /* 0x00108 (safe R/W scratch when idle) */
#define KGSL_RBBM_RAC_THRESH_CNT   (0x00044U * 4U)  /* 0x00110 (safe R/W scratch when idle) */
#define KGSL_RBBM_CLOCK_CNTL       (0x000AEU * 4U)  /* 0x002B8 */
#define KGSL_RBBM_INT_0_STATUS     (0x00201U * 4U)  /* 0x00804 */
#define KGSL_RBBM_STATUS           (0x00210U * 4U)  /* 0x00840 */
#define KGSL_RBBM_STATUS3          (0x00213U * 4U)  /* 0x0084C */
#define KGSL_CP_RB_RPTR            (0x00806U * 4U)  /* 0x02018 */
#define KGSL_CP_RB_WPTR            (0x00807U * 4U)  /* 0x0201C */
#define KGSL_CP_SQE_CNTL           (0x00808U * 4U)  /* 0x02020 */
#define KGSL_GBIF_SCACHE_CNTL0     (0x03C01U * 4U)  /* 0x0F004 */
#define KGSL_GBIF_HALT             (0x03C45U * 4U)  /* 0x0F114 */
#define KGSL_GBIF_HALT_ACK         (0x03C46U * 4U)  /* 0x0F118 */
#define KGSL_SECVID_TRUST_CNTL     (0x0F400U * 4U)  /* 0x3D000 */
#define KGSL_SECVID_TSB_CNTL       (0x0F803U * 4U)  /* 0x3E00C */

/* ============================================================================
 * Helpers: MMIO, Barriers, Timing
 * ============================================================================ */
static inline void InitObjAttr(POBJECT_ATTRIBUTES oa, PUNICODE_STRING name, uint32_t attr) {
    oa->Length = sizeof(OBJECT_ATTRIBUTES);
    oa->Pad0 = 0;
    oa->RootDirectory = NULL;
    oa->ObjectName = name;
    oa->Attributes = attr;
    oa->Pad1 = 0;
    oa->SecurityDescriptor = NULL;
    oa->SecurityQualityOfService = NULL;
}

static inline void dmb_sy(void) {
    __asm__ volatile("dmb sy" ::: "memory");
}

static inline uint32_t mmio_r32(volatile uint8_t *base, uint32_t off) {
    dmb_sy();
    uint32_t v = *(volatile uint32_t *)(base + off);
    dmb_sy();
    return v;
}

static inline void mmio_w32(volatile uint8_t *base, uint32_t off, uint32_t v) {
    dmb_sy();
    *(volatile uint32_t *)(base + off) = v;
    dmb_sy();
}

static void SleepMs(uint32_t ms) {
    if (ms == 0) return;
    LARGE_INTEGER li;
    li.QuadPart = -((int64_t)ms * 10000LL);
    KeDelayExecutionThread(0, 0, &li);
}

static void StallUs(uint32_t us) {
    while (us > 1000U) {
        KeStallExecutionProcessor(1000U);
        us -= 1000U;
    }
    if (us > 0U) {
        KeStallExecutionProcessor(us);
    }
}

/* ============================================================================
 * Pre-Allocated 64 KB Write-Through Log (C:\Users\Public\s23gpuprobe.log)
 * ============================================================================ */
static HANDLE   g_log_handle = NULL;
static int64_t  g_log_off    = 0;
static char     g_log_buf[4096];
static uint32_t g_log_pos    = 0;

static void LogOpen(void) {
    UNICODE_STRING u;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    RtlInitUnicodeString(&u, L"\\??\\C:\\Users\\Public\\s23gpuprobe.log");
    InitObjAttr(&oa, &u, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE);
    g_log_off = 0;
    g_log_pos = 0;
    if (!NT_SUCCESS(ZwCreateFile(
            &g_log_handle, FILE_GENERIC_WRITE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
            FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF,
            FILE_WRITE_THROUGH | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0))) {
        g_log_handle = NULL;
    }
}

static void LogFlush(void) {
    if (!g_log_handle || g_log_pos == 0) {
        g_log_pos = 0;
        return;
    }
    if (g_log_off + (int64_t)g_log_pos > 65000LL) {
        g_log_pos = 0;
        return;
    }
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER off;
    off.QuadPart = g_log_off;
    ZwWriteFile(g_log_handle, NULL, NULL, NULL, &iosb, g_log_buf, g_log_pos, &off, NULL);
    g_log_off += (int64_t)g_log_pos;
    g_log_pos = 0;
}

static void LogClose(void) {
    LogFlush();
    if (g_log_handle) {
        ZwClose(g_log_handle);
        g_log_handle = NULL;
    }
}

static void LogC(char c) {
    if (g_log_pos >= sizeof(g_log_buf) - 16U) {
        LogFlush();
    }
    if (g_log_pos < sizeof(g_log_buf)) {
        g_log_buf[g_log_pos++] = c;
    }
}

static void LogS(const char *s) {
    while (*s) LogC(*s++);
}

static void LogHex(uint64_t v, int digits) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = (digits - 1) * 4; i >= 0; i -= 4) {
        LogC(hex[(v >> i) & 0xFU]);
    }
}

static void LogDec(int64_t v) {
    if (v < 0) {
        LogC('-');
        v = -v;
    }
    char tmp[24];
    int n = 0;
    uint64_t uv = (uint64_t)v;
    if (uv == 0) tmp[n++] = '0';
    while (uv > 0) {
        tmp[n++] = (char)('0' + (uv % 10U));
        uv /= 10U;
    }
    while (n > 0) LogC(tmp[--n]);
}

static void LogNL(void) {
    LogC('\r');
    LogC('\n');
    LogFlush();
}

static void LogReg(const char *name, uint32_t pa, uint32_t val) {
    LogS("  ");
    LogS(name);
    LogS(" (0x");
    LogHex(pa, 8);
    LogS(") = 0x");
    LogHex(val, 8);
    LogNL();
}

/* ============================================================================
 * Registry Reader (Safe Default StageMask = 0x01 Read-Only!)
 * ============================================================================ */
static uint32_t ReadStageMask(void) {
    UNICODE_STRING uk, uv;
    OBJECT_ATTRIBUTES oa;
    HANDLE hk = NULL;
    uint32_t val = 0x01U; /* SAFE DEFAULT: Read-Only Telemetry */

    RtlInitUnicodeString(&uk, L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\s23gpuprobe\\Parameters");
    InitObjAttr(&oa, &uk, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE);
    if (!NT_SUCCESS(ZwOpenKey(&hk, KEY_READ, &oa))) {
        return val;
    }
    RtlInitUnicodeString(&uv, L"StageMask");
    KEY_VALUE_PARTIAL_INFORMATION kv;
    uint32_t rlen = 0;
    if (NT_SUCCESS(ZwQueryValueKey(hk, &uv, KeyValuePartialInformation, &kv, sizeof(kv), &rlen))) {
        if (kv.DataLength >= 4U) {
            val = *(uint32_t *)kv.Data;
        }
    }
    ZwClose(hk);
    return val;
}

/* ============================================================================
 * CMD_DB Parser (0x81C60000) & APPS_RSC DRV2 TCS0 (0x17A20000)
 * ============================================================================ */
static volatile uint8_t *g_cmddb    = NULL;
static volatile uint8_t *g_chipinfo = NULL;
static volatile uint8_t *g_rsc      = NULL;
static volatile uint8_t *g_gcc      = NULL;
static volatile uint8_t *g_gpucc    = NULL;
static volatile uint8_t *g_gmu      = NULL;
static volatile uint8_t *g_kgsl     = NULL;

typedef struct {
    uint32_t addr;
    uint16_t slv_id;
    uint16_t aux_len;
    uint16_t vlvls[16];
    uint32_t num_vlvls;
} CMD_DB_ARC_INFO;

static int CmdDbLookupArc(const char *name, CMD_DB_ARC_INFO *out) {
    out->addr = 0;
    out->slv_id = 0;
    out->aux_len = 0;
    out->num_vlvls = 0;
    if (!g_cmddb) return 0;
    if (g_cmddb[4] != 0xDBU || g_cmddb[5] != 0x30U || g_cmddb[6] != 0x03U || g_cmddb[7] != 0x0CU) {
        return 0;
    }
    uint32_t nlen = 0;
    while (name[nlen] && nlen < 8U) nlen++;

    for (uint32_t i = 0; i < 8U; i++) {
        const volatile uint8_t *r = g_cmddb + 8U + 16U * i;
        uint16_t slv_id   = *(const volatile uint16_t *)(r + 0);
        uint16_t hdr_off  = *(const volatile uint16_t *)(r + 2);
        uint16_t data_off = *(const volatile uint16_t *)(r + 4);
        uint16_t cnt      = *(const volatile uint16_t *)(r + 6);
        if (slv_id == 0U) break;
        if (144U + (uint32_t)hdr_off + 24U * (uint32_t)cnt > SZ_CMD_DB) continue;

        const volatile uint8_t *e = g_cmddb + 144U + hdr_off;
        for (uint32_t j = 0; j < cnt; j++, e += 24U) {
            int match = 1;
            for (uint32_t k = 0; k < 8U; k++) {
                uint8_t ch = (k < nlen) ? (uint8_t)name[k] : 0U;
                if (e[k] != ch) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                out->addr    = *(const volatile uint32_t *)(e + 16);
                out->aux_len = *(const volatile uint16_t *)(e + 20);
                uint16_t aoff = *(const volatile uint16_t *)(e + 22);
                out->slv_id  = slv_id;

                uint32_t abs_off = 144U + (uint32_t)data_off + (uint32_t)aoff;
                if (out->aux_len >= 2U && abs_off + out->aux_len <= SZ_CMD_DB) {
                    uint32_t max_u16 = out->aux_len >> 1;
                    if (max_u16 > 16U) max_u16 = 16U;
                    const volatile uint16_t *aux = (const volatile uint16_t *)(g_cmddb + abs_off);
                    uint32_t n = 0;
                    for (uint32_t idx = 0; idx < max_u16; idx++) {
                        uint16_t lv = aux[idx];
                        out->vlvls[idx] = lv;
                        if (idx > 0U && out->vlvls[idx - 1U] != 0U && lv == 0U) {
                            break;
                        }
                        n = idx + 1U;
                    }
                    out->num_vlvls = n;
                }
                return 1;
            }
        }
    }
    return 0;
}

static int FindArcLevelIndex(const CMD_DB_ARC_INFO *arc, uint16_t min_vlvl, uint32_t *idx_out, uint16_t *vlvl_out) {
    if (!arc || arc->addr == 0U || arc->num_vlvls == 0U) return 0;
    for (uint32_t i = 0; i < arc->num_vlvls; i++) {
        if (arc->vlvls[i] >= min_vlvl) {
            *idx_out = i;
            *vlvl_out = arc->vlvls[i];
            return 1;
        }
    }
    /* Fallback to highest available non-zero level */
    for (int i = (int)arc->num_vlvls - 1; i >= 0; i--) {
        if (arc->vlvls[i] != 0U) {
            *idx_out = (uint32_t)i;
            *vlvl_out = arc->vlvls[i];
            return 1;
        }
    }
    return 0;
}

#define RSC_TCS_OFF  0xD00U
#define V3_IRQ_EN    0x00U
#define V3_IRQ_ST    0x04U
#define V3_IRQ_CLR   0x08U
#define V3_CTRL      0x24U
#define V3_STATUS    0x28U
#define V3_CMDEN     0x2CU
#define V3_MSGID     0x34U
#define V3_ADDR      0x38U
#define V3_DATA      0x3CU
#define V3_CMDST     0x40U
#define AMC_ENABLE   (1U << 16)
#define AMC_TRIGGER  (1U << 24)

static inline uint32_t TcsOff(uint32_t id, uint32_t reg) {
    return RSC_TCS_OFF + 672U * id + reg;
}

static inline uint32_t TcsCmdOff(uint32_t id, uint32_t cmd, uint32_t reg) {
    return TcsOff(id, reg) + 24U * cmd;
}

/* Hybrid PASSIVE_LEVEL poll: fast 1 us stalls up to 200 us, then 1 ms sleeps up to max_ms */
static int WaitTcsBitHybrid(uint32_t reg_off, uint32_t mask, uint32_t want, uint32_t max_ms, uint32_t *waited_us_out) {
    for (uint32_t us = 0U; us < 200U; us++) {
        if ((mmio_r32(g_rsc, reg_off) & mask) == want) {
            if (waited_us_out) *waited_us_out = us;
            return 1;
        }
        KeStallExecutionProcessor(1U);
    }
    for (uint32_t ms = 0U; ms < max_ms; ms++) {
        SleepMs(1U);
        if ((mmio_r32(g_rsc, reg_off) & mask) == want) {
            if (waited_us_out) *waited_us_out = 200U + (ms + 1U) * 1000U;
            return 1;
        }
    }
    if (waited_us_out) *waited_us_out = 200U + max_ms * 1000U;
    return (mmio_r32(g_rsc, reg_off) & mask) == want;
}

static int RpmhWriteTcs0(const char *label, uint32_t addr, uint32_t data) {
    if (!g_rsc || addr == 0U) return 0;
    const uint32_t id = 0U;

    /* 1. Wait for TCS0_STATUS (+0x28 bit 0 == 1) idle before touching TCS0 */
    if (!WaitTcsBitHybrid(TcsOff(id, V3_STATUS), 1U, 1U, 10U, NULL)) {
        LogS("  RPMh["); LogS(label); LogS(" @ 0x"); LogHex(addr, 5);
        LogS("] ABORT: TCS0 busy (STATUS=0x"); LogHex(mmio_r32(g_rsc, TcsOff(id, V3_STATUS)), 8);
        LogS(")"); LogNL();
        return 0;
    }

    /* 2. Save IRQ_EN, CMDEN, and slot 0..3 command registers */
    uint32_t sv_irqen = mmio_r32(g_rsc, RSC_TCS_OFF + V3_IRQ_EN);
    uint32_t sv_cmden = mmio_r32(g_rsc, TcsOff(id, V3_CMDEN));
    uint32_t sv_msg[4], sv_addr[4], sv_data[4];
    for (uint32_t i = 0; i < 4U; i++) {
        sv_msg[i]  = mmio_r32(g_rsc, TcsCmdOff(id, i, V3_MSGID));
        sv_addr[i] = mmio_r32(g_rsc, TcsCmdOff(id, i, V3_ADDR));
        sv_data[i] = mmio_r32(g_rsc, TcsCmdOff(id, i, V3_DATA));
    }

    mmio_w32(g_rsc, RSC_TCS_OFF + V3_IRQ_CLR, 1U << id);
    if ((sv_irqen & (1U << id)) == 0U) {
        mmio_w32(g_rsc, RSC_TCS_OFF + V3_IRQ_EN, sv_irqen | (1U << id));
    }

    mmio_w32(g_rsc, TcsOff(id, V3_CMDEN), 0U);
    mmio_w32(g_rsc, TcsCmdOff(id, 0U, V3_MSGID), 8U | 0x100U | 0x10000U);
    mmio_w32(g_rsc, TcsCmdOff(id, 0U, V3_ADDR), addr);
    mmio_w32(g_rsc, TcsCmdOff(id, 0U, V3_DATA), data);
    mmio_w32(g_rsc, TcsOff(id, V3_CMDEN), 1U);

    uint32_t en = mmio_r32(g_rsc, TcsOff(id, V3_CTRL));
    en &= ~AMC_TRIGGER; mmio_w32(g_rsc, TcsOff(id, V3_CTRL), en); (void)mmio_r32(g_rsc, TcsOff(id, V3_CTRL));
    en &= ~AMC_ENABLE;  mmio_w32(g_rsc, TcsOff(id, V3_CTRL), en); (void)mmio_r32(g_rsc, TcsOff(id, V3_CTRL));
    en |= AMC_ENABLE;   mmio_w32(g_rsc, TcsOff(id, V3_CTRL), en); (void)mmio_r32(g_rsc, TcsOff(id, V3_CTRL));
    en |= AMC_TRIGGER;  mmio_w32(g_rsc, TcsOff(id, V3_CTRL), en);

    uint32_t wait_us = 0U;
    int irq_ok = WaitTcsBitHybrid(RSC_TCS_OFF + V3_IRQ_ST, 1U << id, 1U << id, 100U, &wait_us);
    uint32_t st    = mmio_r32(g_rsc, RSC_TCS_OFF + V3_IRQ_ST);
    uint32_t cmdst = mmio_r32(g_rsc, TcsCmdOff(id, 0U, V3_CMDST));
    int idle_ok    = WaitTcsBitHybrid(TcsOff(id, V3_STATUS), 1U, 1U, 10U, NULL);

    /* 3. Restore sv_msg/sv_addr/sv_data BEFORE restoring V3_CMDEN, only when TCS0 is idle */
    if (idle_ok) {
        mmio_w32(g_rsc, TcsOff(id, V3_CMDEN), 0U);
        for (uint32_t i = 0; i < 4U; i++) {
            mmio_w32(g_rsc, TcsCmdOff(id, i, V3_MSGID), sv_msg[i]);
            mmio_w32(g_rsc, TcsCmdOff(id, i, V3_ADDR),  sv_addr[i]);
            mmio_w32(g_rsc, TcsCmdOff(id, i, V3_DATA),  sv_data[i]);
        }
        mmio_w32(g_rsc, TcsOff(id, V3_CMDEN), sv_cmden);
    }
    mmio_w32(g_rsc, RSC_TCS_OFF + V3_IRQ_CLR, 1U << id);
    if (mmio_r32(g_rsc, RSC_TCS_OFF + V3_IRQ_EN) != sv_irqen) {
        mmio_w32(g_rsc, RSC_TCS_OFF + V3_IRQ_EN, sv_irqen);
    }

    LogS("  RPMh["); LogS(label); LogS(" @ 0x"); LogHex(addr, 5);
    LogS("] <- 0x"); LogHex(data, 4);
    LogS(" : irq=0x"); LogHex(st, 2);
    LogS(" cmdst=0x"); LogHex(cmdst, 2);
    LogS(" idle="); LogDec(idle_ok);
    LogS(" ("); LogDec(wait_us); LogS(" us)"); LogNL();
    return irq_ok && idle_ok;
}

/* ============================================================================
 * Strict 3-Tier AXI Safety Gates (Prevents Synchronous External Abort / SError)
 * ============================================================================ */
static int GpuccGateOpen(void) {
    if (!g_gcc) return 0;
    uint32_t bcr = mmio_r32(g_gcc, GCC_GPU_BCR);
    uint32_t cfg = mmio_r32(g_gcc, GCC_GPU_CFG_AHB_CBCR);
    if (bcr == 0xFFFFFFFFU || cfg == 0xFFFFFFFFU) return 0;
    if (bcr & 1U) return 0;
    /* Require CFG_AHB enabled AND not halted (or HWCG active with enable set) */
    if ((cfg & 1U) == 0U) return 0;
    if ((cfg & 0x80000000U) != 0U && (cfg & 2U) == 0U) return 0;
    return 1;
}

static int GpuCxAxiGateOpen(uint32_t *cx_gdscr_out, uint32_t *ahb_out) {
    if (!GpuccGateOpen() || !g_gpucc) return 0;
    uint32_t cx_bcr     = mmio_r32(g_gpucc, GPUCC_CX_BCR);
    uint32_t cx_gds     = mmio_r32(g_gpucc, GPUCC_CX_GDSCR);
    uint32_t cx_hw      = mmio_r32(g_gpucc, GPUCC_CX_GDSC_HW_CTRL);
    uint32_t ahb        = mmio_r32(g_gpucc, GPUCC_AHB_CBCR);
    uint32_t hub_aon    = mmio_r32(g_gpucc, GPUCC_HUB_AON_CBCR);
    uint32_t hub_cx_int = mmio_r32(g_gpucc, GPUCC_HUB_CX_INT_CBCR);
    uint32_t cx_ff      = mmio_r32(g_gpucc, GPUCC_CX_FF_CBCR);
    uint32_t memnoc_gfx = mmio_r32(g_gpucc, GPUCC_MEMNOC_GFX_CBCR);
    uint32_t cxo        = mmio_r32(g_gpucc, GPUCC_CXO_CBCR);
    uint32_t cx_gmu     = mmio_r32(g_gpucc, GPUCC_CX_GMU_CBCR);
    if (cx_gdscr_out) *cx_gdscr_out = cx_gds;
    if (ahb_out)      *ahb_out      = ahb;
    if (cx_bcr == 0xFFFFFFFFU || cx_gds == 0xFFFFFFFFU || ahb == 0xFFFFFFFFU) return 0;
    if (cx_bcr & 1U) return 0;
    /* Per G290GpuPower.c (G530 StockAhbRawReady & G721 Flags=64 ahb-not-sticky):
     * On SM8550, 0x03D9911C (gpu_cc_ahb_clk) has non-sticky bit 0 and is ready when
     * ((ahb >> 28) == 0 || (ahb >> 28) == 2) AND CX_GDSCR + CX_HW_CTRL + HUB/CX/GMU branches are ON. */
    int cx_on  = ((cx_gds & 0x80000001U) == 0x80000000U) &&
                 ((cx_hw != 0xFFFFFFFFU) && ((cx_hw & 0x80000000U) != 0U));
    int ahb_ok = (ahb != 0xFFFFFFFFU) && (((ahb >> 28) == 0U) || ((ahb >> 28) == 2U));
    int leaves_on = ((hub_aon    & 0x80000001U) == 1U) &&
                    ((hub_cx_int & 0x80000001U) == 1U) &&
                    ((cx_ff      & 0x80000001U) == 1U) &&
                    ((memnoc_gfx & 0x80000001U) == 1U) &&
                    ((cxo        & 0x80000001U) == 1U) &&
                    ((cx_gmu     & 0x80000001U) == 1U);
    return (cx_on && ahb_ok && leaves_on) ? 1 : 0;
}

static int GpuGxAxiGateOpen(uint32_t *gx_gdscr_out, uint32_t *gx_cfg_out) {
    if (!GpuCxAxiGateOpen(NULL, NULL)) return 0;
    uint32_t gx_bcr = mmio_r32(g_gpucc, GPUCC_GX_BCR);
    uint32_t gx_gds = mmio_r32(g_gpucc, GPUCC_GX_GDSCR);
    uint32_t gx_cfg = mmio_r32(g_gpucc, GPUCC_GX_CFG_GDSCR);
    if (gx_gdscr_out) *gx_gdscr_out = gx_gds;
    if (gx_cfg_out)   *gx_cfg_out   = gx_cfg;
    if (gx_bcr == 0xFFFFFFFFU || gx_gds == 0xFFFFFFFFU || gx_cfg == 0xFFFFFFFFU) return 0;
    if (gx_bcr & 1U) return 0;
    /* GX_GDSCR PWR_ON(bit31)==1, SW_COLLAPSE(bit0)==0, CFG PWR_DOWN_COMPLETE(bit15)==0 */
    return ((gx_gds & 0x80000001U) == 0x80000000U) && ((gx_cfg & (1U << 15)) == 0U);
}

/* ============================================================================
 * Clock / PLL / RCG / Branch Helpers (Verified vs gpucc-kalama.c & G736r2)
 * ============================================================================ */
static int PollReg(volatile uint8_t *base, uint32_t off, uint32_t mask, uint32_t expected, uint32_t timeout_us) {
    for (uint32_t t = 0; t <= timeout_us; t += 5U) {
        uint32_t v = mmio_r32(base, off);
        if (v == 0xFFFFFFFFU) return 0;
        if ((v & mask) == expected) return 1;
        StallUs(5U);
    }
    return 0;
}

static int EnableBranch(volatile uint8_t *base, uint32_t off, const char *name, int allow_hwcg, int delay_only) {
    uint32_t v = mmio_r32(base, off);
    if (v == 0xFFFFFFFFU) {
        LogS("  [ERR] "); LogS(name); LogS(" read 0xFFFFFFFF"); LogNL();
        return 0;
    }
    if ((v & 1U) == 0U) {
        mmio_w32(base, off, v | 1U);
    }
    if (delay_only) {
        StallUs(15U);
        v = mmio_r32(base, off);
        LogS("  CLK "); LogS(name); LogS(" -> 0x"); LogHex(v, 8); LogS(" (delay)"); LogNL();
        return (v != 0xFFFFFFFFU);
    }
    for (uint32_t t = 0; t <= 500U; t += 5U) {
        v = mmio_r32(base, off);
        if (v == 0xFFFFFFFFU) return 0;
        if ((v & 1U) && (((v & 0x80000000U) == 0U) || (allow_hwcg && (v & 2U)))) {
            LogS("  CLK "); LogS(name); LogS(" -> 0x"); LogHex(v, 8); LogS(" (ON)"); LogNL();
            return 1;
        }
        StallUs(5U);
    }
    LogS("  [WARN] CLK "); LogS(name); LogS(" timeout val=0x"); LogHex(v, 8); LogNL();
    return 0;
}

static int ProgramRcg(volatile uint8_t *base, uint32_t cmd_off, uint32_t cfg_val, const char *name) {
    uint32_t cmd = mmio_r32(base, cmd_off);
    if (cmd == 0xFFFFFFFFU) return 0;
    int forced = 0;
    if ((cmd & 2U) == 0U) {
        mmio_w32(base, cmd_off, cmd | 2U);
        forced = 1;
    }
    if (!PollReg(base, cmd_off, 0x80000000U, 0U, 500U)) {
        LogS("  [WARN] RCG "); LogS(name); LogS(" ROOT_OFF timeout cmd=0x");
        LogHex(mmio_r32(base, cmd_off), 8); LogNL();
        return 0;
    }
    uint32_t cfg = mmio_r32(base, cmd_off + 4U);
    uint32_t new_cfg = (cfg & ~0x0010371FU) | cfg_val;
    if (new_cfg != cfg) {
        mmio_w32(base, cmd_off + 4U, new_cfg);
    }
    cmd = mmio_r32(base, cmd_off);
    mmio_w32(base, cmd_off, cmd | 1U);
    int ok = PollReg(base, cmd_off, 1U, 0U, 500U);
    if (forced) {
        cmd = mmio_r32(base, cmd_off);
        mmio_w32(base, cmd_off, cmd & ~2U);
    }
    LogS("  RCG "); LogS(name);
    LogS(" cmd=0x"); LogHex(mmio_r32(base, cmd_off), 8);
    LogS(" cfg=0x"); LogHex(mmio_r32(base, cmd_off + 4U), 8);
    LogS(ok ? " (OK)" : " (TIMEOUT)"); LogNL();
    return ok;
}

static int ProgramLucidOlePll(volatile uint8_t *gpucc, uint32_t pll_off, uint32_t l_val, uint32_t alpha_val, const char *name) {
    uint32_t mode = mmio_r32(gpucc, pll_off + PLL_OFF_MODE);
    uint32_t op   = mmio_r32(gpucc, pll_off + PLL_OFF_OPMODE);
    uint32_t cur_l = mmio_r32(gpucc, pll_off + PLL_OFF_L_VAL);
    uint32_t cur_a = mmio_r32(gpucc, pll_off + PLL_OFF_ALPHA_VAL);
    uint32_t user  = mmio_r32(gpucc, pll_off + PLL_OFF_USER_CTL);

    if (mode != 0xFFFFFFFFU &&
        (mode & 0x80000005U) == 0x80000005U && op == 1U && (user & 1U) &&
        (cur_l & 0xFFFFU) == (l_val & 0xFFFFU) && (cur_a & 0xFFFFU) == (alpha_val & 0xFFFFU)) {
        LogS("  PLL "); LogS(name); LogS(" already locked (mode=0x");
        LogHex(mode, 8); LogS(")"); LogNL();
        return 1;
    }

    /* Lucid OLE safe sequence: clear OUTCTRL and enter STANDBY BEFORE modifying L/ALPHA/CFG/TEST */
    if (mode == 0xFFFFFFFFU) return 0;
    mmio_w32(gpucc, pll_off + PLL_OFF_MODE, mode & ~1U);  /* OUTCTRL off */
    mmio_w32(gpucc, pll_off + PLL_OFF_OPMODE, 0U);        /* STANDBY */
    StallUs(2U);

    /* Exact gpucc-kalama.c clk_lucid_ole_pll_configure + enable sequence */
    mmio_w32(gpucc, pll_off + PLL_OFF_L_VAL,         0x44440000U | (l_val & 0xFFFFU));
    mmio_w32(gpucc, pll_off + PLL_OFF_ALPHA_VAL,     alpha_val & 0xFFFFU);
    mmio_w32(gpucc, pll_off + PLL_OFF_CONFIG_CTL,    0x20485699U);
    mmio_w32(gpucc, pll_off + PLL_OFF_CONFIG_CTL_U,  0x00182261U);
    mmio_w32(gpucc, pll_off + PLL_OFF_CONFIG_CTL_U1, 0x82AA299CU);
    mmio_w32(gpucc, pll_off + PLL_OFF_USER_CTL_U,    0x00000005U);
    mmio_w32(gpucc, pll_off + PLL_OFF_TEST_CTL_U,    0x00000003U);
    mmio_w32(gpucc, pll_off + PLL_OFF_TEST_CTL_U1,   0x00009000U);
    mmio_w32(gpucc, pll_off + PLL_OFF_TEST_CTL_U2,   0x00000034U);

    mode = mmio_r32(gpucc, pll_off + PLL_OFF_MODE);
    if (mode == 0xFFFFFFFFU) return 0;
    mmio_w32(gpucc, pll_off + PLL_OFF_MODE, mode | 4U);   /* RESET_N high */
    mmio_w32(gpucc, pll_off + PLL_OFF_OPMODE, 1U);        /* RUN */

    if (!PollReg(gpucc, pll_off + PLL_OFF_MODE, 0x80000000U, 0x80000000U, 1500U)) {
        LogS("  [WARN] PLL "); LogS(name); LogS(" LOCK timeout mode=0x");
        LogHex(mmio_r32(gpucc, pll_off + PLL_OFF_MODE), 8); LogNL();
        return 0;
    }
    user = mmio_r32(gpucc, pll_off + PLL_OFF_USER_CTL);
    mmio_w32(gpucc, pll_off + PLL_OFF_USER_CTL, user | 1U);
    mode = mmio_r32(gpucc, pll_off + PLL_OFF_MODE);
    mmio_w32(gpucc, pll_off + PLL_OFF_MODE, mode | 1U);   /* OUTCTRL on */

    mode = mmio_r32(gpucc, pll_off + PLL_OFF_MODE);
    LogS("  PLL "); LogS(name); LogS(" LOCKED mode=0x"); LogHex(mode, 8);
    LogS(" op=0x"); LogHex(mmio_r32(gpucc, pll_off + PLL_OFF_OPMODE), 2);
    LogS(" user=0x"); LogHex(mmio_r32(gpucc, pll_off + PLL_OFF_USER_CTL), 8); LogNL();
    return ((mode & 0x80000005U) == 0x80000005U);
}

/* ============================================================================
 * Telemetry Dumpers
 * ============================================================================ */
static void DumpGpuccTelemetry(void) {
    LogS("--- GPUCC (0x03D90000) Telemetry ---"); LogNL();
    if (!GpuccGateOpen()) {
        LogS("  [GATE CLOSED] GCC_GPU_BCR/CFG_AHB not active; skipping 0x03D90000 read (SError guard)."); LogNL();
        return;
    }
    if (!g_gpucc) return;
    LogReg("GPUCC_PLL0_MODE",      0x03D90000U, mmio_r32(g_gpucc, GPUCC_PLL0_BASE + PLL_OFF_MODE));
    LogReg("GPUCC_PLL0_OPMODE",    0x03D90004U, mmio_r32(g_gpucc, GPUCC_PLL0_BASE + PLL_OFF_OPMODE));
    LogReg("GPUCC_PLL0_L_VAL",     0x03D90010U, mmio_r32(g_gpucc, GPUCC_PLL0_BASE + PLL_OFF_L_VAL));
    LogReg("GPUCC_PLL1_MODE",      0x03D91000U, mmio_r32(g_gpucc, GPUCC_PLL1_BASE + PLL_OFF_MODE));
    LogReg("GPUCC_PLL1_OPMODE",    0x03D91004U, mmio_r32(g_gpucc, GPUCC_PLL1_BASE + PLL_OFF_OPMODE));
    LogReg("GPUCC_PLL1_L_VAL",     0x03D91010U, mmio_r32(g_gpucc, GPUCC_PLL1_BASE + PLL_OFF_L_VAL));
    LogReg("GPUCC_HLOS1_SMMU",     0x03D97000U, mmio_r32(g_gpucc, GPUCC_HLOS1_VOTE_SMMU_CBCR));
    LogReg("GPUCC_CXO_AON",        0x03D99004U, mmio_r32(g_gpucc, GPUCC_CXO_AON_CBCR));
    LogReg("GPUCC_GX_BCR",         0x03D99058U, mmio_r32(g_gpucc, GPUCC_GX_BCR));
    LogReg("GPUCC_GX_GDSCR",       0x03D9905CU, mmio_r32(g_gpucc, GPUCC_GX_GDSCR));
    LogReg("GPUCC_GX_CFG_GDSCR",   0x03D99060U, mmio_r32(g_gpucc, GPUCC_GX_CFG_GDSCR));
    LogReg("GPUCC_GX_GFX3D_CBCR",  0x03D990A8U, mmio_r32(g_gpucc, GPUCC_GX_GFX3D_CBCR));
    LogReg("GPUCC_GX_GMU_CBCR",    0x03D990BCU, mmio_r32(g_gpucc, GPUCC_GX_GMU_CBCR));
    LogReg("GPUCC_GX_FF_CBCR",     0x03D990C0U, mmio_r32(g_gpucc, GPUCC_GX_FF_CBCR));
    LogReg("GPUCC_CX_BCR",         0x03D99104U, mmio_r32(g_gpucc, GPUCC_CX_BCR));
    LogReg("GPUCC_CX_GDSCR",       0x03D99108U, mmio_r32(g_gpucc, GPUCC_CX_GDSCR));
    LogReg("GPUCC_CX_HW_CTRL",     0x03D9953CU, mmio_r32(g_gpucc, GPUCC_CX_GDSC_HW_CTRL));
    LogReg("GPUCC_AHB_CBCR",       0x03D9911CU, mmio_r32(g_gpucc, GPUCC_AHB_CBCR));
    LogReg("GPUCC_CRC_AHB_CBCR",   0x03D99120U, mmio_r32(g_gpucc, GPUCC_CRC_AHB_CBCR));
    LogReg("GPUCC_CX_GMU_CBCR",    0x03D9913CU, mmio_r32(g_gpucc, GPUCC_CX_GMU_CBCR));
    LogReg("GPUCC_CXO_CBCR",       0x03D99144U, mmio_r32(g_gpucc, GPUCC_CXO_CBCR));
    LogReg("GPUCC_HUB_CX_INT",     0x03D99148U, mmio_r32(g_gpucc, GPUCC_HUB_CX_INT_CBCR));
    LogReg("GPUCC_CX_FF_CBCR",     0x03D9914CU, mmio_r32(g_gpucc, GPUCC_CX_FF_CBCR));
    LogReg("GPUCC_MEMNOC_GFX",     0x03D99150U, mmio_r32(g_gpucc, GPUCC_MEMNOC_GFX_CBCR));
    LogReg("GPUCC_GMU_RCG_CMD",    0x03D99318U, mmio_r32(g_gpucc, GPUCC_GMU_RCG_CMD));
    LogReg("GPUCC_GMU_RCG_CFG",    0x03D9931CU, mmio_r32(g_gpucc, GPUCC_GMU_RCG_CFG));
    LogReg("GPUCC_HUB_AON_CBCR",   0x03D993E8U, mmio_r32(g_gpucc, GPUCC_HUB_AON_CBCR));
    LogReg("GPUCC_HUB_RCG_CMD",    0x03D993ECU, mmio_r32(g_gpucc, GPUCC_HUB_RCG_CMD));
    LogReg("GPUCC_HUB_RCG_CFG",    0x03D993F0U, mmio_r32(g_gpucc, GPUCC_HUB_RCG_CFG));
    LogReg("GPUCC_FF_RCG_CMD",     0x03D99474U, mmio_r32(g_gpucc, GPUCC_FF_RCG_CMD));
    LogReg("GPUCC_FF_RCG_CFG",     0x03D99478U, mmio_r32(g_gpucc, GPUCC_FF_RCG_CFG));
    LogReg("GPUCC_GX_MISC3",       0x03D99504U, mmio_r32(g_gpucc, GPUCC_GX_DOMAIN_MISC3));
}

static void ProbeGmuAndKgslIfGatedOpen(void) {
    uint32_t cx_gds = 0, ahb = 0, gx_gds = 0, gx_cfg = 0;

    if (!GpuCxAxiGateOpen(&cx_gds, &ahb)) {
        LogS("--- GMU/KGSL AXI GATE CLOSED (CX_GDSCR=0x"); LogHex(cx_gds, 8);
        LogS(" AHB=0x"); LogHex(ahb, 8);
        LogS("): Refusing 0x03D6A000 / 0x03D00000 access (SError prevented). ---"); LogNL();
        return;
    }

    if (!g_gmu) {
        PHYSICAL_ADDRESS pa;
        pa.QuadPart = (int64_t)PA_GMU_CORE;
        g_gmu = (volatile uint8_t *)MmMapIoSpace(pa, SZ_GMU_CORE, MmNonCached);
    }
    if (g_gmu) {
        LogS("--- GMU_CX / GMU_AO Live Telemetry (CX Gate OPEN) ---"); LogNL();
        LogReg("GMU_CM3_SYSRESET",      0x03D7E000U, mmio_r32(g_gmu, GMU_CM3_SYSRESET));
        LogReg("GMU_CM3_BOOT_CONFIG",   0x03D7E004U, mmio_r32(g_gmu, GMU_CM3_BOOT_CONFIG));
        LogReg("GMU_CM3_FW_BUSY",       0x03D7E068U, mmio_r32(g_gmu, GMU_CM3_FW_BUSY));
        LogReg("GMU_CM3_FW_INIT_RES",   0x03D7E070U, mmio_r32(g_gmu, GMU_CM3_FW_INIT_RESULT));
        LogReg("GMU_CM3_CFG",           0x03D7E0B4U, mmio_r32(g_gmu, GMU_CM3_CFG));
        LogReg("GMU_AO_CNT_LO",         0x03D7E220U, mmio_r32(g_gmu, GMU_ALWAYS_ON_CNT_L));
        LogReg("GMU_AO_CNT_HI",         0x03D7E224U, mmio_r32(g_gmu, GMU_ALWAYS_ON_CNT_H));
        LogReg("GMU_GFX_PWR_CLK_ST",    0x03D7E340U, mmio_r32(g_gmu, GMU_GFX_PWR_CLK_STATUS));
        LogReg("GMU_RPMH_PWR_STATE",    0x03D7E3B0U, mmio_r32(g_gmu, GMU_RPMH_POWER_STATE));
        LogReg("GMU_HFI_CTRL_STATUS",   0x03D7E600U, mmio_r32(g_gmu, GMU_HFI_CTRL_STATUS));
        LogReg("GMU_G2H_INTR_INFO",     0x03D7E648U, mmio_r32(g_gmu, GMU_G2H_INTR_INFO));
        LogReg("GMU_GENERAL_0",         0x03D7E714U, mmio_r32(g_gmu, GMU_GENERAL_0));
        LogReg("GMU_GENERAL_10",        0x03D7E73CU, mmio_r32(g_gmu, GMU_GENERAL_10));
        LogReg("GMU_AO_HOST_INTR_ST",   0x03D8EC14U, mmio_r32(g_gmu, GMU_AO_HOST_INTR_STATUS));
        LogReg("GMU_AO_CX_BUSY_ST",     0x03D8EC30U, mmio_r32(g_gmu, GMU_AO_GPU_CX_BUSY_STATUS));
        LogReg("GMU_AO_CX_BUSY_ST2",    0x03D8EC34U, mmio_r32(g_gmu, GMU_AO_GPU_CX_BUSY_STATUS2));
        LogReg("GMU_AO_AHB_FENCE_CTL",  0x03D8EC40U, mmio_r32(g_gmu, GMU_AO_AHB_FENCE_CTRL));
        LogReg("GMU_AO_AHB_FENCE_ST",   0x03D8EC4CU, mmio_r32(g_gmu, GMU_AO_AHB_FENCE_STATUS));
        LogReg("GMU_AO_SPARE_CNTL",     0x03D8EC58U, mmio_r32(g_gmu, GMU_AO_SPARE_CNTL));
    }

    if (!GpuGxAxiGateOpen(&gx_gds, &gx_cfg)) {
        LogS("--- KGSL GX GATE CLOSED (GX_GDSCR=0x"); LogHex(gx_gds, 8);
        LogS(" GX_CFG=0x"); LogHex(gx_cfg, 8);
        LogS("): Refusing 0x03D00000 RBBM/GBIF read (SError prevented). ---"); LogNL();
        return;
    }

    if (!g_kgsl) {
        PHYSICAL_ADDRESS pa;
        pa.QuadPart = (int64_t)PA_KGSL;
        g_kgsl = (volatile uint8_t *)MmMapIoSpace(pa, SZ_KGSL, MmNonCached);
    }
    if (g_kgsl) {
        LogS("--- Adreno 740 KGSL (0x03D00000) Live Telemetry (CX+GX Gates OPEN) ---"); LogNL();
        LogS("  [NOTE] Adreno Gen7 identifies ChipID via SoCInfo/ChipInfo (0x07020100 = GEN7_2_1), not legacy RBBM_CHIP_ID."); LogNL();
        LogReg("RBBM_STATUS",           0x03D00840U, mmio_r32(g_kgsl, KGSL_RBBM_STATUS));
        LogReg("RBBM_STATUS3",          0x03D0084CU, mmio_r32(g_kgsl, KGSL_RBBM_STATUS3));
        LogReg("RBBM_INT_0_STATUS",     0x03D00804U, mmio_r32(g_kgsl, KGSL_RBBM_INT_0_STATUS));
        LogReg("RBBM_GBIF_HALT",        0x03D00058U, mmio_r32(g_kgsl, KGSL_RBBM_GBIF_HALT));
        LogReg("RBBM_GBIF_HALT_ACK",    0x03D0005CU, mmio_r32(g_kgsl, KGSL_RBBM_GBIF_HALT_ACK));
        LogReg("RBBM_SP_HYST_CNT",      0x03D00108U, mmio_r32(g_kgsl, KGSL_RBBM_SP_HYST_CNT));
        LogReg("RBBM_RAC_THRESH_CNT",   0x03D00110U, mmio_r32(g_kgsl, KGSL_RBBM_RAC_THRESH_CNT));
        LogReg("RBBM_CLOCK_CNTL",       0x03D002B8U, mmio_r32(g_kgsl, KGSL_RBBM_CLOCK_CNTL));
        LogReg("CP_RB_RPTR",            0x03D02018U, mmio_r32(g_kgsl, KGSL_CP_RB_RPTR));
        LogReg("CP_RB_WPTR",            0x03D0201CU, mmio_r32(g_kgsl, KGSL_CP_RB_WPTR));
        LogReg("CP_SQE_CNTL",           0x03D02020U, mmio_r32(g_kgsl, KGSL_CP_SQE_CNTL));
        LogReg("GBIF_SCACHE_CNTL0",     0x03D0F004U, mmio_r32(g_kgsl, KGSL_GBIF_SCACHE_CNTL0));
        LogReg("GBIF_HALT",             0x03D0F114U, mmio_r32(g_kgsl, KGSL_GBIF_HALT));
        LogReg("GBIF_HALT_ACK",         0x03D0F118U, mmio_r32(g_kgsl, KGSL_GBIF_HALT_ACK));
        LogReg("SECVID_TRUST_CNTL",     0x03D3D000U, mmio_r32(g_kgsl, KGSL_SECVID_TRUST_CNTL));
        LogReg("SECVID_TSB_CNTL",       0x03D3E00CU, mmio_r32(g_kgsl, KGSL_SECVID_TSB_CNTL));
    }
}

/* ============================================================================
 * Stage 0x01: Safe Read-Only Telemetry (Default)
 * ============================================================================ */
static CMD_DB_ARC_INFO g_arc_cx, g_arc_mx, g_arc_mxc, g_arc_gfx;
static int g_arcs_loaded = 0;

static void EnsureArcRailsLoaded(void) {
    if (g_arcs_loaded) return;
    (void)CmdDbLookupArc("cx.lvl",  &g_arc_cx);
    (void)CmdDbLookupArc("mx.lvl",  &g_arc_mx);
    (void)CmdDbLookupArc("mxc.lvl", &g_arc_mxc);
    (void)CmdDbLookupArc("gfx.lvl", &g_arc_gfx);
    g_arcs_loaded = 1;
}

static void EnsureGmuAndKgslMapped(void) {
    if (!g_gmu && GpuCxAxiGateOpen(NULL, NULL)) {
        PHYSICAL_ADDRESS pa;
        pa.QuadPart = (int64_t)PA_GMU_CORE;
        g_gmu = (volatile uint8_t *)MmMapIoSpace(pa, SZ_GMU_CORE, MmNonCached);
    }
    if (!g_kgsl && GpuGxAxiGateOpen(NULL, NULL)) {
        PHYSICAL_ADDRESS pa;
        pa.QuadPart = (int64_t)PA_KGSL;
        g_kgsl = (volatile uint8_t *)MmMapIoSpace(pa, SZ_KGSL, MmNonCached);
    }
}

static void DumpArcInfo(const char *name, CMD_DB_ARC_INFO *dst) {
    CMD_DB_ARC_INFO tmp;
    CMD_DB_ARC_INFO *p = dst ? dst : &tmp;
    if (CmdDbLookupArc(name, p)) {
        LogS("  CMD_DB["); LogS(name); LogS("] = 0x"); LogHex(p->addr, 5);
        LogS(" slv="); LogDec(p->slv_id);
        LogS(" aux_len="); LogDec(p->aux_len);
        LogS(" vlvls["); LogDec(p->num_vlvls); LogS("]:");
        for (uint32_t i = 0; i < p->num_vlvls; i++) {
            LogS(" "); LogHex(p->vlvls[i], 3);
        }
        LogNL();
    } else {
        LogS("  CMD_DB["); LogS(name); LogS("] = NOT FOUND"); LogNL();
    }
}

typedef struct {
    const char      *name;
    CMD_DB_ARC_INFO *dst;
} ARC_TABLE_ENTRY;

static const ARC_TABLE_ENTRY g_arc_table[] = {
    { "cx.lvl",   &g_arc_cx  },
    { "mx.lvl",   &g_arc_mx  },
    { "mxc.lvl",  &g_arc_mxc },
    { "gfx.lvl",  &g_arc_gfx },
    { "gx.lvl",   NULL       },
    { "gmxc.lvl", NULL       },
    { "mmcx.lvl", NULL       },
    { "xo.lvl",   NULL       },
};

static void RunStage1ReadOnly(void) {
    LogS("=== STAGE 0x01: Safe Read-Only GPU Telemetry ==="); LogNL();

    LogS("--- CMD_DB (0x81C60000) GPU ARC Rails ---"); LogNL();
    for (uint32_t i = 0; i < (uint32_t)(sizeof(g_arc_table) / sizeof(g_arc_table[0])); i++) {
        DumpArcInfo(g_arc_table[i].name, g_arc_table[i].dst);
    }
    g_arcs_loaded = 1;

    if (g_chipinfo) {
        LogS("--- ChipInfo Region (0x81CF4000) ---"); LogNL();
        for (uint32_t off = 0; off < 32U; off += 4U) {
            LogReg("CHIPINFO", 0x81CF4000U + off, mmio_r32(g_chipinfo, off));
        }
    }

    if (g_gcc) {
        LogS("--- GCC (0x00100000) GPU Clocks & GPLL0 ---"); LogNL();
        LogReg("GCC_GPLL0_MODE",         0x00100000U, mmio_r32(g_gcc, GCC_GPLL0_MODE));
        LogReg("GCC_GPLL0_L_VAL",        0x00100004U, mmio_r32(g_gcc, GCC_GPLL0_L_VAL));
        LogReg("GCC_GPLL0_USER_CTL",     0x00100010U, mmio_r32(g_gcc, GCC_GPLL0_USER_CTL));
        LogReg("GCC_APCS_CLK_VOTE",      0x00152000U, mmio_r32(g_gcc, GCC_APCS_CLK_BR_ENA_VOTE));
        LogReg("GCC_APCS_GPLL_VOTE",     0x00152018U, mmio_r32(g_gcc, GCC_APCS_GPLL_ENA_VOTE));
        LogReg("GCC_GPU_BCR",            0x00171000U, mmio_r32(g_gcc, GCC_GPU_BCR));
        LogReg("GCC_GPU_CFG_AHB_CBCR",   0x00171004U, mmio_r32(g_gcc, GCC_GPU_CFG_AHB_CBCR));
        LogReg("GCC_QMIP_GPU_AHB_CBCR",  0x00171008U, mmio_r32(g_gcc, GCC_QMIP_GPU_AHB_CBCR));
        LogReg("GCC_GPU_MEMNOC_GFX",     0x00171010U, mmio_r32(g_gcc, GCC_GPU_MEMNOC_GFX_CBCR));
        LogReg("GCC_GPU_SNOC_DVM_GFX",   0x00171018U, mmio_r32(g_gcc, GCC_GPU_SNOC_DVM_GFX_CBCR));
        LogReg("GCC_DDRSS_GPU_AXI",      0x00171154U, mmio_r32(g_gcc, GCC_DDRSS_GPU_AXI_CBCR));
    }

    DumpGpuccTelemetry();
    ProbeGmuAndKgslIfGatedOpen();
}

/* ============================================================================
 * Stage 0x02: Opt-In Clock/Power Bring-Up & Live Adreno 740 Hardware Read
 * ============================================================================ */
static int RunStage2BringUp(void) {
    LogS("=== STAGE 0x02: Opt-In GPU Clock/Power Bring-Up ==="); LogNL();
    if (!g_gcc || !g_gpucc) {
        LogS("  [ERR] Missing GCC or GPUCC mapping."); LogNL();
        return 0;
    }

    /* Self-contained Stage 2: ensure CMD_DB ARC addresses/VLVL tables are loaded even if Stage 1 skipped */
    EnsureArcRailsLoaded();

    /* 1. Ensure GCC_GPU_BCR is not asserted (observe-only safety rule per G736r2) */
    uint32_t bcr = mmio_r32(g_gcc, GCC_GPU_BCR);
    if (bcr == 0xFFFFFFFFU || (bcr & 1U)) {
        LogS("  [ABORT] GCC_GPU_BCR asserted or unreadable (0x"); LogHex(bcr, 8); LogS(")"); LogNL();
        return 0;
    }

    /* 2. RPMh ARC votes via APPS_RSC DRV2 TCS0 */
    if (!g_rsc) {
        PHYSICAL_ADDRESS pa;
        pa.QuadPart = (int64_t)PA_APPS_RSC_DRV2;
        g_rsc = (volatile uint8_t *)MmMapIoSpace(pa, SZ_APPS_RSC_DRV2, MmNonCached);
    }
    int gfx_voted = 0;
    if (g_rsc) {
        uint32_t idx = 0; uint16_t vlvl = 0;
        if (FindArcLevelIndex(&g_arc_cx, 0x100U, &idx, &vlvl)) {
            LogS("  Voting cx.lvl idx="); LogDec(idx); LogS(" (vlvl=0x"); LogHex(vlvl, 3); LogS(")"); LogNL();
            RpmhWriteTcs0("cx.lvl", g_arc_cx.addr, idx);
        }
        if (FindArcLevelIndex(&g_arc_mx, 0x100U, &idx, &vlvl)) {
            LogS("  Voting mx.lvl idx="); LogDec(idx); LogS(" (vlvl=0x"); LogHex(vlvl, 3); LogS(")"); LogNL();
            RpmhWriteTcs0("mx.lvl", g_arc_mx.addr, idx);
        }
        if (FindArcLevelIndex(&g_arc_mxc, 0x100U, &idx, &vlvl)) {
            LogS("  Voting mxc.lvl idx="); LogDec(idx); LogS(" (vlvl=0x"); LogHex(vlvl, 3); LogS(")"); LogNL();
            RpmhWriteTcs0("mxc.lvl", g_arc_mxc.addr, idx);
        }
        if (FindArcLevelIndex(&g_arc_gfx, 0x034U, &idx, &vlvl)) {
            LogS("  Voting gfx.lvl idx="); LogDec(idx); LogS(" (vlvl=0x"); LogHex(vlvl, 3); LogS(")"); LogNL();
            gfx_voted = RpmhWriteTcs0("gfx.lvl", g_arc_gfx.addr, idx);
        }
        SleepMs(2U);
    } else {
        LogS("  [WARN] APPS_RSC_DRV2 map failed; proceeding with existing boot rail levels."); LogNL();
    }

    /* 3. Enable GCC_GPU_CFG_AHB & GPLL0 votes */
    if (!EnableBranch(g_gcc, GCC_GPU_CFG_AHB_CBCR, "gcc_gpu_cfg_ahb_clk", 1, 0)) {
        LogS("  [ABORT] gcc_gpu_cfg_ahb_clk failed to enable."); LogNL();
        return 0;
    }
    /* Vote GPLL0 + gcc_gpu_gpll0_clk_src (BIT 15 of 0x52000) */
    uint32_t gpll_vote = mmio_r32(g_gcc, GCC_APCS_GPLL_ENA_VOTE);
    if ((gpll_vote & 1U) == 0U) {
        mmio_w32(g_gcc, GCC_APCS_GPLL_ENA_VOTE, gpll_vote | 1U);
    }
    if (!PollReg(g_gcc, GCC_GPLL0_MODE, 0x80000000U, 0x80000000U, 500U)) {
        LogS("  [ABORT] GCC GPLL0 not locked!"); LogNL();
        return 0;
    }
    uint32_t br_vote = mmio_r32(g_gcc, GCC_APCS_CLK_BR_ENA_VOTE);
    if ((br_vote & (1U << 15)) == 0U) {
        mmio_w32(g_gcc, GCC_APCS_CLK_BR_ENA_VOTE, br_vote | (1U << 15));
        StallUs(10U);
    }
    LogReg("GCC_APCS_CLK_VOTE_AFTER", 0x00152000U, mmio_r32(g_gcc, GCC_APCS_CLK_BR_ENA_VOTE));

    /* 4. Enable GPUCC_CX_GDSC (0x03D99108) */
    uint32_t cx_gds = mmio_r32(g_gpucc, GPUCC_CX_GDSCR);
    if (cx_gds == 0xFFFFFFFFU) return 0;
    if (cx_gds & 2U) {
        LogS("  [WARN] GPUCC_CX_GDSCR has HW_CONTROL bit1 set (0x"); LogHex(cx_gds, 8); LogS(")"); LogNL();
    }
    if (cx_gds & 1U) {
        mmio_w32(g_gpucc, GPUCC_CX_GDSCR, cx_gds & ~1U);
        StallUs(2U);
    }
    int cx_ok = PollReg(g_gpucc, GPUCC_CX_GDSC_HW_CTRL, 0x80000000U, 0x80000000U, 1500U);
    if (!cx_ok) {
        cx_ok = PollReg(g_gpucc, GPUCC_CX_GDSCR, 0x80000000U, 0x80000000U, 1500U);
    }
    LogReg("GPUCC_CX_GDSCR_AFTER",   0x03D99108U, mmio_r32(g_gpucc, GPUCC_CX_GDSCR));
    LogReg("GPUCC_CX_HW_CTRL_AFTER", 0x03D9953CU, mmio_r32(g_gpucc, GPUCC_CX_GDSC_HW_CTRL));
    if (!cx_ok) {
        LogS("  [ABORT] GPUCC_CX_GDSC failed to power on."); LogNL();
        return 0;
    }

    /* 5. SMMU/NoC clocks (exact G736r2 ClockOrder: SMMU -> MEMNOC -> DVM -> DDRSS) */
    EnableBranch(g_gpucc, GPUCC_HLOS1_VOTE_SMMU_CBCR, "gpu_cc_hlos1_vote_gpu_smmu", 0, 0);
    EnableBranch(g_gcc,   GCC_GPU_MEMNOC_GFX_CBCR,    "gcc_gpu_memnoc_gfx_clk",     1, 0);
    EnableBranch(g_gcc,   GCC_GPU_SNOC_DVM_GFX_CBCR,  "gcc_gpu_snoc_dvm_gfx_clk",   0, 1);
    EnableBranch(g_gcc,   GCC_DDRSS_GPU_AXI_CBCR,     "gcc_ddrss_gpu_axi_clk",      0, 1);

    /* 6. HUB (200 MHz = GPLL0/3: src=5, hid=5, HW_CLK_CTRL=BIT20), FF (200 MHz), AHB/CRC_AHB */
    ProgramRcg(g_gpucc, GPUCC_HUB_RCG_CMD, (5U << 8) | 5U | 0x00100000U, "gpu_cc_hub_clk_src");
    EnableBranch(g_gpucc, GPUCC_HUB_AON_CBCR,    "gpu_cc_hub_aon_clk",    0, 0);
    EnableBranch(g_gpucc, GPUCC_HUB_CX_INT_CBCR, "gpu_cc_hub_cx_int_clk", 0, 0);
    EnableBranch(g_gpucc, GPUCC_CRC_AHB_CBCR,    "gpu_cc_crc_ahb_clk",    0, 0);
    EnableBranch(g_gpucc, GPUCC_AHB_CBCR,        "gpu_cc_ahb_clk",        0, 1);
    ProgramRcg(g_gpucc, GPUCC_FF_RCG_CMD,  (5U << 8) | 5U | 0x00100000U, "gpu_cc_ff_clk_src");
    EnableBranch(g_gpucc, GPUCC_CX_FF_CBCR,      "gpu_cc_cx_ff_clk",      0, 0);
    EnableBranch(g_gpucc, GPUCC_MEMNOC_GFX_CBCR, "gpu_cc_memnoc_gfx_clk", 0, 0);

    /* 7. Lock GPU_CC_PLL0 (0x03D90000, L=0x1E ALPHA=0xBAAA) & GPU_CC_PLL1 (0x03D91000, 440 MHz) */
    ProgramLucidOlePll(g_gpucc, GPUCC_PLL0_BASE, 0x1EU, 0xBAAAU, "gpu_cc_pll0");
    int pll1_ok = ProgramLucidOlePll(g_gpucc, GPUCC_PLL1_BASE, 0x16U, 0xEAAAU, "gpu_cc_pll1");

    /* 8. GMU RCG (220 MHz = PLL1/2: src=3, hid=3, HW_CLK_CTRL) & GMU CX branches */
    if (pll1_ok) {
        ProgramRcg(g_gpucc, GPUCC_GMU_RCG_CMD, (3U << 8) | 3U | 0x00100000U, "gpu_cc_gmu_clk_src");
    } else {
        ProgramRcg(g_gpucc, GPUCC_GMU_RCG_CMD, 0x00100001U, "gpu_cc_gmu_clk_src(xo)");
    }
    EnableBranch(g_gpucc, GPUCC_CXO_AON_CBCR, "gpu_cc_cxo_aon_clk", 0, 0);
    EnableBranch(g_gpucc, GPUCC_CXO_CBCR,     "gpu_cc_cxo_clk",     0, 0);
    EnableBranch(g_gpucc, GPUCC_CX_GMU_CBCR,  "gpu_cc_cx_gmu_clk",  0, 0);

    /* 9. GX GDSC bring-up (requires gfx.lvl rail powered) */
    if (gfx_voted || g_arc_gfx.addr != 0U) {
        uint32_t gx_gds = mmio_r32(g_gpucc, GPUCC_GX_GDSCR);
        if (gx_gds != 0xFFFFFFFFU) {
            if (gx_gds & 1U) {
                mmio_w32(g_gpucc, GPUCC_GX_GDSCR, gx_gds & ~1U);
                StallUs(5U);
            }
            int gx_ok = PollReg(g_gpucc, GPUCC_GX_GDSCR, 0x80000000U, 0x80000000U, 3000U);
            LogReg("GPUCC_GX_GDSCR_AFTER",     0x03D9905CU, mmio_r32(g_gpucc, GPUCC_GX_GDSCR));
            LogReg("GPUCC_GX_CFG_GDSCR_AFTER", 0x03D99060U, mmio_r32(g_gpucc, GPUCC_GX_CFG_GDSCR));
            if (gx_ok) {
                EnableBranch(g_gpucc, GPUCC_GX_GMU_CBCR,   "gpu_cc_gx_gmu_clk",   0, 0);
                EnableBranch(g_gpucc, GPUCC_GX_FF_CBCR,    "gpu_cc_gx_ff_clk",    0, 0);
                EnableBranch(g_gpucc, GPUCC_GX_GFX3D_CBCR, "gpu_cc_gx_gfx3d_clk", 0, 0);
            } else {
                LogS("  [WARN] GPUCC_GX_GDSCR did not assert PWR_ON (bit 31); skipping GX branches."); LogNL();
            }
        }
    }

    SleepMs(2U);
    ProbeGmuAndKgslIfGatedOpen();
    return GpuCxAxiGateOpen(NULL, NULL);
}

/* ============================================================================
 * Stage 0x04: Opt-In Non-Destructive Scratch R/W & GX Power-Collapse Self-Test
 * ============================================================================ */
static void RunStage3ScratchAndGxTest(void) {
    LogS("=== STAGE 0x04: Opt-In Non-Destructive Scratch & GX Collapse Test ==="); LogNL();

    /* Self-contained Stage 3: populate CMD_DB arcs and map GMU/KGSL independently if gates are open */
    EnsureArcRailsLoaded();
    EnsureGmuAndKgslMapped();

    if (!GpuCxAxiGateOpen(NULL, NULL) || !g_gmu) {
        LogS("  [SKIP] CX gate closed or GMU unmapped; refusing Stage 0x04."); LogNL();
        return;
    }

    /* 1. CX-domain GMU_GENERAL_0 scratch test (ONLY while CM3 is held in reset!) */
    uint32_t cm3_rst = mmio_r32(g_gmu, GMU_CM3_SYSRESET);
    if (cm3_rst == 1U) {
        uint32_t orig = mmio_r32(g_gmu, GMU_GENERAL_0);
        mmio_w32(g_gmu, GMU_GENERAL_0, 0xA7405231U);
        uint32_t r1 = mmio_r32(g_gmu, GMU_GENERAL_0);
        mmio_w32(g_gmu, GMU_GENERAL_0, 0x58BFADCEU);
        uint32_t r2 = mmio_r32(g_gmu, GMU_GENERAL_0);
        mmio_w32(g_gmu, GMU_GENERAL_0, orig);
        uint32_t r3 = mmio_r32(g_gmu, GMU_GENERAL_0);
        LogS("  [CX SCRATCH] GMU_GENERAL_0: orig=0x"); LogHex(orig, 8);
        LogS(" r1=0x"); LogHex(r1, 8); LogS(" r2=0x"); LogHex(r2, 8);
        LogS(" restored=0x"); LogHex(r3, 8);
        LogS((r1 == 0xA7405231U && r2 == 0x58BFADCEU && r3 == orig) ? " -> PASS" : " -> MISMATCH"); LogNL();
    } else {
        LogS("  [SKIP] GMU CM3_SYSRESET=0x"); LogHex(cm3_rst, 8); LogS(" (not in reset); skipping GMU_GENERAL_0 write."); LogNL();
    }

    /* 2. GX-domain RBBM scratch test (ONLY while GX gate open & CP_SQE_CNTL == 0) */
    if (!GpuGxAxiGateOpen(NULL, NULL) || !g_kgsl) {
        LogS("  [SKIP] GX gate closed or KGSL unmapped; skipping GX scratch & collapse test."); LogNL();
        return;
    }
    uint32_t sqe = mmio_r32(g_kgsl, KGSL_CP_SQE_CNTL);
    if (sqe != 0U) {
        LogS("  [SKIP] CP_SQE_CNTL=0x"); LogHex(sqe, 8); LogS(" != 0; skipping GX register writes."); LogNL();
        return;
    }

    uint32_t orig_sp  = mmio_r32(g_kgsl, KGSL_RBBM_SP_HYST_CNT);
    uint32_t orig_rac = mmio_r32(g_kgsl, KGSL_RBBM_RAC_THRESH_CNT);
    mmio_w32(g_kgsl, KGSL_RBBM_SP_HYST_CNT, 0x00005A3CU);
    uint32_t sp1 = mmio_r32(g_kgsl, KGSL_RBBM_SP_HYST_CNT);
    mmio_w32(g_kgsl, KGSL_RBBM_RAC_THRESH_CNT, 0x0000A5C3U);
    uint32_t rac1 = mmio_r32(g_kgsl, KGSL_RBBM_RAC_THRESH_CNT);
    LogS("  [GX SCRATCH] RBBM_SP_HYST_CNT: orig=0x"); LogHex(orig_sp, 8);
    LogS(" w=0x00005A3C r=0x"); LogHex(sp1, 8);
    LogS(" | RBBM_RAC_THRESH_CNT: orig=0x"); LogHex(orig_rac, 8);
    LogS(" w=0x0000A5C3 r=0x"); LogHex(rac1, 8); LogNL();

    /* 3. GX Power-Collapse & Recovery Cycle */
    LogS("  [GX COLLAPSE] Setting GPUCC_GX_GDSCR SW_COLLAPSE=1..."); LogNL();
    uint32_t gx = mmio_r32(g_gpucc, GPUCC_GX_GDSCR);
    mmio_w32(g_gpucc, GPUCC_GX_GDSCR, gx | 1U);
    int down_ok = PollReg(g_gpucc, GPUCC_GX_CFG_GDSCR, (1U << 15), (1U << 15), 3000U);
    LogS("  [GX COLLAPSE] down="); LogS(down_ok ? "OK" : "TIMEOUT");
    LogS(" GDSCR=0x"); LogHex(mmio_r32(g_gpucc, GPUCC_GX_GDSCR), 8);
    LogS(" CFG=0x"); LogHex(mmio_r32(g_gpucc, GPUCC_GX_CFG_GDSCR), 8); LogNL();

    SleepMs(2U);

    LogS("  [GX RECOVER] Clearing GPUCC_GX_GDSCR SW_COLLAPSE=0..."); LogNL();
    gx = mmio_r32(g_gpucc, GPUCC_GX_GDSCR);
    mmio_w32(g_gpucc, GPUCC_GX_GDSCR, gx & ~1U);
    int up_ok  = PollReg(g_gpucc, GPUCC_GX_GDSCR, 0x80000000U, 0x80000000U, 3000U);
    int cfg_ok = PollReg(g_gpucc, GPUCC_GX_CFG_GDSCR, (1U << 15), 0U, 3000U);
    uint32_t cx_hw_post = mmio_r32(g_gpucc, GPUCC_CX_GDSC_HW_CTRL);
    uint32_t gx_misc3   = mmio_r32(g_gpucc, GPUCC_GX_DOMAIN_MISC3);
    LogS("  [GX RECOVER] up="); LogS((up_ok && cfg_ok) ? "OK" : "TIMEOUT");
    LogS(" GDSCR=0x"); LogHex(mmio_r32(g_gpucc, GPUCC_GX_GDSCR), 8);
    LogS(" CFG=0x"); LogHex(mmio_r32(g_gpucc, GPUCC_GX_CFG_GDSCR), 8);
    LogS(" CX_HW_CTRL=0x"); LogHex(cx_hw_post, 8);
    LogS(" GX_MISC3=0x"); LogHex(gx_misc3, 8); LogNL();

    /* Re-verify / re-enable GX branch CBCRs after GX collapse/recovery before any KGSL MMIO */
    int gx_gmu_ok = 0, gx_ff_ok = 0, gx_gfx_ok = 0;
    if (up_ok && cfg_ok && (cx_hw_post != 0xFFFFFFFFU) && ((cx_hw_post & 0x80000000U) != 0U)) {
        gx_gmu_ok = EnableBranch(g_gpucc, GPUCC_GX_GMU_CBCR,   "gpu_cc_gx_gmu_clk(post)",   0, 0);
        gx_ff_ok  = EnableBranch(g_gpucc, GPUCC_GX_FF_CBCR,    "gpu_cc_gx_ff_clk(post)",    0, 0);
        gx_gfx_ok = EnableBranch(g_gpucc, GPUCC_GX_GFX3D_CBCR, "gpu_cc_gx_gfx3d_clk(post)", 0, 0);
    }

    if (up_ok && cfg_ok && gx_gmu_ok && gx_ff_ok && gx_gfx_ok &&
        (cx_hw_post != 0xFFFFFFFFU) && ((cx_hw_post & 0x80000000U) != 0U) &&
        GpuGxAxiGateOpen(NULL, NULL)) {
        uint32_t sp_post  = mmio_r32(g_kgsl, KGSL_RBBM_SP_HYST_CNT);
        uint32_t rac_post = mmio_r32(g_kgsl, KGSL_RBBM_RAC_THRESH_CNT);
        uint32_t rbbm_st  = mmio_r32(g_kgsl, KGSL_RBBM_STATUS);
        /* Restore original values */
        mmio_w32(g_kgsl, KGSL_RBBM_SP_HYST_CNT, orig_sp);
        mmio_w32(g_kgsl, KGSL_RBBM_RAC_THRESH_CNT, orig_rac);
        LogS("  [GX POST-RECOVER] RBBM_STATUS=0x"); LogHex(rbbm_st, 8);
        LogS(" SP_HYST(post)=0x"); LogHex(sp_post, 8);
        LogS(" RAC_THRESH(post)=0x"); LogHex(rac_post, 8);
        LogS(" (originals restored)"); LogNL();
    } else {
        LogS("  [WARN] GX gate/branches/CX_HW_CTRL not ready after collapse cycle; skipping post-recover KGSL read."); LogNL();
    }
}

/* ============================================================================
 * Stage 0x08: Read-Only GMU ITCM/DTCM + CX_MISC + G482_RSCC + GMU_PDC + GPUCC GX/ACD/MEMNOC
 * ============================================================================ */
static void RunStage4GmuRsccPdcDump(void) {
    LogS("=== STAGE 0x08: Read-Only GMU ITCM/DTCM + RSCC + GMU_PDC + GPUCC GX/ACD/MEMNOC ==="); LogNL();

    /* 1. GPUCC GX / ACD / MEMNOC detailed state (safe whenever GPUCC gate is open) */
    if (GpuccGateOpen() && g_gpucc) {
        LogS("--- GPUCC GX / ACD / MEMNOC / Aux State (0x03D90000) ---"); LogNL();
        LogReg("GPUCC_GX_BCR",           0x03D99058U, mmio_r32(g_gpucc, GPUCC_GX_BCR));
        LogReg("GPUCC_GX_GDSCR",         0x03D9905CU, mmio_r32(g_gpucc, GPUCC_GX_GDSCR));
        LogReg("GPUCC_GX_CFG_GDSCR",     0x03D99060U, mmio_r32(g_gpucc, GPUCC_GX_CFG_GDSCR));
        LogReg("GPUCC_GX_GFX3D_CBCR",    0x03D990A8U, mmio_r32(g_gpucc, GPUCC_GX_GFX3D_CBCR));
        LogReg("GPUCC_GX_VSENSE_CBCR",   0x03D990B0U, mmio_r32(g_gpucc, GPUCC_GX_VSENSE_CBCR));
        LogReg("GPUCC_GX_GMU_CBCR",      0x03D990BCU, mmio_r32(g_gpucc, GPUCC_GX_GMU_CBCR));
        LogReg("GPUCC_GX_FF_CBCR",       0x03D990C0U, mmio_r32(g_gpucc, GPUCC_GX_FF_CBCR));
        LogReg("GPUCC_GX_GFX3D_RDVM",    0x03D990C8U, mmio_r32(g_gpucc, GPUCC_GX_GFX3D_RDVM_CBCR));
        LogReg("GPUCC_GX_DOMAIN_MISC3",  0x03D99504U, mmio_r32(g_gpucc, GPUCC_GX_DOMAIN_MISC3));
        LogReg("GPUCC_ACD_BCR",          0x03D99358U, mmio_r32(g_gpucc, GPUCC_ACD_BCR));
        LogReg("GPUCC_GFX3D_AON_BCR",    0x03D99198U, mmio_r32(g_gpucc, GPUCC_GFX3D_AON_BCR));
        LogReg("GPUCC_MND1X_0_CBCR",     0x03D99288U, mmio_r32(g_gpucc, GPUCC_MND1X_0_CBCR));
        LogReg("GPUCC_MND1X_1_CBCR",     0x03D9928CU, mmio_r32(g_gpucc, GPUCC_MND1X_1_CBCR));
        LogReg("GPUCC_SLEEP_CBCR",       0x03D99134U, mmio_r32(g_gpucc, GPUCC_SLEEP_CBCR));
        LogReg("GPUCC_FREQ_MEAS_CBCR",   0x03D99008U, mmio_r32(g_gpucc, GPUCC_FREQ_MEAS_CBCR));
        LogReg("GPUCC_DEMET_CBCR",       0x03D9900CU, mmio_r32(g_gpucc, GPUCC_DEMET_CBCR));
        LogReg("GPUCC_MEMNOC_GFX_CBCR",  0x03D99150U, mmio_r32(g_gpucc, GPUCC_MEMNOC_GFX_CBCR));
        if (g_gcc) {
            LogReg("GCC_GPU_MEMNOC_GFX",   0x00171010U, mmio_r32(g_gcc, GCC_GPU_MEMNOC_GFX_CBCR));
            LogReg("GCC_GPU_SNOC_DVM_GFX", 0x00171018U, mmio_r32(g_gcc, GCC_GPU_SNOC_DVM_GFX_CBCR));
            LogReg("GCC_DDRSS_GPU_AXI",    0x00171154U, mmio_r32(g_gcc, GCC_DDRSS_GPU_AXI_CBCR));
        }
    } else {
        LogS("  [SKIP] GPUCC gate closed; skipping GPUCC GX/ACD/MEMNOC dump."); LogNL();
    }

    /* 2. Strict CX AXI gate required before touching ITCM, DTCM, CX_MISC, RSCC, or GMU_PDC */
    uint32_t cx_gds = 0, ahb = 0;
    if (!GpuCxAxiGateOpen(&cx_gds, &ahb)) {
        LogS("  [SKIP] CX AXI gate closed (CX_GDSCR=0x"); LogHex(cx_gds, 8);
        LogS(" AHB=0x"); LogHex(ahb, 8);
        LogS("); refusing ITCM/DTCM/RSCC/PDC reads (SError prevented)."); LogNL();
        return;
    }

    EnsureGmuAndKgslMapped();

    /* 2a. GPU CX_MISC (0x03D9E000) */
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = (int64_t)PA_CX_MISC;
    volatile uint8_t *cx_misc = (volatile uint8_t *)MmMapIoSpace(pa, SZ_CX_MISC, MmNonCached);
    if (cx_misc) {
        LogS("--- GPU CX_MISC (0x03D9E000, G482_GMU_HIGH+0x4000) ---"); LogNL();
        LogReg("CX_MISC_TCM_RET_CNTL", 0x03D9E0E4U, mmio_r32(cx_misc, CX_MISC_TCM_RET_CNTL));
        MmUnmapIoSpace((PVOID)cx_misc, SZ_CX_MISC);
    } else {
        LogS("  [WARN] Failed to map CX_MISC @ 0x03D9E000"); LogNL();
    }

    /* 2b. GMU ITCM (0x03D6D000, 16 KB) & DTCM (0x03D71000, 16 KB) + GMU Control/HFI/RSCC-bridge */
    if (g_gmu) {
        LogS("--- GMU ITCM (0x03D6D000) & DTCM (0x03D71000) Window Status ---"); LogNL();
        uint32_t itcm_nz = 0, itcm_ff = 0;
        for (uint32_t i = 0; i < 4096U; i++) {
            uint32_t v = mmio_r32(g_gmu, GMU_ITCM_OFF + i * 4U);
            if (v != 0U) itcm_nz++;
            if (v == 0xFFFFFFFFU) itcm_ff++;
        }
        LogS("  ITCM[0..3] (0x03D6D000): 0x"); LogHex(mmio_r32(g_gmu, GMU_ITCM_OFF + 0x00U), 8);
        LogS(" 0x"); LogHex(mmio_r32(g_gmu, GMU_ITCM_OFF + 0x04U), 8);
        LogS(" 0x"); LogHex(mmio_r32(g_gmu, GMU_ITCM_OFF + 0x08U), 8);
        LogS(" 0x"); LogHex(mmio_r32(g_gmu, GMU_ITCM_OFF + 0x0CU), 8);
        LogS(" | scan(4096w): nz="); LogDec(itcm_nz); LogS(" ff="); LogDec(itcm_ff); LogNL();

        uint32_t dtcm_nz = 0, dtcm_ff = 0;
        for (uint32_t i = 0; i < 4096U; i++) {
            uint32_t v = mmio_r32(g_gmu, GMU_DTCM_OFF + i * 4U);
            if (v != 0U) dtcm_nz++;
            if (v == 0xFFFFFFFFU) dtcm_ff++;
        }
        LogS("  DTCM[0..3] (0x03D71000): 0x"); LogHex(mmio_r32(g_gmu, GMU_DTCM_OFF + 0x00U), 8);
        LogS(" 0x"); LogHex(mmio_r32(g_gmu, GMU_DTCM_OFF + 0x04U), 8);
        LogS(" 0x"); LogHex(mmio_r32(g_gmu, GMU_DTCM_OFF + 0x08U), 8);
        LogS(" 0x"); LogHex(mmio_r32(g_gmu, GMU_DTCM_OFF + 0x0CU), 8);
        LogS(" | scan(4096w): nz="); LogDec(dtcm_nz); LogS(" ff="); LogDec(dtcm_ff); LogNL();
        LogReg("GMU_DTCM_VERSION",       0x03D74FE0U, mmio_r32(g_gmu, GMU_DTCM_VERSION_OFF));

        LogS("--- GMU Cache / HFI / AO RSCC-Bridge (0x03D6A000) ---"); LogNL();
        LogReg("GMU_ICACHE_CONFIG",      0x03D7D000U, mmio_r32(g_gmu, GMU_ICACHE_CONFIG));
        LogReg("GMU_DCACHE_CONFIG",      0x03D7D004U, mmio_r32(g_gmu, GMU_DCACHE_CONFIG));
        LogReg("GMU_SYS_BUS_CONFIG",     0x03D7D03CU, mmio_r32(g_gmu, GMU_SYS_BUS_CONFIG));
        LogReg("GMU_RPMH_HYST_CTRL",     0x03D7E3C0U, mmio_r32(g_gmu, GMU_RPMH_HYST_CTRL));
        LogReg("GMU_RPMH_CTRL",          0x03D7E3C4U, mmio_r32(g_gmu, GMU_RPMH_CTRL));
        LogReg("GMU_HFI_QTBL_INFO",      0x03D7E610U, mmio_r32(g_gmu, GMU_HFI_QTBL_INFO));
        LogReg("GMU_HFI_QTBL_ADDR",      0x03D7E614U, mmio_r32(g_gmu, GMU_HFI_QTBL_ADDR));
        LogReg("GMU_G2H_INTR_MASK",      0x03D7E64CU, mmio_r32(g_gmu, GMU_G2H_INTR_MASK));
        LogReg("GMU_GENERAL_8(LOG)",     0x03D7E734U, mmio_r32(g_gmu, GMU_GENERAL_8));
        LogReg("GMU_GENERAL_9",          0x03D7E738U, mmio_r32(g_gmu, GMU_GENERAL_9));
        LogReg("GMU_AO_HOST_INTR_MASK",  0x03D8EC18U, mmio_r32(g_gmu, GMU_AO_HOST_INTR_MASK));
        LogReg("GMU_RSCC_CONTROL_REQ",   0x03D8EC1CU, mmio_r32(g_gmu, GMU_RSCC_CONTROL_REQ));
        LogReg("GMU_RSCC_CONTROL_ACK",   0x03D8EC20U, mmio_r32(g_gmu, GMU_RSCC_CONTROL_ACK));
        LogReg("GMU_AO_AHB_FENCE_RNG0",  0x03D8EC44U, mmio_r32(g_gmu, GMU_AO_AHB_FENCE_RANGE_0));
    }

    /* 2c. G482_RSCC (0x03D50000, verified s23_live.dts reg-names="rscc") */
    pa.QuadPart = (int64_t)PA_RSCC;
    volatile uint8_t *rscc = (volatile uint8_t *)MmMapIoSpace(pa, SZ_RSCC, MmNonCached);
    if (rscc) {
        LogS("--- GPU G482_RSCC (0x03D50000, s23_live.dts rscc) ---"); LogNL();
        LogReg("RSCC_RSC_STATUS0_DRV0",  0x03D50010U, mmio_r32(rscc, RSCC_RSC_STATUS0_DRV0));
        LogReg("RSCC_PDC_SEQ_START",     0x03D50020U, mmio_r32(rscc, RSCC_PDC_SEQ_START_ADDR));
        LogReg("RSCC_PDC_MATCH_LO",      0x03D50024U, mmio_r32(rscc, RSCC_PDC_MATCH_VALUE_LO));
        LogReg("RSCC_PDC_MATCH_HI",      0x03D50028U, mmio_r32(rscc, RSCC_PDC_MATCH_VALUE_HI));
        LogReg("RSCC_PDC_SLAVE_ID_DRV0", 0x03D5002CU, mmio_r32(rscc, RSCC_PDC_SLAVE_ID_DRV0));
        LogReg("RSCC_OVERRIDE_START",    0x03D50400U, mmio_r32(rscc, RSCC_OVERRIDE_START_ADDR));
        LogReg("RSCC_SEQ_BUSY_DRV0",     0x03D50404U, mmio_r32(rscc, RSCC_SEQ_BUSY_DRV0));
        for (uint32_t i = 0; i < 5U; i++) {
            LogS("  RSCC_SEQ_MEM_"); LogDec(i);
            LogS("_DRV0 (0x"); LogHex(0x03D50550U + i * 4U, 8);
            LogS(") = 0x"); LogHex(mmio_r32(rscc, RSCC_SEQ_MEM_0_DRV0 + i * 4U), 8); LogNL();
        }
        LogReg("RSCC_TCS0_DRV0_STATUS",  0x03D50D28U, mmio_r32(rscc, RSCC_TCS0_DRV0_STATUS));
        LogReg("RSCC_TCS1_DRV0_STATUS",  0x03D50FC8U, mmio_r32(rscc, RSCC_TCS1_DRV0_STATUS));
        LogReg("RSCC_TCS2_DRV0_STATUS",  0x03D51268U, mmio_r32(rscc, RSCC_TCS2_DRV0_STATUS));
        LogReg("RSCC_TCS3_DRV0_STATUS",  0x03D51508U, mmio_r32(rscc, RSCC_TCS3_DRV0_STATUS));
        MmUnmapIoSpace((PVOID)rscc, SZ_RSCC);
    } else {
        LogS("  [WARN] Failed to map G482_RSCC @ 0x03D50000"); LogNL();
    }

    /* 2d. GMU_PDC (0x0B280000, verified s23_live.dts reg-names="gmu_pdc") */
    pa.QuadPart = (int64_t)PA_GMU_PDC;
    volatile uint8_t *pdc = (volatile uint8_t *)MmMapIoSpace(pa, SZ_GMU_PDC, MmNonCached);
    if (pdc) {
        LogS("--- GPU GMU_PDC (0x0B280000, G482_PDC) ---"); LogNL();
        LogReg("PDC_GPU_STATUS_1024",    0x0B281024U, mmio_r32(pdc, PDC_GPU_STATUS_1024));
        LogReg("PDC_GPU_ENABLE_PDC",     0x0B284500U, mmio_r32(pdc, PDC_GPU_ENABLE_PDC));
        LogReg("PDC_GPU_MATCH_VALUE_LO", 0x0B284510U, mmio_r32(pdc, PDC_GPU_MATCH_VALUE_LO));
        LogReg("PDC_GPU_MATCH_VALUE_HI", 0x0B284514U, mmio_r32(pdc, PDC_GPU_MATCH_VALUE_HI));
        LogReg("PDC_GPU_SEQ_START_ADDR", 0x0B284520U, mmio_r32(pdc, PDC_GPU_SEQ_START_ADDR));
        MmUnmapIoSpace((PVOID)pdc, SZ_GMU_PDC);
    } else {
        LogS("  [WARN] Failed to map GMU_PDC @ 0x0B280000"); LogNL();
    }
}

/* ============================================================================
 * Stage 0x10 (Stage 5A): Read-Only ARM64 SCM + AXI-Gated KGSL-SMMU Telemetry
 * ============================================================================ */
static int ScmCall64(
    uint64_t fid, uint64_t arginfo,
    uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t *r0_out, uint64_t *r1_out, uint64_t *r2_out, uint64_t *r3_out)
{
    uint64_t x1 = arginfo, x2 = a0, x3 = a1, x4 = a2, x5 = a3, x6 = 0, x7 = 0;
    uint64_t last_r0 = ~0ULL;
    for (uint32_t tries = 0; tries < 64U; tries++) {
        register uint64_t rx0 __asm__("x0") = (tries == 0U) ? fid : 1ULL;
        register uint64_t rx1 __asm__("x1") = x1;
        register uint64_t rx2 __asm__("x2") = x2;
        register uint64_t rx3 __asm__("x3") = x3;
        register uint64_t rx4 __asm__("x4") = x4;
        register uint64_t rx5 __asm__("x5") = x5;
        register uint64_t rx6 __asm__("x6") = x6;
        register uint64_t rx7 __asm__("x7") = x7;
        __asm__ volatile(
            "smc #0"
            : "+r"(rx0), "+r"(rx1), "+r"(rx2), "+r"(rx3), "+r"(rx4), "+r"(rx5), "+r"(rx6), "+r"(rx7)
            :
            : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "cc", "memory"
        );
        last_r0 = rx0;
        if (rx0 == 1ULL) { /* QCOM_SCM_INTERRUPTED */
            x1 = rx1; x2 = rx2; x3 = rx3; x4 = rx4; x5 = rx5; x6 = rx6; x7 = rx7;
            continue;
        }
        if (r0_out) *r0_out = rx0;
        if (r1_out) *r1_out = rx1;
        if (r2_out) *r2_out = rx2;
        if (r3_out) *r3_out = rx3;
        return 1;
    }
    if (r0_out) *r0_out = last_r0;
    if (r1_out) *r1_out = 0;
    if (r2_out) *r2_out = 0;
    if (r3_out) *r3_out = 0;
    return 0;
}

static int ProbeScmCallAvail(const char *label, uint64_t target_fid) {
    uint64_t r0 = ~0ULL, r1 = 0, r2 = 0, r3 = 0;
    uint64_t used_fid = 0xC2000601ULL; /* QCOM_SCM_INFO_IS_CALL_AVAIL (FAST64, SiP) */
    (void)ScmCall64(used_fid, 1ULL, target_fid, 0, 0, 0, &r0, &r1, &r2, &r3);
    if (r0 == ~0ULL) {
        used_fid = 0x42000601ULL; /* STD64 fallback if FAST64 unknown */
        (void)ScmCall64(used_fid, 1ULL, target_fid, 0, 0, 0, &r0, &r1, &r2, &r3);
    }
    LogS("  SCM IS_CALL_AVAIL["); LogS(label);
    LogS(" fid=0x"); LogHex(target_fid, 8);
    LogS(" via 0x"); LogHex(used_fid, 8);
    LogS("]: r0=0x"); LogHex(r0, 16);
    LogS(" avail=0x"); LogHex(r1, 16);
    LogNL();
    return (r0 == 0ULL && r1 != 0ULL) ? 1 : 0;
}

static void RunStage5AScmAndSmmu(uint32_t mask) {
    LogS("=== STAGE 0x10 (Stage 5A): Read-Only ARM64 SCM + KGSL-SMMU Telemetry ==="); LogNL();

    ProbeScmCallAvail("SET_CP_POOL_SIZE",  0x02000C1BULL);
    ProbeScmCallAvail("PAS_INIT_IMAGE",    0x02000201ULL);
    ProbeScmCallAvail("PAS_MEM_SETUP",     0x02000202ULL);
    ProbeScmCallAvail("PAS_AUTH_AND_RST",  0x02000205ULL);
    ProbeScmCallAvail("PAS_SHUTDOWN",      0x02000206ULL);
    int pas_supp_avail = ProbeScmCallAvail("PAS_IS_SUPPORTED", 0x02000207ULL);

    if (pas_supp_avail) {
        uint64_t r0 = ~0ULL, r1 = 0, r2 = 0, r3 = 0;
        uint64_t fid = 0x42000207ULL; /* STD64 QCOM_SCM_PIL_PAS_IS_SUPPORTED */
        (void)ScmCall64(fid, 1ULL, 13ULL, 0, 0, 0, &r0, &r1, &r2, &r3);
        if (r0 == ~0ULL) {
            fid = 0xC2000207ULL;
            (void)ScmCall64(fid, 1ULL, 13ULL, 0, 0, 0, &r0, &r1, &r2, &r3);
        }
        LogS("  SCM PAS_IS_SUPPORTED(PAS_ID=13 GPU ZAP via 0x"); LogHex(fid, 8);
        LogS("): r0=0x"); LogHex(r0, 16);
        LogS(" r1=0x"); LogHex(r1, 16);
        LogS(" r2=0x"); LogHex(r2, 16); LogNL();
    }

    /* Strict AXI-gated KGSL-SMMU (0x03DA0000) read-only dump */
    uint32_t cx_gds = 0, ahb = 0;
    if (!GpuCxAxiGateOpen(&cx_gds, &ahb) || !g_gpucc) {
        LogS("  [SKIP] CX AXI gate closed (CX_GDSCR=0x"); LogHex(cx_gds, 8);
        LogS(" AHB=0x"); LogHex(ahb, 8);
        LogS("); refusing 0x03DA0000 KGSL-SMMU read (SError prevented)."); LogNL();
        return;
    }

    uint32_t smmu_cbcr = mmio_r32(g_gpucc, GPUCC_HLOS1_VOTE_SMMU_CBCR);
    if (smmu_cbcr != 0xFFFFFFFFU && ((smmu_cbcr & 0x80000000U) != 0U || (smmu_cbcr & 1U) == 0U)) {
        if (mask & 0x02U) {
            (void)EnableBranch(g_gpucc, GPUCC_HLOS1_VOTE_SMMU_CBCR, "gpu_cc_hlos1_vote_gpu_smmu", 0, 0);
            (void)PollReg(g_gpucc, GPUCC_HLOS1_VOTE_SMMU_CBCR, 0x80000001U, 0x00000001U, 5000U);
            smmu_cbcr = mmio_r32(g_gpucc, GPUCC_HLOS1_VOTE_SMMU_CBCR);
        }
    }
    LogReg("GPUCC_HLOS1_SMMU(5A)", 0x03D97000U, smmu_cbcr);
    if (smmu_cbcr == 0xFFFFFFFFU || (smmu_cbcr & 0x80000000U) != 0U || (smmu_cbcr & 1U) == 0U) {
        LogS("  [SKIP] GPUCC_HLOS1_VOTE_SMMU_CBCR CLK_OFF=1; refusing 0x03DA0000 KGSL-SMMU read (SError prevented)."); LogNL();
        return;
    }

    PHYSICAL_ADDRESS pa;
    pa.QuadPart = (int64_t)PA_KGSL_SMMU;
    volatile uint8_t *smmu = (volatile uint8_t *)MmMapIoSpace(pa, SZ_KGSL_SMMU, MmNonCached);
    if (!smmu) {
        LogS("  [WARN] Failed to map KGSL-SMMU @ 0x03DA0000"); LogNL();
        return;
    }
    LogS("--- KGSL-SMMU (0x03DA0000, QSMMU-v500) Read-Only Dump ---"); LogNL();
    LogReg("SMMU_sCR0",   0x03DA0000U, mmio_r32(smmu, 0x0000U));
    LogReg("SMMU_sACR",   0x03DA0010U, mmio_r32(smmu, 0x0010U));
    LogReg("SMMU_sIDR0",  0x03DA0020U, mmio_r32(smmu, 0x0020U));
    LogReg("SMMU_sIDR1",  0x03DA0024U, mmio_r32(smmu, 0x0024U));
    LogReg("SMMU_sIDR2",  0x03DA0028U, mmio_r32(smmu, 0x0028U));
    LogReg("SMMU_sIDR7",  0x03DA003CU, mmio_r32(smmu, 0x003CU));
    LogReg("SMMU_sGFSR",  0x03DA0048U, mmio_r32(smmu, 0x0048U));
    MmUnmapIoSpace((PVOID)smmu, SZ_KGSL_SMMU);
}

/* ============================================================================
 * Stage 0x20 (Stage 5B): Opt-In GMU CM3 Firmware Load, PDC/RSCC, HFI & OOB
 * ============================================================================ */
static uint8_t g_gmu_fw_buf[131072];

static uint32_t ReadLe32U(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int ReadGmuFwFile(uint32_t *out_len) {
    UNICODE_STRING u;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE hf = NULL;
    RtlInitUnicodeString(&u, L"\\??\\C:\\Users\\Public\\gmu_gen70200.bin");
    InitObjAttr(&oa, &u, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE);
    if (!NT_SUCCESS(ZwCreateFile(
            &hf, FILE_GENERIC_READ, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
            FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
            FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0))) {
        return 0;
    }
    uint32_t total = 0;
    LARGE_INTEGER off;
    off.QuadPart = 0;
    while (total < sizeof(g_gmu_fw_buf)) {
        NTSTATUS st = ZwReadFile(hf, NULL, NULL, NULL, &iosb,
                                 g_gmu_fw_buf + total, (uint32_t)(sizeof(g_gmu_fw_buf) - total),
                                 &off, NULL);
        if (st == STATUS_END_OF_FILE || iosb.Information == 0) break;
        if (!NT_SUCCESS(st)) {
            ZwClose(hf);
            return 0;
        }
        total += (uint32_t)iosb.Information;
        off.QuadPart = (int64_t)total;
    }
    ZwClose(hf);
    *out_len = total;
    return (total >= 16U) ? 1 : 0;
}

static void ZeroBuf32(volatile uint32_t *p, uint32_t words) {
    for (uint32_t i = 0; i < words; i++) p[i] = 0U;
}

static void RunStage5BGmuBoot(void) {
    LogS("=== STAGE 0x20 (Stage 5B): Opt-In GMU CM3 FW Load + PDC/RSCC + HFI + OOB ==="); LogNL();

    uint32_t fw_len = 0;
    if (!ReadGmuFwFile(&fw_len)) {
        LogS("  [SKIP] C:\\Users\\Public\\gmu_gen70200.bin not found -- pull via Lineage Recovery pull_s23_gpu_fw.py first"); LogNL();
        return;
    }
    LogS("  Loaded C:\\Users\\Public\\gmu_gen70200.bin: "); LogDec((int64_t)fw_len); LogS(" bytes"); LogNL();

    uint32_t cx_gds = 0, ahb = 0;
    if (!GpuCxAxiGateOpen(&cx_gds, &ahb)) {
        LogS("  [SKIP] CX AXI gate closed (CX_GDSCR=0x"); LogHex(cx_gds, 8);
        LogS(" AHB=0x"); LogHex(ahb, 8); LogS("); aborting Stage 5B."); LogNL();
        return;
    }
    EnsureGmuAndKgslMapped();
    if (!g_gmu) {
        LogS("  [ERR] GMU mapping unavailable."); LogNL();
        return;
    }

    uint32_t cm3_rst = mmio_r32(g_gmu, GMU_CM3_SYSRESET);
    if (cm3_rst != 1U) {
        LogS("  [SKIP] GMU_CM3_SYSRESET=0x"); LogHex(cm3_rst, 8); LogS(" != 1 (not in cold reset)."); LogNL();
        return;
    }

    PHYSICAL_ADDRESS pa;
    pa.QuadPart = (int64_t)PA_RSCC;
    volatile uint8_t *rscc = (volatile uint8_t *)MmMapIoSpace(pa, SZ_RSCC, MmNonCached);
    pa.QuadPart = (int64_t)PA_GMU_PDC;
    volatile uint8_t *pdc = (volatile uint8_t *)MmMapIoSpace(pa, SZ_GMU_PDC, MmNonCached);
    pa.QuadPart = (int64_t)PA_CX_MISC;
    volatile uint8_t *cx_misc = (volatile uint8_t *)MmMapIoSpace(pa, SZ_CX_MISC, MmNonCached);
    if (!rscc || !pdc || !cx_misc) {
        LogS("  [ERR] Failed mapping RSCC/PDC/CX_MISC."); LogNL();
        if (cx_misc) MmUnmapIoSpace((PVOID)cx_misc, SZ_CX_MISC);
        if (pdc)     MmUnmapIoSpace((PVOID)pdc, SZ_GMU_PDC);
        if (rscc)    MmUnmapIoSpace((PVOID)rscc, SZ_RSCC);
        return;
    }

    uint32_t rsc_busy = mmio_r32(rscc, RSCC_SEQ_BUSY_DRV0);
    if (rsc_busy != 0U) {
        LogS("  [SKIP] RSCC_SEQ_BUSY_DRV0=0x"); LogHex(rsc_busy, 8); LogS(" != 0."); LogNL();
        goto cleanup_maps;
    }

    /* Inspect FW blocks and validate ITCM / DTCM / ICACHE placement */
    uint32_t off = 0, blk_idx = 0, itcm_blks = 0, dtcm_blks = 0, icache_blks = 0;
    while (off + 16U <= fw_len) {
        uint32_t addr  = ReadLe32U(g_gmu_fw_buf + off);
        uint32_t blen  = ReadLe32U(g_gmu_fw_buf + off + 4U);
        uint32_t btype = ReadLe32U(g_gmu_fw_buf + off + 8U);
        off += 16U;
        if (blen > fw_len - off || (addr & 3U) || (blen & 3U)) {
            LogS("  [ERR] Invalid GMU block header at #"); LogDec((int64_t)blk_idx); LogNL();
            goto cleanup_maps;
        }
        if (blen > 0U) {
            if (addr + blen <= 0x00004000U) {
                itcm_blks++;
                for (uint32_t w = 0; w < blen; w += 4U) {
                    uint32_t want = ReadLe32U(g_gmu_fw_buf + off + w);
                    mmio_w32(g_gmu, GMU_ITCM_OFF + addr + w, want);
                }
                for (uint32_t w = 0; w < blen; w += 4U) {
                    uint32_t want = ReadLe32U(g_gmu_fw_buf + off + w);
                    uint32_t got  = mmio_r32(g_gmu, GMU_ITCM_OFF + addr + w);
                    if (got != want) {
                        LogS("  [ERR] ITCM verify mismatch at 0x"); LogHex(addr + w, 4); LogNL();
                        goto cleanup_maps;
                    }
                }
            } else if (addr >= 0x10004000U && (addr - 0x10004000U) + blen <= 0x00004000U) {
                dtcm_blks++;
                uint32_t dbase = addr - 0x10004000U;
                for (uint32_t w = 0; w < blen; w += 4U) {
                    uint32_t want = ReadLe32U(g_gmu_fw_buf + off + w);
                    mmio_w32(g_gmu, GMU_DTCM_OFF + dbase + w, want);
                }
                for (uint32_t w = 0; w < blen; w += 4U) {
                    uint32_t want = ReadLe32U(g_gmu_fw_buf + off + w);
                    uint32_t got  = mmio_r32(g_gmu, GMU_DTCM_OFF + dbase + w);
                    if (got != want) {
                        LogS("  [ERR] DTCM verify mismatch at 0x"); LogHex(dbase + w, 4); LogNL();
                        goto cleanup_maps;
                    }
                }
            } else if (addr >= 0x00004000U && addr < 0x10004000U) {
                icache_blks++;
                LogS("  FW Region2(ICACHE) blk#"); LogDec((int64_t)blk_idx);
                LogS(" addr=0x"); LogHex(addr, 8); LogS(" len=0x"); LogHex(blen, 6);
                LogS(" type="); LogDec((int64_t)btype); LogNL();
            } else {
                LogS("  [ERR] Unmapped GMU block addr=0x"); LogHex(addr, 8); LogNL();
                goto cleanup_maps;
            }
        }
        off += blen;
        blk_idx++;
    }
    LogS("  Parsed GMU FW blocks: total="); LogDec((int64_t)blk_idx);
    LogS(" ITCM="); LogDec((int64_t)itcm_blks);
    LogS(" ICACHE="); LogDec((int64_t)icache_blks);
    LogS(" DTCM="); LogDec((int64_t)dtcm_blks); LogNL();

    if (icache_blks > 0U) {
        LogS("  [SKIP] GMU Region 2 (ICACHE IOVA) requires active KGSL-SMMU CB page tables before CM3 reset release; ITCM/DTCM verified cleanly."); LogNL();
        goto cleanup_maps;
    }

    /* Allocate 16 KB HFI queue table + 16 KB log buffer below 4 GB */
    PHYSICAL_ADDRESS lo, hi, bound;
    lo.QuadPart = 0;
    hi.QuadPart = 0x00000000FFFFF000LL;
    bound.QuadPart = 0x1000LL;
    volatile uint32_t *hfi_va = (volatile uint32_t *)MmAllocateContiguousMemorySpecifyCache(16384U, lo, hi, bound, MmNonCached);
    volatile uint32_t *log_va = (volatile uint32_t *)MmAllocateContiguousMemorySpecifyCache(16384U, lo, hi, bound, MmNonCached);
    if (!hfi_va || !log_va) {
        LogS("  [ERR] Failed to allocate <4GB contiguous HFI/log DMA buffers."); LogNL();
        goto cleanup_dma;
    }
    uint64_t hfi_pa = (uint64_t)MmGetPhysicalAddress((PVOID)hfi_va).QuadPart;
    uint64_t log_pa = (uint64_t)MmGetPhysicalAddress((PVOID)log_va).QuadPart;
    if ((hfi_pa & 0xFFFULL) || hfi_pa > 0xFFFFC000ULL || (log_pa & 0xFFFULL) || log_pa > 0xFFFFC000ULL) {
        LogS("  [ERR] Contiguous PA invalid for 32-bit GMU IOVA."); LogNL();
        goto cleanup_dma;
    }

    ZeroBuf32(hfi_va, 4096U);
    ZeroBuf32(log_va, 4096U);
    hfi_va[1] = 168U; hfi_va[2] = 6U; hfi_va[3] = 12U; hfi_va[4] = 3U; hfi_va[5] = 3U;
    for (uint32_t q = 0; q < 3U; q++) {
        volatile uint32_t *qh = hfi_va + 6U + 12U * q;
        qh[0] = 1U;
        qh[1] = (uint32_t)hfi_pa + (q + 1U) * 4096U;
        qh[2] = 0xA00U | q;
        qh[3] = 1024U;
        qh[6] = 1U; qh[7] = 1U; qh[8] = 1U;
    }
    dmb_sy();

    /* Mask & clear both G2H and AO interrupt banks before configuring */
    mmio_w32(g_gmu, GMU_G2H_INTR_MASK, 0xFFFFFFFFU);
    mmio_w32(g_gmu, GMU_AO_HOST_INTR_MASK, 0x7FU);
    mmio_w32(g_gmu, GMU_G2H_INTR_CLEAR, 0xFFFFFFFFU);
    mmio_w32(g_gmu, GMU_AO_HOST_INTR_CLR, 0xFFFFFFFFU);

    /* G482ConfigureCore preboot programming */
    mmio_w32(g_gmu, GMU_RPMH_CTRL, 1U);
    mmio_w32(g_gmu, GMU_RPMH_HYST_CTRL, 1U);
    mmio_w32(cx_misc, CX_MISC_TCM_RET_CNTL, 1U);
    mmio_w32(g_gmu, GMU_AO_AHB_FENCE_RANGE_0, 0x80C808A0U);
    mmio_w32(g_gmu, GMU_CM3_CFG, 0x4052U);
    mmio_w32(g_gmu, GMU_GENERAL_9, 0U);
    mmio_w32(g_gmu, GMU_GENERAL_10, 0x07020100U);
    mmio_w32(g_gmu, GMU_GENERAL_8, ((uint32_t)log_pa & 0xFFFFF000U) | 3U);
    mmio_w32(g_gmu, GMU_SYS_BUS_CONFIG, 1U);
    mmio_w32(g_gmu, GMU_ICACHE_CONFIG, 1U);
    mmio_w32(g_gmu, GMU_DCACHE_CONFIG, 1U);

    uint32_t fw_ver = mmio_r32(g_gmu, GMU_DTCM_VERSION_OFF);
    LogReg("GMU_DTCM_VERSION(pre)", 0x03D74FE0U, fw_ver);

    mmio_w32(g_gmu, GMU_CM3_FW_INIT_RESULT, 0U);
    mmio_w32(g_gmu, GMU_CM3_BOOT_CONFIG, 2U);
    mmio_w32(g_gmu, GMU_HFI_QTBL_ADDR, (uint32_t)hfi_pa);
    mmio_w32(g_gmu, GMU_HFI_QTBL_INFO, 1U);

    /* Release CM3 reset & poll FW ready (500 ms max) */
    mmio_w32(g_gmu, GMU_CM3_SYSRESET, 0U);
    uint32_t rmask = (fw_ver <= 0x20010004U) ? 0xFFFFFFFFU : 0x1FFU;
    uint32_t rwant = (fw_ver <= 0x20010004U) ? 0xBABEFACEU : 0x100U;
    int fw_ok = 0;
    for (uint32_t ms = 0; ms < 500U; ms++) {
        uint32_t res = mmio_r32(g_gmu, GMU_CM3_FW_INIT_RESULT);
        if ((res & rmask) == rwant) {
            fw_ok = 1;
            break;
        }
        SleepMs(1U);
    }
    LogReg("GMU_CM3_FW_INIT_RES(post)", 0x03D7E070U, mmio_r32(g_gmu, GMU_CM3_FW_INIT_RESULT));
    if (!fw_ok) {
        LogS("  [WARN] GMU CM3 FW_INIT_RESULT timed out."); LogNL();
        goto stop_cm3;
    }

    /* Post-FW, pre-HFI: PDC + RSCC + S23 GEN7_2_1 20B ucode (g482_board.c order) */
    mmio_w32(pdc, PDC_GPU_SEQ_START_ADDR, 0U);
    mmio_w32(pdc, PDC_GPU_ENABLE_PDC, 0x80000001U);
    mmio_w32(rscc, RSCC_RSC_STATUS0_DRV0, 1U << 24);
    static const uint32_t kRscPairs[][2] = {
        { 0x002CU, 1U }, { 0x0038U, 0U }, { 0x0034U, 0U }, { 0x0040U, 0U },
        { 0x003CU, 0U }, { 0x0048U, 0x80000021U }, { 0x0044U, 0U }, { 0x0400U, 0U },
        { 0x0020U, 0x4520U }, { 0x0024U, 0x4510U }, { 0x0028U, 0x4514U }
    };
    for (uint32_t i = 0; i < (uint32_t)(sizeof(kRscPairs) / sizeof(kRscPairs[0])); i++) {
        mmio_w32(rscc, kRscPairs[i][0], kRscPairs[i][1]);
    }
    static const uint32_t kS23RscUcode[5] = {
        0xEAAAE5A0U, 0xE1A1EBABU, 0xA2E0A581U, 0xECAC82E2U, 0x0020EDADU
    };
    for (uint32_t i = 0; i < 5U; i++) {
        mmio_w32(rscc, RSCC_SEQ_MEM_0_DRV0 + i * 4U, kS23RscUcode[i]);
    }

    /* Start HFI */
    mmio_w32(g_gmu, GMU_HFI_INIT, 1U);
    int hfi_ok = 0;
    for (uint32_t ms = 0; ms < 200U; ms++) {
        if (mmio_r32(g_gmu, GMU_HFI_CTRL_STATUS) & 1U) {
            hfi_ok = 1;
            break;
        }
        SleepMs(1U);
    }
    LogReg("GMU_HFI_CTRL_STATUS(post)", 0x03D7E600U, mmio_r32(g_gmu, GMU_HFI_CTRL_STATUS));
    if (!hfi_ok) {
        LogS("  [WARN] GMU HFI_INIT timed out."); LogNL();
        goto stop_cm3;
    }

    /* Request OOB_REQUEST (BIT 30), wait OOB_ACK (BIT 31), clear ACK */
    mmio_w32(g_gmu, GMU_H2G_SET, 1U << 30);
    int oob_ok = 0;
    for (uint32_t i = 0; i < 100U; i++) {
        if (mmio_r32(g_gmu, GMU_G2H_INTR_INFO) & (1U << 31)) {
            oob_ok = 1;
            break;
        }
        StallUs(100U);
    }
    LogReg("GMU_G2H_INTR_INFO(oob)", 0x03D7E648U, mmio_r32(g_gmu, GMU_G2H_INTR_INFO));
    if (oob_ok) {
        mmio_w32(g_gmu, GMU_G2H_INTR_CLEAR, 1U << 31);
    }

    uint32_t gx_gds = mmio_r32(g_gpucc, GPUCC_GX_GDSCR);
    LogReg("GPUCC_GX_GDSCR(5B)", 0x03D9905CU, gx_gds);
    if ((gx_gds & 0x80000001U) == 0x80000000U && GpuGxAxiGateOpen(NULL, NULL)) {
        if (!g_kgsl) {
            pa.QuadPart = (int64_t)PA_KGSL;
            g_kgsl = (volatile uint8_t *)MmMapIoSpace(pa, SZ_KGSL, MmNonCached);
        }
        if (g_kgsl) {
            LogReg("KGSL_RBBM_STATUS(5B)",  0x03D00840U, mmio_r32(g_kgsl, KGSL_RBBM_STATUS));
            LogReg("KGSL_RBBM_STATUS3(5B)", 0x03D0084CU, mmio_r32(g_kgsl, KGSL_RBBM_STATUS3));
        }
    } else {
        LogS("  [INFO] GPUCC_GX_GDSCR PWR_ON=0; skipping KGSL RBBM read."); LogNL();
    }

    /* Release OOB cleanly before stopping CM3 */
    mmio_w32(g_gmu, GMU_H2G_SET, 1U << 31);

stop_cm3:
    mmio_w32(g_gmu, GMU_CM3_SYSRESET, 1U);
    (void)PollReg(g_gmu, GMU_CM3_SYSRESET, 1U, 1U, 1000U);
    LogReg("GMU_CM3_SYSRESET(stopped)", 0x03D7E000U, mmio_r32(g_gmu, GMU_CM3_SYSRESET));

cleanup_dma:
    if (log_va) MmFreeContiguousMemorySpecifyCache((PVOID)log_va, 16384U, MmNonCached);
    if (hfi_va) MmFreeContiguousMemorySpecifyCache((PVOID)hfi_va, 16384U, MmNonCached);

cleanup_maps:
    if (cx_misc) MmUnmapIoSpace((PVOID)cx_misc, SZ_CX_MISC);
    if (pdc)     MmUnmapIoSpace((PVOID)pdc, SZ_GMU_PDC);
    if (rscc)    MmUnmapIoSpace((PVOID)rscc, SZ_RSCC);
}

/* ============================================================================
 * DriverUnload & DriverEntry
 * ============================================================================ */
static void DriverUnload(PDRIVER_OBJECT DriverObject) {
    (void)DriverObject;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    (void)RegistryPath;
    if (DriverObject) {
        DriverObject->DriverUnload = DriverUnload;
    }

    LogOpen();
    uint32_t mask = ReadStageMask();
    LogS("=== s23gpuprobe.sys v1.0.0.5 started (StageMask=0x");
    LogHex(mask, 2);
    LogS(") ===");
    LogNL();

    PHYSICAL_ADDRESS pa;
    pa.QuadPart = (int64_t)PA_CMD_DB;
    g_cmddb = (volatile uint8_t *)MmMapIoSpace(pa, SZ_CMD_DB, MmNonCached);
    LogS("Map CMD_DB   @ 0x81C60000 -> "); LogS(g_cmddb ? "OK" : "FAIL"); LogNL();

    pa.QuadPart = (int64_t)PA_CHIPINFO;
    g_chipinfo = (volatile uint8_t *)MmMapIoSpace(pa, SZ_CHIPINFO, MmNonCached);
    LogS("Map CHIPINFO @ 0x81CF4000 -> "); LogS(g_chipinfo ? "OK" : "FAIL"); LogNL();

    pa.QuadPart = (int64_t)PA_GCC;
    g_gcc = (volatile uint8_t *)MmMapIoSpace(pa, SZ_GCC, MmNonCached);
    LogS("Map GCC      @ 0x00100000 -> "); LogS(g_gcc ? "OK" : "FAIL"); LogNL();

    pa.QuadPart = (int64_t)PA_GPUCC;
    g_gpucc = (volatile uint8_t *)MmMapIoSpace(pa, SZ_GPUCC, MmNonCached);
    LogS("Map GPUCC    @ 0x03D90000 -> "); LogS(g_gpucc ? "OK" : "FAIL"); LogNL();

    if (mask & 0x01U) {
        RunStage1ReadOnly();
    }
    if (mask & 0x02U) {
        int s2_ok = RunStage2BringUp();
        if (s2_ok && (mask & 0x04U)) {
            RunStage3ScratchAndGxTest();
        } else if (mask & 0x04U) {
            LogS("  [SKIP] Stage 0x04 requires Stage 0x02 CX gate open."); LogNL();
        }
    } else if (mask & 0x04U) {
        RunStage3ScratchAndGxTest();
    }
    if (mask & 0x08U) {
        RunStage4GmuRsccPdcDump();
    }
    if (mask & 0x10U) {
        RunStage5AScmAndSmmu(mask);
    }
    if (mask & 0x20U) {
        RunStage5BGmuBoot();
    }

    LogS("=== s23gpuprobe.sys v1.0.0.5 completed cleanly ==="); LogNL();

    if (g_kgsl)     { MmUnmapIoSpace((PVOID)g_kgsl,     SZ_KGSL);          g_kgsl     = NULL; }
    if (g_gmu)      { MmUnmapIoSpace((PVOID)g_gmu,      SZ_GMU_CORE);      g_gmu      = NULL; }
    if (g_rsc)      { MmUnmapIoSpace((PVOID)g_rsc,      SZ_APPS_RSC_DRV2); g_rsc      = NULL; }
    if (g_gpucc)    { MmUnmapIoSpace((PVOID)g_gpucc,    SZ_GPUCC);         g_gpucc    = NULL; }
    if (g_gcc)      { MmUnmapIoSpace((PVOID)g_gcc,      SZ_GCC);           g_gcc      = NULL; }
    if (g_chipinfo) { MmUnmapIoSpace((PVOID)g_chipinfo, SZ_CHIPINFO);      g_chipinfo = NULL; }
    if (g_cmddb)    { MmUnmapIoSpace((PVOID)g_cmddb,    SZ_CMD_DB);        g_cmddb    = NULL; }

    LogClose();
    /* Return STATUS_UNSUCCESSFUL after logging so Demand-Start service auto-unloads cleanly */
    return STATUS_UNSUCCESSFUL;
}
