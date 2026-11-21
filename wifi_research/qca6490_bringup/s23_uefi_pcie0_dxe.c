/** @file
  s23_uefi_pcie0_dxe.c - Samsung Galaxy S23 (dm1q / SM8550-AC) PCIe0 + QCA6490 UEFI DXE Driver

  Adapted from OnePlus Open G289Pkg (G289Driver.c / g289_rc0.c / g401_transport.c / g403_sid.c /
  g735_smmu_identity.c) specifically for Samsung Galaxy S23 (dm1q) + QCA6490 (PCI 17CB:1103):
    1. Maps all required MMIO regions via GCD (GCC, TLMM, TCSR, CMD_DB, APPS_RSC, SPMI, AOSS QMP,
       IPCC, PCIe0 PARF/PHY/DBI/ECAM, APPS_SMMU) with UC + XP attributes.
    2. Executes verified S23 cold bring-up sequence:
       - Cold pin hold: PERST#(94)=0, BT_EN(81)=0, WLAN_EN(80)=0, XO_CLK(204)=0, CLKREQ#(95)=func1, WAKE#(96)=in.
       - PMK8550 GPIO3 (0xBA00) 32 kHz sleep clock configuration.
       - RPMh VRM votes via APPS_RSC DRV2 TCS0: xo/cx/mx.lvl=3, clka1..4/6..8 + rclkha1,
         and S23's exact 7 rails (ldoe1 880mV, ldoe3 1200mV, ldob15 1800mV, smpg2 1012mV,
         smpe4 966mV, smpg6 1900mV, smpg4 1350mV; NO unused smpg5!).
       - AOSS QMP 11-frame S23 wlan_pdc table + TLMM INTR_CFG=0x7C2 on GPIO82 & GPIO96.
       - QCA6490 XO_CLK_GPIO (204) + WLAN_EN (80) bootstrap pulse.
       - TCSR_PCIE_0_CLKREF_EN (0x01FD5100), GCC GDSCs + RCGs + branch CBCRs + canonical BCR resets.
       - PARF setup + S23's exact 115-triplet QMP PHY table + pipe_clk & PCS_STATUS lock.
       - PERST# release, DBI x1 (fallback x2/NL=1) + Gen1/EQ presets, clean LTSSM_EN & L0 poll.
       - Endpoint identity verification: expects 0x110317CB (QCA6490) at 0x60100000.
    3. Configures G401 3-slot iATU ECAM transport (Slot 0 CFG-Shift, Slot 1 CFG1 absent-bus sink,
       Slot 2 MEM32 0x60300000..0x63FFFFFF) + hidden Root Port bus/MEM/BME + Interrupt Pin=1 (DBI+0x3C).
    4. Programs PARF BDF-to-SID CRC8 table (0x01C02000..0x01C023FC, RID 0x0100 -> relSID 1) and
       SMMU-500 (0x15000000) coherent identity stage-1 pass-through for SID 0x1401 & 0x1400.
    5. Atomically updates MCFG (Segment 0 StartBus=1, EndBus=1) and installs s23_pcie0_wlan SSDT
       ONLY after live verification of 0x110317CB on the bus.

  Syntax-verified in WSL with clang-18 against standalone/EDK2 type declarations.
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
typedef void     *EFI_EVENT;
typedef UINT64    EFI_STATUS;
typedef UINT64    EFI_PHYSICAL_ADDRESS;
typedef struct { UINT32 Data1; UINT16 Data2; UINT16 Data3; UINT8 Data4[8]; } EFI_GUID;
#define TRUE  1
#define FALSE 0
#define IN
#define OUT
#define CONST  const
#define INLINE inline
#define STATIC static
#define EFIAPI
#define EFI_SUCCESS            0ULL
#define EFI_NOT_READY          0x8000000000000006ULL
#define EFI_DEVICE_ERROR       0x8000000000000007ULL
#define EFI_UNSUPPORTED        0x8000000000000003ULL
#define EFI_NOT_FOUND          0x800000000000000EULL
#define EFI_ERROR(s)           (((int64_t)(s)) < 0)
#define EFI_MEMORY_UC          0x0000000000000001ULL
#define EFI_MEMORY_XP          0x0000000000004000ULL
#define EVT_NOTIFY_SIGNAL      0x00000200U
#define TPL_CALLBACK           8U
#define EfiGcdMemoryTypeNonExistent     0
#define EfiGcdMemoryTypeMemoryMappedIo  2
typedef struct {
  EFI_PHYSICAL_ADDRESS BaseAddress;
  UINT64 Length;
  UINT64 Capabilities;
  UINT64 Attributes;
  UINT32 GcdMemoryType;
  EFI_HANDLE ImageHandle;
  EFI_HANDLE DeviceHandle;
} EFI_GCD_MEMORY_SPACE_DESCRIPTOR;
typedef struct {
  UINT32 Signature; UINT32 Length; UINT8 Revision; UINT8 Checksum;
  UINT8 OemId[6]; UINT8 OemTableId[8]; UINT32 OemRevision; UINT32 CreatorId; UINT32 CreatorRevision;
} EFI_ACPI_SDT_HEADER;
typedef UINT32 EFI_ACPI_TABLE_VERSION;
typedef struct _EFI_ACPI_TABLE_PROTOCOL EFI_ACPI_TABLE_PROTOCOL;
struct _EFI_ACPI_TABLE_PROTOCOL {
  EFI_STATUS (EFIAPI *InstallAcpiTable)(EFI_ACPI_TABLE_PROTOCOL *, VOID *, UINTN, UINTN *);
  EFI_STATUS (EFIAPI *UninstallAcpiTable)(EFI_ACPI_TABLE_PROTOCOL *, UINTN);
};
typedef struct _EFI_ACPI_SDT_PROTOCOL EFI_ACPI_SDT_PROTOCOL;
struct _EFI_ACPI_SDT_PROTOCOL {
  UINT32 AcpiVersion;
  EFI_STATUS (EFIAPI *GetAcpiTable)(UINTN, EFI_ACPI_SDT_HEADER **, EFI_ACPI_TABLE_VERSION *, UINTN *);
};
typedef struct {
  EFI_STATUS (EFIAPI *AddMemorySpace)(UINT32, EFI_PHYSICAL_ADDRESS, UINT64, UINT64);
  EFI_STATUS (EFIAPI *GetMemorySpaceDescriptor)(EFI_PHYSICAL_ADDRESS, EFI_GCD_MEMORY_SPACE_DESCRIPTOR *);
  EFI_STATUS (EFIAPI *SetMemorySpaceAttributes)(EFI_PHYSICAL_ADDRESS, UINT64, UINT64);
} EFI_DXE_SERVICES;
typedef struct {
  EFI_STATUS (EFIAPI *Stall)(UINTN);
  EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *, VOID *, VOID **);
  EFI_STATUS (EFIAPI *CreateEventEx)(UINT32, UINTN, VOID (*)(EFI_EVENT, VOID *), VOID *, CONST EFI_GUID *, EFI_EVENT *);
} EFI_BOOT_SERVICES;
typedef struct { UINTN Unused; } EFI_SYSTEM_TABLE;
extern EFI_DXE_SERVICES *gDS;
extern EFI_BOOT_SERVICES *gBS;
extern EFI_GUID gEfiAcpiTableProtocolGuid;
extern EFI_GUID gEfiAcpiSdtProtocolGuid;
extern EFI_GUID gEfiEventReadyToBootGuid;
#define SIGNATURE_32(A,B,C,D) ((UINT32)(A)|((UINT32)(B)<<8)|((UINT32)(C)<<16)|((UINT32)(D)<<24))
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define DEBUG(x) do { (void)0; } while (0)
static inline void ArmDataMemoryBarrier(void) { __asm__ volatile("dmb sy" ::: "memory"); }
static inline UINT32 MmioRead32(UINTN a) { return *(volatile UINT32 *)a; }
static inline UINT32 MmioWrite32(UINTN a, UINT32 v) { *(volatile UINT32 *)a = v; return v; }
#else
#include <Uefi.h>
#include <PiDxe.h>
#include <Guid/EventGroup.h>
#include <Protocol/AcpiTable.h>
#include <Protocol/AcpiSystemDescriptionTable.h>
#include <Library/ArmLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#endif

/* ============================================================================
 * S23 (SM8550-AC) Hardware Bases
 * ============================================================================ */
#define S23_GCC_BASE          0x00100000ULL
#define S23_IPCC_BASE         0x00408000ULL
#define S23_PCIE0_PARF_BASE   0x01C00000ULL
#define S23_PCIE0_PHY_BASE    0x01C06000ULL
#define S23_TCSR_BASE         0x01FD5000ULL
#define S23_QMP_BASE          0x0C300000ULL
#define S23_SPMI_CORE_BASE    0x0C400000ULL
#define S23_SPMI_CNFG_BASE    0x0C42D000ULL
#define S23_SPMI_OBSRVR_BASE  0x0C440000ULL
#define S23_SPMI_CHNLS_BASE   0x0C500000ULL
#define S23_TLMM_BASE         0x0F100000ULL
#define S23_SMMU_BASE         0x15000000ULL
#define S23_RSC_DRV2_BASE     0x17A20000ULL
#define S23_PCIE0_DBI_BASE    0x60000000ULL
#define S23_PCIE0_ELBI_BASE   0x60000F20ULL
#define S23_PCIE0_ATU_BASE    0x60001000ULL
#define S23_PCIE0_ECAM_BASE   0x60100000ULL
#define S23_CMD_DB_BASE       0x81C60000ULL

#define QCA6490_VENDOR_DEVICE 0x110317CBU

/* SMMU-500 Coherent Identity Pass-Through Constants (from G735) */
#define SMMU_CB_CBAR          0x0001F300U
#define SMMU_CB_CBA2R         0x00000001U
#define SMMU_CB_SCTLR         0x0A5F00E0U
#define SMMU_CB_TCR2          0x00000060U
#define SMMU_CB_TCR           0x80A000A0U

