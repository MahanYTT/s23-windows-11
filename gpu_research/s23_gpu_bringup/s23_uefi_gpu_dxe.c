/** @file
  s23_uefi_gpu_dxe.c - Samsung Galaxy S23 (dm1q / SM8550-AC) Adreno 740 Pre-EBS UEFI DXE Driver

  Adapted from OnePlus Open firmware/G736r2/G290Pkg/G290GpuPower.c (G721/G722/G723/G736r2)
  and G991/OpenOwnerPkg/Publish.c specifically for Samsung Galaxy S23 (dm1q / SM8550-AC):
    1. Maps GCC (0x00100000), GPUCC (0x03D90000), KGSL_SMMU (0x03DA0000), CMD_DB (0x81C60000),
       and APPS_RSC_DRV2 (0x17A20000) via GCD with EFI_MEMORY_UC | EFI_MEMORY_XP.
    2. Dual-mode RPMh voltage rail voting:
       - Uses OEM EFI_NPA_PROTOCOL (/vcs/vdd_cx, /vcs/vdd_mxa, /vcs/vdd_mxc -> VLVL 384) if present.
       - Otherwise falls back automatically to direct APPS_RSC DRV2 TCS0 + CMD_DB ARC votes
         (cx.lvl, mx.lvl, mxc.lvl >= 0x180 TURBO; gfx.lvl >= 0x34 SVS), essential for Mu-dm1q
         standalone UEFI builds that do not ship OEM NPA/ClockDxe drivers.
    3. Executes the verified Kalama (SM8550) root-first GPU clock & CX bring-up sequence:
       - Observe-only GCC_GPU_BCR (0x00171000) guard (refuses if bit 0 asserted).
       - Enable GCC_GPU_CFG_AHB (0x00171004) + APCS GPLL0 vote (0x00152018) + GPU GPLL0 branch
         (0x00152000 bit 15) and verify GPLL0 LOCK.
       - Enable GPUCC_CX_GDSC (0x03D99108) and poll HW_CTRL (0x03D9953C) / GDSCR bit 31.
       - Enable SMMU/NoC clocks: HLOS1_VOTE_GPU_SMMU (0x03D97000), GCC_GPU_MEMNOC_GFX (0x00171010),
         GCC_GPU_SNOC_DVM_GFX (0x00171018), GCC_DDRSS_GPU_AXI (0x00171154).
       - Program GPU_CC_HUB_CLK_SRC (0x03D993EC) -> 200 MHz (GPLL0/3) and enable HUB_AON,
         HUB_CX_INT, CRC_AHB, AHB, GPUCC_MEMNOC_GFX.
       - Program GPU_CC_FF_CLK_SRC (0x03D99474) -> 200 MHz (GPLL0/3) and enable CX_FF.
       - Configure & lock GPU_CC_PLL1 (0x03D91000, LUCID_OLE) at 440.0 MHz (L=0x16, ALPHA=0xEAAA).
       - Clear CX_GMU (0x03D9913C), CXO (0x03D99144), and GX_GMU (0x03D990BC) bit 0 per G722-r2
         so the Windows OpenSmmuD0 broker clock FSM (gmu_clock.c) sees clean cold state.
       - Observe (do not forcibly mutate) GX_GDSCR (0x03D9905C) / GX_CFG_GDSCR (0x03D99060).
    4. Publishes a checksummed 'S23G' ACPI telemetry table and optionally installs s23_gpu_acpi.aml
       when compiled with -DS23_INCLUDE_GPU_SSDT_AML.

  DO NOT modify ~/mu_s23/Mu-Silicium from this standalone staging file.
**/

#ifdef S23_STANDALONE_SYNTAX_CHECK
#include <stdint.h>
#include <stddef.h>
typedef char      CHAR8;
typedef uint8_t   UINT8;
typedef uint16_t  UINT16;
typedef int32_t   INT32;
typedef uint32_t  UINT32;
typedef uint64_t  UINT64;
typedef uintptr_t UINTN;
typedef uint8_t   BOOLEAN;
typedef void      VOID;
typedef void     *EFI_HANDLE;
typedef UINT64    EFI_STATUS;
typedef UINT64    EFI_PHYSICAL_ADDRESS;
typedef struct { UINT32 Data1; UINT16 Data2; UINT16 Data3; UINT8 Data4[8]; } EFI_GUID;

#define TRUE  1
#define FALSE 0
#define IN
#define OUT
#define CONST  const
#define STATIC static
#define EFIAPI
#define MAX_UINT32             0xFFFFFFFFU
#define EFI_SUCCESS            0ULL
#define EFI_ERROR(s)           (((INT32)((s) >> 63)) != 0)
#define EFI_UNSUPPORTED        0x8000000000000003ULL
#define EFI_DEVICE_ERROR       0x8000000000000007ULL
#define EFI_ACCESS_DENIED      0x800000000000000FULL
#define EFI_TIMEOUT            0x8000000000000012ULL
#define EFI_NOT_READY          0x8000000000000006ULL
#define EFI_ALREADY_STARTED    0x8000000000000014ULL
#define EFI_MEMORY_UC          0x0000000000000001ULL
#define EFI_MEMORY_XP          0x0000000000004000ULL
#define SIGNATURE_32(A,B,C,D)  ((UINT32)(A) | ((UINT32)(B)<<8) | ((UINT32)(C)<<16) | ((UINT32)(D)<<24))
#define SIGNATURE_64(A,B,C,D,E,F,G,H) ((UINT64)SIGNATURE_32(A,B,C,D) | ((UINT64)SIGNATURE_32(E,F,G,H)<<32))
#define DEBUG_INFO             0x00000040U
#define DEBUG_ERROR            0x80000000U
#define DEBUG(Expression)      do { (VOID)0; } while (0)

typedef enum {
  EfiGcdMemoryTypeNonExistent = 0,
  EfiGcdMemoryTypeReserved,
  EfiGcdMemoryTypeSystemMemory,
  EfiGcdMemoryTypeMemoryMappedIo
} EFI_GCD_MEMORY_TYPE;

