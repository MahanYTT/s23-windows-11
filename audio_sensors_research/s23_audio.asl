/*
 * s23_audio.asl - Samsung Galaxy S23 (dm1q / SM8550-AC) Native Audio & Haptics ACPI SSDT
 *
 * Hardware Verified Directly Against:
 *   - /home/mahan/mu_s23/s23_live.dts
 *   - /home/mahan/s23/src/kernel_platform/msm-kernel/sound/soc/codecs/{cs35l43.h,cs35l43.c,cs40l26.c}
 *   - /home/mahan/s23/src/kernel_platform/msm-kernel/drivers/pinctrl/qcom/pinctrl-kalama.c
 *   - /home/mahan/s23/src/vendor/qcom/opensource/audio-kernel/include/dsp/audio_prm.h
 *   - /home/mahan/s23/src/vendor/qcom/opensource/audio-hal/primary-hal/configs/kalama/audconf/dm1q/
 *   - Live S23 probe logs: s23sensprobe_v3_m13_live.log & s23pilprobe_m7_live.log
 *
 * Namespace Collision Check (against live Mu-Silicium dm1q DSDT.aml + Common/SSDT.aml):
 *   - dm1q DSDT.aml declares ONLY: \_SB.{SYSM,CLS0..2,CPU0..7,UFS0,UUPC,URS0,USB0,UFN0}.
 *   - Zero collisions with \_SB.AUD0 and \_SB.HAP0.
 *   - Keep s23_subsys_pil.asl S23P = 0x00 while S23A = 0x0F (since both declare SMEM/IPCC).
 *
 * SAFETY DEFAULT:
 *   Master gates S23A = 0x00 (Audio) and S23H = 0x00 (Haptics) default to disabled (_STA = 0)
 *   so loading this SSDT is 100% inert until matching Windows drivers (s23audio.sys / s23haptics.sys)
 *   are installed and MemoryMapLib PIL Reserved covers 0x89800000..0xA43B4000 (len 0x1ABB4000).
 *
 * Compile: iasl -tc s23_audio.asl  (0 Errors, 0 Warnings, 0 Remarks)
 */

