/*
 * s23_pcie0_wlan.asl - Samsung Galaxy S23 (dm1q / SM8550-AC) PCIe0 + QCA6490 (17CB:1103) ACPI SSDT
 *
 * Verified against:
 *   - /home/mahan/mu_s23/s23_live.dts (qcom,pcie@1c00000, pcie0_rp, cnss_pci0)
 *   - Mu-Silicium/Silicium-ACPI/Silicon/Qualcomm/Kailua/Decompiled/MCFG.dsl & IORT.dsl
 *   - repos/oneplus-open-windows/firmware/abi7/G289Pkg/Wlan/G401Publish.c + WIFI401Aml.h
 *   - repos/rm9-windows-arm64/docs/wifi-bringup.md (Gunyah ITS Root-Port & MBI SPI findings)
 *
 * ============================================================================
 * S23 PCIe0 HARDWARE MAP (from s23_live.dts L23478-23566)
 * ============================================================================
 *   PARF          : 0x01C00000 (0x00003000)
 *   MHI           : 0x01C03000 (0x00001000)
 *   QMP PHY       : 0x01C06000 (0x00002000)
 *   DBI (dm_core) : 0x60000000 (0x00000F1D)  -- Hidden Type-1 Root Port (Bus 0 Dev 0 Fn 0)
 *   ELBI          : 0x60000F20 (0x000000A8)
 *   iATU          : 0x60001000 (0x00001000)
 *   ECAM (Bus 1)  : 0x60100000 (0x00100000)  -- iATU Slot 0 CFG-Shift (0x60100000..0x60107FFF)
 *                                               + iATU Slot 1 CFG1 sink (0x60108000..0x601FFFFF -> bus 0xFF)
 *   IO Window     : 0x60200000 (0x00100000)  -- 1 MiB
 *   MEM32 Window  : 0x60300000 (0x03D00000)  -- 61 MiB (0x60300000..0x63FFFFFF), iATU Slot 2 identity MEM
 *
 *   GIC SPIs (s23_live.dts: <0x00 spi 0x04> -> ACPI GSI = spi + 32):
 *     int_global_int : SPI 140 (0x8C) -> GSI 172 (0xAC)
 *     INTA           : SPI 149 (0x95) -> GSI 181 (0xB5)
 *     INTB           : SPI 150 (0x96) -> GSI 182 (0xB6)
 *     INTC           : SPI 151 (0x97) -> GSI 183 (0xB7)
 *     INTD           : SPI 152 (0x98) -> GSI 184 (0xB8)
 *
 *   SMMU (apps_smmu@15000000, ARM SMMU-500, NumSMR=105, NumCB=70):
 *     qcom,smmu-sid-base = 0x1400
 *     iommu-map          = RID 0x0000 -> SID 0x1400 (Root Complex)
 *                          RID 0x0100 -> SID 0x1401 (QCA6490 Endpoint Bus 1 Dev 0 Fn 0)
 *     msi-map            = RID 0x0000 -> DeviceID 0x1400 (GIC ITS)
 *                          RID 0x0100 -> DeviceID 0x1401 (GIC ITS)
 *     PARF_BDF_TO_SID    = Clear bypass bit 0 at 0x01C02C00; zero 0x01C02000..0x01C023FC;
 *                          write 0x01000100 at 0x01C02000 + 4 * crc8_poly07(be16(0x0100)).
 *     dma-coherent       = TRUE (_CCA = 1)
 *
 * ============================================================================
 * COMPANION MCFG & IORT NOTES (MUST BE PAIRED WITH THIS SSDT IN UEFI!)
 * ============================================================================
 *   1. MCFG (Kailua MCFG.dsl):
 *      - Segment 0 Entry: BaseAddress = 0x0000000060000000, SegmentGroup = 0x0000,
 *        StartBus = 0x01, EndBus = 0x01.
 *      - CRITICAL: MCFG BaseAddress stays 0x60000000 (Bus-0 ECAM origin) while
 *        StartBus = 1 so Windows accesses Bus 1 Dev 0 Fn 0 at 0x60000000 + (1 << 20) = 0x60100000!
 *      - Publishing PCI0 at _BBN = 1 hides the virtual/hidden Type-1 Root Port (Bus 0)
 *        from Windows, preventing Windows from registering Root Port MSI DeviceID 0x1400
 *        with Gunyah's virtual GIC ITS (which causes a teardown/reboot hang!).
 *      - NEVER install this SSDT unless s23_uefi_pcie0_dxe has ALREADY trained PCIe0 to L0
 *        and confirmed 0x110317CB at 0x60100000, otherwise Windows pci.sys faults on ECAM!
 *   2. IORT (Kailua IORT.dsl):
 *      - Root Complex Node (Segment 0, MemorySizeLimit = 36 / 0x24):
 *        Maps InputBase 0x0100 (IDCount 1) -> OutputBase 0x1401 -> apps_smmu (0x15000000) / GIC ITS.
 *      - On Gunyah EL2, if apps_smmu (0x15000000) is EL1-writable during UEFI, s23_uefi_pcie0_dxe
 *        pre-programs inactive SMMU-500 context banks for SID 0x1401 & 0x1400 in coherent
 *        identity stage-1 pass-through (M=0, CB_SCTLR=0x0A5F00E0, CBAR=0x0001F300, CBA2R=1).
 *      - If Gunyah blocks GIC ITS MAPD/MAPTI for MSI, use GIC MBI frame (0x17110040) targeting
 *        SPIs 800..811 via companion ACPI helper device \_SB.S23I (declared below, disabled by
 *        default via _STA=0 until MBI mode is selected).
 */