typedef struct {
  EFI_PHYSICAL_ADDRESS BaseAddress;
  UINT64               Length;
  UINT64               Capabilities;
  UINT64               Attributes;
  EFI_GCD_MEMORY_TYPE  GcdMemoryType;
  EFI_HANDLE           ImageHandle;
  EFI_HANDLE           DeviceHandle;
} EFI_GCD_MEMORY_SPACE_DESCRIPTOR;

typedef struct {
  EFI_STATUS (EFIAPI *AddMemorySpace)(EFI_GCD_MEMORY_TYPE, EFI_PHYSICAL_ADDRESS, UINT64, UINT64);
  EFI_STATUS (EFIAPI *GetMemorySpaceDescriptor)(EFI_PHYSICAL_ADDRESS, EFI_GCD_MEMORY_SPACE_DESCRIPTOR *);
  EFI_STATUS (EFIAPI *SetMemorySpaceAttributes)(EFI_PHYSICAL_ADDRESS, UINT64, UINT64);
} EFI_DXE_SERVICES;

typedef struct {
  EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *, VOID *, VOID **);
  EFI_STATUS (EFIAPI *Stall)(UINTN);
} EFI_BOOT_SERVICES;

typedef struct { UINTN Dummy; } EFI_SYSTEM_TABLE;

typedef struct {
  EFI_STATUS (EFIAPI *InstallAcpiTable)(VOID *, VOID *, UINTN, UINTN *);
} EFI_ACPI_TABLE_PROTOCOL;

extern EFI_BOOT_SERVICES *gBS;
extern EFI_DXE_SERVICES  *gDS;
extern EFI_GUID           gEfiAcpiTableProtocolGuid;

extern UINT32 MmioRead32(UINTN Address);
extern UINT32 MmioWrite32(UINTN Address, UINT32 Value);
extern VOID   MemoryFence(VOID);
extern UINTN  MicroSecondDelay(UINTN MicroSeconds);
extern VOID  *ZeroMem(VOID *Buffer, UINTN Length);
extern VOID  *CopyMem(VOID *DestinationBuffer, CONST VOID *SourceBuffer, UINTN Length);
extern UINT8  CalculateCheckSum8(CONST UINT8 *Buffer, UINTN Length);
#else
#include <Uefi.h>
#include <PiDxe.h>
#include <Protocol/AcpiTable.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#endif

#ifdef S23_INCLUDE_GPU_SSDT_AML
#include "s23_gpu_acpi.hex"
#endif

/* ============================================================================
 * S23 (dm1q / SM8550-AC) Physical Addresses & Register Offsets
 * ============================================================================ */
#define S23_PA_GCC              0x00100000ULL
#define S23_SZ_GCC              0x001F5000ULL
#define S23_PA_GPUCC            0x03D90000ULL
#define S23_SZ_GPUCC            0x0000A000ULL
#define S23_PA_KGSL_SMMU        0x03DA0000ULL
#define S23_SZ_KGSL_SMMU        0x00040000ULL
#define S23_PA_APPS_RSC_DRV2    0x17A20000ULL
#define S23_SZ_APPS_RSC_DRV2    0x00002000ULL
#define S23_PA_CMD_DB           0x81C60000ULL
#define S23_SZ_CMD_DB           0x00020000ULL

/* GCC registers */
#define S23_GCC_GPLL0_MODE      0x00100000ULL
#define S23_GCC_APCS_CLK_VOTE   0x00152000ULL
#define S23_GCC_APCS_GPLL_VOTE  0x00152018ULL
#define S23_GCC_GPU_BCR         0x00171000ULL
#define S23_GCC_GPU_CFG_AHB     0x00171004ULL
#define S23_GCC_GPU_MEMNOC_GFX  0x00171010ULL
#define S23_GCC_GPU_SNOC_DVM    0x00171018ULL
#define S23_GCC_DDRSS_GPU_AXI   0x00171154ULL

/* GPUCC registers */
#define S23_GPUCC_PLL1_MODE     0x03D91000ULL
#define S23_GPUCC_PLL1_OPMODE   0x03D91004ULL
#define S23_GPUCC_PLL1_L_VAL    0x03D91010ULL
#define S23_GPUCC_PLL1_ALPHA    0x03D91014ULL
#define S23_GPUCC_PLL1_USER     0x03D91018ULL
#define S23_GPUCC_PLL1_USER_U   0x03D9101CULL
#define S23_GPUCC_PLL1_CFG      0x03D91020ULL
#define S23_GPUCC_PLL1_CFG_U    0x03D91024ULL
#define S23_GPUCC_PLL1_CFG_U1   0x03D91028ULL
#define S23_GPUCC_PLL1_TEST_U   0x03D91030ULL
#define S23_GPUCC_PLL1_TEST_U1  0x03D91034ULL
#define S23_GPUCC_PLL1_TEST_U2  0x03D91038ULL

#define S23_GPUCC_HLOS1_SMMU    0x03D97000ULL
#define S23_GPUCC_GX_GDSCR      0x03D9905CULL
#define S23_GPUCC_GX_CFG_GDSCR  0x03D99060ULL
#define S23_GPUCC_GX_GMU_CBCR   0x03D990BCULL
#define S23_GPUCC_CX_BCR        0x03D99104ULL
#define S23_GPUCC_CX_GDSCR      0x03D99108ULL
#define S23_GPUCC_AHB_CBCR      0x03D9911CULL
#define S23_GPUCC_CRC_AHB_CBCR  0x03D99120ULL
#define S23_GPUCC_CX_GMU_CBCR   0x03D9913CULL
#define S23_GPUCC_CXO_CBCR      0x03D99144ULL
#define S23_GPUCC_HUB_CX_INT    0x03D99148ULL
#define S23_GPUCC_CX_FF_CBCR    0x03D9914CULL
#define S23_GPUCC_MEMNOC_GFX    0x03D99150ULL
#define S23_GPUCC_GMU_RCG_CMD   0x03D99318ULL
#define S23_GPUCC_GMU_RCG_CFG   0x03D9931CULL
#define S23_GPUCC_HUB_AON_CBCR  0x03D993E8ULL
#define S23_GPUCC_HUB_RCG_CMD   0x03D993ECULL
#define S23_GPUCC_HUB_RCG_CFG   0x03D993F0ULL
#define S23_GPUCC_FF_RCG_CMD    0x03D99474ULL
#define S23_GPUCC_FF_RCG_CFG    0x03D99478ULL
#define S23_GPUCC_CX_HW_CTRL    0x03D9953CULL