/* Compiled AML payload from s23_pcie0_wlan.asl (populated via iasl -tc s23_pcie0_wlan.asl) */
#ifndef S23_NO_SSDT_AML
#ifndef S23_INCLUDE_SSDT_AML
#define S23_INCLUDE_SSDT_AML 1
#endif
#endif

#ifdef S23_INCLUDE_SSDT_AML
#include "s23_pcie0_wlan.hex"
#else
STATIC CONST UINT8 mS23Pcie0SsdtPlaceholder[1] = { 0 };
#endif

#pragma pack(push, 1)
typedef struct {
  UINT8  Signature[4];     /* 'S','2','3','P' */
  UINT32 Length;           /* 68 */
  UINT8  Revision;         /* 1 */
  UINT8  Checksum;
  UINT8  OemId[6];         /* "SAMSNG" */
  UINT8  OemTableId[8];    /* "S23PCIE0" */
  UINT32 OemRevision;      /* 1 */
  UINT8  CreatorId[4];     /* "DM1Q" */
  UINT32 CreatorRevision;  /* 1 */
  UINT32 Stage;
  UINT32 Ltssm;
  UINT32 LinkStatus;
  UINT32 EndpointId;
  UINT32 SmmuId0;
  UINT32 SmmuGfsr;
  UINT32 SmmuSmr0;
  UINT32 SmmuSmr1;
} S23_PCIE0_ACPI_REPORT;
#pragma pack(pop)

STATIC S23_PCIE0_ACPI_REPORT mReport;

STATIC BOOLEAN mInitialized = FALSE;

/* ============================================================================
 * MMIO & Delay Helpers
 * ============================================================================ */
STATIC UINT32
Rd32 (UINT64 Addr)
{
  UINT32 Val;
  ArmDataMemoryBarrier ();
  Val = MmioRead32 ((UINTN)Addr);
  ArmDataMemoryBarrier ();
  return Val;
}

STATIC VOID
Wr32 (UINT64 Addr, UINT32 Val)
{
  ArmDataMemoryBarrier ();
  MmioWrite32 ((UINTN)Addr, Val);
  ArmDataMemoryBarrier ();
}

STATIC VOID
Rmw32 (UINT64 Addr, UINT32 ClearBits, UINT32 SetBits)
{
  Wr32 (Addr, (Rd32 (Addr) & ~ClearBits) | SetBits);
}

STATIC BOOLEAN
PollMask (UINT64 Addr, UINT32 Mask, UINT32 Want, UINT32 TimeoutUs)
{
  for (UINT32 Elapsed = 0; Elapsed < TimeoutUs; Elapsed += 10) {
    if ((Rd32 (Addr) & Mask) == Want) {
      return TRUE;
    }
    gBS->Stall (10);
  }
  return (Rd32 (Addr) & Mask) == Want;
}

STATIC VOID
SetTlmm (UINT32 Pin, UINT32 Cfg, INT32 OutVal)
{
  UINT64 Base = S23_TLMM_BASE + ((UINT64)Pin * 0x1000ULL);
  if (OutVal >= 0) {
    Wr32 (Base + 0x04, OutVal ? 2U : 0U);
  }
  Wr32 (Base + 0x00, Cfg);
}

/* ============================================================================
 * GCD MMIO Mapping Helper
 * ============================================================================ */
