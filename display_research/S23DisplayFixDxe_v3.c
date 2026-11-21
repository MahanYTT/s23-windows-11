/** @file
  S23DisplayFixDxe.c - Samsung Galaxy S23 (dm1q / SM8550-AC) Command-Mode Display Auto-Refresh & Wake Fix v3

  1. Full 5-register SM8550 G116Gate on DISP_CC (0x0AF00000).
  2. Live DSI0 BTA panel state check (0x0A POWER_MODE, 0x52 BRIGHTNESS) at DXE entry:
     wakes S6E3FAC via DSI0 TPG-FIFO (DT=0x39 DCS Long Write + 0x15 + F1/FC unlock) whenever
     Sleep Out/Display On is cleared, DBV <= 0x0010, or TE_INT_CNT (0x36298) is frozen.
  3. Accurate command-mode progress detection (AR_STATUS +0x2C0, TE_INT_CNT +0x298, FRAME_CNT +0xAC)
     and non-colliding ExitBootServices handoff.
**/

#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Library/ArmLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define DISP_CC_BASE                      0x0AF00000ULL
#define DISP_CC_MDSS_CORE_BCR             (DISP_CC_BASE + 0x8000ULL)
#define DISP_CC_MDSS_MDP_CLK_CBCR         (DISP_CC_BASE + 0x800CULL)
#define DISP_CC_MDSS_ESC0_CLK_CBCR        (DISP_CC_BASE + 0x8038ULL)
#define DISP_CC_MDSS_AHB_CLK_CBCR         (DISP_CC_BASE + 0x80A4ULL)
#define DISP_CC_MDSS_CORE_GDSCR           (DISP_CC_BASE + 0x9000ULL)
#define DISP_CC_MDSS_CORE_CFG_GDSCR       (DISP_CC_BASE + 0x9004ULL)

#define MDSS_BASE                         0x0AE00000ULL
#define MDSS_HW_VERSION                   (MDSS_BASE + 0x00000ULL)
#define MDP_CTL_0_START                   (MDSS_BASE + 0x1601CULL)

/* INTF_1 @ MDSS + 0x36000 */
#define MDP_INTF_1_FLC_EN                 (MDSS_BASE + 0x360A8ULL)
#define MDP_INTF_1_FRAME_COUNT            (MDSS_BASE + 0x360ACULL)
#define MDP_INTF_1_TEAR_CHECK_EN          (MDSS_BASE + 0x36284ULL)
#define MDP_INTF_1_SYNC_VSYNC             (MDSS_BASE + 0x36288ULL)
#define MDP_INTF_1_TEAR_INT_COUNT         (MDSS_BASE + 0x36298ULL)
#define MDP_INTF_1_AUTOREFRESH_CONFIG     (MDSS_BASE + 0x362B4ULL)
#define MDP_INTF_1_AUTOREFRESH_STATUS     (MDSS_BASE + 0x362C0ULL)

/* DSI0_CTRL @ MDSS + 0x94000 */
#define DSI0_BASE                         (MDSS_BASE + 0x94000ULL)
#define DSI0_CTRL                         (DSI0_BASE + 0x004ULL)
#define DSI0_STATUS                       (DSI0_BASE + 0x008ULL)
#define DSI0_CMD_MODE_DMA_CTRL            (DSI0_BASE + 0x03CULL)
#define DSI0_DMA_CMD_LENGTH               (DSI0_BASE + 0x04CULL)
#define DSI0_RDBK_DATA0                   (DSI0_BASE + 0x06CULL)
#define DSI0_CMD_MODE_DMA_SW_TRIGGER      (DSI0_BASE + 0x090ULL)
#define DSI0_INT_CTRL                     (DSI0_BASE + 0x110ULL)
#define DSI0_TPG_MAIN_CONTROL             (DSI0_BASE + 0x15CULL)
#define DSI0_TPG_DMA_FIFO_DATA            (DSI0_BASE + 0x17CULL)
#define DSI0_RDBK_DATA_CTRL               (DSI0_BASE + 0x1D4ULL)
#define DSI0_TPG_DMA_FIFO_RESET           (DSI0_BASE + 0x1ECULL)

#define AUTOREFRESH_ENABLE_60HZ           0x80000002U

#ifndef S23_ENABLE_DSI_TPG_WAKE
#define S23_ENABLE_DSI_TPG_WAKE           1
#endif