DefinitionBlock ("s23_audio.aml", "SSDT", 2, "SAMSNG", "S23AUD85", 0x00000001)
{
    Scope (\_SB)
    {
        /* Master activation gates: 0x00 = inert default, 0x0F = present & enabled */
        Name (S23A, 0x00)
        Name (S23H, 0x00)

        /*
         * Composite SM8550 Audio & ADSP AudioReach Controller (\_SB.AUD0, ACPI\S23A8550)
         * Owns exclusive/brokered ADSP PIL + IPC + CS35L43 I2C Hub SE6 resources without
         * requiring unavailable Qualcomm PEP/GPIO/I2C/GLink bus drivers.
         *
         * MMIO Apertures:
         *   - 0x81D00000 (2 MiB)   : SMEM v12 (APPS:ADSP partition @+0x19F000, $TOC @+0x1FF000)
         *   - 0x81C60000 (128 KiB) : AOP CMD_DB (read-only)
         *   - 0x00408000 (4 KiB)   : IPCC APSS MPROC mailbox
         *   - 0x0C300000 (4 KiB)   : AOSS QMP message RAM
         *   - 0x17A20000 (8 KiB)   : RPMh RSC DRV2 TCS bank
         *   - 0x03000000 (64 KiB)  : remoteproc-adsp@03000000 Q6v5 register block
         *   - 0x009C0000 (8 KiB)   : QUPv3_I2C_HUB wrapper
         *   - 0x00998000 (16 KiB)  : QUPv3_I2C_HUB SE6 (Dual CS35L43 smart speaker amps)
         *
         * Interrupts (GIC SPI + 32 = ACPI GSI):
         *   - SPI 229 (0x0E5) -> GSI 261 (0x105) : IPCC MPROC IRQ (Level, ActiveHigh, Shared)
         *   - SPI 486 (0x1E6) -> GSI 518 (0x206) : ADSP WDOG PDC 6 (Edge, ActiveHigh, Exclusive)
         *   - SPI 470 (0x1D6) -> GSI 502 (0x1F6) : QUPv3_I2C_HUB SE6 IRQ (Level, ActiveHigh, Exclusive)
         */
        Device (AUD0)
        {
            Name (_HID, "S23A8550")
            Name (_UID, 0x00)
            Name (_DDN, "Samsung Galaxy S23 SM8550-AC Native ADSP + Dual CS35L43 Audio Controller")
            Name (_CCA, 0x00)
            Name (_S0W, 0x00)

            Method (_STA, 0, NotSerialized)
            {
                Return (S23A)
            }

            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x81D00000, 0x00200000)  // SMEM v12
                Memory32Fixed (ReadOnly,  0x81C60000, 0x00020000)  // AOP CMD_DB
                Memory32Fixed (ReadWrite, 0x00408000, 0x00001000)  // IPCC APSS MPROC
                Memory32Fixed (ReadWrite, 0x0C300000, 0x00001000)  // AOSS QMP
                Memory32Fixed (ReadWrite, 0x17A20000, 0x00002000)  // RPMh RSC DRV2
                Memory32Fixed (ReadWrite, 0x03000000, 0x00010000)  // ADSP remoteproc
                Memory32Fixed (ReadWrite, 0x009C0000, 0x00002000)  // QUPv3_I2C_HUB wrapper
                Memory32Fixed (ReadWrite, 0x00998000, 0x00004000)  // QUPv3_I2C_HUB SE6

                Interrupt (ResourceConsumer, Level, ActiveHigh, Shared, ,, )
                {
                    0x00000105          // IPCC MPROC: SPI 229 -> GSI 261
                }
                Interrupt (ResourceConsumer, Edge, ActiveHigh, Exclusive, ,, )
                {
                    0x00000206          // ADSP WDOG (PDC 6): SPI 486 -> GSI 518
                }
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive, ,, )
                {
                    0x000001F6          // I2C_HUB SE6: SPI 470 -> GSI 502
                }
            })

            Name (_DSD, Package ()
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                Package ()
                {
                    // ADSP PIL & IPC parameters (s23_live.dts remoteproc-adsp@03000000)
                    Package () { "qcom,pas-id", 1 },
                    Package () { "qcom,dtb-pas-id", 36 },
                    Package () { "qcom,fw-base", 0x9EA00000 },
                    Package () { "qcom,fw-size", 0x059B4000 },
                    Package () { "qcom,dtb-base", 0x9E980000 },
                    Package () { "qcom,dtb-size", 0x00080000 },
                    Package () { "qcom,remote-pid", 2 },
                    Package () { "qcom,ipcc-lpass-client", 3 },
                    Package () { "qcom,smp2p-in-smem", 443 },
                    Package () { "qcom,smp2p-out-smem", 429 },
                    Package () { "qcom,glink-channel", "adsp_apps" },

                    // QUPv3_I2C_HUB SE6 (GPIO8 SDA fn1, GPIO9 SCL fn1)
                    Package () { "qcom,i2c-sda-gpio", 8 },
                    Package () { "qcom,i2c-scl-gpio", 9 },

                    // Dual Cirrus Logic CS35L43 (verified live DEVID 0x00035A42 Rev A1)
                    Package () { "cirrus,right-bot-addr", 0x40 },
                    Package () { "cirrus,right-bot-rst-gpio", 42 },
                    Package () { "cirrus,right-bot-slot", 1 },
                    Package () { "cirrus,left-rcv-addr", 0x41 },
                    Package () { "cirrus,left-rcv-rst-gpio", 55 },
                    Package () { "cirrus,left-rcv-slot", 0 },
                    Package () { "cirrus,shared-irq-gpio", 14 },

                    // Primary MI2S / TDM0 pins (pinctrl-kalama.c FUNC_SEL = 1)
                    Package () { "qcom,i2s0-sck-gpio", 126 },
                    Package () { "qcom,i2s0-data0-gpio", 127 },
                    Package () { "qcom,i2s0-data1-gpio", 128 },
                    Package () { "qcom,i2s0-ws-gpio", 129 },
                    Package () { "qcom,i2s0-func-sel", 1 }
                }
            })
        }

        /*
         * Cirrus Logic CS40L26A LRA Haptic Driver (\_SB.HAP0, ACPI\S23H4026)
         * Verified live DEVID = 0x0040A260, REVID = 0x000000A1 on QUPv3_I2C_HUB SE3.
         * MMIO: 0x0098C000 (16 KiB) | IRQ: SPI 467 (0x1D3) -> GSI 499 (0x1F3)
         * Pins: GPIO22 SDA fn1, GPIO23 SCL fn1, GPIO145 Reset (active HIGH HIGH), GPIO88 IRQ
         */
        Device (HAP0)
        {
            Name (_HID, "S23H4026")
            Name (_UID, 0x00)
            Name (_DDN, "Samsung Galaxy S23 Cirrus CS40L26A LRA Haptics Controller")
            Name (_CCA, 0x00)
            Name (_S0W, 0x00)

            Method (_STA, 0, NotSerialized)
            {
                Return (S23H)
            }

            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x0098C000, 0x00004000)  // QUPv3_I2C_HUB SE3

                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive, ,, )
                {
                    0x000001F3          // I2C_HUB SE3: SPI 467 -> GSI 499
                }
            })

            Name (_DSD, Package ()
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                Package ()
                {
                    Package () { "qcom,qup-wrapper-base", 0x009C0000 },
                    Package () { "qcom,i2c-sda-gpio", 22 },
                    Package () { "qcom,i2c-scl-gpio", 23 },
                    Package () { "cirrus,i2c-addr", 0x40 },
                    Package () { "cirrus,rst-gpio", 145 },
                    Package () { "cirrus,irq-gpio", 88 },
                    Package () { "cirrus,devid", 0x0040A260 }
                }
            })
        }
    }
}
