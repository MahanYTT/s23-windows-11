"""extract_and_verify_s23_gpu_fw.py
Offline parser & G1287-cold-aperture pin validator for Samsung Galaxy S23 (dm1q / SM8550-AC)
Adreno 740 (GEN7_2_1 / 0x07020100) GPU firmware images:
  - gmu_gen70200.bin / a740_gmu.bin  (GMU Cortex-M3 firmware blocks)
  - a740_sqe.fw                      (CP SQE microcode)
  - a740_zap.mbn                     (TrustZone PAS ZAP shader ELF/MBN)
  - s23_rsc_seq_ucode.bin            (20-byte GEN7_2_1 RSCC sequencer ucode)

100% offline — performs no ADB/fastboot/network I/O.
"""

import hashlib
import os
import struct
import sys

FW_DIR = os.path.dirname(os.path.abspath(__file__))

# Reference pins from repos/oneplus-open-windows/experiments/G1287-cold-aperture/reference/runtime/
G191_STOCK_GMU_BYTES  = 66844
G191_STOCK_GMU_BLOCKS = 15
G191_STOCK_GMU_SHA256 = "1a2a419c39046d3141fc5fed5aa7f971de2db40cc7a1d89693c3e26fad64dd98"

G433_SQE_FILE_BYTES   = 76724
G433_SQE_SHA256       = "96fee336424b139100fc60b5b45a907360e4b3936d7e1d00406b9bd80ca48473"

G482_RSC_BYTES        = 20
G482_RSC_SHA256       = "20175df4f9aafab7f958205d71bf931a30a484bd6e94db348da5497a4e13ce90"
S23_RSC_WORDS         = (0xEAAAE5A0, 0xE1A1EBAB, 0xA2E0A581, 0xECAC82E2, 0x0020EDAD)

# GMU address windows from adreno_gen7_gmu.c & g190_backend.h
GMU_REGIONS = [
    ("ITCM",   0x00000000, 0x00004000, 1),
    ("ICACHE", 0x00004000, 0x01000000, 2),
    ("DTCM",   0x10004000, 0x00004000, 3),
]


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def classify_gmu_addr(addr: int, size: int) -> str:
    if size == 0:
        return "META"
    for name, start, length, _rid in GMU_REGIONS:
        if addr >= start and (addr + size) <= (start + length):
            return name
    return "UNMAPPED"


def verify_rsc_ucode(path: str) -> None:
    print(f"=== RSCC Sequencer Ucode: {os.path.basename(path)} ===")
    if not os.path.exists(path):
        raw = struct.pack("<5I", *S23_RSC_WORDS)
        with open(path, "wb") as f:
            f.write(raw)
        print(f"  Generated {len(raw)} B from S23 adreno_gen7_gmu.c L191-195")
    else:
        raw = open(path, "rb").read()
    h = sha256_hex(raw)
    words = struct.unpack("<5I", raw) if len(raw) == 20 else ()
    print(f"  Size   : {len(raw)} B (G482 pin: {G482_RSC_BYTES} B)")
    print(f"  Words  : {' '.join(f'0x{w:08X}' for w in words)}")
    print(f"  SHA256 : {h}")
    print(f"  Match G482RscSha: {'EXACT MATCH' if h == G482_RSC_SHA256 else 'DIFFERS'}")