/* APPS_RSC DRV2 TCS0 */
#define S23_RSC_TCS_OFF         0xD00U
#define S23_V3_IRQ_EN           0x00U
#define S23_V3_IRQ_ST           0x04U
#define S23_V3_IRQ_CLR          0x08U
#define S23_V3_CTRL             0x24U
#define S23_V3_STATUS           0x28U
#define S23_V3_CMDEN            0x2CU
#define S23_V3_MSGID            0x34U
#define S23_V3_ADDR             0x38U
#define S23_V3_DATA             0x3CU
#define S23_AMC_ENABLE          (1U << 16)
#define S23_AMC_TRIGGER         (1U << 24)

/* Optional OEM NPA protocol ABI (G290GpuPower.c) */
typedef struct {
  UINT32 Type;
  UINT32 Pad;
  union {
    UINT32 State;
    UINT64 Raw;
  } Data;
} S23_NPA_QUERY;

typedef struct {
  UINT64 Version;
  UINT64 Unused8;
  EFI_STATUS (EFIAPI *Create)(CONST CHAR8 *, CONST CHAR8 *, UINT32, UINT32, VOID *, VOID **);
  UINT64 Unused24;
  UINT64 Unused32;
  EFI_STATUS (EFIAPI *Request)(VOID *, UINT32);
  UINT64 Unused48Through104[8];
  EFI_STATUS (EFIAPI *Available)(CONST CHAR8 *, UINT32 *);
  UINT64 Unused120Through168[7];
  EFI_STATUS (EFIAPI *Query)(CONST CHAR8 *, UINT32, S23_NPA_QUERY *, UINT32 *);
} S23_NPA_PROTOCOL;

STATIC EFI_GUID mNpaProtocolGuid = {
  0x79D6C870, 0x725E, 0x489E, { 0xA0, 0xA1, 0x27, 0xE7, 0xA5, 0xD0, 0xCB, 0x35 }
};

/* ============================================================================
 * 'S23G' ACPI Telemetry Table (Published for Windows Diagnostics)
 * ============================================================================ */
#pragma pack(push, 1)
typedef struct {
  UINT32 Signature;
  UINT32 Length;
  UINT8  Revision;
  UINT8  Checksum;
  CHAR8  OemId[6];
  UINT64 OemTableId;
  UINT32 OemRevision;
  UINT32 CreatorId;
  UINT32 CreatorRevision;
} S23_ACPI_HEADER;

typedef struct {
  S23_ACPI_HEADER Header;
  UINT32          Version;
  UINT32          Stage;
  UINT64          Status;
  UINT32          RailMethod;       /* 1 = EFI_NPA_PROTOCOL, 2 = Direct APPS_RSC TCS0 */
  UINT32          Flags;
  UINT32          GccBcr;
  UINT32          GccCfgAhb[2];
  UINT32          GpllMode;
  UINT32          CxGdscr[2];
  UINT32          CxHwCtrl[2];
  UINT32          GxGdscr[2];
  UINT32          GxCfgGdscr[2];
  UINT32          HubCmd[2];
  UINT32          HubCfg[2];
  UINT32          Ahb[2];
  UINT32          Pll1Mode;
  UINT32          Pll1OpMode;
  UINT32          Pll1UserCtl;
  UINT32          CxGmuAfter;
  UINT32          CxoAfter;
  UINT32          GxGmuAfter;
  UINT32          ArcAddr[4];       /* cx, mx, mxc, gfx */
  UINT32          ArcIndex[4];
} S23_GPU_POWER_REPORT;
#pragma pack(pop)

STATIC S23_GPU_POWER_REPORT mReport;
STATIC VOID                 *mNpaClients[3];
STATIC BOOLEAN              mAttempted = FALSE;

STATIC UINT32
ReadReg32 (
  IN UINT64 Pa
  )
{
  UINT32 Val;
  MemoryFence ();
  Val = MmioRead32 ((UINTN)Pa);
  MemoryFence ();
  return Val;
}

STATIC VOID
WriteReg32 (
  IN UINT64 Pa,
  IN UINT32 Val
  )
{
  MemoryFence ();
  MmioWrite32 ((UINTN)Pa, Val);
  MemoryFence ();
}

STATIC EFI_STATUS
PollReg32 (
  IN UINT64 Pa,
  IN UINT32 Mask,
  IN UINT32 Expected,
  IN UINTN  BudgetUs
  )
{
  UINTN Elapsed;
  for (Elapsed = 0; Elapsed <= BudgetUs; Elapsed += 5) {
    UINT32 Val = ReadReg32 (Pa);
    if (Val == MAX_UINT32) {
      return EFI_DEVICE_ERROR;
    }
    if ((Val & Mask) == Expected) {
      return EFI_SUCCESS;
    }
    MicroSecondDelay (5);
  }
  return EFI_TIMEOUT;
}

/* ============================================================================
 * GCD MMIO Mapping Page-by-Page (0x1000, UC | XP, matching G290GpuPower.c)
 * ============================================================================ */
