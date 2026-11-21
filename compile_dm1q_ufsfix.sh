#!/bin/bash
set -e
cd ~/mu_s23/Mu-Silicium
cat << 'EOF' > Silicium-ACPI/Platforms/Samsung/dm1q/DSDT.asl
DefinitionBlock ("DSDT.aml", "DSDT", 2, "QCOMM ", "SM8550 ", 3)
{
    Scope (\_SB)
    {
        Name (PSUB, "SM8550")
        Name (STOR, 0x1)
        Name (EMUL, 0xFFFFFFFF)
        Device (UFS0)
        {
            Alias (\_SB.PSUB, _SUB)
            Alias (\_SB.EMUL, EMUL)
            Name (_HID, "QCOM24A5")
            Name (_UID, 0)
            Name (_CCA, 1)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x01D84000, 0x0001C000)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive, ,, ) { 0x129 }
            })
            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
            Device (DEV0)
            {
                Name (_ADR, 8)
                Name (_RMV, 0)
            }
        }
        Device (CPU0)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 0)
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }
        Device (CPU1)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 1)
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }
        Device (CPU2)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 2)
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }
        Device (CPU3)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 3)
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }
        Device (CPU4)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 4)
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }
        Device (CPU5)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 5)
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }
        Device (CPU6)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 6)
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }
        Device (CPU7)
        {
            Name (_HID, "ACPI0007")
            Name (_UID, 7)
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }
    }
}
EOF
iasl -tc Silicium-ACPI/Platforms/Samsung/dm1q/DSDT.asl
ls -la Silicium-ACPI/Platforms/Samsung/dm1q/DSDT.aml
source ~/mu_s23/venv/bin/activate
python3 build_uefi.py -d dm1q -r RELEASE
cp Mu-dm1q.img /mnt/c/Users/Mahan/Documents/windows-on-android/s23_dm1q/Mu-dm1q-ufsfix.img
ls -lh /mnt/c/Users/Mahan/Documents/windows-on-android/s23_dm1q/Mu-dm1q-ufsfix.img
echo UFSFIX_BUILT_OK