STATIC EFI_EVENT  mRefreshTimer    = NULL;
STATIC EFI_EVENT  mExitBootEvent   = NULL;
STATIC UINT32     mLastFrameCount  = 0;
STATIC UINT32     mLastTeIntCnt    = 0;
STATIC UINT32     mLastArStatus    = 0;
STATIC BOOLEAN    mHaveSample      = FALSE;
STATIC BOOLEAN    mObservedTicking = FALSE;
STATIC UINT32     mStallStreak     = 0;

STATIC UINT32
Rd32 (
  IN UINT64  Addr
  )
{
  UINT32  Val;

  ArmDataMemoryBarrier ();
  Val = MmioRead32 ((UINTN)Addr);
  ArmDataMemoryBarrier ();
  return Val;
}

STATIC VOID
Wr32 (
  IN UINT64  Addr,
  IN UINT32  Val
  )
{
  ArmDataMemoryBarrier ();
  MmioWrite32 ((UINTN)Addr, Val);
  ArmDataMemoryBarrier ();
}

STATIC VOID
DelayMs (
  IN UINT32  Ms
  )
{
  UINT32  I;

  for (I = 0; I < Ms; I++) {
    gBS->Stall (1000);
  }
}

STATIC BOOLEAN
IsMdssG116GateOpen (
  VOID
  )
{
  UINT32  CoreBcr;
  UINT32  MdpCbcr;
  UINT32  AhbCbcr;
  UINT32  Gdscr;
  UINT32  CfgGdscr;

  CoreBcr  = Rd32 (DISP_CC_MDSS_CORE_BCR);
  MdpCbcr  = Rd32 (DISP_CC_MDSS_MDP_CLK_CBCR);
  AhbCbcr  = Rd32 (DISP_CC_MDSS_AHB_CLK_CBCR);
  Gdscr    = Rd32 (DISP_CC_MDSS_CORE_GDSCR);
  CfgGdscr = Rd32 (DISP_CC_MDSS_CORE_CFG_GDSCR);

  if ((CoreBcr & 1U) != 0U) {
    return FALSE;
  }
  if ((MdpCbcr & 0x80000001U) != 1U) {
    return FALSE;
  }
  if ((AhbCbcr & 0x80000001U) != 1U) {
    return FALSE;
  }
  if ((Gdscr & 0x80000007U) != 0x80000000U) {
    return FALSE;
  }
  if ((CfgGdscr & 0x00018000U) != 0x00010000U) {
    return FALSE;
  }
  return TRUE;
}

#if S23_ENABLE_DSI_TPG_WAKE
STATIC BOOLEAN
Dsi0WaitIdle (
  VOID
  )
{
  UINT32  I;

  for (I = 0; I < 60U; I++) {
    if ((Rd32 (DSI0_STATUS) & 0x07U) == 0U) {
      return TRUE;
    }
    gBS->Stall ((I < 10U) ? 20U : 1000U);
  }
  return FALSE;
}

STATIC BOOLEAN
Dsi0SendTpgFifoPkt (
  IN CONST UINT32  *Dwords,
  IN UINT32        NumDwords
  )
{
  UINT32   SavedDmaCtrl;
  UINT32   DmaCtrl;
  UINT32   IntCtrl;
  UINT32   FinalInt = 0;
  UINT32   I;
  BOOLEAN  Done = FALSE;

  if (!Dsi0WaitIdle ()) {
    return FALSE;
  }

  SavedDmaCtrl = Rd32 (DSI0_CMD_MODE_DMA_CTRL);

  Wr32 (DSI0_TPG_MAIN_CONTROL, 0U);
  Wr32 (DSI0_TPG_DMA_FIFO_RESET, 1U);
  gBS->Stall (2);
  Wr32 (DSI0_TPG_DMA_FIFO_RESET, 0U);

  Wr32 (DSI0_TPG_MAIN_CONTROL, (1U << 1) | (1U << 2) | (3U << 16));

  for (I = 0; I < NumDwords; I++) {
    Wr32 (DSI0_TPG_DMA_FIFO_DATA, Dwords[I]);
  }
  if ((NumDwords & 1U) != 0U) {
    Wr32 (DSI0_TPG_DMA_FIFO_DATA, 0U);
  }

  DmaCtrl  = SavedDmaCtrl;
  DmaCtrl &= ~((1U << 31) | (1U << 30) | (1U << 29) | (1U << 24));
  DmaCtrl |= (1U << 28) | (1U << 26);
  Wr32 (DSI0_CMD_MODE_DMA_CTRL, DmaCtrl);

  IntCtrl = Rd32 (DSI0_INT_CTRL);
  Wr32 (DSI0_INT_CTRL, (IntCtrl & 0xAAEEAAFEU) | 1U);

  Wr32 (DSI0_DMA_CMD_LENGTH, NumDwords * 4U);
  Wr32 (DSI0_CMD_MODE_DMA_SW_TRIGGER, 1U);

  for (I = 0; I < 75U; I++) {
    FinalInt = Rd32 (DSI0_INT_CTRL);
    if ((FinalInt & 1U) != 0U) {
      Wr32 (DSI0_INT_CTRL, (FinalInt & 0xAAEEAAFEU) | 1U);
      Done = TRUE;
      break;
    }
    gBS->Stall ((I < 25U) ? 20U : 1000U);
  }

  Wr32 (DSI0_TPG_MAIN_CONTROL, 0U);
  Wr32 (DSI0_TPG_DMA_FIFO_RESET, 1U);
  gBS->Stall (2);
  Wr32 (DSI0_TPG_DMA_FIFO_RESET, 0U);
  Wr32 (DSI0_CMD_MODE_DMA_CTRL, SavedDmaCtrl);
  return Done;
}