STATIC EFI_STATUS
MapMmioRegion (
  IN EFI_PHYSICAL_ADDRESS Base,
  IN UINT64               Length
  )
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR Desc;
  EFI_STATUS                      Status;
  UINT64                          End = Base + Length;
  UINT64                          Addr;

  for (Addr = Base; Addr < End; Addr += 0x1000ULL) {
    UINT64 Attr;

    Status = gDS->GetMemorySpaceDescriptor (Addr, &Desc);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (Desc.GcdMemoryType == EfiGcdMemoryTypeNonExistent) {
      Status = gDS->AddMemorySpace (
                      EfiGcdMemoryTypeMemoryMappedIo,
                      Addr,
                      0x1000ULL,
                      EFI_MEMORY_UC | EFI_MEMORY_XP
                      );
      if (EFI_ERROR (Status)) {
        return Status;
      }
      Status = gDS->GetMemorySpaceDescriptor (Addr, &Desc);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    } else if (Desc.GcdMemoryType != EfiGcdMemoryTypeMemoryMappedIo &&
               Desc.GcdMemoryType != EfiGcdMemoryTypeReserved) {
      return EFI_ACCESS_DENIED;
    }

    Attr = EFI_MEMORY_UC | ((Desc.Capabilities & EFI_MEMORY_XP) ? EFI_MEMORY_XP : 0ULL);
    if (Desc.Attributes != Attr) {
      Status = gDS->SetMemorySpaceAttributes (Addr, 0x1000ULL, Attr);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
PrepareMappings (
  VOID
  )
{
  STATIC CONST struct {
    EFI_PHYSICAL_ADDRESS Base;
    UINT64               Length;
  } Regions[] = {
    { S23_PA_GCC,           S23_SZ_GCC           },
    { S23_PA_GPUCC,         S23_SZ_GPUCC         },
    { S23_PA_KGSL_SMMU,     S23_SZ_KGSL_SMMU     },
    { S23_PA_APPS_RSC_DRV2, S23_SZ_APPS_RSC_DRV2 }
  };
  UINTN Index;
  for (Index = 0; Index < sizeof (Regions) / sizeof (Regions[0]); Index++) {
    EFI_STATUS Status = MapMmioRegion (Regions[Index].Base, Regions[Index].Length);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }
  return EFI_SUCCESS;
}

/* ============================================================================
 * Direct CMD_DB ARC Lookup + APPS_RSC DRV2 TCS0 Fallback (for Mu-dm1q)
 * ============================================================================ */
STATIC BOOLEAN
CmdDbFindArcVote (
  IN  CONST CHAR8 *Name,
  IN  UINT16       MinVlvl,
  OUT UINT32      *AddrOut,
  OUT UINT32      *IdxOut
  )
{
  CONST volatile UINT8 *Db = (CONST volatile UINT8 *)(UINTN)S23_PA_CMD_DB;
  UINT32                NameLen = 0;
  UINT32                I;

  *AddrOut = 0;
  *IdxOut  = 0;

  if (Db[4] != 0xDB || Db[5] != 0x30 || Db[6] != 0x03 || Db[7] != 0x0C) {
    return FALSE;
  }
  while (Name[NameLen] != '\0' && NameLen < 8U) {
    NameLen++;
  }

  for (I = 0; I < 8U; I++) {
    CONST volatile UINT8 *R      = Db + 8U + 16U * I;
    UINT16                SlvId  = *(CONST volatile UINT16 *)(R + 0);
    UINT16                HdrOff = *(CONST volatile UINT16 *)(R + 2);
    UINT16                DatOff = *(CONST volatile UINT16 *)(R + 4);
    UINT16                Cnt    = *(CONST volatile UINT16 *)(R + 6);
    UINT32                J;

    if (SlvId == 0U) {
      break;
    }
    if (144U + (UINT32)HdrOff + 24U * (UINT32)Cnt > S23_SZ_CMD_DB) {
      continue;
    }
    for (J = 0; J < Cnt; J++) {
      CONST volatile UINT8 *E = Db + 144U + HdrOff + 24U * J;
      BOOLEAN               Match = TRUE;
      UINT32                K;

      for (K = 0; K < 8U; K++) {
        UINT8 Ch = (K < NameLen) ? (UINT8)Name[K] : 0U;
        if (E[K] != Ch) {
          Match = FALSE;
          break;
        }
      }
      if (Match) {
        UINT32 Addr   = *(CONST volatile UINT32 *)(E + 16);
        UINT16 AuxLen = *(CONST volatile UINT16 *)(E + 20);
        UINT16 AuxOff = *(CONST volatile UINT16 *)(E + 22);
        UINT32 AbsOff = 144U + (UINT32)DatOff + (UINT32)AuxOff;
        UINT32 Best   = 0;

        *AddrOut = Addr;
        if (AuxLen >= 2U && AbsOff + AuxLen <= S23_SZ_CMD_DB) {
          CONST volatile UINT16 *Aux = (CONST volatile UINT16 *)(Db + AbsOff);
          UINT32                 Num = (AuxLen >> 1);
          UINT32                 Idx;
          if (Num > 16U) {
            Num = 16U;
          }
          for (Idx = 0; Idx < Num; Idx++) {
            UINT16 Vlvl = Aux[Idx];
            if (Idx > 0U && Aux[Idx - 1U] != 0U && Vlvl == 0U) {
              break;
            }
            if (Vlvl != 0U) {
              Best = Idx;
            }
            if (Vlvl >= MinVlvl) {
              *IdxOut = Idx;
              return TRUE;
            }
          }
        }
        *IdxOut = Best;
        return (Addr != 0U);
      }
    }
  }
  return FALSE;
}

STATIC EFI_STATUS
RpmhWriteTcs0 (
  IN UINT32 Addr,
  IN UINT32 Data
  )
{
  CONST UINT64 Base = S23_PA_APPS_RSC_DRV2;
  UINT32       SvIrqEn, SvCmdEn;
  UINT32       SvMsg[4], SvAddr[4], SvData[4];
  UINT32       I, En, St = 0;
  EFI_STATUS   IdleSt;

  if (Addr == 0U) {
    return EFI_UNSUPPORTED;
  }

  /* 1. Wait up to 10 ms for TCS0 controller to report idle (STATUS +0x28 bit 0 == 1) */
  IdleSt = PollReg32 (Base + S23_RSC_TCS_OFF + S23_V3_STATUS, 1U, 1U, 10000U);
  if (EFI_ERROR (IdleSt)) {
    return EFI_NOT_READY;
  }

  /* 2. Save IRQ_EN, CMDEN, and slot 0..3 command registers */
  SvIrqEn = ReadReg32 (Base + S23_RSC_TCS_OFF + S23_V3_IRQ_EN);
  SvCmdEn = ReadReg32 (Base + S23_RSC_TCS_OFF + S23_V3_CMDEN);
  for (I = 0; I < 4U; I++) {
    UINT32 Off = S23_RSC_TCS_OFF + 24U * I;
    SvMsg[I]  = ReadReg32 (Base + Off + S23_V3_MSGID);
    SvAddr[I] = ReadReg32 (Base + Off + S23_V3_ADDR);
    SvData[I] = ReadReg32 (Base + Off + S23_V3_DATA);
  }

  WriteReg32 (Base + S23_RSC_TCS_OFF + S23_V3_IRQ_CLR, 1U);
  if ((SvIrqEn & 1U) == 0U) {
    WriteReg32 (Base + S23_RSC_TCS_OFF + S23_V3_IRQ_EN, SvIrqEn | 1U);
  }

  WriteReg32 (Base + S23_RSC_TCS_OFF + S23_V3_CMDEN, 0U);
  WriteReg32 (Base + S23_RSC_TCS_OFF + S23_V3_MSGID, 8U | 0x100U | 0x10000U);
  WriteReg32 (Base + S23_RSC_TCS_OFF + S23_V3_ADDR, Addr);
  WriteReg32 (Base + S23_RSC_TCS_OFF + S23_V3_DATA, Data);
  WriteReg32 (Base + S23_RSC_TCS_OFF + S23_V3_CMDEN, 1U);

  En = ReadReg32 (Base + S23_RSC_TCS_OFF + S23_V3_CTRL);
  En &= ~S23_AMC_TRIGGER; WriteReg32 (Base + S23_RSC_TCS_OFF + S23_V3_CTRL, En); (VOID)ReadReg32 (Base + S23_RSC_TCS_OFF + S23_V3_CTRL);
  En &= ~S23_AMC_ENABLE;  WriteReg32 (Base + S23_RSC_TCS_OFF + S23_V3_CTRL, En); (VOID)ReadReg32 (Base + S23_RSC_TCS_OFF + S23_V3_CTRL);
  En |= S23_AMC_ENABLE;   WriteReg32 (Base + S23_RSC_TCS_OFF + S23_V3_CTRL, En); (VOID)ReadReg32 (Base + S23_RSC_TCS_OFF + S23_V3_CTRL);
  En |= S23_AMC_TRIGGER;  WriteReg32 (Base + S23_RSC_TCS_OFF + S23_V3_CTRL, En);

  for (I = 0; I < 100000U; I++) {
    St = ReadReg32 (Base + S23_RSC_TCS_OFF + S23_V3_IRQ_ST);
    if ((St & 1U) != 0U) {
      break;
    }
    MicroSecondDelay (1);
  }

  /* 3. Confirm TCS0 idle, then restore MSGID/ADDR/DATA BEFORE restoring V3_CMDEN */
  IdleSt = PollReg32 (Base + S23_RSC_TCS_OFF + S23_V3_STATUS, 1U, 1U, 10000U);
  if (!EFI_ERROR (IdleSt)) {
    WriteReg32 (Base + S23_RSC_TCS_OFF + S23_V3_CMDEN, 0U);
    for (I = 0; I < 4U; I++) {
      UINT32 Off = S23_RSC_TCS_OFF + 24U * I;
      WriteReg32 (Base + Off + S23_V3_MSGID, SvMsg[I]);
      WriteReg32 (Base + Off + S23_V3_ADDR,  SvAddr[I]);
      WriteReg32 (Base + Off + S23_V3_DATA,  SvData[I]);
    }
    WriteReg32 (Base + S23_RSC_TCS_OFF + S23_V3_CMDEN, SvCmdEn);
  }
  WriteReg32 (Base + S23_RSC_TCS_OFF + S23_V3_IRQ_CLR, 1U);
  if (ReadReg32 (Base + S23_RSC_TCS_OFF + S23_V3_IRQ_EN) != SvIrqEn) {
    WriteReg32 (Base + S23_RSC_TCS_OFF + S23_V3_IRQ_EN, SvIrqEn);
  }

  return ((St & 1U) != 0U && !EFI_ERROR (IdleSt)) ? EFI_SUCCESS : EFI_TIMEOUT;
}

STATIC EFI_STATUS
VoteGpuRails (
  VOID
  )
{
  S23_NPA_PROTOCOL *Npa = NULL;
  EFI_STATUS        Status;
  UINTN             I;

  /* Mode 1: Use OEM EFI_NPA_PROTOCOL if available in FV */
  Status = gBS->LocateProtocol (&mNpaProtocolGuid, NULL, (VOID **)&Npa);
  if (!EFI_ERROR (Status) && Npa != NULL && Npa->Version == 0x10003U &&
      Npa->Create != NULL && Npa->Request != NULL && Npa->Available != NULL) {
    STATIC CONST CHAR8 *NpaNames[3]   = { "/vcs/vdd_cx", "/vcs/vdd_mxa", "/vcs/vdd_mxc" };
    STATIC CONST CHAR8 *ClientNames[3] = { "S23-GPU-CX", "S23-GPU-MXA", "S23-GPU-MXC" };
    mReport.RailMethod = 1U;
    for (I = 0; I < 3U; I++) {
      UINT32 Avail = MAX_UINT32;
      Status = Npa->Available (NpaNames[I], &Avail);
      if (EFI_ERROR (Status) || Avail != 0U) {
        goto fallback_rpmh;
      }
      Status = Npa->Create (NpaNames[I], ClientNames[I], 0x40U, 0U, NULL, &mNpaClients[I]);
      if (EFI_ERROR (Status) || mNpaClients[I] == NULL) {
        goto fallback_rpmh;
      }
      Status = Npa->Request (mNpaClients[I], 384U); /* TURBO 384 VLVL proxy */
      if (EFI_ERROR (Status)) {
        goto fallback_rpmh;
      }
    }
    return EFI_SUCCESS;
  }

fallback_rpmh:
  /* Mode 2: Direct APPS_RSC DRV2 TCS0 + CMD_DB ARC votes (Mu-dm1q standalone path) */
  mReport.RailMethod = 2U;
  {
    STATIC CONST struct {
      CONST CHAR8 *Name;
      UINT16       MinVlvl;
    } Rails[4] = {
      { "cx.lvl",  0x180U },
      { "mx.lvl",  0x180U },
      { "mxc.lvl", 0x180U },
      { "gfx.lvl", 0x034U }
    };
    for (I = 0; I < 4U; I++) {
      UINT32 Addr = 0, Idx = 0;
      if (CmdDbFindArcVote (Rails[I].Name, Rails[I].MinVlvl, &Addr, &Idx)) {
        mReport.ArcAddr[I]  = Addr;
        mReport.ArcIndex[I] = Idx;
        Status = RpmhWriteTcs0 (Addr, Idx);
        if (EFI_ERROR (Status) && I < 3U) {
          return Status;
        }
      } else if (I < 3U) {
        return EFI_NOT_READY;
      }
    }
  }
  MicroSecondDelay (2000);
  return EFI_SUCCESS;
}

/* ============================================================================
 * Kalama GPUCC RCG / Branch / PLL1 Bring-Up (G721 / G722 / G723 / G736r2)
 * ============================================================================ */
STATIC EFI_STATUS
ProgramRcg (
  IN UINT64 CmdPa,
  IN UINT32 CfgVal
  )
{
  UINT32     Cmd = ReadReg32 (CmdPa);
  UINT32     Cfg, NewCfg;
  BOOLEAN    ForcedRoot = FALSE;
  EFI_STATUS Status;

  if (Cmd == MAX_UINT32) {
    return EFI_DEVICE_ERROR;
  }
  if ((Cmd & 2U) == 0U) {
    WriteReg32 (CmdPa, Cmd | 2U);
    ForcedRoot = TRUE;
  }
  Status = PollReg32 (CmdPa, 0x80000000U, 0U, 500);
  if (EFI_ERROR (Status)) {
    if (ForcedRoot) {
      WriteReg32 (CmdPa, ReadReg32 (CmdPa) & ~2U);
    }
    return Status;
  }
  Cfg = ReadReg32 (CmdPa + 4U);
  if (Cfg == MAX_UINT32) {
    if (ForcedRoot) {
      WriteReg32 (CmdPa, ReadReg32 (CmdPa) & ~2U);
    }
    return EFI_DEVICE_ERROR;
  }
  NewCfg = (Cfg & ~0x0010371FU) | CfgVal;
  if (NewCfg != Cfg) {
    WriteReg32 (CmdPa + 4U, NewCfg);
  }
  Cmd = ReadReg32 (CmdPa);
  WriteReg32 (CmdPa, Cmd | 1U);
  Status = PollReg32 (CmdPa, 1U, 0U, 500);
  if (ForcedRoot) {
    WriteReg32 (CmdPa, ReadReg32 (CmdPa) & ~2U);
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (ReadReg32 (CmdPa + 4U) != NewCfg) {
    return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
EnableBranch (
  IN UINT64  Pa,
  IN BOOLEAN AllowHwcg,
  IN BOOLEAN DelayOnly
  )
{
  UINT32 Val = ReadReg32 (Pa);
  UINTN  Elapsed;

  if (Val == MAX_UINT32) {
    return EFI_DEVICE_ERROR;
  }
  if ((Val & 1U) == 0U) {
    WriteReg32 (Pa, Val | 1U);
  }
  if (DelayOnly) {
    MicroSecondDelay (15);
    return EFI_SUCCESS;
  }
  for (Elapsed = 0; Elapsed <= 500; Elapsed += 5) {
    Val = ReadReg32 (Pa);
    if (Val == MAX_UINT32) {
      return EFI_DEVICE_ERROR;
    }
    if ((Val & 1U) != 0U && ((Val & 0x80000000U) == 0U || (AllowHwcg && (Val & 2U) != 0U))) {
      return EFI_SUCCESS;
    }
    MicroSecondDelay (5);
  }
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
LockGpuccPll1At440MHz (
  VOID
  )
{
  UINT32     Mode  = ReadReg32 (S23_GPUCC_PLL1_MODE);
  UINT32     Op    = ReadReg32 (S23_GPUCC_PLL1_OPMODE);
  UINT32     LVal  = ReadReg32 (S23_GPUCC_PLL1_L_VAL);
  UINT32     Alpha = ReadReg32 (S23_GPUCC_PLL1_ALPHA);
  UINT32     User  = ReadReg32 (S23_GPUCC_PLL1_USER);
  EFI_STATUS Status;

  if (Mode != MAX_UINT32 && (Mode & 0x80000005U) == 0x80000005U && Op == 1U &&
      (User & 1U) != 0U && (LVal & 0xFFFFU) == 0x16U && (Alpha & 0xFFFFU) == 0xEAAAU) {
    mReport.Flags |= (1U << 13); /* PLL1 already locked */
    return EFI_SUCCESS;
  }

  /* Lucid OLE safe sequence: clear OUTCTRL and switch to STANDBY BEFORE writing L/ALPHA/CFG/TEST */
  if (Mode == MAX_UINT32) {
    return EFI_DEVICE_ERROR;
  }
  WriteReg32 (S23_GPUCC_PLL1_MODE, Mode & ~1U); /* OUTCTRL off */
  WriteReg32 (S23_GPUCC_PLL1_OPMODE, 0U);       /* STANDBY */
  MicroSecondDelay (2);

  /* Exact gpucc-kalama.c LUCID_OLE 440.0 MHz config (19.2 * (22 + 0xEAAA/65536)) */
  WriteReg32 (S23_GPUCC_PLL1_L_VAL,   0x44440016U);
  WriteReg32 (S23_GPUCC_PLL1_ALPHA,   0x0000EAAAU);
  WriteReg32 (S23_GPUCC_PLL1_CFG,     0x20485699U);
  WriteReg32 (S23_GPUCC_PLL1_CFG_U,   0x00182261U);
  WriteReg32 (S23_GPUCC_PLL1_CFG_U1,  0x82AA299CU);
  WriteReg32 (S23_GPUCC_PLL1_USER_U,  0x00000005U);
  WriteReg32 (S23_GPUCC_PLL1_TEST_U,  0x00000003U);
  WriteReg32 (S23_GPUCC_PLL1_TEST_U1, 0x00009000U);
  WriteReg32 (S23_GPUCC_PLL1_TEST_U2, 0x00000034U);

  Mode = ReadReg32 (S23_GPUCC_PLL1_MODE);
  if (Mode == MAX_UINT32) {
    return EFI_DEVICE_ERROR;
  }
  WriteReg32 (S23_GPUCC_PLL1_MODE, Mode | 4U);  /* RESET_N de-assert */
  WriteReg32 (S23_GPUCC_PLL1_OPMODE, 1U);       /* RUN */

  Status = PollReg32 (S23_GPUCC_PLL1_MODE, 0x80000000U, 0x80000000U, 1500);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  User = ReadReg32 (S23_GPUCC_PLL1_USER);
  WriteReg32 (S23_GPUCC_PLL1_USER, User | 1U);
  Mode = ReadReg32 (S23_GPUCC_PLL1_MODE);
  WriteReg32 (S23_GPUCC_PLL1_MODE, Mode | 1U);  /* OUTCTRL on */

  Mode = ReadReg32 (S23_GPUCC_PLL1_MODE);
  if ((Mode & 0x80000005U) != 0x80000005U || ReadReg32 (S23_GPUCC_PLL1_OPMODE) != 1U) {
    return EFI_DEVICE_ERROR;
  }
  mReport.Flags |= (1U << 12); /* PLL1 programmed */
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
PrepareS23GpuClocks (
  VOID
  )
{
  EFI_STATUS Status;
  UINT32     Val;

  mReport.Stage = 1U;
  Status = PrepareMappings ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  /* Observe-only GCC_GPU_BCR check */
  mReport.GccBcr = ReadReg32 (S23_GCC_GPU_BCR);
  if (mReport.GccBcr == MAX_UINT32 || (mReport.GccBcr & 1U) != 0U) {
    return EFI_ACCESS_DENIED;
  }

  mReport.Stage = 2U;
  Status = VoteGpuRails ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  /* Enable GCC_GPU_CFG_AHB + GPLL0 votes */
  mReport.Stage = 3U;
  mReport.GccCfgAhb[0] = ReadReg32 (S23_GCC_GPU_CFG_AHB);
  Status = EnableBranch (S23_GCC_GPU_CFG_AHB, TRUE, FALSE);
  mReport.GccCfgAhb[1] = ReadReg32 (S23_GCC_GPU_CFG_AHB);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Val = ReadReg32 (S23_GCC_APCS_GPLL_VOTE);
  if (Val != MAX_UINT32 && (Val & 1U) == 0U) {
    WriteReg32 (S23_GCC_APCS_GPLL_VOTE, Val | 1U);
  }
  Status = PollReg32 (S23_GCC_GPLL0_MODE, 0x80000000U, 0x80000000U, 500);
  mReport.GpllMode = ReadReg32 (S23_GCC_GPLL0_MODE);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Val = ReadReg32 (S23_GCC_APCS_CLK_VOTE);
  if (Val != MAX_UINT32 && (Val & (1U << 15)) == 0U) {
    WriteReg32 (S23_GCC_APCS_CLK_VOTE, Val | (1U << 15));
    MicroSecondDelay (10);
  }

  /* Capture GX GDSC state (observe-only per G736r2; never force GX writes pre-EBS) */
  mReport.GxGdscr[0]    = ReadReg32 (S23_GPUCC_GX_GDSCR);
  mReport.GxCfgGdscr[0] = ReadReg32 (S23_GPUCC_GX_CFG_GDSCR);

  /* Power on GPUCC_CX_GDSC */
  mReport.Stage = 4U;
  mReport.CxGdscr[0]  = ReadReg32 (S23_GPUCC_CX_GDSCR);
  mReport.CxHwCtrl[0] = ReadReg32 (S23_GPUCC_CX_HW_CTRL);
  if (mReport.CxGdscr[0] == MAX_UINT32 || (mReport.CxGdscr[0] & 2U) != 0U) {
    return EFI_ACCESS_DENIED;
  }
  if ((mReport.CxGdscr[0] & 1U) != 0U) {
    WriteReg32 (S23_GPUCC_CX_GDSCR, mReport.CxGdscr[0] & ~1U);
    MicroSecondDelay (2);
  }
  Status = PollReg32 (S23_GPUCC_CX_HW_CTRL, 0x80000000U, 0x80000000U, 1500);
  if (EFI_ERROR (Status)) {
    Status = PollReg32 (S23_GPUCC_CX_GDSCR, 0x80000000U, 0x80000000U, 1500);
  }
  mReport.CxGdscr[1]  = ReadReg32 (S23_GPUCC_CX_GDSCR);
  mReport.CxHwCtrl[1] = ReadReg32 (S23_GPUCC_CX_HW_CTRL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  /* SMMU/NoC/HUB/FF/AHB root-first sequence (G721 + G723) */
  mReport.Stage = 5U;
  Status = EnableBranch (S23_GPUCC_HLOS1_SMMU, FALSE, FALSE);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  (VOID)EnableBranch (S23_GCC_GPU_MEMNOC_GFX, TRUE,  FALSE);
  (VOID)EnableBranch (S23_GCC_GPU_SNOC_DVM,   FALSE, TRUE);
  (VOID)EnableBranch (S23_GCC_DDRSS_GPU_AXI,  FALSE, TRUE);

  mReport.HubCmd[0] = ReadReg32 (S23_GPUCC_HUB_RCG_CMD);
  mReport.HubCfg[0] = ReadReg32 (S23_GPUCC_HUB_RCG_CFG);
  Status = ProgramRcg (S23_GPUCC_HUB_RCG_CMD, (5U << 8) | 5U | 0x00100000U);
  mReport.HubCmd[1] = ReadReg32 (S23_GPUCC_HUB_RCG_CMD);
  mReport.HubCfg[1] = ReadReg32 (S23_GPUCC_HUB_RCG_CFG);
  if (EFI_ERROR (Status)) {
    mReport.Flags |= (1U << 4);
    return Status;
  }

  (VOID)EnableBranch (S23_GPUCC_HUB_AON_CBCR, FALSE, FALSE);
  (VOID)EnableBranch (S23_GPUCC_HUB_CX_INT,   FALSE, FALSE);
  (VOID)EnableBranch (S23_GPUCC_CRC_AHB_CBCR, FALSE, FALSE);
  mReport.Ahb[0] = ReadReg32 (S23_GPUCC_AHB_CBCR);
  (VOID)EnableBranch (S23_GPUCC_AHB_CBCR,     FALSE, TRUE);
  mReport.Ahb[1] = ReadReg32 (S23_GPUCC_AHB_CBCR);
  (VOID)EnableBranch (S23_GPUCC_MEMNOC_GFX,   FALSE, FALSE);

  (VOID)ProgramRcg (S23_GPUCC_FF_RCG_CMD, (5U << 8) | 5U | 0x00100000U);
  (VOID)EnableBranch (S23_GPUCC_CX_FF_CBCR, FALSE, FALSE);

  /* Lock GPU_CC_PLL1 at 440.0 MHz (G722) */
  mReport.Stage = 6U;
  Status = LockGpuccPll1At440MHz ();
  mReport.Pll1Mode    = ReadReg32 (S23_GPUCC_PLL1_MODE);
  mReport.Pll1OpMode  = ReadReg32 (S23_GPUCC_PLL1_OPMODE);
  mReport.Pll1UserCtl = ReadReg32 (S23_GPUCC_PLL1_USER);
  if (EFI_ERROR (Status)) {
    mReport.Flags |= (1U << 11);
    return Status;
  }

  /* Clear broker-owned GMU leaves (G722-r2) so Windows gmu_clock.c finds clean cold state */
  {
    STATIC CONST UINT64 BrokerLeaves[3] = {
      S23_GPUCC_CX_GMU_CBCR,
      S23_GPUCC_CXO_CBCR,
      S23_GPUCC_GX_GMU_CBCR
    };
    UINTN I;
    for (I = 0; I < 3U; I++) {
      Val = ReadReg32 (BrokerLeaves[I]);
      if (Val != MAX_UINT32 && (Val & 1U) != 0U) {
        WriteReg32 (BrokerLeaves[I], Val & ~1U);
        MicroSecondDelay (5);
      }
    }
  }

  mReport.CxGmuAfter    = ReadReg32 (S23_GPUCC_CX_GMU_CBCR);
  mReport.CxoAfter      = ReadReg32 (S23_GPUCC_CXO_CBCR);
  mReport.GxGmuAfter    = ReadReg32 (S23_GPUCC_GX_GMU_CBCR);
  mReport.GxGdscr[1]    = ReadReg32 (S23_GPUCC_GX_GDSCR);
  mReport.GxCfgGdscr[1] = ReadReg32 (S23_GPUCC_GX_CFG_GDSCR);
  mReport.Stage         = 7U;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
S23GpuDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_ACPI_TABLE_PROTOCOL *Acpi = NULL;
  EFI_STATUS               Status;
  EFI_STATUS               AcpiStatus;
  UINTN                    TableKey;

  (VOID)ImageHandle;
  (VOID)SystemTable;

  if (mAttempted) {
    return EFI_ALREADY_STARTED;
  }
  mAttempted = TRUE;

  ZeroMem (&mReport, sizeof (mReport));
  mReport.Header.Signature       = SIGNATURE_32 ('S', '2', '3', 'G');
  mReport.Header.Length          = sizeof (mReport);
  mReport.Header.Revision        = 1;
  CopyMem (mReport.Header.OemId, "SAMSNG", 6);
  mReport.Header.OemTableId      = SIGNATURE_64 ('S', '2', '3', 'G', 'P', 'U', '7', '4');
  mReport.Header.OemRevision     = 1;
  mReport.Header.CreatorId       = SIGNATURE_32 ('D', 'M', '1', 'Q');
  mReport.Header.CreatorRevision = 1;
  mReport.Version                = 1;
  mReport.Status                 = EFI_NOT_READY;

  Status = PrepareS23GpuClocks ();
  mReport.Status = Status;
  mReport.Header.Checksum = 0;
  mReport.Header.Checksum = CalculateCheckSum8 ((CONST UINT8 *)&mReport, sizeof (mReport));

  AcpiStatus = gBS->LocateProtocol (&gEfiAcpiTableProtocolGuid, NULL, (VOID **)&Acpi);
  if (!EFI_ERROR (AcpiStatus) && Acpi != NULL) {
    (VOID)Acpi->InstallAcpiTable (Acpi, &mReport, sizeof (mReport), &TableKey);

#ifdef S23_INCLUDE_GPU_SSDT_AML
    if (!EFI_ERROR (Status)) {
      (VOID)Acpi->InstallAcpiTable (
                    Acpi,
                    (VOID *)s23_gpu_acpi_aml_code,
                    sizeof (s23_gpu_acpi_aml_code),
                    &TableKey
                    );
    }
#endif
  }

  DEBUG ((DEBUG_INFO, "S23GpuDxe: Stage=%u Status=%r RailMethod=%u\n", mReport.Stage, Status, mReport.RailMethod));
  /* Never abort DXE dispatch if GPU pre-EBS init fails; report status in S23G table */
  return EFI_SUCCESS;
}
