#!/usr/bin/env python3
"""Package audited preview inputs; never signs, installs, launches, or uploads."""
import argparse
import hashlib
import json
import plistlib
import shutil
import struct
import subprocess
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VERSION, MAC_BUILD, HOST_BUILD = "0.6.0", "9", 1000013


def require(condition, message):
    if not condition:
        raise ValueError(message)


def run(*args):
    result = subprocess.run(args, cwd=ROOT, check=True, capture_output=True, text=True)
    return result.stdout + result.stderr


def sha256(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest() if hasattr(hashlib, "file_digest") else hashlib.sha256(source.read()).hexdigest()


def validate_mac(app):
    require(app.name == "HarmonyRemote.app" and app.is_dir(), "Expected HarmonyRemote.app")
    expected = {"Contents/Info.plist", "Contents/MacOS/HarmonyRemote", "Contents/_CodeSignature/CodeResources"}
    files = list(app.rglob("*"))
    require(not any(p.is_symlink() for p in files), "App must not contain symlinks")
    require({p.relative_to(app).as_posix() for p in files if p.is_file()} == expected,
            "Unexpected app contents; refuse to package local settings or extra resources")
    info = plistlib.loads((app / "Contents/Info.plist").read_bytes())
    require(info.get("CFBundleIdentifier") == "com.longxin.harmonyremote.viewer", "Wrong Mac bundle")
    require(info.get("CFBundleShortVersionString") == VERSION and info.get("CFBundleVersion") == MAC_BUILD,
            "Unexpected Mac version/build")
    require(info.get("LSMinimumSystemVersion") == "14.0", "Unexpected macOS minimum")
    require(run("/usr/bin/lipo", "-archs", str(app / "Contents/MacOS/HarmonyRemote")).strip() == "arm64",
            "Mac executable must be arm64")
    run("/usr/bin/codesign", "--verify", "--strict", str(app))
    require("Signature=adhoc" in run("/usr/bin/codesign", "-dvv", str(app)), "Expected ad-hoc Mac signature")


def validate_hap(hap):
    require(hap.name == "entry-default-unsigned.hap", "Only entry-default-unsigned.hap may be published")
    data = hap.read_bytes()
    with zipfile.ZipFile(hap) as archive:
        require(archive.testzip() is None, "HAP CRC validation failed")
        entries = sorted(archive.infolist(), key=lambda item: item.header_offset)
        require(len({item.filename for item in entries}) == len(entries), "Duplicate HAP entries")
        cursor = 0
        for entry in entries:
            # HAP signatures are inserted before the ZIP central directory. Require
            # contiguous ordinary ZIP entries, rejecting even a renamed signed HAP.
            require(entry.header_offset == cursor, "HAP contains a signing block or unexplained ZIP gap")
            header = struct.unpack_from("<4s5H3I2H", data, cursor)
            require(header[0] == b"PK\x03\x04" and not header[2] & 1, "Unsupported ZIP entry")
            cursor += 30 + header[-2] + header[-1] + entry.compress_size
            if header[2] & 8:
                if data[cursor:cursor + 4] == b"PK\x07\x08":
                    cursor += 4
                require(struct.unpack_from("<III", data, cursor) == (entry.CRC, entry.compress_size, entry.file_size),
                        "Unsupported ZIP64 or invalid data descriptor")
                cursor += 12
            name = entry.filename.lower()
            require(not name.startswith("/") and ".." not in Path(name).parts, "Unsafe HAP entry path")
            require(not name.endswith((".p7b", ".p12", ".pfx", ".pem", ".cer", ".key")),
                    "HAP contains signing material")
        require(cursor == archive.start_dir, "HAP contains a signing block before its central directory")
        end = data.rfind(b"PK\x05\x06")
        require(end >= 0 and end + 22 == len(data), "Unsupported HAP trailer or ZIP comment")
        eocd = struct.unpack_from("<4s4H2IH", data, end)
        require(eocd[1:3] == (0, 0) and eocd[3] == eocd[4] == len(entries)
                and eocd[6] == cursor and cursor + eocd[5] == end, "Unsupported ZIP layout")
        metadata = json.loads(archive.read("module.json"))
        app = metadata["app"]
        require(app.get("bundleName") == "com.longxin.harmonyremote.probe", "Wrong Harmony bundle")
        require(app.get("versionName") == VERSION and app.get("versionCode") == HOST_BUILD,
                "Unexpected Harmony version/build")
        libraries = [entry.filename for entry in entries if entry.filename.endswith(".so")]
        require(libraries and all(name.startswith("libs/arm64-v8a/") for name in libraries),
                "HAP libraries must be arm64-v8a")
        return app


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mac-app", required=True, type=Path)
    parser.add_argument("--hap", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    app, hap, output = args.mac_app.resolve(), args.hap.resolve(), args.output.resolve()
    validate_mac(app)
    host = validate_hap(hap)
    commit = run("git", "rev-parse", "HEAD").strip()
    run("git", "diff", "--quiet", "HEAD", "--")
    output.mkdir(parents=True, exist_ok=True)
    mac_name = f"HarmonyRemote-{VERSION}-macOS-arm64.zip"
    hap_name = f"HarmonyRemote-{VERSION}-HarmonyOS-arm64-unsigned.hap"
    with tempfile.TemporaryDirectory(prefix=".preview-", dir=output) as temporary:
        stage = Path(temporary)
        run("/usr/bin/ditto", "-c", "-k", "--sequesterRsrc", "--keepParent", str(app), str(stage / mac_name))
        shutil.copyfile(hap, stage / hap_name)
        manifest = {
            "version": VERSION, "channel": "preview", "sourceCommit": commit,
            "macOS": {"asset": mac_name, "build": MAC_BUILD, "architecture": "arm64",
                      "minimumOS": "14.0", "signature": "ad-hoc", "notarized": False},
            "harmonyOS": {"asset": hap_name, "build": HOST_BUILD, "architecture": "arm64-v8a",
                          "sdk": host.get("compileSdkVersion"), "minimumAPIVersion": host.get("minAPIVersion"),
                          "buildMode": host.get("buildMode"), "signature": "unsigned",
                          "requiresUserSigning": True},
            "sha256": {name: sha256(stage / name) for name in (mac_name, hap_name)},
        }
        (stage / "release-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        for name in (mac_name, hap_name, "release-manifest.json"):
            (stage / name).replace(output / name)
    names = [mac_name, hap_name, "release-manifest.json"]
    names += [name for name in ("INSTALL.md", "RELEASE_NOTES.md") if (output / name).is_file()]
    (output / "SHA256SUMS").write_text("".join(f"{sha256(output / name)}  {name}\n" for name in sorted(names)))
    print(f"Packaged {VERSION}; source commit {commit}; Mac ad-hoc; Harmony unsigned")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError, struct.error, zipfile.BadZipFile, subprocess.CalledProcessError) as error:
        raise SystemExit(f"Packaging refused: {error}")