STATIC BOOLEAN
Dsi0ReadDcsShortBta (
  IN  UINT8   Reg,
  IN  UINT32  MaxRet,
  OUT UINT32  *D0Out
  )
{
  UINT32   SetMax[1];
  UINT32   SavedDmaCtrl;
  UINT32   DmaCtrl;
  UINT32   IntCtrl;
  UINT32   FinalInt = 0;
  UINT32   RdbkCtrl;
  UINT32   I;
  BOOLEAN  Done = FALSE;

  if (D0Out != NULL) {
    *D0Out = 0;
  }
  if (!Dsi0WaitIdle ()) {
    return FALSE;
  }

  Wr32 (DSI0_RDBK_DATA_CTRL, 1U);
  gBS->Stall (5);
  Wr32 (DSI0_RDBK_DATA_CTRL, 0U);
  gBS->Stall (5);

  SetMax[0] = 0x80370000U | (MaxRet & 0xFFFFU);
  if (!Dsi0SendTpgFifoPkt (SetMax, 1)) {
    return FALSE;
  }

  SavedDmaCtrl = Rd32 (DSI0_CMD_MODE_DMA_CTRL);
  Wr32 (DSI0_TPG_MAIN_CONTROL, 0U);
  Wr32 (DSI0_TPG_DMA_FIFO_RESET, 1U);
  gBS->Stall (2);
  Wr32 (DSI0_TPG_DMA_FIFO_RESET, 0U);
  Wr32 (DSI0_TPG_MAIN_CONTROL, (1U << 1) | (1U << 2) | (3U << 16));
  Wr32 (DSI0_TPG_DMA_FIFO_DATA, 0xA0060000U | (UINT32)Reg);
  Wr32 (DSI0_TPG_DMA_FIFO_DATA, 0U);

  DmaCtrl  = SavedDmaCtrl;
  DmaCtrl &= ~((1U << 31) | (1U << 30) | (1U << 29) | (1U << 24));
  DmaCtrl |= (1U << 28) | (1U << 26);
  Wr32 (DSI0_CMD_MODE_DMA_CTRL, DmaCtrl);

  IntCtrl = Rd32 (DSI0_INT_CTRL);
  Wr32 (DSI0_INT_CTRL, (IntCtrl & 0xAAEEAAFEU) | 1U | (1U << 20));
  Wr32 (DSI0_DMA_CMD_LENGTH, 4U);
  Wr32 (DSI0_CMD_MODE_DMA_SW_TRIGGER, 1U);

  for (I = 0; I < 100U; I++) {
    FinalInt = Rd32 (DSI0_INT_CTRL);
    if (((FinalInt & 1U) != 0U) && ((FinalInt & (1U << 20)) != 0U)) {
      Done = TRUE;
      break;
    }
    DelayMs (1);
  }
  Wr32 (DSI0_INT_CTRL, (FinalInt & 0xAAEEAAFEU) | 1U | (1U << 20));

  RdbkCtrl = Rd32 (DSI0_RDBK_DATA_CTRL);
  if (Done && ((RdbkCtrl >> 16) >= 4U) && (D0Out != NULL)) {
    *D0Out = Rd32 (DSI0_RDBK_DATA0);
  } else {
    Done = FALSE;
  }

  Wr32 (DSI0_TPG_MAIN_CONTROL, 0U);
  Wr32 (DSI0_TPG_DMA_FIFO_RESET, 1U);
  gBS->Stall (2);
  Wr32 (DSI0_TPG_DMA_FIFO_RESET, 0U);
  Wr32 (DSI0_CMD_MODE_DMA_CTRL, SavedDmaCtrl);
  return Done;
}

