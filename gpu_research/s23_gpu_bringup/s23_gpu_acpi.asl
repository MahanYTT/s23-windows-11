/*
 * s23_gpu_acpi.asl - Samsung Galaxy S23 (dm1q / SM8550-AC) Adreno 740 + KGSL-SMMU ACPI SSDT
 *
 * Adapted from OnePlus Open G991/G1287 split broker/owner ACPI contract (Mmu1.asl + Gpu0.asl)
 * with exact Samsung Galaxy S23 (SM-S911B / SM8550-AC) hardware resources verified from
 * /home/mahan/mu_s23/s23_live.dts:
 *   - \_SB.MMU1 (_HID "OPEN0D01"): Dedicated GPU SMMU-500 (0x03DA0000) + GPUCC/GCC clock lease
 *     pages + level-triggered GIC GSIs 705 (Global), 711 (CB2), 709 (CB0 / SID0).
 *   - \_SB.GPU0 (_HID "OPEN0515"): Split 5-bank Adreno 740 (GEN7_2_1, 0x07020100) excluding
 *     GPUCC/GCC clock pages owned by MMU1 + S23 GIC GSIs 332 (KGSL), 336 (HFI), 337 (GMU).
 *   - _CCA = 1 (dma-coherent per s23_live.dts) and _S0W = 0 (disable D3Cold under Gunyah EL2).
 *
 * Compile check: iasl -tc s23_gpu_acpi.asl  (must produce 0 Errors, 0 Warnings)
 */
DefinitionBlock ("s23_gpu_acpi.aml", "SSDT", 2, "SAMSNG", "S23GPU74", 0x00000001)
{
    Scope (\_SB)
    {
        /* ====================================================================
         * Dedicated Adreno 740 GPU SMMU-500 & GPUCC/GCC Clock Lease Broker
         * ==================================================================== */
        Device (MMU1)
        {
            Name (_HID, "OPEN0D01")
            Name (_UID, One)
            Name (_DDN, "Samsung S23 SM8550-AC Dedicated Adreno 740 SMMU-500 Clock Broker")
            Name (_CCA, One)
            Name (_S0W, Zero)

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }

            Name (_CRS, ResourceTemplate ()
            {
                /* kgsl-smmu@3da0000 (qcom,qsmmu-v500, 256 KiB) */
                Memory32Fixed (ReadWrite, 0x03DA0000, 0x00040000, SMRG)
                /* GPUCC HLOS1 SMMU vote page (0x03D97000) */
                Memory32Fixed (ReadWrite, 0x03D97000, 0x00001000, SMCK)
                /* GPUCC GDSC / RCG / CBCR control page (0x03D99000) */
                Memory32Fixed (ReadWrite, 0x03D99000, 0x00001000, GPCK)
                /* GCC GPU branch clock page (0x00171000) */
                Memory32Fixed (ReadWrite, 0x00171000, 0x00001000, GCCK)
                /* GCC GPLL0 read-only observation page (0x00100000) */
                Memory32Fixed (ReadOnly,  0x00100000, 0x00001000, GPLL)
                /* GPUCC PLL1 (440 MHz LUCID_OLE) read-only observation page (0x03D91000) */
                Memory32Fixed (ReadOnly,  0x03D91000, 0x00001000, PLL1)

                /* Exact S23 s23_live.dts kgsl-smmu@3da0000 GIC SPIs (+32 = GSI):
                 *   SPI 0x2A1 (673) -> GSI 705 (Global NS fault)
                 *   SPI 0x2A7 (679) -> GSI 711 (CB2 bootstrap/secure)
                 *   SPI 0x2A5 (677) -> GSI 709 (CB0 gfx3d_user SID 0)
                 */
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive, ,, GLBL) { 705 }
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive, ,, CB02) { 711 }
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive, ,, CB00) { 709 }
            })
        }

        /* ====================================================================
         * Adreno 740 (GEN7_2_1, SM8550-AC 719 MHz Bin) Split-Bank GPU Device
         * ==================================================================== */
        Device (GPU0)
        {
            Name (_HID, "OPEN0515")
            Name (_UID, Zero)
            Name (_DDN, "Samsung S23 SM8550-AC Adreno 740 (GEN7_2_1)")
            Name (_DEP, Package () { \_SB.MMU1 })
            Name (_CCA, One)
            Name (_S0W, Zero)

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }

            Name (_CRS, ResourceTemplate ()
            {
                /* Bank 0: KGSL-3D0 core registers (RBBM/CP/UCHE/GBIF/SECVID) */
                Memory32Fixed (ReadWrite, 0x03D00000, 0x00040000, KGSL)
                /* Bank 1: GPU RSCC sequencer */
                Memory32Fixed (ReadWrite, 0x03D50000, 0x00010000, RSCC)
                /* Bank 2: GMU + GMU Core + ISENSE + GMU_AO (ends before GPUCC 0x03D90000) */
                Memory32Fixed (ReadWrite, 0x03D68000, 0x00028000, GMUR)
                /* Bank 3: GPU CX_MISC aperture (0x03D9E000 inside 0x03D9A000..0x03D9EFFF) */
                Memory32Fixed (ReadWrite, 0x03D9A000, 0x00005000, CXMS)
                /* Bank 4: GMU PDC */
                Memory32Fixed (ReadWrite, 0x0B280000, 0x00010000, PDCR)

                /* Exact S23 s23_live.dts GPU + GMU GIC SPIs (+32 = GSI):
                 *   kgsl_3d0_irq : SPI 0x12C (300) -> GSI 332
                 *   gmu_hfi_irq  : SPI 0x130 (304) -> GSI 336
                 *   gmu_irq      : SPI 0x131 (305) -> GSI 337
                 */
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive, ,, G3D0) { 332 }
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive, ,, GHFI) { 336 }
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive, ,, GMUI) { 337 }
            })

            /* Device-specific properties exposing S23 SM8550-AC silicon metadata */
            Name (_DSD, Package ()
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                Package ()
                {
                    Package () { "qcom,adreno-rev",      0x07020100 },
                    Package () { "qcom,max-gpu-freq-hz", 719000000  },
                    Package () { "qcom,gmu-hub-freq-hz", 200000000  },
                    Package () { "qcom,gmem-size",       0x00300000 },
                    Package () { "qcom,highest-bank-bit", 16        },
                    Package () { "qcom,ubwc-mode",       4          },
                    Package () { "qcom,zap-region-base", 0x9B09A000 },
                    Package () { "qcom,zap-region-size", 0x00002000 },
                }
            })
        }
    }
}