DefinitionBlock ("s23_pcie0_wlan.aml", "SSDT", 2, "SAMSNG", "S23PCIE0", 0x00000001)
{
    Scope (\_SB)
    {
        /*
         * Reserve PCIe0 DBI + iATU + Bus-1 ECAM physical apertures (0x60000000..0x601FFFFF)
         * so Windows does not allocate conflicting MMIO resources into ECAM space.
         */
        Device (R401)
        {
            Name (_HID, EisaId ("PNP0C02"))
            Name (_UID, 0x0191)
            Name (_STA, 0x0F)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite,
                    0x60000000,         // DBI (0x60000000) + Bus-1 ECAM (0x60100000)
                    0x00200000          // 2 MiB total reserved aperture
                    )
            })
        }

        /*
         * PCIe0 Host Bridge exposing Bus 1 directly (hidden Root Port trained by UEFI DXE).
         * Matches Kailua MCFG (Base=0x60000000, Seg=0, StartBus=1, EndBus=1) and
         * G401 3-slot iATU ECAM transport.
         */
        Device (PCI0)
        {
            Name (_HID, EisaId ("PNP0A08"))     // PCI Express Root Bridge
            Name (_CID, EisaId ("PNP0A03"))     // Compatible PCI Root Bridge
            Name (_UID, 0x0000)
            Name (_SEG, 0x0000)                 // PCI Segment Group 0
            Name (_BBN, 0x0001)                 // Base Bus Number = 1 (hidden RP on Bus 0)
            Name (_CCA, 0x0001)                 // Cache-Coherent DMA (s23_live.dts: dma-coherent)
            Name (_STA, 0x0F)

            /*
             * Current Resource Settings:
             *   - Bus range: Bus 1..1
             *   - 32-bit Non-Prefetchable MMIO: 0x60300000..0x63FFFFFF (61 MiB, iATU Slot 2)
             */
            Name (_CRS, ResourceTemplate ()
            {
                WordBusNumber (ResourceProducer, MinFixed, MaxFixed, PosDecode,
                    0x0000,             // Granularity
                    0x0001,             // Range Minimum (Bus 1)
                    0x0001,             // Range Maximum (Bus 1)
                    0x0000,             // Translation Offset
                    0x0001,             // Length (1 bus)
                    ,, )
                DWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed, NonCacheable, ReadWrite,
                    0x00000000,         // Granularity
                    0x60300000,         // Range Minimum (s23_live.dts ranges MEM32)
                    0x63FFFFFF,         // Range Maximum
                    0x00000000,         // Translation Offset (identity)
                    0x03D00000,         // Length (61 MiB)
                    ,, , AddressRangeMemory, TypeStatic)
            })

            /*
             * Legacy INTx PCI Routing Table (s23_live.dts interrupts: SPI 149..152 -> GSI 181..184).
             * Device 0 on Bus 1 (QCA6490) INTA..INTD route directly to GIC GSIs 0xB5..0xB8.
             */
            Name (_PRT, Package ()
            {
                Package () { 0x0000FFFF, 0x00, 0x00, 0x000000B5 }, // Dev 0 INTA -> SPI 149 (GSI 181)
                Package () { 0x0000FFFF, 0x01, 0x00, 0x000000B6 }, // Dev 0 INTB -> SPI 150 (GSI 182)
                Package () { 0x0000FFFF, 0x02, 0x00, 0x000000B7 }, // Dev 0 INTC -> SPI 151 (GSI 183)
                Package () { 0x0000FFFF, 0x03, 0x00, 0x000000B8 }  // Dev 0 INTD -> SPI 152 (GSI 184)
            })

            /*
             * Operating System Capabilities (_OSC) for PCI Host Bridge
             * UUID: 33DB4D5B-1FF7-401C-9657-7441C03DD766
             * Grants Windows control of native PCIe PME (bit 2), AER (bit 3), and
             * Capability Structure (bit 4); keeps Hot Plug (bit 0), SHPC (bit 1),
             * and LTR (bit 5) masked for soldered-down QCA6490 endpoint.
             */
            Name (SUPP, 0x00)
            Name (CTRL, 0x00)
            Method (_OSC, 4, NotSerialized)
            {
                CreateDWordField (Arg3, 0x00, CDW1)
                If (Arg0 == ToUUID ("33db4d5b-1ff7-401c-9657-7441c03dd766"))
                {
                    CreateDWordField (Arg3, 0x04, CDW2)
                    CreateDWordField (Arg3, 0x08, CDW3)
                    SUPP = CDW2
                    CTRL = CDW3
                    /* Grant PME (bit 2), AER (bit 3), PCIe Capability (bit 4); mask HotPlug/SHPC/LTR */
                    CTRL &= 0x0000001C
                    If (Arg1 != 0x01)
                    {
                        CDW1 |= 0x08    // Unrecognized Revision
                    }
                    If (CDW3 != CTRL)
                    {
                        CDW1 |= 0x10    // Capabilities bits were masked
                    }
                    CDW3 = CTRL
                    Return (Arg3)
                }
                Else
                {
                    CDW1 |= 0x04        // Unrecognized UUID
                    Return (Arg3)
                }
            }

            /*
             * PCI Firmware Specification _DSM
             * UUID: E5C937D0-3553-4D7A-9117-EA4D19C3434D
             *   Func 0: Supported functions bitmap (funcs 0, 5, 8)
             *   Func 5: Ignore PCI boot configuration -> 0 (preserve UEFI BAR/bus setup!)
             *   Func 8: Avoid Power-On Reset Delay -> 1 (link already trained in UEFI)
             */
            Method (_DSM, 4, Serialized)
            {
                If (Arg0 == ToUUID ("e5c937d0-3553-4d7a-9117-ea4d19c3434d"))
                {
                    If (Arg2 == 0x00)
                    {
                        Return (Buffer () { 0x21, 0x01 })
                    }
                    If (Arg2 == 0x05)
                    {
                        Return (0x00)
                    }
                    If (Arg2 == 0x08)
                    {
                        Return (0x01)
                    }
                }
                Return (Buffer () { 0x00 })
            }

            /*
             * QCA6490 (WCN685x / FastConnect 6900) Endpoint on Bus 1, Device 0, Function 0
             * PCI ID: VEN_17CB & DEV_1103
             * Explicit _PRW omitted and _S0W = 0 (D0) declared so Windows keeps endpoint
             * in D0 (awake-pinned profile, preventing D3cold power collapse).
             */
            Device (WLAN)
            {
                Name (_ADR, 0x00000000)         // Device 0, Function 0 on Bus 1
                Name (_CCA, 0x0001)             // Cache-coherent DMA
                Name (_SUN, 0x0001)             // Slot User Number
                Name (_S0W, 0x0000)             // Stay in D0 (prevent D3cold power collapse!)
            }
        }

        /*
         * Optional Gunyah GIC MBI SPI Helper Device (inspired by rm9-windows-arm64 RM9I0001).
         * Disabled by default (_STA = 0x00). Change _STA to 0x0F if Gunyah EL2 stalls GIC ITS
         * MAPD/MAPTI commands and QCA6490 MSIs are redirected to GIC MBI frame (0x17110040)
         * targeting edge-triggered SPIs 800..811 (GSIs 832..843).
         */
        Device (S23I)
        {
            Name (_HID, "S23I0001")
            Name (_UID, 0x0000)
            Name (_STA, 0x00)                   // Set to 0x0F when enabling MBI SPI redirect
            Name (_CRS, ResourceTemplate ()
            {
                Interrupt (ResourceConsumer, Edge, ActiveHigh, Exclusive, ,, )
                {
                    832, 833, 834, 835, 836, 837, 838, 839, 840, 841, 842, 843
                }
            })
        }
    }
}