STATIC VOID
TryDsi0S6e3facWakeFallback (
  IN BOOLEAN  SendSleepOut
  )
{
  STATIC CONST UINT32  PktUnlock9f[2] = { 0xC0390003U, 0xFFA5A59FU }; /* 9F A5 A5 */
  STATIC CONST UINT32  PktUnlockF0[2] = { 0xC0390003U, 0xFF5A5AF0U }; /* F0 5A 5A */
  STATIC CONST UINT32  PktUnlockF1[2] = { 0xC0390003U, 0xFF5A5AF1U }; /* F1 5A 5A */
  STATIC CONST UINT32  PktUnlockFc[2] = { 0xC0390003U, 0xFF5A5AFCU }; /* FC 5A 5A */
  STATIC CONST UINT32  PktSlpOut[1]   = { 0x80050011U };              /* 11 (Sleep Out) */
  STATIC CONST UINT32  PktTeOn[1]     = { 0x80150035U };              /* 35 00 (TE ON) */
  STATIC CONST UINT32  PktWrCtrld[1]  = { 0x80152053U };              /* 53 20 (BCTRL on) */
  STATIC CONST UINT32  PktDbvMax[2]   = { 0xC0390003U, 0xFFFF0751U }; /* 51 07 FF */
  STATIC CONST UINT32  PktLatchF7[1]  = { 0x80150FF7U };              /* F7 0F (update latch) */
  STATIC CONST UINT32  PktDispOn[1]   = { 0x80050029U };              /* 29 (Display On) */
  STATIC CONST UINT32  PktLockFc[2]   = { 0xC0390003U, 0xFFA5A5FCU }; /* FC A5 A5 */
  STATIC CONST UINT32  PktLockF1[2]   = { 0xC0390003U, 0xFFA5A5F1U }; /* F1 A5 A5 */
  STATIC CONST UINT32  PktLockF0[2]   = { 0xC0390003U, 0xFFA5A5F0U }; /* F0 A5 A5 */
  STATIC CONST UINT32  PktLock9f[2]   = { 0xC0390003U, 0xFF5A5A9FU }; /* 9F 5A 5A */

  UINT32  EscCbcr = Rd32 (DISP_CC_MDSS_ESC0_CLK_CBCR);
  UINT32  DsiCtrl = Rd32 (DSI0_CTRL);
  UINT32  PrevAr;

  if ((EscCbcr & 0x80000001U) != 1U || (DsiCtrl & 0x05U) != 0x05U) {
    return;
  }

  PrevAr = Rd32 (MDP_INTF_1_AUTOREFRESH_CONFIG);
  if ((PrevAr & 0x80000000U) != 0U) {
    Wr32 (MDP_INTF_1_AUTOREFRESH_CONFIG, PrevAr & ~0x80000000U);
    DelayMs (20);
  }

  DEBUG ((DEBUG_WARN, "S23DisplayFixDxe v3: Running DSI0 TPG-FIFO S6E3FAC wake (SlpOut=%u)\n", SendSleepOut ? 1U : 0U));

  (VOID)Dsi0SendTpgFifoPkt (PktUnlock9f, 2);
  (VOID)Dsi0SendTpgFifoPkt (PktUnlockF0, 2);
  (VOID)Dsi0SendTpgFifoPkt (PktUnlockF1, 2);
  (VOID)Dsi0SendTpgFifoPkt (PktUnlockFc, 2);
  if (SendSleepOut) {
    (VOID)Dsi0SendTpgFifoPkt (PktSlpOut, 1);
    DelayMs (120);
  }
  (VOID)Dsi0SendTpgFifoPkt (PktTeOn,    1);
  (VOID)Dsi0SendTpgFifoPkt (PktWrCtrld, 1);
  (VOID)Dsi0SendTpgFifoPkt (PktDbvMax,  2);
  (VOID)Dsi0SendTpgFifoPkt (PktLatchF7, 1);
  (VOID)Dsi0SendTpgFifoPkt (PktDispOn,  1);
  DelayMs (20);
  (VOID)Dsi0SendTpgFifoPkt (PktLockFc,  2);
  (VOID)Dsi0SendTpgFifoPkt (PktLockF1,  2);
  (VOID)Dsi0SendTpgFifoPkt (PktLockF0,  2);
  (VOID)Dsi0SendTpgFifoPkt (PktLock9f,  2);

  if ((PrevAr & 0x80000000U) != 0U) {
    Wr32 (MDP_INTF_1_AUTOREFRESH_CONFIG, PrevAr);
  }
}
#endif

