/*
 * s23_subsys_pil.asl - Samsung Galaxy S23 (dm1q / SM8550-AC) SMEM, IPCC,
 *                      MSS (X70 5G), ADSP, CDSP, SPSS & IPA v5.0 ACPI SSDT
 *
 * Verified directly against:
 *   - /home/mahan/mu_s23/s23_live.dts
 *   - /home/mahan/mu_s23/Mu-Silicium/Binaries/dm1q/RawFiles/uefipil.cfg
 *   - /home/mahan/mu_s23/Mu-Silicium/Platforms/Samsung/dm1qPkg/Library/MemoryMapLib/MemoryMapLib.c
 *
 * SAFETY DEFAULT:
 *   Master gate S23P = 0x00 -> ALL devices return _STA = 0x00 (100% inert).
 *   Flip individual device gates or S23P = 0x0F only when matching Windows drivers
 *   and MemoryMapLib carveouts (mpss_dsm 0xD4D00000 + full adspslpi 0x9EA00000) are ready.
 *
 * Compile: iasl -tc s23_subsys_pil.asl  (0 Errors, 0 Warnings, 0 Remarks)
 */

DefinitionBlock ("s23_subsys_pil.aml", "SSDT", 2, "SAMSNG", "S23PIL00", 0x00000001)
{
    Scope (\_SB)
    {
        /* Master inert gate: 0x00 = all disabled (safe default), 0x0F = enabled */
        Name (S23P, 0x00)

        /*
         * Qualcomm SMEM v12 Shared Memory Region + CMD_DB + ChipInfo + IMEM PIL Info
         *   SMEM   : 0x81D00000 (0x200000, $TOC @ 0x81EFF000)
         *   CMD_DB : 0x81C60000 (0x020000)
         *   ADSPMHI: 0x81F00000 (0x020000)
         */
        Device (SMEM)
        {
            Name (_HID, "QCOM0C85")
            Name (_UID, 0x00)
            Name (_CCA, 0x00)

            Method (_STA, 0, NotSerialized)
            {
                Return (S23P)
            }

            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x81D00000, 0x00200000)
                Memory32Fixed (ReadOnly,  0x81C60000, 0x00020000)
                Memory32Fixed (ReadWrite, 0x81F00000, 0x00020000)
            })

            Name (_DSD, Package ()
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                Package ()
                {
                    Package () { "qcom,smem-version", 12 },
                    Package () { "qcom,imem-pil-info-base", 0x146AA94C },
                    Package () { "qcom,imem-pil-info-size", 200 },
                    Package () { "qcom,tcsr-hwlock-id", 3 }
                }
            })
        }

        /*
         * Inter-Processor Communication Controller (qcom,ipcc@00408000)
         *   MMIO: 0x00408000 (0x1000)
         *   IRQ : GIC SPI 229 (0xE5) + 32 = ACPI GSI 261 (0x105), Level ActiveHigh
         */
        Device (IPCC)
        {
            Name (_HID, "QCOM06C2")
            Name (_UID, 0x00)
            Name (_CCA, 0x00)

            Method (_STA, 0, NotSerialized)
            {
                Return (S23P)
            }

            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x00408000, 0x00001000)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive, ,, )
                {
                    0x00000105          // GIC SPI 229 (0xE5) -> GSI 261
                }
            })

            Name (_DSD, Package ()
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                Package ()
                {
                    Package () { "qcom,ipcc-client-aop", 0 },
                    Package () { "qcom,ipcc-client-tz", 1 },
                    Package () { "qcom,ipcc-client-mpss", 2 },
                    Package () { "qcom,ipcc-client-lpass", 3 },
                    Package () { "qcom,ipcc-client-cdsp", 6 },
                    Package () { "qcom,ipcc-client-apss", 8 },
                    Package () { "qcom,ipcc-client-spss", 16 }
                }
            })
        }

        /*
         * Snapdragon X70 5G Modem Q6v5 Remoteproc (remoteproc-mss@04080000)
         *   PAS ID      : 4 (main), 38 / 0x26 (modem_dtb)
         *   DDR Carveout: 0x8A800000 (0x10800000 = 264 MiB) + DTB 0x9B000000 (512 KiB)
         *                 + DSM 0xD4D00000 (0x03300000 = 51 MiB)
         *   WDOG IRQ    : GIC SPI 264 (0x108) + 32 = ACPI GSI 296 (0x128), Edge ActiveHigh
         */
        Device (MSS0)
        {
            Name (_HID, "QCOM0C1C")
            Name (_UID, 0x00)
            Name (_CCA, 0x00)

            Method (_STA, 0, NotSerialized)
            {
                Return (S23P)
            }

            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x04080000, 0x00010000)
                Interrupt (ResourceConsumer, Edge, ActiveHigh, Exclusive, ,, )
                {
                    0x00000128          // GIC SPI 264 (0x108) -> GSI 296
                }
            })

            Name (_DSD, Package ()
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                Package ()
                {
                    Package () { "qcom,pas-id", 4 },
                    Package () { "qcom,dtb-pas-id", 38 },
                    Package () { "qcom,fw-base", 0x8A800000 },
                    Package () { "qcom,fw-size", 0x10800000 },
                    Package () { "qcom,dtb-base", 0x9B000000 },
                    Package () { "qcom,dtb-size", 0x00080000 },
                    Package () { "qcom,dsm-base", 0xD4D00000 },
                    Package () { "qcom,dsm-size", 0x03300000 },
                    Package () { "qcom,remote-pid", 1 },
                    Package () { "qcom,smp2p-in-smem", 435 },
                    Package () { "qcom,smp2p-out-smem", 428 }
                }
            })
        }

        /*
         * Audio & Sensors DSP Remoteproc (remoteproc-adsp@03000000)
         *   PAS ID      : 1 (main), 36 / 0x24 (adsp_dtb)
         *   DDR Carveout: 0x9EA00000 (0x059B4000) + DTB 0x9E980000 (512 KiB)
         *   WDOG IRQ    : PDC 6 -> GIC SPI 486 (0x1E6) + 32 = ACPI GSI 518 (0x206)
         */
        Device (ADSP)
        {
            Name (_HID, "QCOM0C1B")
            Name (_UID, 0x00)
            Name (_CCA, 0x00)

            Method (_STA, 0, NotSerialized)
            {
                Return (S23P)
            }

            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x03000000, 0x00010000)
                Interrupt (ResourceConsumer, Edge, ActiveHigh, Exclusive, ,, )
                {
                    0x00000206          // PDC 6 -> SPI 486 (0x1E6) -> GSI 518
                }
            })

            Name (_DSD, Package ()
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                Package ()
                {
                    Package () { "qcom,pas-id", 1 },
                    Package () { "qcom,dtb-pas-id", 36 },
                    Package () { "qcom,fw-base", 0x9EA00000 },
                    Package () { "qcom,fw-size", 0x059B4000 },
                    Package () { "qcom,dtb-base", 0x9E980000 },
                    Package () { "qcom,dtb-size", 0x00080000 },
                    Package () { "qcom,remote-pid", 2 },
                    Package () { "qcom,smp2p-in-smem", 443 },
                    Package () { "qcom,smp2p-out-smem", 429 }
                }
            })
        }

        /*
         * Hexagon NPU / Compute DSP Remoteproc (remoteproc-cdsp@32300000)
         *   PAS ID      : 18 / 0x12 (main), 37 / 0x25 (cdsp_dtb)
         *   DDR Carveout: 0x9C900000 (0x02000000 = 32 MiB) + DTB 0x9E900000 (512 KiB)
         *   WDOG IRQ    : GIC SPI 578 (0x242) + 32 = ACPI GSI 610 (0x262)
         */
        Device (CDSP)
        {
            Name (_HID, "QCOM0CB0")
            Name (_UID, 0x00)
            Name (_CCA, 0x00)

            Method (_STA, 0, NotSerialized)
            {
                Return (S23P)
            }

            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x32300000, 0x00010000)
                Interrupt (ResourceConsumer, Edge, ActiveHigh, Exclusive, ,, )
                {
                    0x00000262          // GIC SPI 578 (0x242) -> GSI 610
                }
            })

            Name (_DSD, Package ()
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                Package ()
                {
                    Package () { "qcom,pas-id", 18 },
                    Package () { "qcom,dtb-pas-id", 37 },
                    Package () { "qcom,fw-base", 0x9C900000 },
                    Package () { "qcom,fw-size", 0x02000000 },
                    Package () { "qcom,dtb-base", 0x9E900000 },
                    Package () { "qcom,dtb-size", 0x00080000 },
                    Package () { "qcom,remote-pid", 5 },
                    Package () { "qcom,smp2p-in-smem", 94 },
                    Package () { "qcom,smp2p-out-smem", 432 }
                }
            })
        }

        /*
         * Secure Processor Subsystem (remoteproc-spss@01880000)
         *   PAS ID      : 14 / 0x0E (\image\spss1t)
         *   DDR Carveout: 0x9B100000 (0x00180000 = 1.5 MiB)
         *   Generic IRQ : GIC SPI 352 (0x160) + 32 = ACPI GSI 384 (0x180)
         */
        Device (SPSS)
        {
            Name (_HID, "QCOM0C8D")
            Name (_UID, 0x00)
            Name (_CCA, 0x00)

            Method (_STA, 0, NotSerialized)
            {
                Return (S23P)
            }

            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x01881000, 0x00005000)
                Interrupt (ResourceConsumer, Edge, ActiveHigh, Exclusive, ,, )
                {
                    0x00000180          // GIC SPI 352 (0x160) -> GSI 384
                }
            })

            Name (_DSD, Package ()
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                Package ()
                {
                    Package () { "qcom,pas-id", 14 },
                    Package () { "qcom,fw-base", 0x9B100000 },
                    Package () { "qcom,fw-size", 0x00180000 },
                    Package () { "qcom,remote-pid", 8 }
                }
            })
        }

        /*
         * IPA v5.0 + GSI Offload Engine (qcom,ipa@03E00000, PAS ID = 0x0F)
         *   NOTE: SM8550 IPA moved from legacy 0x01E00000 to 0x03E00000 (ipa-hw-ver 0x18 = v5.0)!
         *   IPA Regs : 0x03E00000 (0x084000) | ipa-irq SPI 654 (0x28E) -> GSI 686 (0x2AE)
         *   GSI Regs : 0x03E04000 (0x0FC000) | gsi-irq SPI 432 (0x1B0) -> GSI 464 (0x1D0)
         */
        Device (IPA0)
        {
            Name (_HID, "QCOM0C6A")
            Name (_UID, 0x00)
            Name (_CCA, 0x01)

            Method (_STA, 0, NotSerialized)
            {
                Return (S23P)
            }

            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x03E00000, 0x00084000)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive, ,, )
                {
                    0x000002AE          // GIC SPI 654 (0x28E) -> GSI 686
                }
                Memory32Fixed (ReadWrite, 0x03E04000, 0x000FC000)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive, ,, )
                {
                    0x000001D0          // GIC SPI 432 (0x1B0) -> GSI 464
                }
            })

            Name (_DSD, Package ()
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                Package ()
                {
                    Package () { "qcom,ipa-hw-ver", 0x18 },
                    Package () { "qcom,pas-id", 15 },
                    Package () { "qcom,ipa-fw-base", 0x9B080000 },
                    Package () { "qcom,ipa-fw-size", 0x00010000 },
                    Package () { "qcom,ipa-gsi-base", 0x9B090000 },
                    Package () { "qcom,ipa-gsi-size", 0x0000A000 },
                    Package () { "qcom,smmu-ap-sid", 0x4A0 }
                }
            })
        }
    }
}
