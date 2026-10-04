#!/usr/bin/env python3
"""Verify this project's signed HAP and install it on one explicitly named device."""
import argparse
import hashlib
import json
from pathlib import Path
import tempfile
import zipfile
from probe_tools import ROOT, SDK, HDC, JAVA, run, check_devices

BUNDLE = "com.longxin.harmonyremote.probe"
OUTPUT = ROOT / "host-harmony/entry/build/default/outputs/default"


def abort(message):
    raise SystemExit(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", required=True, help="Exact HDC target ID")
    parser.add_argument("--hap", type=Path, required=True,
                        help="This project's entry-default-signed.hap in its build output directory")
    args = parser.parse_args()
    if not args.target or args.target.startswith("-") or any(c.isspace() for c in args.target):
        parser.error("--target must be one explicit device ID")
    hap = args.hap.expanduser().resolve()
    expected = (OUTPUT / "entry-default-signed.hap").resolve()
    if hap != expected or not hap.is_file():
        abort(f"Refusing HAP: expected this project's existing signed artifact at {expected}")
    try:
        with zipfile.ZipFile(hap) as archive:
            metadata = json.loads(archive.read("module.json"))
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        abort(f"Cannot validate HAP module.json: {error}")
    if metadata.get("app", {}).get("bundleName") != BUNDLE:
        abort("Refusing HAP: bundleName does not match this project.")
    if metadata.get("module", {}).get("name") != "entry":
        abort("Refusing HAP: expected the entry module.")
    digest = hashlib.sha256(hap.read_bytes()).hexdigest()
    signer = SDK / "openharmony/toolchains/lib/hap-sign-tool.jar"
    with tempfile.TemporaryDirectory(prefix="harmony-remote-verify-") as temporary:
        directory = Path(temporary)
        verification = run([JAVA, "-jar", signer, "verify-app", "-inFile", hap,
                            "-outCertChain", directory / "certificate.cer",
                            "-outProfile", directory / "profile.p7b"], timeout=60)
        if verification["returncode"] != 0:
            abort("Signature verification did not succeed; no installation was attempted. "
                  + verification.get("error", "See the SDK verify-app command for diagnostics."))
        if not (directory / "certificate.cer").is_file() or not (directory / "profile.p7b").is_file():
            abort("Signature verifier did not emit its required results; no installation was attempted.")
    discovery = check_devices(args.target)
    if discovery["status"] != "READY":
        abort(f"Device check: {discovery['status']}; no installation was attempted.")
    if digest != hashlib.sha256(hap.read_bytes()).hexdigest():
        abort("HAP changed after verification; refusing installation.")
    print(f"Verified bundle: {BUNDLE}\nSHA-256: {digest}\nTarget: {args.target}", flush=True)
    result = run([HDC, "-t", args.target, "install", hap], timeout=60)
    print(result.get("stdout", ""))
    if result.get("stderr"):
        print(result["stderr"])
    if result["returncode"] != 0 or "success" not in result.get("stdout", "").lower():
        abort("Installation did not report success. Check device state before retrying.")
    print("Installed. Launch Harmony Remote Probe on the device and perform the visible consent flow.")


if __name__ == "__main__":
    main()