STATIC VOID
MonitorAndHealRefresh (
  IN BOOLEAN  IsExitBootServices
  )
{
  UINT32   HwVer;
  UINT32   DsiCtrl;
  UINT32   SyncVsync;
  UINT32   FlcEn;
  UINT32   FrameCnt;
  UINT32   TearEn;
  UINT32   TeIntCnt;
  UINT32   AutoRef;
  UINT32   ArStatus;
  BOOLEAN  ArArmed;
  BOOLEAN  Ticked = FALSE;
  BOOLEAN  NeedKick = FALSE;

  if (!IsMdssG116GateOpen ()) {
    return;
  }

  HwVer = Rd32 (MDSS_HW_VERSION);
  if ((HwVer == 0U) || (HwVer == 0xFFFFFFFFU)) {
    return;
  }

  DsiCtrl   = Rd32 (DSI0_CTRL);
  SyncVsync = Rd32 (MDP_INTF_1_SYNC_VSYNC);
  if (((DsiCtrl & 0x05U) != 0x05U) || ((SyncVsync & (1U << 19)) == 0U)) {
    return;
  }

  FlcEn    = Rd32 (MDP_INTF_1_FLC_EN);
  (VOID)FlcEn;
  FrameCnt = Rd32 (MDP_INTF_1_FRAME_COUNT);
  TearEn   = Rd32 (MDP_INTF_1_TEAR_CHECK_EN);
  TeIntCnt = Rd32 (MDP_INTF_1_TEAR_INT_COUNT);
  AutoRef  = Rd32 (MDP_INTF_1_AUTOREFRESH_CONFIG);
  ArStatus = Rd32 (MDP_INTF_1_AUTOREFRESH_STATUS);

  ArArmed = ((TearEn & 1U) != 0U) && ((AutoRef & 0x80000000U) != 0U);

  if (mHaveSample) {
    if ((ArStatus != mLastArStatus) || (TeIntCnt != mLastTeIntCnt) ||
        (FrameCnt != mLastFrameCount)) {
      Ticked = TRUE;
      mObservedTicking = TRUE;
      mStallStreak = 0;
    } else if (ArArmed) {
      mStallStreak++;
    }
  }

  if (!ArArmed) {
    NeedKick = TRUE;
  } else if (IsExitBootServices) {
    if (!mObservedTicking || (mStallStreak >= 2U)) {
      NeedKick = TRUE;
    }
  } else if (mHaveSample && !Ticked && (mStallStreak >= 2U)) {
    NeedKick = TRUE;
  }

  if (NeedKick) {
    Wr32 (MDP_INTF_1_TEAR_CHECK_EN, 1U);
    Wr32 (MDP_INTF_1_AUTOREFRESH_CONFIG, AUTOREFRESH_ENABLE_60HZ);
    Wr32 (MDP_CTL_0_START, 1U);
    mStallStreak = 0;
  }

  mLastFrameCount = FrameCnt;
  mLastTeIntCnt   = TeIntCnt;
  mLastArStatus   = ArStatus;
  mHaveSample     = TRUE;
}

STATIC VOID
EFIAPI
OnRefreshTimer (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  (VOID)Event;
  (VOID)Context;
  MonitorAndHealRefresh (FALSE);
}

STATIC VOID
EFIAPI
OnExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  (VOID)Event;
  (VOID)Context;

  if (mRefreshTimer != NULL) {
    gBS->SetTimer (mRefreshTimer, TimerCancel, 0);
  }

  MonitorAndHealRefresh (TRUE);
}

