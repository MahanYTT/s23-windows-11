/*
 * s23_bt_uart14.asl - Samsung Galaxy S23 (dm1q / SM8550-AC) QCA6490 Bluetooth UART14 ACPI SSDT
 *
 * Verified against:
 *   - /home/mahan/mu_s23/s23_live.dts (qcom,qup_uart@898000, bt_qca6490, qupv3_se14_*)
 *   - repos/Silicium-ACPI/Silicon/Qualcomm/Kailua/Decompiled/DSDT_MTP.dsl (\_SB.UR15 + \_SB.BTH0)
 *
 * Hardware Summary:
 *   - QUP2 SE6 4-wire HSUART (qupv3_se14_4uart):
 *       MMIO       : 0x00898000 (size 0x00004000)
 *       Interrupt  : GIC SPI 461 (0x1CD) -> ACPI GSI 493 (0x1ED), Level ActiveHigh
 *       Pins       : GPIO76 (CTS), GPIO77 (RTS), GPIO78 (TX), GPIO79 (RX + wake IRQ)
 *       Transfer   : GENI FIFO mode (no GPI/GSI DMA required)
 *   - QCA6490 Bluetooth Controller (\_SB.BTH0):
 *       UART Bus   : 115200 bps initial (3.0/3.2 Mbps after TLV download), 8N1, HW RTS/CTS
 *       BT_EN GPIO : TLMM GPIO 81 (0x51)
 *       SW_CTRL    : TLMM GPIO 82 (0x52)
 *       XO_CLK     : TLMM GPIO 204 (0xCC)
 *
 * SAFETY DEFAULT:
 *   Both UR15 and BTH0 return _STA = 0x00 (inert/disabled) so loading this SSDT does not
 *   expose unbound QCOM0C16 / QCOM0C6B devices or reference absent \_SB.GIO0 / \_SB.PEP0
 *   until matching UART/GPIO/BT drivers are staged. Flip S23B = 0x0F to activate.
 */

DefinitionBlock ("s23_bt_uart14.aml", "SSDT", 2, "SAMSNG", "S23BTU14", 0x00000001)
{
    Scope (\_SB)
    {
        /* Master activation gate: 0x00 = inert default, 0x0F = present & enabled */
        Name (S23B, 0x00)

        /*
         * QUP2 SE6 4-Wire Bluetooth HSUART (qupv3_se14_4uart @ 0x00898000)
         * Matches Kailua DSDT_MTP \_SB.UR15 (QCOM0C16 / QUP_2_SE_6,4W,BT).
         */
        Device (UR15)
        {
            Name (_HID, "QCOM0C16")
            Name (_UID, 0x0F)
            Name (_CCA, 0x00)
            Name (_STR, Unicode ("QUP_2_SE_6,4W,BT"))

            Method (_STA, 0, NotSerialized)
            {
                Return (S23B)
            }

            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite,
                    0x00898000,         // QUP2 SE6 GENI HSUART base
                    0x00004000          // 16 KiB SE aperture
                    )
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive, ,, )
                {
                    0x000001ED          // GIC SPI 461 (0x1CD) + 32 = GSI 493
                }
            })

            /* Hardware metadata for open/custom GENI BT transport drivers */
            Name (_DSD, Package ()
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                Package ()
                {
                    Package () { "qcom,qup-wrapper-base", 0x008C0000 },
                    Package () { "qcom,geni-fifo-mode", 0x01 },
                    Package () { "qcom,uart-cts-gpio", 76 },
                    Package () { "qcom,uart-rts-gpio", 77 },
                    Package () { "qcom,uart-tx-gpio", 78 },
                    Package () { "qcom,uart-rx-gpio", 79 },
                    Package () { "qcom,wakeup-byte", 0xFD }
                }
            })
        }

        /*
         * QCA6490 (WCN6855 / Hastings Prime) Bluetooth 5.3 Controller on UART14
         * Matches Kailua DSDT_MTP \_SB.BTH0 (QCOM0C6B) with UartSerialBusV2.
         */
        Device (BTH0)
        {
            Name (_HID, "QCOM0C6B")
            Name (_UID, 0x00)
            Name (_S0W, 0x00)           // Awake-pinned D0 profile
            Name (_S4W, 0x00)

            Method (_STA, 0, NotSerialized)
            {
                Return (S23B)
            }

            Name (_CRS, ResourceTemplate ()
            {
                UartSerialBusV2 (
                    0x0001C200,         // 115200 bps initial ROM baud rate
                    DataBitsEight,
                    StopBitsOne,
                    0xC0,               // LinesInUse: RTS + CTS
                    LittleEndian,
                    ParityTypeNone,
                    FlowControlHardware,
                    0x0020,             // ReceiveBufferSize (32 B)
                    0x0020,             // TransmitBufferSize (32 B)
                    "\\_SB.UR15",
                    0x00,
                    ResourceConsumer,
                    ,
                    Exclusive,
                    )
            })

            /* S23 QCA6490 BT pin & firmware metadata (avoids broken \_SB.GIO0 ref while inert) */
            Name (_DSD, Package ()
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                Package ()
                {
                    Package () { "qcom,bt-en-gpio", 81 },
                    Package () { "qcom,sw-ctrl-gpio", 82 },
                    Package () { "qcom,xo-clk-gpio", 204 },
                    Package () { "qcom,bt-fw-patch", "hpbtfw21.tlv" },
                    Package () { "qcom,bt-fw-nvm", "hpnv21.bin" },
                    Package () { "qcom,bt-fw-nvm-gf", "hpnv21g.bin" },
                    Package () { "qcom,max-baud", 3200000 }
                }
            })
        }
    }
}