def inspect_gmu_fw(path: str) -> None:
    print(f"\n=== GMU Firmware: {os.path.basename(path)} ===")
    if not os.path.exists(path):
        print("  [NOT YET PULLED FROM S23 /vendor/firmware/gmu_gen70200.bin]")
        print(f"  Expected G191 pin: {G191_STOCK_GMU_BYTES} B, {G191_STOCK_GMU_BLOCKS} blocks, SHA256={G191_STOCK_GMU_SHA256}")
        return
    data = open(path, "rb").read()
    h = sha256_hex(data)
    print(f"  Size   : {len(data)} B (G191 pin: {G191_STOCK_GMU_BYTES} B)")
    print(f"  SHA256 : {h}")
    print(f"  Match G191_STOCK_GMU_SHA256: {'EXACT MATCH' if h == G191_STOCK_GMU_SHA256 else 'DIFFERS (Samsung/Qualcomm newer build)'}")

    off = 0
    blocks = []
    dtcm_ver = None
    while off + 16 <= len(data):
        addr, size, btype, value = struct.unpack_from("<4I", data, off)
        off += 16
        if off + size > len(data):
            print(f"  [ERROR] Block #{len(blocks)} truncated: addr=0x{addr:08X} size=0x{size:X} rem={len(data)-off}")
            break
        region = classify_gmu_addr(addr, size)
        payload = data[off : off + size]
        if region == "DTCM" and addr <= 0x10007FE0 and (addr + size) >= 0x10007FE4:
            dtcm_ver = struct.unpack_from("<I", payload, 0x10007FE0 - addr)[0]
        blocks.append((addr, size, btype, value, region))
        off += size

    print(f"  Parsed blocks: {len(blocks)} (G191 pin: {G191_STOCK_GMU_BLOCKS}), final_off={off}/{len(data)}")
    for idx, (addr, size, btype, value, region) in enumerate(blocks):
        print(f"    [{idx:02d}] {region:<8} addr=0x{addr:08X} size=0x{size:05X} ({size:6d} B) type={btype} val=0x{value:08X}")
    if dtcm_ver is not None:
        print(f"  GMU DTCM Version Word (0x10007FE0): 0x{dtcm_ver:08X}")


def inspect_sqe_fw(path: str) -> None:
    print(f"\n=== SQE Microcode: {os.path.basename(path)} ===")
    if not os.path.exists(path):
        print("  [NOT YET PULLED FROM S23 /vendor/firmware/a740_sqe.fw]")
        print(f"  Expected G433 pin: {G433_SQE_FILE_BYTES} B, SHA256={G433_SQE_SHA256}")
        return
    data = open(path, "rb").read()
    h = sha256_hex(data)
    w0, w1, w2, w3 = struct.unpack_from("<4I", data, 0) if len(data) >= 16 else (0, 0, 0, 0)
    print(f"  Size   : {len(data)} B (G433 pin: {G433_SQE_FILE_BYTES} B)")
    print(f"  Head   : 0x{w0:08X} 0x{w1:08X} 0x{w2:08X} 0x{w3:08X}")
    print(f"  SHA256 : {h}")
    print(f"  Match G433SqeSha: {'EXACT MATCH' if h == G433_SQE_SHA256 else 'DIFFERS'}")


def inspect_zap_mbn(path: str) -> None:
    print(f"\n=== ZAP Shader MBN: {os.path.basename(path)} ===")
    if not os.path.exists(path):
        print("  [NOT YET PULLED FROM S23 /vendor/firmware/a740_zap.mbn (or /vendor/firmware_mnt/image/)]")
        return
    data = open(path, "rb").read()
    print(f"  Size   : {len(data)} B, ELF_Magic={data[:4]!r}, SHA256={sha256_hex(data)}")


def main() -> None:
    fw_dir = sys.argv[1] if len(sys.argv) > 1 else FW_DIR
    verify_rsc_ucode(os.path.join(fw_dir, "s23_rsc_seq_ucode.bin"))

    gmu_path = os.path.join(fw_dir, "gmu_gen70200.bin")
    if not os.path.exists(gmu_path) and os.path.exists(os.path.join(fw_dir, "a740_gmu.bin")):
        gmu_path = os.path.join(fw_dir, "a740_gmu.bin")
    inspect_gmu_fw(gmu_path)
    inspect_sqe_fw(os.path.join(fw_dir, "a740_sqe.fw"))
    inspect_zap_mbn(os.path.join(fw_dir, "a740_zap.mbn"))


if __name__ == "__main__":
    main()