EFI_STATUS
EFIAPI
S23DisplayFixEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  (VOID)ImageHandle;
  (VOID)SystemTable;

  if (IsMdssG116GateOpen ()) {
    UINT32  HwVer    = Rd32 (MDSS_HW_VERSION);
    UINT32  DsiCtrl  = Rd32 (DSI0_CTRL);
    UINT32  SyncVs   = Rd32 (MDP_INTF_1_SYNC_VSYNC);
    UINT32  Te1      = Rd32 (MDP_INTF_1_TEAR_INT_COUNT);
    UINT32  ArSt1    = Rd32 (MDP_INTF_1_AUTOREFRESH_STATUS);
    UINT32  Te2;
    UINT32  ArSt2;

    DEBUG ((DEBUG_INFO, "S23DisplayFixDxe v3: G116Gate OPEN (HW_VER=0x%08X DSI0=0x%08X SYNC=0x%08X)\n",
      HwVer, DsiCtrl, SyncVs));

    DelayMs (35);
    Te2   = Rd32 (MDP_INTF_1_TEAR_INT_COUNT);
    ArSt2 = Rd32 (MDP_INTF_1_AUTOREFRESH_STATUS);

    if ((ArSt2 != ArSt1) || (Te2 != Te1)) {
      mObservedTicking = TRUE;
    }

#if S23_ENABLE_DSI_TPG_WAKE
    if (((Rd32 (DISP_CC_MDSS_ESC0_CLK_CBCR) & 0x80000001U) == 1U) &&
        ((DsiCtrl & 0x05U) == 0x05U) && ((SyncVs & (1U << 19)) != 0U)) {
      UINT32   PrevAr   = Rd32 (MDP_INTF_1_AUTOREFRESH_CONFIG);
      UINT32   D0Pwr    = 0;
      UINT32   D0Dbv    = 0;
      BOOLEAN  OkPwr;
      BOOLEAN  OkDbv;
      BOOLEAN  NeedWake = FALSE;
      BOOLEAN  NeedSlp  = FALSE;

      if ((PrevAr & 0x80000000U) != 0U) {
        Wr32 (MDP_INTF_1_AUTOREFRESH_CONFIG, PrevAr & ~0x80000000U);
        DelayMs (20);
      }

      OkPwr = Dsi0ReadDcsShortBta (0x0AU, 2U, &D0Pwr);
      OkDbv = Dsi0ReadDcsShortBta (0x52U, 2U, &D0Dbv);

      if ((PrevAr & 0x80000000U) != 0U) {
        Wr32 (MDP_INTF_1_AUTOREFRESH_CONFIG, PrevAr);
      }

      if (OkPwr && OkDbv) {
        UINT8   Pwr = (UINT8)((D0Pwr >> 16) & 0xFFU);
        UINT16  Dbv = (UINT16)((D0Dbv >> 8) & 0xFFFFU);
        if (((Pwr & 0x14U) != 0x14U) || (Dbv <= 0x0010U) || (Te2 == Te1)) {
          NeedWake = TRUE;
          NeedSlp  = ((Pwr & 0x10U) == 0U) || (Te2 == Te1);
        }
        DEBUG ((DEBUG_WARN, "S23DisplayFixDxe v3: BTA Pwr=0x%02X Dbv=0x%04X NeedWake=%u Slp=%u\n",
          Pwr, Dbv, NeedWake ? 1U : 0U, NeedSlp ? 1U : 0U));
      } else if (Te2 == Te1) {
        NeedWake = TRUE;
        NeedSlp  = TRUE;
      }

      if (NeedWake) {
        TryDsi0S6e3facWakeFallback (NeedSlp);
      }
    }
#endif

    mLastFrameCount = Rd32 (MDP_INTF_1_FRAME_COUNT);
    mLastTeIntCnt   = Rd32 (MDP_INTF_1_TEAR_INT_COUNT);
    mLastArStatus   = Rd32 (MDP_INTF_1_AUTOREFRESH_STATUS);
    mHaveSample     = TRUE;

    MonitorAndHealRefresh (FALSE);
  } else {
    DEBUG ((DEBUG_WARN, "S23DisplayFixDxe v3: 5-register G116Gate CLOSED at DXE entry; skipping MMIO\n"));
  }

  Status = gBS->CreateEvent (
                  EVT_TIMER | EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  OnRefreshTimer,
                  NULL,
                  &mRefreshTimer
                  );
  if (!EFI_ERROR (Status)) {
    gBS->SetTimer (mRefreshTimer, TimerPeriodic, 1000000ULL);
  }

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  OnExitBootServices,
                  NULL,
                  &gEfiEventExitBootServicesGuid,
                  &mExitBootEvent
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "S23DisplayFixDxe v3: CreateEventEx(ExitBootServices) failed: %r\n", Status));
  }

  return EFI_SUCCESS;
}
