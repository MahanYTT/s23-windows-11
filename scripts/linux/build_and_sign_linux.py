#!/usr/bin/env python3
"""Build and (optionally) Authenticode-sign S23 Clang/LLD drivers natively on Linux.

Works on CachyOS / Arch (`clang`, `lld`, `llvm`), Debian / Ubuntu (`clang-18`,
`lld-18`, `llvm-18`), and Fedora (`clang`, `lld`, `llvm`).

Usage:
    python3 scripts/linux/build_and_sign_linux.py
    python3 scripts/linux/build_and_sign_linux.py --no-sign
    python3 scripts/linux/build_and_sign_linux.py --drivers s23touch s23batt
"""
import argparse
import os
import shutil
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_RUNTIME_DRIVERS = ["s23touch", "s23batt", "s23keys", "s23bthx", "s23perf"]
CERT_DIR = os.path.join(REPO_ROOT, ".s23_local_cert")


def ensure_test_cert(cert_dir: str = CERT_DIR):
    """Create or reuse a 2020..2040 CN=S23TouchTest2020 test-signing key/cert pair via openssl."""
    openssl = shutil.which("openssl")
    if not openssl:
        raise FileNotFoundError("openssl not found in PATH")
    os.makedirs(cert_dir, exist_ok=True)
    key_path = os.path.join(cert_dir, "s23test.key")
    cer_pem = os.path.join(cert_dir, "s23test.pem")
    cer_der = os.path.join(cert_dir, "s23test.cer")
    if not (os.path.isfile(key_path) and os.path.isfile(cer_pem) and os.path.isfile(cer_der)):
        ext_cfg = os.path.join(cert_dir, "codesign.cnf")
        with open(ext_cfg, "w", encoding="utf-8") as f:
            f.write(
                "[req]\n"
                "distinguished_name = dn\n"
                "x509_extensions = v3_codesign\n"
                "prompt = no\n"
                "[dn]\n"
                "CN = S23TouchTest2020\n"
                "[v3_codesign]\n"
                "basicConstraints = CA:FALSE\n"
                "keyUsage = digitalSignature\n"
                "extendedKeyUsage = codeSigning\n"
            )
        # Note: UEFI RTC boots near 2024-04-01; -days 7305 from current system clock keeps it valid past 2040.
        subprocess.check_call([
            openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", key_path, "-out", cer_pem,
            "-days", "7305", "-sha256", "-config", ext_cfg,
        ])
        subprocess.check_call([
            openssl, "x509", "-in", cer_pem, "-outform", "DER", "-out", cer_der,
        ])
    return key_path, cer_pem, cer_der


def build_driver(drv_name: str, check_stack: bool = True) -> str:
    """Compile drivers/<drv_name>/build_wsl.py and return path to unsigned .sys."""
    drv_dir = os.path.join(REPO_ROOT, "drivers", drv_name)
    build_py = os.path.join(drv_dir, "build_wsl.py")
    if not os.path.isfile(build_py):
        raise FileNotFoundError(f"Missing build script: {build_py}")
    print(f"=== Building {drv_name} ===")
    subprocess.check_call([sys.executable, build_py])
    sys_path = os.path.join(drv_dir, "out", f"{drv_name}.sys")
    if not os.path.isfile(sys_path):
        raise FileNotFoundError(f"Expected output binary missing: {sys_path}")
    if check_stack:
        stack_chk = os.path.join(REPO_ROOT, "scripts", "ci", "check_driver_stack.py")
        subprocess.check_call([sys.executable, stack_chk, sys_path])
    return sys_path


def sign_driver(drv_name: str, sys_path: str, key_path: str, cer_pem: str, cer_der: str) -> str:
    """Sign .sys via osslsigncode into drivers/<drv_name>/pkg/ alongside INF + s23test.cer."""
    ossl = shutil.which("osslsigncode")
    if not ossl:
        raise FileNotFoundError(
            "osslsigncode not found (install osslsigncode, or re-run with --no-sign)"
        )
    drv_dir = os.path.join(REPO_ROOT, "drivers", drv_name)
    pkg_dir = os.path.join(drv_dir, "pkg")
    os.makedirs(pkg_dir, exist_ok=True)
    signed_sys = os.path.join(pkg_dir, f"{drv_name}.sys")
    if os.path.exists(signed_sys):
        os.remove(signed_sys)
    subprocess.check_call([
        ossl, "sign",
        "-certs", cer_pem,
        "-key", key_path,
        "-h", "sha256",
        "-n", f"Samsung Galaxy S23 {drv_name} Driver",
        "-in", sys_path,
        "-out", signed_sys,
    ])
    inf_src = os.path.join(drv_dir, f"{drv_name}.inf")
    if os.path.isfile(inf_src):
        shutil.copy2(inf_src, os.path.join(pkg_dir, f"{drv_name}.inf"))
    shutil.copy2(cer_der, os.path.join(pkg_dir, "s23test.cer"))
    return signed_sys


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--drivers", nargs="+", default=DEFAULT_RUNTIME_DRIVERS,
                    help=f"Drivers to build (default: {' '.join(DEFAULT_RUNTIME_DRIVERS)})")
    ap.add_argument("--no-sign", action="store_true",
                    help="Skip osslsigncode Authenticode signing (build + stack check only)")
    ap.add_argument("--cert-dir", default=CERT_DIR,
                    help="Directory for reusable test-signing cert/key (default: .s23_local_cert)")
    args = ap.parse_args(argv)

    built = []
    for drv in args.drivers:
        built.append((drv, build_driver(drv, check_stack=True)))

    if args.no_sign:
        print(f"Built and stack-checked {len(built)} driver(s) (--no-sign).")
        return 0

    key_path, cer_pem, cer_der = ensure_test_cert(args.cert_dir)
    for drv, unsigned_sys in built:
        signed = sign_driver(drv, unsigned_sys, key_path, cer_pem, cer_der)
        print(f"Signed -> {signed}")
    print(f"Built and signed {len(built)} driver(s) into drivers/*/pkg/.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
