#!/usr/bin/env python3
"""Build a Mocha-patched APK from a Unity IL2CPP Android build.

Replaces lib/<abi>/libmain.so with the Mocha shim, adds libmocha.so next to it,
rewrites every native library as STORED (required for direct mmap from the APK
on modern Android), then aligns to 16 KiB pages and signs the result.

Usage:
  mocha_patch.py --apk in.apk --out out.apk \
      --shim build/android/native/libmain.so \
      --mocha build/android/native/libmocha.so \
      [--abi arm64-v8a] [--zipalign PATH] [--apksigner PATH] [--keystore PATH]

The tool never modifies the input APK.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

STORED_SUFFIXES = (".so",)


def find_tool(name: str, explicit: str | None) -> str:
    if explicit:
        return explicit
    candidates = [
        Path.home() / "Android/Sdk/build-tools/36.0.0" / name,
        Path.home() / "Android/Sdk/build-tools/35.0.0" / name,
    ]
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    found = shutil.which(name)
    if found:
        return found
    configured = os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT")
    if configured:
        for entry in sorted((Path(configured) / "build-tools").glob("*/" + name), reverse=True):
            if entry.is_file():
                return str(entry)
    raise SystemExit(f"error: {name} not found; pass --{name.lower()} or set ANDROID_HOME")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def patched_entries(source: zipfile.ZipFile, abi: str, shim: Path, mocha: Path):
    prefix = f"lib/{abi}/"
    replacements = {prefix + "libmain.so": shim, prefix + "libmocha.so": mocha}
    seen = set()
    for info in source.infolist():
        name = info.filename
        if name in replacements:
            seen.add(name)
            yield replacements[name], name
            continue
        if name == prefix + "libmocha.so":
            yield mocha, name
            continue
        if name.startswith("META-INF/") and name.endswith((".RSA", ".DSA", ".EC", ".SF")):
            continue
        if name == "META-INF/MANIFEST.MF":
            continue
        yield None, name
    for name, path in replacements.items():
        if name not in seen:
            yield path, name


def write_apk(source_path: Path, target_path: Path, abi: str, shim: Path, mocha: Path) -> None:
    with zipfile.ZipFile(source_path) as source, zipfile.ZipFile(
        target_path, "w", compression=zipfile.ZIP_DEFLATED, allowZip64=True
    ) as target:
        for replacement, name in patched_entries(source, abi, shim, mocha):
            if replacement is not None:
                data = replacement.read_bytes()
                info = zipfile.ZipInfo(name, date_time=(1981, 1, 1, 1, 1, 0))
                info.compress_type = zipfile.ZIP_STORED
                info.external_attr = 0o755 << 16
                target.writestr(info, data)
                continue
            original = source.getinfo(name)
            data = source.read(name)
            if name.startswith("lib/") and name.endswith(STORED_SUFFIXES):
                info = zipfile.ZipInfo(name, date_time=original.date_time)
                info.compress_type = zipfile.ZIP_STORED
                info.external_attr = original.external_attr
                target.writestr(info, data)
            else:
                target.writestr(original, data)


def run(command: list[str]) -> None:
    result = subprocess.run(command, check=False)
    if result.returncode != 0:
        raise SystemExit(f"error: command failed ({result.returncode}): {' '.join(command)}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--apk", required=True, type=Path, help="input Unity APK")
    parser.add_argument("--out", required=True, type=Path, help="patched APK to create")
    parser.add_argument("--shim", required=True, type=Path, help="built libmain.so shim")
    parser.add_argument("--mocha", required=True, type=Path, help="built libmocha.so")
    parser.add_argument("--abi", default="arm64-v8a")
    parser.add_argument("--zipalign")
    parser.add_argument("--apksigner")
    parser.add_argument("--keystore", type=Path, default=Path.home() / ".android/debug.keystore")
    parser.add_argument("--ks-pass", default="android")
    parser.add_argument("--key-alias", default="androiddebugkey")
    parser.add_argument("--skip-sign", action="store_true")
    args = parser.parse_args()

    for path, label in ((args.apk, "APK"), (args.shim, "shim"), (args.mocha, "libmocha")):
        if not path.is_file():
            raise SystemExit(f"error: {label} not found: {path}")
    if args.apk.resolve() == args.out.resolve():
        raise SystemExit("error: --out must differ from --apk")

    zipalign = find_tool("zipalign", args.zipalign)
    apksigner = find_tool("apksigner", args.apksigner)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="mocha-patch-") as temporary:
        unsigned = Path(temporary) / "unsigned.apk"
        aligned = Path(temporary) / "aligned.apk"
        print(f"rewriting {args.apk} -> {unsigned}")
        write_apk(args.apk, unsigned, args.abi, args.shim, args.mocha)
        print("aligning 16 KiB pages")
        run([zipalign, "-P", "16", "-f", "4", str(unsigned), str(aligned)])
        shutil.copyfile(aligned, args.out)

    if not args.skip_sign:
        print(f"signing {args.out}")
        run(
            [
                apksigner,
                "sign",
                "--ks",
                str(args.keystore),
                "--ks-pass",
                f"pass:{args.ks_pass}",
                "--key-pass",
                f"pass:{args.ks_pass}",
                "--ks-key-alias",
                args.key_alias,
                str(args.out),
            ]
        )
        run([apksigner, "verify", "--verbose", str(args.out)])

    with zipfile.ZipFile(args.out) as result:
        names = set(result.namelist())
        required = {f"lib/{args.abi}/libmain.so", f"lib/{args.abi}/libmocha.so"}
        missing = required - names
        if missing:
            raise SystemExit(f"error: patched APK is missing {sorted(missing)}")
        for name in sorted(required):
            info = result.getinfo(name)
            if info.compress_type != zipfile.ZIP_STORED:
                raise SystemExit(f"error: {name} is not STORED")

    print(f"patched: {args.out} ({args.out.stat().st_size} bytes, sha256 {sha256(args.out)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