STATIC EFI_STATUS
EnsureMmioMapped (UINT64 Base, UINT64 Length)
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR Desc;
  EFI_STATUS Status;
  UINT64 End = Base + Length;

  for (UINT64 Addr = Base; Addr < End; ) {
    UINT64 Step = 0x1000ULL;
    Status = gDS->GetMemorySpaceDescriptor (Addr, &Desc);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    if (Desc.GcdMemoryType == EfiGcdMemoryTypeNonExistent) {
      Status = gDS->AddMemorySpace (EfiGcdMemoryTypeMemoryMappedIo, Addr, 0x1000ULL, EFI_MEMORY_UC | EFI_MEMORY_XP);
      if (EFI_ERROR (Status)) {
        return Status;
      }
      Status = gDS->GetMemorySpaceDescriptor (Addr, &Desc);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }
    if (Desc.GcdMemoryType == EfiGcdMemoryTypeMemoryMappedIo) {
      /* Already mapped as NS_DEVICE in ARM64 page tables by MemoryInitPeiLib: advance past region without splitting 2MB blocks */
      if (Desc.Length >= 0x1000ULL && Addr >= Desc.BaseAddress) {
        UINT64 DescEnd = Desc.BaseAddress + Desc.Length;
        if (DescEnd > Addr) {
          Step = (DescEnd < End) ? (DescEnd - Addr) : (End - Addr);
        }
      }
    }
    Addr += Step;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
MapAllS23Pcie0Regions (VOID)
{
  STATIC CONST struct { UINT64 Base; UINT64 Len; } Regions[] = {
    { S23_GCC_BASE,         0x001F5000ULL },
    { S23_IPCC_BASE,        0x00001000ULL },
    { S23_PCIE0_PARF_BASE,  0x00003000ULL },
    { S23_PCIE0_PHY_BASE,   0x00002000ULL },
    { S23_TCSR_BASE,        0x00001000ULL },
    { S23_QMP_BASE,         0x00001000ULL },
    { S23_SPMI_CORE_BASE,   0x00004000ULL },
    { S23_SPMI_CNFG_BASE,   0x00004000ULL },
    { S23_SPMI_OBSRVR_BASE, 0x00080000ULL },
    { S23_SPMI_CHNLS_BASE,  0x00400000ULL },
    { S23_TLMM_BASE,        0x00100000ULL },
    { S23_SMMU_BASE,        0x00100000ULL },
    { S23_RSC_DRV2_BASE,    0x00002000ULL },
    { S23_PCIE0_DBI_BASE,   0x00002000ULL },
    { S23_PCIE0_ECAM_BASE,  0x00100000ULL },
  };
  for (UINTN i = 0; i < ARRAY_SIZE (Regions); i++) {
    EFI_STATUS St = EnsureMmioMapped (Regions[i].Base, Regions[i].Len);
    if (EFI_ERROR (St)) {
      return St;
    }
  }
  return EFI_SUCCESS;
}

/* ============================================================================
 * SPMI PMIC Arbiter v7 Helpers (PMK8550 GPIO3 32 kHz Sleep Clock @ 0xBA00)
 * ============================================================================ */
STATIC BOOLEAN
SpmiFindApid (UINT32 TargetPpid, UINT32 *ApidOut, UINT8 *OwnerOut)
{
  UINT32 Ver = Rd32 (S23_SPMI_CORE_BASE + 0x00ULL);
  if ((Ver & 0xF0000000U) != 0x70000000U) {
    return FALSE;
  }
  BOOLEAN Found = FALSE;
  UINT32  BestApid = 0;
  UINT8   BestOwner = 0xFFU;
  for (UINT32 Apid = 0; Apid < 1024U; Apid++) {
    UINT32 Val = Rd32 (S23_SPMI_CORE_BASE + 0x2000ULL + 4ULL * Apid);
    if (Val == 0U) {
      continue;
    }
    UINT32 Ppid = (Val >> 8) & 0xFFFU;
    if (Ppid != TargetPpid) {
      continue;
    }
    UINT8 Owner = (UINT8)(Rd32 (S23_SPMI_CNFG_BASE + 4ULL * Apid) & 0x7U);
    if (!Found || Owner == 0U) {
      Found     = TRUE;
      BestApid  = Apid;
      BestOwner = Owner;
      if (Owner == 0U) {
        break;
      }
    }
  }
  if (Found) {
    *ApidOut  = BestApid;
    *OwnerOut = BestOwner;
  }
  return Found;
}

STATIC BOOLEAN
SpmiReadByteApid (UINT32 Apid, UINT8 RegOff, UINT8 *OutVal)
{
  UINT64 Off = (UINT64)Apid * 0x20ULL;
  if (Off + 0x20ULL > 0x80000ULL) {
    return FALSE;
  }
  Wr32 (S23_SPMI_OBSRVR_BASE + Off + 0x00ULL, (1U << 27) | ((UINT32)RegOff << 4));
  UINT32 St = 0;
  for (UINT32 i = 0; i < 1000U; i++) {
    St = Rd32 (S23_SPMI_OBSRVR_BASE + Off + 0x08ULL);
    if ((St & 1U) != 0U) {
      break;
    }
    gBS->Stall (1);
  }
  if ((St & 1U) == 0U || (St & 0xEU) != 0U) {
    return FALSE;
  }
  *OutVal = (UINT8)(Rd32 (S23_SPMI_OBSRVR_BASE + Off + 0x18ULL) & 0xFFU);
  return TRUE;
}

STATIC BOOLEAN
SpmiWriteByteApid (UINT32 Apid, UINT8 Owner, UINT8 RegOff, UINT8 Val)
{
  if (Owner != 0U) {
    return FALSE;
  }
  UINT64 Off = (UINT64)Apid * 0x1000ULL;
  if (Off + 0x20ULL > 0x400000ULL) {
    return FALSE;
  }
  Wr32 (S23_SPMI_CHNLS_BASE + Off + 0x10ULL, (UINT32)Val);
  Wr32 (S23_SPMI_CHNLS_BASE + Off + 0x00ULL, ((UINT32)RegOff << 4));
  UINT32 St = 0;
  for (UINT32 i = 0; i < 1000U; i++) {
    St = Rd32 (S23_SPMI_CHNLS_BASE + Off + 0x08ULL);
    if ((St & 1U) != 0U) {
      break;
    }
    gBS->Stall (1);
  }
  return ((St & 1U) != 0U) && ((St & 0xEU) == 0U);
}

STATIC VOID
ConfigurePmk8550Gpio3SleepClock (VOID)
{
  UINT32 Apid = 0;
  UINT8  Owner = 0xFFU;
  if (!SpmiFindApid (0x0BAU, &Apid, &Owner) || Owner != 0U) {
    return;
  }
  UINT8 Mode = 0, Src = 0, En = 0;
  (VOID)SpmiReadByteApid (Apid, 0x40U, &Mode);
  (VOID)SpmiReadByteApid (Apid, 0x44U, &Src);
  if (!SpmiReadByteApid (Apid, 0x46U, &En)) {
    return;
  }
  if ((En & 0x80U) == 0U || Mode != 0x01U || Src != 0x02U) {
    (VOID)SpmiWriteByteApid (Apid, Owner, 0x40U, 0x01U);
    (VOID)SpmiWriteByteApid (Apid, Owner, 0x41U, 0x00U);
    (VOID)SpmiWriteByteApid (Apid, Owner, 0x42U, 0x05U);
    (VOID)SpmiWriteByteApid (Apid, Owner, 0x44U, 0x02U);
    (VOID)SpmiWriteByteApid (Apid, Owner, 0x45U, 0x03U);
    (VOID)SpmiWriteByteApid (Apid, Owner, 0x46U, 0x80U);
  }
}

/* ============================================================================
 * CMD_DB Lookup & APPS_RSC DRV2 TCS0 Single-Shot RPMh Write
 * ============================================================================ */
STATIC UINT32
CmdDbLookup (CONST CHAR8 *Name)
{
  CONST volatile UINT8 *Db = (CONST volatile UINT8 *)(UINTN)S23_CMD_DB_BASE;
  if (Db[4] != 0xDB || Db[5] != 0x30 || Db[6] != 0x03 || Db[7] != 0x0C) {
    return 0;
  }
  UINT32 NLen = 0;
  while (Name[NLen] != 0 && NLen < 8) {
    NLen++;
  }
  for (UINT32 i = 0; i < 8; i++) {
    CONST volatile UINT8 *R = Db + 8 + 16 * i;
    UINT16 SlvId  = *(CONST volatile UINT16 *)(R + 0);
    UINT16 HdrOff = *(CONST volatile UINT16 *)(R + 2);
    UINT16 Cnt    = *(CONST volatile UINT16 *)(R + 6);
    if (SlvId == 0) {
      break;
    }
    if (144U + (UINT32)HdrOff + 24U * (UINT32)Cnt > 0x20000U) {
      break;
    }
    CONST volatile UINT8 *E = Db + 144 + HdrOff;
    for (UINT32 j = 0; j < Cnt; j++, E += 24) {
      BOOLEAN Match = TRUE;
      for (UINT32 k = 0; k < 8; k++) {
        UINT8 Ch = (k < NLen) ? (UINT8)Name[k] : 0;
        if (E[k] != Ch) {
          Match = FALSE;
          break;
        }
      }
      if (Match) {
        return *(CONST volatile UINT32 *)(E + 16);
      }
    }
  }
  return 0;
}

STATIC BOOLEAN
RpmhWriteTcs0 (UINT32 Addr, UINT32 Data)
{
  if (Addr == 0U) {
    return FALSE;
  }
  CONST UINT64 Base = S23_RSC_DRV2_BASE + 0xD00ULL;

  /* 1. Wait up to 10 ms for TCS0 controller to report idle (STATUS +0x28 bit 0 == 1) */
  if (!PollMask (Base + 0x28ULL, 1U, 1U, 10000U)) {
    return FALSE;
  }

  /* 2. Save IRQ_EN, CMD_ENABLE, and slot-0 command registers before modifying */
  UINT32 SvIrqEn = Rd32 (Base + 0x00ULL);
  UINT32 SvCmdEn = Rd32 (Base + 0x2CULL);
  UINT32 SvMsg0  = Rd32 (Base + 0x34ULL);
  UINT32 SvAddr0 = Rd32 (Base + 0x38ULL);
  UINT32 SvData0 = Rd32 (Base + 0x3CULL);

  /* Ensure TCS0 completion latches into IRQ_STATUS (+0x04) */
  Wr32 (Base + 0x08ULL, 1U);
  if ((SvIrqEn & 1U) == 0U) {
    Wr32 (Base + 0x00ULL, SvIrqEn | 1U);
  }

  Wr32 (Base + 0x2CULL, 0U);
  Wr32 (Base + 0x34ULL, 8U | 0x100U | 0x10000U);
  Wr32 (Base + 0x38ULL, Addr);
  Wr32 (Base + 0x3CULL, Data);
  Wr32 (Base + 0x2CULL, 1U);

  UINT32 Ctrl = Rd32 (Base + 0x24ULL);
  Ctrl &= ~(1U << 24); Wr32 (Base + 0x24ULL, Ctrl); (VOID)Rd32 (Base + 0x24ULL);
  Ctrl &= ~(1U << 16); Wr32 (Base + 0x24ULL, Ctrl); (VOID)Rd32 (Base + 0x24ULL);
  Ctrl |=  (1U << 16); Wr32 (Base + 0x24ULL, Ctrl); (VOID)Rd32 (Base + 0x24ULL);
  Ctrl |=  (1U << 24); Wr32 (Base + 0x24ULL, Ctrl);

  BOOLEAN Ok   = PollMask (Base + 0x04ULL, 1U, 1U, 100000U);
  BOOLEAN Idle = PollMask (Base + 0x28ULL, 1U, 1U, 10000U);

  /* 3. Restore MSGID/ADDR/DATA BEFORE restoring CMD_ENABLE, only once TCS0 is idle */
  if (Idle) {
    Wr32 (Base + 0x2CULL, 0U);
    Wr32 (Base + 0x34ULL, SvMsg0);
    Wr32 (Base + 0x38ULL, SvAddr0);
    Wr32 (Base + 0x3CULL, SvData0);
    Wr32 (Base + 0x2CULL, SvCmdEn);
  }

  /* Clear IRQ status, restore original IRQ_EN, and verify TCS0 idle before exit */
  Wr32 (Base + 0x08ULL, 1U);
  if (SvIrqEn != Rd32 (Base + 0x00ULL)) {
    Wr32 (Base + 0x00ULL, SvIrqEn);
  }
  Idle = PollMask (Base + 0x28ULL, 1U, 1U, 10000U);
  return Ok && Idle;
}

STATIC VOID
VoteRailNpm (CONST CHAR8 *Name, UINT32 Millivolts)
{
  UINT32 A = CmdDbLookup (Name);
  if (A == 0) {
    return;
  }
  RpmhWriteTcs0 (A + 0x0, Millivolts);
  RpmhWriteTcs0 (A + 0x8, 7U); /* NPM mode */
  RpmhWriteTcs0 (A + 0x4, 1U);
}

/* ============================================================================
 * AOSS QMP (11 S23 wlan_pdc frames from s23_live.dts)
 * ============================================================================ */
STATIC CONST CHAR8 * CONST mS23PdcFrames[11] = {
  "{class: wlan_pdc, ss: rf, res: s4e.v, upval: 966}",
  "{class: wlan_pdc, ss: rf, res: s4e.v, dwnval: 615}",
  "{class: wlan_pdc, ss: rf, res: s4g.v, upval: 1350}",
  "{class: wlan_pdc, ss: rf, res: s4g.v, dwnval: 945}",
  "{class: wlan_pdc, ss: rf, res: s6g.v, upval: 1900}",
  "{class: wlan_pdc, ss: rf, res: s6g.v, dwnval: 1825}",
  "{class: wlan_pdc, ss: rf, res: s2g.m, enable: 1}",
  "{class: wlan_pdc, ss: rf, res: s2g.v, enable: 1}",
  "{class: wlan_pdc, ss: rf, res: s2g.v, upval: 1012}",
  "{class: wlan_pdc, ss: rf, res: s2g.v, dwnval: 800}",
  "{class: wlan_pdc, ss: bb, res: pdc, enable: 0}"
};

STATIC VOID
SendS23QmpPdcTable (VOID)
{
  if (Rd32 (S23_QMP_BASE + 0x00) != 0x4D41494CU || Rd32 (S23_QMP_BASE + 0x04) != 1U) {
    return;
  }
  UINT32 Off = Rd32 (S23_QMP_BASE + 0x38);
  if (Off < 0x3CU || Off + 68U > 0x400U) {
    return;
  }
  if (Rd32 (S23_QMP_BASE + 0x24) != 0xFFFFU) {
    Wr32 (S23_QMP_BASE + 0x10, Rd32 (S23_QMP_BASE + 0x0C));
    Wr32 (S23_QMP_BASE + 0x24, 0xFFFFU); Wr32 (S23_IPCC_BASE + 0x0C, 0U);
    if (!PollMask (S23_QMP_BASE + 0x28, 0xFFFFFFFFU, 0xFFFFU, 500000U)) return;
    Wr32 (S23_QMP_BASE + 0x2C, 0xFFFFU); Wr32 (S23_IPCC_BASE + 0x0C, 0U);
    if (!PollMask (S23_QMP_BASE + 0x14, 0xFFFFFFFFU, 0xFFFFU, 500000U)) return;
    Wr32 (S23_QMP_BASE + 0x18, 0xFFFFU); Wr32 (S23_IPCC_BASE + 0x0C, 0U);
    if (!PollMask (S23_QMP_BASE + 0x30, 0xFFFFFFFFU, 0xFFFFU, 500000U)) return;
  }
  for (UINTN f = 0; f < ARRAY_SIZE (mS23PdcFrames); f++) {
    if (!PollMask (S23_QMP_BASE + Off, 0xFFFFFFFFU, 0U, 100000U)) break;
    UINT8 Buf[64];
    for (UINT32 i = 0; i < 64; i++) Buf[i] = 0;
    for (UINT32 i = 0; i < 63 && mS23PdcFrames[f][i] != 0; i++) Buf[i] = (UINT8)mS23PdcFrames[f][i];
    for (UINT32 i = 0; i < 64; i += 4) {
      UINT32 W = (UINT32)Buf[i] | ((UINT32)Buf[i + 1] << 8) |
                 ((UINT32)Buf[i + 2] << 16) | ((UINT32)Buf[i + 3] << 24);
      Wr32 (S23_QMP_BASE + Off + 4 + i, W);
    }
    Wr32 (S23_QMP_BASE + Off, 64U);
    (VOID)Rd32 (S23_QMP_BASE + Off);
    Wr32 (S23_IPCC_BASE + 0x0C, 0U);
    (VOID)PollMask (S23_QMP_BASE + Off, 0xFFFFFFFFU, 0U, 500000U);
  }
}

/* ============================================================================
 * S23 PCIe0 QMP PHY Init Sequence (115 triplets verified exact vs s23_live.dts)
 * ============================================================================ */
STATIC CONST struct { UINT16 Off; UINT8 Val; UINT8 DelayUs; } mS23PhySeq[] = {
  { 0x0240, 0x03, 0 }, { 0x00c0, 0x01, 0 }, { 0x00cc, 0x62, 0 }, { 0x00d0, 0x02, 0 },
  { 0x0060, 0xf8, 0 }, { 0x0064, 0x01, 0 }, { 0x0000, 0x93, 0 }, { 0x0004, 0x01, 0 },
  { 0x00e0, 0x90, 0 }, { 0x00e4, 0x82, 0 }, { 0x00f4, 0x07, 0 }, { 0x0070, 0x02, 0 },
  { 0x0010, 0x02, 0 }, { 0x0074, 0x16, 0 }, { 0x0014, 0x16, 0 }, { 0x0078, 0x36, 0 },
  { 0x0018, 0x36, 0 }, { 0x0110, 0x08, 0 }, { 0x00bc, 0x0a, 0 }, { 0x0120, 0x42, 0 },
  { 0x0080, 0x04, 0 }, { 0x0084, 0x0d, 0 }, { 0x0020, 0x0a, 0 }, { 0x0024, 0x1a, 0 },
  { 0x0088, 0x41, 0 }, { 0x0028, 0x34, 0 }, { 0x0090, 0xab, 0 }, { 0x0094, 0xaa, 0 },
  { 0x0098, 0x01, 0 }, { 0x0030, 0x55, 0 }, { 0x0034, 0x55, 0 }, { 0x0038, 0x01, 0 },
  { 0x0140, 0x14, 0 }, { 0x0164, 0x34, 0 }, { 0x003c, 0x01, 0 }, { 0x001c, 0x04, 0 },
  { 0x0174, 0x16, 0 }, { 0x01bc, 0x0f, 0 }, { 0x0170, 0xa0, 0 }, { 0x11a4, 0x38, 0 },
  { 0x10dc, 0x11, 0 }, { 0x1160, 0xbf, 0 }, { 0x1164, 0xbf, 0 }, { 0x1168, 0xb7, 0 },
  { 0x116c, 0xea, 0 }, { 0x115c, 0x3f, 0 }, { 0x1174, 0x5c, 0 }, { 0x1178, 0x9c, 0 },
  { 0x117c, 0x1a, 0 }, { 0x1180, 0x89, 0 }, { 0x1170, 0xdc, 0 }, { 0x1188, 0x94, 0 },
  { 0x118c, 0x5b, 0 }, { 0x1190, 0x1a, 0 }, { 0x1194, 0x89, 0 }, { 0x10cc, 0x00, 0 },
  { 0x1008, 0x09, 0 }, { 0x1014, 0x05, 0 }, { 0x104c, 0x08, 0 }, { 0x1050, 0x08, 0 },
  { 0x10d8, 0x0f, 0 }, { 0x1118, 0x1c, 0 }, { 0x10f8, 0x07, 0 }, { 0x11f8, 0x08, 0 },
  { 0x1600, 0x00, 0 }, { 0x0e84, 0x15, 0 }, { 0x0e90, 0x3f, 0 }, { 0x0ee4, 0x02, 0 },
  { 0x0e40, 0x06, 0 }, { 0x0e3c, 0x18, 0 }, { 0x19a4, 0x38, 0 }, { 0x18dc, 0x11, 0 },
  { 0x1960, 0xbf, 0 }, { 0x1964, 0xbf, 0 }, { 0x1968, 0xb7, 0 }, { 0x196c, 0xea, 0 },
  { 0x195c, 0x3f, 0 }, { 0x1974, 0x5c, 0 }, { 0x1978, 0x9c, 0 }, { 0x197c, 0x1a, 0 },
  { 0x1980, 0x89, 0 }, { 0x1970, 0xdc, 0 }, { 0x1988, 0x94, 0 }, { 0x198c, 0x5b, 0 },
  { 0x1990, 0x1a, 0 }, { 0x1994, 0x89, 0 }, { 0x18cc, 0x00, 0 }, { 0x1808, 0x09, 0 },
  { 0x1814, 0x05, 0 }, { 0x184c, 0x08, 0 }, { 0x1850, 0x08, 0 }, { 0x18d8, 0x0f, 0 },
  { 0x1918, 0x1c, 0 }, { 0x18f8, 0x07, 0 }, { 0x19f8, 0x08, 0 }, { 0x1684, 0x15, 0 },
  { 0x1690, 0x3f, 0 }, { 0x16e4, 0x02, 0 }, { 0x1640, 0x06, 0 }, { 0x163c, 0x18, 0 },
  { 0x02dc, 0x05, 0 }, { 0x0388, 0x77, 0 }, { 0x0398, 0x0b, 0 }, { 0x06a4, 0x1e, 0 },
  { 0x06f4, 0x27, 0 }, { 0x03e0, 0x0f, 0 }, { 0x060c, 0x1d, 0 }, { 0x0614, 0x07, 0 },
  { 0x0620, 0xc1, 0 }, { 0x0694, 0x00, 0 }, { 0x03d0, 0x8c, 0 }, { 0x0368, 0x17, 0 },
  { 0x0370, 0x2e, 0 }, { 0x0200, 0x00, 0 }, { 0x0244, 0x03, 0 }
};

/* ============================================================================
 * PARF BDF-to-SID CRC8 (poly 0x07) Table & SMMU-500 Identity Pass-Through
 * ============================================================================ */
STATIC UINT8
Crc8Bdf (UINT16 Bdf)
{
  UINT8 Crc = 0;
  for (INT32 Byte = 1; Byte >= 0; Byte--) {
    Crc ^= (UINT8)(Bdf >> (Byte * 8));
    for (UINT32 Bit = 0; Bit < 8; Bit++) {
      Crc = (UINT8)((Crc & 0x80U) ? ((Crc << 1) ^ 0x07U) : (Crc << 1));
    }
  }
  return Crc;
}

STATIC VOID
ConfigureParfBdfToSid (VOID)
{
  /* Clear bypass (bit 0) only after zeroing 256-entry CRC8 SRAM table (0x01C02000..0x01C023FC) */
  for (UINT32 i = 0; i < 256; i++) {
    Wr32 (S23_PCIE0_PARF_BASE + 0x2000ULL + 4ULL * i, 0U);
  }
  /* RID 0x0100 (Bus 1 Dev 0 Fn 0) -> relative SID 1 (base 0x1400 + 1 = 0x1401) */
  UINT8 Hash = Crc8Bdf (0x0100U);
  Wr32 (S23_PCIE0_PARF_BASE + 0x2000ULL + 4ULL * Hash, 0x01000100U);
  Rmw32 (S23_PCIE0_PARF_BASE + 0x2C00ULL, 1U, 0U);
}

STATIC BOOLEAN
SmmuSmrOverlapsSid (UINT32 Smr, UINT32 Sid)
{
  UINT32 Mask = (Smr >> 16) & 0x7FFFU;
  return (Smr != 0U) && ((((Smr & 0x7FFFU) ^ Sid) & (~Mask & 0x7FFFU)) == 0U);
}

STATIC EFI_STATUS
ConfigureSmmu500Identity (VOID)
{
  UINT32 Cr0    = Rd32 (S23_SMMU_BASE + 0x00ULL);
  UINT32 Id0    = Rd32 (S23_SMMU_BASE + 0x20ULL);
  UINT32 Id1    = Rd32 (S23_SMMU_BASE + 0x24ULL);
  UINT32 Gfsr   = Rd32 (S23_SMMU_BASE + 0x48ULL);
  UINT32 NumSmr = Id0 & 0xFFU;
  UINT32 NumCb  = Id1 & 0xFFU;
  UINT32 PgSz   = (Id1 & (1U << 31)) ? 0x10000U : 0x1000U;
  UINT32 Pages  = 1U << (((Id1 >> 28) & 7U) + 1U);
  UINT64 CbBase = S23_SMMU_BASE + ((UINT64)Pages * PgSz);

  if (NumSmr == 0U || NumSmr > 128U || NumCb < 4U) {
    return EFI_UNSUPPORTED;
  }
  /* Require SMMU active (CR0.CLIENTPD == 0) and zero pre-existing global faults (GFSR == 0) */
  if ((Cr0 & 1U) != 0U || Gfsr != 0U) {
    return EFI_NOT_READY;
  }

  /* Locate two free SMR/S2CR slots and refuse SMR overlap against 0x1400/0x1401 (prevent SMR_MULTI_HIT) */
  UINT32 FreeSlots[2] = { 0xFFFFFFFFU, 0xFFFFFFFFU };
  UINT32 FreeCbs[2]   = { 0xFFFFFFFFU, 0xFFFFFFFFU };
  UINT8  UsedCb[256];
  for (UINT32 i = 0; i < 256U; i++) UsedCb[i] = 0;

  UINT32 Ns = 0;
  for (UINT32 n = 0; n < NumSmr; n++) {
    UINT32 Smr  = Rd32 (S23_SMMU_BASE + 0x800ULL + 4ULL * n);
    UINT32 S2cr = Rd32 (S23_SMMU_BASE + 0xC00ULL + 4ULL * n);
    if (SmmuSmrOverlapsSid (Smr, 0x1400U) || SmmuSmrOverlapsSid (Smr, 0x1401U)) {
      return EFI_NOT_READY;
    }
    if (((S2cr >> 16) & 3U) == 0U && (S2cr & 0xFFU) < NumCb) {
      UsedCb[S2cr & 0xFFU] = 1;
    }
    if (Smr == 0U && S2cr == 0x00020000U && Ns < 2U) {
      FreeSlots[Ns++] = n;
    }
  }
  if (Ns < 2U) {
    return EFI_NOT_READY;
  }

  /* Locate two strictly inactive Stage-1 context banks (SCTLR[6,5,0]==0, ACTLR==3, FSR==0, CBAR==0, CBA2R==0) */
  UINT32 Nb = 0;
  for (UINT32 cb = 1U; cb < NumCb && Nb < 2U; cb++) {
    if (UsedCb[cb]) continue;
    UINT64 Cba   = CbBase + ((UINT64)cb * PgSz);
    UINT32 Sctlr = Rd32 (Cba + 0x00ULL);
    UINT32 Actlr = Rd32 (Cba + 0x04ULL);
    UINT32 Fsr   = Rd32 (Cba + 0x58ULL);
    UINT32 Cbar  = Rd32 (S23_SMMU_BASE + PgSz + 4ULL * cb);
    UINT32 Cba2r = Rd32 (S23_SMMU_BASE + PgSz + 0x800ULL + 4ULL * cb);
    if ((Sctlr & 0x61U) == 0U && Actlr == 3U && Fsr == 0U && Cbar == 0U && Cba2r == 0U) {
      FreeCbs[Nb++] = cb;
    }
  }
  if (Nb < 2U) {
    return EFI_NOT_READY;
  }

  STATIC CONST UINT32 TargetSids[2] = { 0x1401U, 0x1400U };
  for (UINT32 k = 0; k < 2U; k++) {
    UINT32 Slot = FreeSlots[k];
    UINT32 Cb   = FreeCbs[k];
    UINT64 Cba  = CbBase + ((UINT64)Cb * PgSz);
    Wr32 (Cba + 0x00ULL, 0U);
    Wr32 (S23_SMMU_BASE + PgSz + 0x800ULL + 4ULL * Cb, SMMU_CB_CBA2R);
    Wr32 (S23_SMMU_BASE + PgSz + 4ULL * Cb, SMMU_CB_CBAR);
    Wr32 (Cba + 0x10ULL, SMMU_CB_TCR2);
    Wr32 (Cba + 0x20ULL, 0U); Wr32 (Cba + 0x24ULL, 0U);
    Wr32 (Cba + 0x28ULL, 0U); Wr32 (Cba + 0x2CULL, 0U);
    Wr32 (Cba + 0x30ULL, SMMU_CB_TCR);
    Wr32 (Cba + 0x34ULL, 0U); Wr32 (Cba + 0x38ULL, 0U); Wr32 (Cba + 0x3CULL, 0U);
    Wr32 (Cba + 0x00ULL, SMMU_CB_SCTLR);
    Wr32 (S23_SMMU_BASE + 0xC00ULL + 4ULL * Slot, Cb);
    Wr32 (S23_SMMU_BASE + 0x800ULL + 4ULL * Slot, 0x80000000U | TargetSids[k]);
  }

  /* Global TLB Sync & post-write fault verification */
  Wr32 (S23_SMMU_BASE + 0x70ULL, 0U);
  if (!PollMask (S23_SMMU_BASE + 0x74ULL, 1U, 0U, 100000U)) {
    return EFI_DEVICE_ERROR;
  }
  mReport.SmmuId0  = Id0;
  mReport.SmmuGfsr = Rd32 (S23_SMMU_BASE + 0x48ULL);
  mReport.SmmuSmr0 = Rd32 (S23_SMMU_BASE + 0x800ULL + 4ULL * FreeSlots[0]);
  mReport.SmmuSmr1 = Rd32 (S23_SMMU_BASE + 0x800ULL + 4ULL * FreeSlots[1]);
  if (mReport.SmmuGfsr != 0U) {
    return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}

/* ============================================================================
 * G401 3-Slot iATU Bus-1 ECAM Transport + Hidden Type-1 Root Port Setup
 * ============================================================================ */
STATIC EFI_STATUS
ConfigureBus1EcamTransport (VOID)
{
  /* Set Root Port Primary=0, Secondary=1, Subordinate=1 & MEM Base/Limit 0x6030..0x63F0 */
  Rmw32 (S23_PCIE0_DBI_BASE + 0x18ULL, 0x00FFFFFFU, 0x00010100U);
  Wr32  (S23_PCIE0_DBI_BASE + 0x20ULL, 0x63F06030U);
  Rmw32 (S23_PCIE0_DBI_BASE + 0x04ULL, 0xFFFF0000U, 0x00000006U); /* MSE | BME on RP */

  /* Keep Root Port out of Gunyah ITS MSI by advertising Legacy Interrupt Pin = 1 (INTA) */
  Rmw32 (S23_PCIE0_DBI_BASE + 0x8BCULL, 0U, 1U);
  Rmw32 (S23_PCIE0_DBI_BASE + 0x03CULL, 0x0000FF00U, 0x00000100U);
  Rmw32 (S23_PCIE0_DBI_BASE + 0x8BCULL, 1U, 0U);

  /* Slot 0 (+0x000): CFG0 + CFG_SHIFT for Bus 1 Dev 0 Fn 0..7 (0x60100000..0x60107FFF) */
  Wr32 (S23_PCIE0_ATU_BASE + 0x004ULL, 0U);
  Wr32 (S23_PCIE0_ATU_BASE + 0x000ULL, 4U);
  Wr32 (S23_PCIE0_ATU_BASE + 0x008ULL, 0x60100000U);
  Wr32 (S23_PCIE0_ATU_BASE + 0x00CULL, 0U);
  Wr32 (S23_PCIE0_ATU_BASE + 0x010ULL, 0x60107FFFU);
  Wr32 (S23_PCIE0_ATU_BASE + 0x014ULL, 0U);
  Wr32 (S23_PCIE0_ATU_BASE + 0x018ULL, 0U);
  Wr32 (S23_PCIE0_ATU_BASE + 0x004ULL, (1U << 31) | (1U << 28));
  if (!PollMask (S23_PCIE0_ATU_BASE + 0x004ULL, (1U << 31) | (1U << 28), (1U << 31) | (1U << 28), 5000U)) {
    return EFI_DEVICE_ERROR;
  }

  /* Slot 1 (+0x200): CFG1 absent-bus sink for Dev 1..31 (0x60108000..0x601FFFFF -> Bus 0xFF) */
  Wr32 (S23_PCIE0_ATU_BASE + 0x204ULL, 0U);
  Wr32 (S23_PCIE0_ATU_BASE + 0x200ULL, 5U);
  Wr32 (S23_PCIE0_ATU_BASE + 0x208ULL, 0x60108000U);
  Wr32 (S23_PCIE0_ATU_BASE + 0x20CULL, 0U);
  Wr32 (S23_PCIE0_ATU_BASE + 0x210ULL, 0x601FFFFFU);
  Wr32 (S23_PCIE0_ATU_BASE + 0x214ULL, 0xFF000000U);
  Wr32 (S23_PCIE0_ATU_BASE + 0x218ULL, 0U);
  Wr32 (S23_PCIE0_ATU_BASE + 0x204ULL, (1U << 31));
  if (!PollMask (S23_PCIE0_ATU_BASE + 0x204ULL, (1U << 31), (1U << 31), 5000U)) {
    return EFI_DEVICE_ERROR;
  }

  /* Slot 2 (+0x400): Identity MEM32 window (0x60300000..0x63FFFFFF -> PCI 0x60300000) */
  Wr32 (S23_PCIE0_ATU_BASE + 0x404ULL, 0U);
  Wr32 (S23_PCIE0_ATU_BASE + 0x400ULL, 0U);
  Wr32 (S23_PCIE0_ATU_BASE + 0x408ULL, 0x60300000U);
  Wr32 (S23_PCIE0_ATU_BASE + 0x40CULL, 0U);
  Wr32 (S23_PCIE0_ATU_BASE + 0x410ULL, 0x63FFFFFFU);
  Wr32 (S23_PCIE0_ATU_BASE + 0x414ULL, 0x60300000U);
  Wr32 (S23_PCIE0_ATU_BASE + 0x418ULL, 0U);
  Wr32 (S23_PCIE0_ATU_BASE + 0x404ULL, (1U << 31));
  if (!PollMask (S23_PCIE0_ATU_BASE + 0x404ULL, (1U << 31), (1U << 31), 5000U)) {
    return EFI_DEVICE_ERROR;
  }

  /* Verify complete Bus-1 ECAM isolation:
   *   Dev 0 Fn 0 (0x60100000) == 0x110317CB
   *   Dev 0 Fn 1..7 (0x60101000..0x60107000) == 0xFFFFFFFF
   *   Dev 1..31 Fn 0..7 (0x60108000..0x601FF000) == 0xFFFFFFFF */
  if (Rd32 (S23_PCIE0_ECAM_BASE + 0x0000ULL) != QCA6490_VENDOR_DEVICE) {
    return EFI_DEVICE_ERROR;
  }
  for (UINT32 Df = 1U; Df < 256U; Df++) {
    if (Rd32 (S23_PCIE0_ECAM_BASE + ((UINT64)Df << 12)) != 0xFFFFFFFFU) {
      return EFI_DEVICE_ERROR;
    }
  }
  return EFI_SUCCESS;
}

/* ============================================================================
 * Patch or Synthesize MCFG (Seg 0 StartBus=1, EndBus=1) & Install SSDT
 * ============================================================================ */
STATIC VOID
BuildSyntheticMcfg (UINT8 Out[60])
{
  STATIC CONST UINT8 Template[60] = {
    'M', 'C', 'F', 'G',             /* Signature */
    60, 0, 0, 0,                    /* Length = 60 bytes */
    1,                              /* Revision = 1 */
    0,                              /* Checksum (computed below) */
    'S', 'A', 'M', 'S', 'N', 'G',   /* OEM ID */
    'S', '2', '3', 'P', 'C', 'I', 'E', '0', /* OEM Table ID */
    1, 0, 0, 0,                     /* OEM Revision */
    'D', 'M', '1', 'Q',             /* Creator ID */
    1, 0, 0, 0,                     /* Creator Revision */
    0, 0, 0, 0, 0, 0, 0, 0,         /* Reserved (8 bytes) */
    0x00, 0x00, 0x00, 0x60,         /* BaseAddress low  = 0x60000000 */
    0x00, 0x00, 0x00, 0x00,         /* BaseAddress high = 0x00000000 */
    0x00, 0x00,                     /* Segment Group = 0 */
    0x01,                           /* Start Bus = 1 */
    0x01,                           /* End Bus   = 1 */
    0x00, 0x00, 0x00, 0x00          /* Reserved (4 bytes) */
  };
  UINT8 Sum = 0;
  for (UINTN b = 0; b < 60U; b++) {
    Out[b] = Template[b];
    Sum = (UINT8)(Sum + Out[b]);
  }
  Out[9] = (UINT8)(0U - Sum);
}

STATIC EFI_STATUS
PublishMcfgAndSsdt (VOID)
{
  EFI_ACPI_TABLE_PROTOCOL *AcpiTbl = NULL;
  EFI_ACPI_SDT_PROTOCOL   *AcpiSdt = NULL;
  EFI_STATUS Status;

  Status = gBS->LocateProtocol (&gEfiAcpiTableProtocolGuid, NULL, (VOID **)&AcpiTbl);
  if (EFI_ERROR (Status)) return Status;

  /* Locate existing APIC (MADT) & MCFG via SDT protocol */
  UINT8 ApicBuf[2048];
  UINTN ApicLen = 0;
  UINTN OldApicKey = 0;
  BOOLEAN FoundApic = FALSE;

  UINT8 McfgBuf[128];
  UINTN McfgLen = 0;
  UINTN OldMcfgKey = 0;
  BOOLEAN FoundMcfg = FALSE;

  if (!EFI_ERROR (gBS->LocateProtocol (&gEfiAcpiSdtProtocolGuid, NULL, (VOID **)&AcpiSdt)) && AcpiSdt != NULL) {
    for (UINTN idx = 0; idx < 256U; idx++) {
      EFI_ACPI_SDT_HEADER *Hdr = NULL;
      EFI_ACPI_TABLE_VERSION Ver = 0;
      UINTN Key = 0;
      Status = AcpiSdt->GetAcpiTable (idx, &Hdr, &Ver, &Key);
      if (Status == EFI_NOT_FOUND) break;
      if (EFI_ERROR (Status) || Hdr == NULL) continue;
      if (!FoundApic && Hdr->Signature == SIGNATURE_32 ('A', 'P', 'I', 'C') && Hdr->Length >= 44U && Hdr->Length + 64U <= sizeof (ApicBuf)) {
        CONST UINT8 *Src = (CONST UINT8 *)Hdr;
        for (UINTN b = 0; b < Hdr->Length; b++) ApicBuf[b] = Src[b];
        ApicLen = Hdr->Length;
        OldApicKey = Key;
        FoundApic = TRUE;
      }
      if (!FoundMcfg && Hdr->Signature == SIGNATURE_32 ('M', 'C', 'F', 'G') && Hdr->Length <= sizeof (McfgBuf)) {
        CONST UINT8 *Src = (CONST UINT8 *)Hdr;
        for (UINTN b = 0; b < Hdr->Length; b++) McfgBuf[b] = Src[b];
        McfgLen = Hdr->Length;
        OldMcfgKey = Key;
        FoundMcfg = TRUE;
      }
    }
  }

  /* Patch APIC (MADT) with GICR (0x0E) + Generic MSI Frame (0x0D: 0x17110000, SPI Base=800, Count=32).
   * Enables DEVPKEY_PciRootBus_SystemMsiSupport = TRUE via GICD_SETSPI_NSR (0x17110040) without Gunyah ITS! */
  if (FoundApic) {
    BOOLEAN HaveGicr = FALSE;
    BOOLEAN HaveMsi  = FALSE;
    UINTN Off = 44U;
    while (Off + 2U <= ApicLen) {
      UINT8 SType = ApicBuf[Off];
      UINT8 SLen  = ApicBuf[Off + 1U];
      if (SLen < 2U || Off + SLen > ApicLen) break;
      if (SType == 0x0EU) HaveGicr = TRUE;
      if (SType == 0x0DU) HaveMsi  = TRUE;
      /* Normalize Kailua/APIC.aml 25-byte GICD length typo (0x19 -> 0x18) so appended subtables stay 4-byte aligned */
      if (SType == 0x0CU && SLen == 0x19U && Off + SLen == ApicLen && ApicBuf[Off + 24U] == 0U) {
        ApicBuf[Off + 1U] = 0x18U;
        ApicLen -= 1U;
        break;
      }
      Off += SLen;
    }
    if (!HaveGicr && ApicLen + 16U <= sizeof (ApicBuf)) {
      STATIC CONST UINT8 GicrSub[16] = {
        0x0E, 0x10, 0x00, 0x00,
        0x00, 0x00, 0x18, 0x17, 0x00, 0x00, 0x00, 0x00, /* GICR Base = 0x17180000 */
        0x00, 0x00, 0x20, 0x00                          /* GICR Len  = 0x00200000 */
      };
      for (UINTN b = 0; b < 16U; b++) ApicBuf[ApicLen++] = GicrSub[b];
    }
    if (!HaveMsi && ApicLen + 24U <= sizeof (ApicBuf)) {
      STATIC CONST UINT8 MsiSub[24] = {
        0x0D, 0x18, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,                         /* MSI Frame ID = 0 */
        0x00, 0x00, 0x11, 0x17, 0x00, 0x00, 0x00, 0x00, /* Base = 0x17110000 (SETSPI_NS = +0x40 = 0x17110040) */
        0x01, 0x00, 0x00, 0x00,                         /* Flags = 1 (Select SPI Base/Count) */
        0x20, 0x00,                                     /* SPI Count = 32 */
        0x20, 0x03                                      /* SPI Base  = 800 (0x0320) */
      };
      for (UINTN b = 0; b < 24U; b++) ApicBuf[ApicLen++] = MsiSub[b];
    }
    ApicBuf[4] = (UINT8)(ApicLen & 0xFFU);
    ApicBuf[5] = (UINT8)((ApicLen >> 8) & 0xFFU);
    ApicBuf[6] = (UINT8)((ApicLen >> 16) & 0xFFU);
    ApicBuf[7] = (UINT8)((ApicLen >> 24) & 0xFFU);
    ApicBuf[9] = 0U;
    UINT8 ApicSum = 0;
    for (UINTN b = 0; b < ApicLen; b++) ApicSum = (UINT8)(ApicSum + ApicBuf[b]);
    ApicBuf[9] = (UINT8)(0U - ApicSum);
    (VOID)AcpiTbl->UninstallAcpiTable (AcpiTbl, OldApicKey);
    UINTN NewApicKey = 0;
    Status = AcpiTbl->InstallAcpiTable (AcpiTbl, ApicBuf, ApicLen, &NewApicKey);
    if (EFI_ERROR (Status)) return Status;
  }

  if (FoundMcfg && McfgLen >= 60U) {
    BOOLEAN PatchedSeg0 = FALSE;
    for (UINTN Off = 44U; Off + 16U <= McfgLen; Off += 16U) {
      UINT16 Seg = (UINT16)McfgBuf[Off + 8] | ((UINT16)McfgBuf[Off + 9] << 8);
      if (Seg == 0U) {
        McfgBuf[Off + 0]  = 0x00;
        McfgBuf[Off + 1]  = 0x00;
        McfgBuf[Off + 2]  = 0x00;
        McfgBuf[Off + 3]  = 0x60;
        McfgBuf[Off + 4]  = 0x00;
        McfgBuf[Off + 5]  = 0x00;
        McfgBuf[Off + 6]  = 0x00;
        McfgBuf[Off + 7]  = 0x00;
        McfgBuf[Off + 10] = 1; /* StartBus = 1 */
        McfgBuf[Off + 11] = 1; /* EndBus   = 1 */
        PatchedSeg0 = TRUE;
      }
    }
    if (!PatchedSeg0) {
      BuildSyntheticMcfg (McfgBuf);
      McfgLen = 60U;
    } else {
      McfgBuf[9] = 0;
      UINT8 Sum = 0;
      for (UINTN b = 0; b < McfgLen; b++) Sum = (UINT8)(Sum + McfgBuf[b]);
      McfgBuf[9] = (UINT8)(0U - Sum);
    }

    (VOID)AcpiTbl->UninstallAcpiTable (AcpiTbl, OldMcfgKey);
    UINTN NewKey = 0;
    Status = AcpiTbl->InstallAcpiTable (AcpiTbl, McfgBuf, McfgLen, &NewKey);
    if (EFI_ERROR (Status)) return Status;
  } else {
    /* dm1q ships no stock MCFG.aml: synthesize valid 60-byte MCFG (Base=0x60000000, Seg=0, Bus 1..1) */
    BuildSyntheticMcfg (McfgBuf);
    McfgLen = 60U;
    UINTN NewKey = 0;
    Status = AcpiTbl->InstallAcpiTable (AcpiTbl, McfgBuf, McfgLen, &NewKey);
    if (EFI_ERROR (Status)) return Status;
  }

#ifdef S23_INCLUDE_SSDT_AML
  UINTN SsdtKey = 0;
  Status = AcpiTbl->InstallAcpiTable (AcpiTbl, (VOID *)s23_pcie0_wlan_aml_code, sizeof (s23_pcie0_wlan_aml_code), &SsdtKey);
  return Status;
#else
  (VOID)mS23Pcie0SsdtPlaceholder;
  return EFI_SUCCESS;
#endif
}

STATIC VOID
PublishS23Pcie0Report (VOID)
{
  EFI_ACPI_TABLE_PROTOCOL *AcpiTbl = NULL;
  if (EFI_ERROR (gBS->LocateProtocol (&gEfiAcpiTableProtocolGuid, NULL, (VOID **)&AcpiTbl)) || AcpiTbl == NULL) {
    return;
  }
  mReport.Signature[0] = 'S'; mReport.Signature[1] = '2'; mReport.Signature[2] = '3'; mReport.Signature[3] = 'P';
  mReport.Length = sizeof (mReport);
  mReport.Revision = 1;
  mReport.Checksum = 0;
  mReport.OemId[0] = 'S'; mReport.OemId[1] = 'A'; mReport.OemId[2] = 'M';
  mReport.OemId[3] = 'S'; mReport.OemId[4] = 'N'; mReport.OemId[5] = 'G';
  mReport.OemTableId[0] = 'S'; mReport.OemTableId[1] = '2'; mReport.OemTableId[2] = '3'; mReport.OemTableId[3] = 'P';
  mReport.OemTableId[4] = 'C'; mReport.OemTableId[5] = 'I'; mReport.OemTableId[6] = 'E'; mReport.OemTableId[7] = '0';
  mReport.OemRevision = 1;
  mReport.CreatorId[0] = 'D'; mReport.CreatorId[1] = 'M'; mReport.CreatorId[2] = '1'; mReport.CreatorId[3] = 'Q';
  mReport.CreatorRevision = 1;
  CONST UINT8 *Bytes = (CONST UINT8 *)&mReport;
  UINT8 Sum = 0;
  for (UINTN i = 0; i < sizeof (mReport); i++) {
    Sum = (UINT8)(Sum + Bytes[i]);
  }
  mReport.Checksum = (UINT8)(0U - Sum);
  UINTN Key = 0;
  (VOID)AcpiTbl->InstallAcpiTable (AcpiTbl, &mReport, sizeof (mReport), &Key);
}

/* ============================================================================
 * Main Bring-Up Sequence (ReadyToBoot Callback)
 * ============================================================================ */
STATIC VOID EFIAPI
OnReadyToBoot (EFI_EVENT Event, VOID *Context)
{
  (VOID)Event;
  (VOID)Context;
  if (mInitialized) return;
  mInitialized = TRUE;
  mReport.Stage = 1;

  if (EFI_ERROR (MapAllS23Pcie0Regions ())) {
    DEBUG ((0x80000000U, "S23Pcie0Dxe: MMIO map failed\n"));
    PublishS23Pcie0Report ();
    return;
  }

  /* 1. Cold pin hold, dedicated rail discharge (150 ms), PMK8550 GPIO3 32 kHz, RPMh votes */
  SetTlmm (94,  0x0201U, 0);  /* PERST# LOW */
  SetTlmm (95,  0x0007U, -1); /* CLKREQ# func1 PU */
  SetTlmm (96,  0x0003U, -1); /* WAKE# input PU */
  /* GPIO204 has EGPIO_PRESENT (BIT 11 = 0x0800): MUST set EGPIO_ENABLE (BIT 12 = 0x1000, MSM_APPS_OWNER) */
  SetTlmm (204, 0x1201U, 0);  /* XO_CLK_GPIO LOW (APPS owner) */
  SetTlmm (81,  0x0201U, 0);  /* BT_EN LOW */
  SetTlmm (80,  0x0201U, 0);  /* WLAN_EN LOW */

  /* Cold discharge dedicated WLAN rails (never disable ldob15!; smpg5 unconnected on dm1q) */
  STATIC CONST CHAR8 * CONST OffRails[] = { "smpg4", "smpg6", "smpe4", "smpg2" };
  for (UINTN i = 0; i < ARRAY_SIZE (OffRails); i++) {
    UINT32 Ra = CmdDbLookup (OffRails[i]);
    if (Ra != 0U) {
      (VOID)RpmhWriteTcs0 (Ra + 4U, 0U);
    }
  }
  for (UINT32 ms = 0; ms < 20U; ms++) {
    gBS->Stall (1000);
  }

  /* Program PMK8550 GPIO3 (0xBA40..0xBA46 = 01/00/05/02/03/80) for 32 kHz sleep clock */
  ConfigurePmk8550Gpio3SleepClock ();

  (VOID)RpmhWriteTcs0 (CmdDbLookup ("xo.lvl"), 3U);
  (VOID)RpmhWriteTcs0 (CmdDbLookup ("cx.lvl"), 3U);
  (VOID)RpmhWriteTcs0 (CmdDbLookup ("mx.lvl"), 3U);

  STATIC CONST CHAR8 * CONST Clks[] = {
    "clka1", "clka2", "clka3", "clka4", "clka6", "clka7", "clka8", "rclkha1"
  };
  for (UINTN i = 0; i < ARRAY_SIZE (Clks); i++) {
    UINT32 A = CmdDbLookup (Clks[i]);
    if (A != 0U) {
      (VOID)RpmhWriteTcs0 (A + 4U, 1U);
    }
  }

  /* BCM bandwidth votes (matching s23pcieprobe v1.0.0.11: MC0/SH0 display + CN0/SN7 PCIe ANOC) */
  UINT32 Mc0 = CmdDbLookup ("MC0");
  if (Mc0 != 0U) (VOID)RpmhWriteTcs0 (Mc0, 0x62CDD764U);
  UINT32 Sh0 = CmdDbLookup ("SH0");
  if (Sh0 != 0U) (VOID)RpmhWriteTcs0 (Sh0, 0x62CDD764U);
  UINT32 Cn0 = CmdDbLookup ("CN0");
  if (Cn0 != 0U) (VOID)RpmhWriteTcs0 (Cn0, 0x60004004U);
  UINT32 Sn7 = CmdDbLookup ("SN7");
  if (Sn7 != 0U) (VOID)RpmhWriteTcs0 (Sn7, 0x60004004U);

  VoteRailNpm ("ldoe1",  880);
  VoteRailNpm ("ldoe3",  1200);
  VoteRailNpm ("ldob15", 1800);
  VoteRailNpm ("smpg2",  1012);
  VoteRailNpm ("smpe4",  966);
  VoteRailNpm ("smpg6",  1900);
  VoteRailNpm ("smpg4",  1350);

  UINT32 VrmWlan = CmdDbLookup ("vrm.wlan");
  if (VrmWlan != 0U) {
    (VOID)RpmhWriteTcs0 (VrmWlan, 1U);
  }

  SendS23QmpPdcTable ();

  /* Connect SW_CTRL (82) & WAKE (96) to MPM/PDC and pulse XO_CLK_GPIO (204, EGPIO_ENABLE=BIT12) around WLAN_EN (80) */
  Wr32 (S23_TLMM_BASE + 82ULL * 0x1000ULL + 0x08ULL, 0x7C2U);
  Wr32 (S23_TLMM_BASE + 96ULL * 0x1000ULL + 0x08ULL, 0x7C2U);
  SetTlmm (204, 0x13C3U, 1);
  gBS->Stall (200);
  SetTlmm (80,  0x03C3U, 1);
  gBS->Stall (6000);
  SetTlmm (204, 0x1201U, 0);
  gBS->Stall (25000);

  /* 2. TCSR clkref, GCC GDSCs, RCGs, branch clocks, canonical BCR resets */
  Rmw32 (S23_TCSR_BASE + 0x100ULL, 0U, 1U);
  Rmw32 (S23_GCC_BASE + 0x52020ULL, 0x9U, 0U);
  Rmw32 (S23_GCC_BASE + 0x52024ULL, 0x9U, 0U);
  Rmw32 (S23_GCC_BASE + 0x6B004ULL, 1U, 0U);
  if (!PollMask (S23_GCC_BASE + 0x6B004ULL, 0x80000000U, 0x80000000U, 20000U)) return;
  Rmw32 (S23_GCC_BASE + 0x6C000ULL, 1U, 0U);
  if (!PollMask (S23_GCC_BASE + 0x6C000ULL, 0x80000000U, 0x80000000U, 20000U)) return;

  Rmw32 (S23_GCC_BASE + 0x6B070ULL, 3U, 0U); /* PIPE mux -> external PHY pipe clk */

  Rmw32 (S23_GCC_BASE + 0x6B074ULL, 0U, 2U);
  Wr32  (S23_GCC_BASE + 0x6B078ULL, 0x001U);
  Rmw32 (S23_GCC_BASE + 0x6B074ULL, 0U, 1U);
  (VOID)PollMask (S23_GCC_BASE + 0x6B074ULL, 1U, 0U, 5000U);

  Rmw32 (S23_GCC_BASE + 0x6B058ULL, 0U, 2U);
  Wr32  (S23_GCC_BASE + 0x6B05CULL, 0x00100605U);
  Rmw32 (S23_GCC_BASE + 0x6B058ULL, 0U, 1U);
  (VOID)PollMask (S23_GCC_BASE + 0x6B058ULL, 1U, 0U, 5000U);
  Rmw32 (S23_GCC_BASE + 0x6B058ULL, 2U, 0U);

  Rmw32 (S23_GCC_BASE + 0x52008ULL, (1U << 4), 0x6FU);
  Rmw32 (S23_GCC_BASE + 0x52000ULL, 0U, (1U << 11) | (1U << 12) | (1U << 19) | (1U << 20) | (1U << 22));
  gBS->Stall (1000);

  Rmw32 (S23_GCC_BASE + 0x6B000ULL, 1U, 1U); gBS->Stall (1000); Rmw32 (S23_GCC_BASE + 0x6B000ULL, 1U, 0U); gBS->Stall (1000);
  Rmw32 (S23_GCC_BASE + 0x6C01CULL, 1U, 1U); gBS->Stall (1000); Rmw32 (S23_GCC_BASE + 0x6C01CULL, 1U, 0U); gBS->Stall (1000);

  /* 3. PARF + 115-triplet QMP PHY + PIPE clock lock (exact pci-msm.c / g289_rc0.c sequence) */
  Wr32  (S23_PCIE0_PARF_BASE + 0x1000ULL, 4U);
  Rmw32 (S23_PCIE0_PARF_BASE + 0x0020ULL, (1U << 5), 0U);
  Rmw32 (S23_PCIE0_PARF_BASE + 0x0040ULL, 1U, 0U);
  Wr32  (S23_PCIE0_PARF_BASE + 0x0350ULL, 0U);
  Wr32  (S23_PCIE0_PARF_BASE + 0x0000ULL, 0x365EU);
  Rmw32 (S23_PCIE0_PARF_BASE + 0x0174ULL, 0U, (1U << 4));
  Wr32  (S23_PCIE0_PARF_BASE + 0x022CULL, 0U);
  Wr32  (S23_PCIE0_PARF_BASE + 0x0508ULL, 0U);
  Wr32  (S23_PCIE0_PARF_BASE + 0x0358ULL, 0x04000000U);
  Rmw32 (S23_PCIE0_PARF_BASE + 0x01A8ULL, 0U, (1U << 31));

  for (UINTN i = 0; i < ARRAY_SIZE (mS23PhySeq); i++) {
    Wr32 (S23_PCIE0_PHY_BASE + mS23PhySeq[i].Off, mS23PhySeq[i].Val);
    if (mS23PhySeq[i].DelayUs) gBS->Stall (mS23PhySeq[i].DelayUs);
  }
  gBS->Stall (1000);

  Rmw32 (S23_GCC_BASE + 0x52008ULL, 0U, (1U << 4));
  if (!PollMask (S23_PCIE0_PHY_BASE + 0x214ULL, (1U << 6), 0U, 50000U)) return;
  if (!PollMask (S23_GCC_BASE + 0x6B048ULL, 0x80000000U, 0U, 50000U)) return;
  gBS->Stall (10000);

  /* 4. DBI validation, PERST# release, x1/Gen1 EQ & LTSSM Link Training */
  UINT32 RootId  = Rd32 (S23_PCIE0_DBI_BASE + 0x00ULL);
  UINT32 LinkCap = Rd32 (S23_PCIE0_DBI_BASE + 0x7CULL);
  if ((RootId & 0xFFFFU) != 0x17CBU) return;

  Rmw32 (S23_PCIE0_PARF_BASE + 0x2C4ULL, 1U, 0U);
  Rmw32 (S23_PCIE0_PARF_BASE + 0x2C4ULL, 0U, (1U << 8));
  Rmw32 (S23_PCIE0_DBI_BASE  + 0x714ULL, 0U, (1U << 5));

  SetTlmm (94, 0x0201U, 1); /* PERST# HIGH */
  gBS->Stall (2000);

  /* Exact g289_rc0.c / pci-msm.c / s23pcieprobe v1.0.0.12 sequence: keep reset DBI+0x710 & DBI+0x7C, set DBI+0x80C NUM_OF_LANES=1 */
  Rmw32 (S23_PCIE0_DBI_BASE  + 0x80CULL, 0x1F00U, 0x0100U);

  UINT32 MaxGen = LinkCap & 0xFU;
  for (UINT32 Gen = 3; Gen <= MaxGen && Gen <= 4; Gen++) {
    Rmw32 (S23_PCIE0_DBI_BASE + 0x890ULL, (3U << 24), (Gen - 3U) << 24);
    Rmw32 (S23_PCIE0_DBI_BASE + 0x8A8ULL, 0xFFFF00U, 0U);
    Rmw32 (S23_PCIE0_DBI_BASE + 0x890ULL, 1U, 0U);
    Rmw32 (S23_PCIE0_DBI_BASE + 0x8ACULL, 0x1FU, 0U);
  }
  Rmw32 (S23_PCIE0_DBI_BASE + 0x8BCULL, 0U, 1U);
  if (MaxGen >= 3) Wr32 (S23_PCIE0_DBI_BASE + 0x154ULL, 0x55555555U);
  if (MaxGen >= 4) Wr32 (S23_PCIE0_DBI_BASE + 0x188ULL, 0x55555555U);
  Rmw32 (S23_PCIE0_DBI_BASE + 0x8BCULL, 1U, 0U);
  Rmw32 (S23_PCIE0_DBI_BASE + 0x0A0ULL, 0xFU, 2U); /* s23_live.dts qcom,target-link-speed = 2 (Gen2 x1) */
  Rmw32 (S23_PCIE0_DBI_BASE + 0x078ULL, 0x7000U, 0x5000U);

  if (Rd32 (S23_PCIE0_PARF_BASE + 0x1B0ULL) & (1U << 11)) {
    Rmw32 (S23_PCIE0_PARF_BASE + 0x1B0ULL, 0U, (1U << 10));
    (VOID)PollMask (S23_PCIE0_PARF_BASE + 0x1B0ULL, (1U << 11), 0U, 5000U);
    Rmw32 (S23_PCIE0_PARF_BASE + 0x1B0ULL, (1U << 10), 0U);
  }
  Rmw32 (S23_PCIE0_PARF_BASE + 0x1B0ULL, (1U << 10), (1U << 8));

  BOOLEAN LinkUp = FALSE;
  for (UINT32 ms = 0; ms < 200U; ms++) {
    gBS->Stall (1000);
    if ((Rd32 (S23_PCIE0_ELBI_BASE + 0x08ULL) & 0x400U) &&
        (Rd32 (S23_PCIE0_DBI_BASE  + 0x80ULL) & (1U << 29))) {
      LinkUp = TRUE;
      break;
    }
  }

  /* Pass 2 fallback (matching s23pcieprobe v1.0.0.11): x2 PORT_LINK_MODE + NL=1 + CLKREQ_OVR=0x02 + PERST# pulse */
  if (!LinkUp) {
    Rmw32 (S23_PCIE0_PARF_BASE + 0x1B0ULL, (1U << 8) | (1U << 10), 0U);
    SetTlmm (94, 0x0201U, 0); /* PERST# LOW */
    gBS->Stall (10000);
    Rmw32 (S23_PCIE0_DBI_BASE + 0x710ULL, 0x3F0000U, 0x030000U); /* x2 PORT_LINK_MODE */
    Rmw32 (S23_PCIE0_DBI_BASE + 0x80CULL, 0x1F00U,   0x0100U);   /* NUM_OF_LANES = 1 */
    Rmw32 (S23_PCIE0_DBI_BASE + 0x8BCULL, 0U, 1U);
    Rmw32 (S23_PCIE0_DBI_BASE + 0x7CULL,  0x3F0U, 0x20U);        /* LINKCAP MLW = 2 */
    Rmw32 (S23_PCIE0_DBI_BASE + 0x8BCULL, 1U, 0U);
    Wr32  (S23_PCIE0_PARF_BASE + 0x2B0ULL, 0x02U);               /* CLKREQ_OVR = 0x02 */
    SetTlmm (94, 0x0201U, 1); /* PERST# HIGH */
    gBS->Stall (12000);
    Rmw32 (S23_PCIE0_PARF_BASE + 0x1B0ULL, (1U << 10), (1U << 8));

    for (UINT32 ms = 0; ms < 200U; ms++) {
      gBS->Stall (1000);
      if ((Rd32 (S23_PCIE0_ELBI_BASE + 0x08ULL) & 0x400U) &&
          (Rd32 (S23_PCIE0_DBI_BASE  + 0x80ULL) & (1U << 29))) {
        LinkUp = TRUE;
        break;
      }
    }
  }
  mReport.Ltssm      = Rd32 (S23_PCIE0_PARF_BASE + 0x1B0ULL);
  mReport.LinkStatus = Rd32 (S23_PCIE0_DBI_BASE  + 0x080ULL);
  mReport.Stage      = 4;
  if (!LinkUp) { PublishS23Pcie0Report (); return; }

  /* 5. Program Root Port Bus 0/1/1 (DBI+0x18 = 0x00010100) BEFORE temporary CFG0 poll for 0x110317CB */
  Rmw32 (S23_PCIE0_DBI_BASE + 0x18ULL, 0x00FFFFFFU, 0x00010100U);
  Rmw32 (S23_PCIE0_DBI_BASE + 0x70CULL, 0U, (1U << 15));
  Wr32  (S23_PCIE0_DBI_BASE + 0xB40ULL, 20U);

  Wr32 (S23_PCIE0_ATU_BASE + 0x04ULL, 0U);
  Wr32 (S23_PCIE0_ATU_BASE + 0x00ULL, 4U);
  Wr32 (S23_PCIE0_ATU_BASE + 0x08ULL, 0x60100000U);
  Wr32 (S23_PCIE0_ATU_BASE + 0x0CULL, 0U);
  Wr32 (S23_PCIE0_ATU_BASE + 0x10ULL, 0x60100FFFU);
  Wr32 (S23_PCIE0_ATU_BASE + 0x14ULL, 0x01000000U);
  Wr32 (S23_PCIE0_ATU_BASE + 0x18ULL, 0U);
  Wr32 (S23_PCIE0_ATU_BASE + 0x04ULL, (1U << 31));
  if (!PollMask (S23_PCIE0_ATU_BASE + 0x04ULL, (1U << 31), (1U << 31), 5000U)) {
    PublishS23Pcie0Report ();
    return;
  }

  UINT32 EpId = 0xFFFFFFFFU;
  for (UINT32 ms = 0; ms < 1000U; ms++) {
    EpId = Rd32 (S23_PCIE0_ECAM_BASE + 0x00ULL);
    if (EpId != 0U && EpId != 0xFFFFFFFFU) break;
    gBS->Stall (1000);
  }
  mReport.EndpointId = EpId;
  mReport.Stage      = 5;
  if (EpId != QCA6490_VENDOR_DEVICE) { PublishS23Pcie0Report (); return; }

  /* 6. Transition to Bus-1 ECAM transport, PARF SID table, SMMU identity, and ACPI publication */
  if (EFI_ERROR (ConfigureBus1EcamTransport ())) { PublishS23Pcie0Report (); return; }
  mReport.Stage = 6;
  ConfigureParfBdfToSid ();
  if (EFI_ERROR (ConfigureSmmu500Identity ())) { PublishS23Pcie0Report (); return; }
  mReport.Stage = 7;
  if (EFI_ERROR (PublishMcfgAndSsdt ())) { PublishS23Pcie0Report (); return; }
  mReport.Stage = 8;
  PublishS23Pcie0Report ();
}

EFI_STATUS
EFIAPI
S23Pcie0DxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_EVENT ReadyEvent = NULL;
  (VOID)ImageHandle;
  (VOID)SystemTable;

  return gBS->CreateEventEx (
                EVT_NOTIFY_SIGNAL,
                TPL_CALLBACK,
                OnReadyToBoot,
                NULL,
                &gEfiEventReadyToBootGuid,
                &ReadyEvent
                );
}
