#!/usr/bin/env python3
"""
build_all.py — Build M8 Capstan firmware for Model:01 and Model:02.

Usage:
    python build_all.py                     # Build both Model:01 and Model:02 (keeps current version)
    python build_all.py model01             # Build Model:01 only (keeps current version)
    python build_all.py model02             # Build Model:02 only (keeps current version)
    python build_all.py --debug             # Build with CAPSTAN_DEBUG=1 (enables Serial diagnostics & freeze timer)
    python build_all.py --release           # Build both models, verify, and package release bundle in dist/
    python build_all.py --bump              # Increment minor version (x.y -> x.(y+1)) in src/config.h, then build
    python build_all.py --bump-major        # Increment major version (x.y -> (x+1).0) in src/config.h, then build
    python build_all.py --version 1.0       # Set explicit version "x.y" in src/config.h, then build
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SKETCH_NAME = "Capstan.ino"
CONFIG_H = ROOT / "src" / "config.h"
BUILD_ROOT = ROOT / "build"
DIST_ROOT = ROOT / "dist"

VERSION_DEFINE_RE = re.compile(r'(#define\s+CAPSTAN_VERSION\s+")(\d+\.\d+)(")')
VERSION_FORMAT_RE = re.compile(r"^\d+\.\d+$")

FALLBACK_ARDUINO_CLI_PATHS = [
    Path(r"C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"),
    Path("/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"),
]

TARGETS = {
    "model01": {
        "label": "M8 Model:01 (Teensy 4.0)",
        "fqbn": "teensy:avr:teensy40:usb=serialmsc",
        "build_dir": BUILD_ROOT / "model01",
        "out_hex": BUILD_ROOT / "M8_Capstan_Model01.hex",
    },
    "model02": {
        "label": "M8 Model:02 (Teensy MicroMod)",
        "fqbn": "teensy:avr:teensyMM:usb=serialmsc",
        "build_dir": BUILD_ROOT / "model02",
        "out_hex": BUILD_ROOT / "M8_Capstan_Model02.hex",
    },
}


def read_version() -> str:
    text = CONFIG_H.read_text(encoding="utf-8")
    m = VERSION_DEFINE_RE.search(text)
    if not m:
        raise RuntimeError(f"CAPSTAN_VERSION define not found in {CONFIG_H}")
    return m.group(2)


def write_version(new_version: str) -> str:
    if not VERSION_FORMAT_RE.match(new_version):
        raise ValueError(
            f"Invalid version '{new_version}': must be formatted as 'x.y' (e.g. '1.0')"
        )
    text = CONFIG_H.read_text(encoding="utf-8")
    m = VERSION_DEFINE_RE.search(text)
    if not m:
        raise RuntimeError(f"CAPSTAN_VERSION define not found in {CONFIG_H}")
    old_version = m.group(2)
    if old_version != new_version:
        updated = VERSION_DEFINE_RE.sub(rf"\g<1>{new_version}\g<3>", text, count=1)
        CONFIG_H.write_text(updated, encoding="utf-8")
        print(f"=== Updated Firmware Version: v{old_version} -> v{new_version} ===")
    else:
        print(f"=== Firmware Version unchanged: v{new_version} ===")
    return new_version


def bump_version_str(cur_version: str, major: bool = False) -> str:
    parts = cur_version.split(".")
    if len(parts) != 2 or not all(p.isdigit() for p in parts):
        raise ValueError(f"Current version '{cur_version}' is not in 'x.y' format")
    x, y = int(parts[0]), int(parts[1])
    if major:
        return f"{x + 1}.0"
    return f"{x}.{y + 1}"


def find_arduino_cli() -> str:
    env_cli = os.environ.get("ARDUINO_CLI")
    if env_cli and Path(env_cli).exists():
        return env_cli
    found = shutil.which("arduino-cli")
    if found:
        return found
    for candidate in FALLBACK_ARDUINO_CLI_PATHS:
        if candidate.exists():
            return str(candidate)
    raise FileNotFoundError(
        "arduino-cli not found in ARDUINO_CLI, PATH, or standard Arduino IDE locations."
    )


def build_target(cli: str, key: str, version: str, debug: bool = False) -> Path:
    cfg = TARGETS[key]
    build_dir: Path = cfg["build_dir"]
    out_hex: Path = cfg["out_hex"]
    build_dir.mkdir(parents=True, exist_ok=True)

    mode_str = "DEBUG" if debug else "RELEASE"
    print(f"\n=== Building {cfg['label']} [v{version} {mode_str}] [{cfg['fqbn']}] ===")
    cmd = [
        cli,
        "compile",
        "-b",
        cfg["fqbn"],
        "--build-path",
        str(build_dir),
    ]
    if debug:
        cmd.extend(["--build-property", "compiler.cpp.extra_flags=-DCAPSTAN_DEBUG=1"])
    cmd.append(str(ROOT))

    res = subprocess.run(cmd, cwd=str(ROOT))
    if res.returncode != 0:
        sys.exit(res.returncode)

    built_hex = build_dir / f"{SKETCH_NAME}.hex"
    if not built_hex.exists():
        print(f"Error: Expected output hex not found at {built_hex}", file=sys.stderr)
        sys.exit(1)

    shutil.copy2(built_hex, out_hex)
    print(f"  -> Output HEX: {out_hex}")
    return out_hex


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(65536), b""):
            h.update(block)
    return h.hexdigest()


def package_release(version: str) -> None:
    DIST_ROOT.mkdir(parents=True, exist_ok=True)
    m01_src = TARGETS["model01"]["out_hex"]
    m02_src = TARGETS["model02"]["out_hex"]
    m01_dst = DIST_ROOT / f"M8_Capstan_v{version}_Model01.hex"
    m02_dst = DIST_ROOT / f"M8_Capstan_v{version}_Model02.hex"
    zip_dst = DIST_ROOT / f"M8_Capstan_v{version}_Release.zip"
    sums_dst = DIST_ROOT / "SHA256SUMS.txt"

    shutil.copy2(m01_src, m01_dst)
    shutil.copy2(m02_src, m02_dst)

    readme_path = ROOT / "README.md"
    with zipfile.ZipFile(zip_dst, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        zf.write(m01_dst, arcname=m01_dst.name)
        zf.write(m02_dst, arcname=m02_dst.name)
        if readme_path.exists():
            zf.write(readme_path, arcname="README.md")

    artifacts = [m01_dst, m02_dst, zip_dst]
    lines = [f"{sha256_file(p)}  {p.name}" for p in artifacts]
    sums_dst.write_text("\n".join(lines) + "\n", encoding="utf-8")

    print(f"\n=== Release Bundle Created (dist/ — Capstan v{version}) ===")
    for p in artifacts + [sums_dst]:
        print(f"  * {p}")


def parse_args():
    parser = argparse.ArgumentParser(
        description="Build M8 Capstan firmware for Model:01 and Model:02."
    )
    parser.add_argument(
        "targets",
        nargs="*",
        help="Optional target(s) to build: model01, model02 (default: both)",
    )
    parser.add_argument(
        "--debug",
        action="store_true",
        help="Compile with CAPSTAN_DEBUG=1 (enables runtime Serial diagnostics and freeze watchdog)",
    )
    parser.add_argument(
        "--release",
        action="store_true",
        help="Build both Model:01 and Model:02, run verification, and create release bundle in dist/",
    )
    ver_group = parser.add_mutually_exclusive_group()
    ver_group.add_argument(
        "--bump",
        "--increment",
        action="store_true",
        dest="bump_minor",
        help="Increment minor version (x.y -> x.(y+1)) in src/config.h before building",
    )
    ver_group.add_argument(
        "--bump-major",
        action="store_true",
        dest="bump_major",
        help="Increment major version (x.y -> (x+1).0) in src/config.h before building",
    )
    ver_group.add_argument(
        "--version",
        "--set-version",
        dest="set_version",
        metavar="X.Y",
        help="Set explicit firmware version 'x.y' in src/config.h before building",
    )
    return parser.parse_args()


def main():
    ns = parse_args()
    args = [a.lower() for a in ns.targets]
    if ns.release or not args:
        selected = ["model01", "model02"]
    else:
        invalid = [a for a in args if a not in TARGETS]
        if invalid:
            print(
                f"Unknown target(s): {', '.join(invalid)}. Choose from: {', '.join(TARGETS.keys())}",
                file=sys.stderr,
            )
            sys.exit(1)
        selected = args

    cur_version = read_version()
    if ns.set_version is not None:
        version = write_version(ns.set_version.strip().lstrip("vV"))
    elif ns.bump_major:
        version = write_version(bump_version_str(cur_version, major=True))
    elif ns.bump_minor:
        version = write_version(bump_version_str(cur_version, major=False))
    else:
        version = cur_version
        print(f"=== Firmware Version: v{version} (use --bump or --version X.Y to update) ===")

    patch_script = ROOT / "tools" / "teensy_msc" / "patch.py"
    if patch_script.exists():
        res = subprocess.run([sys.executable, str(patch_script)], cwd=str(ROOT))
        if res.returncode != 0:
            sys.exit(res.returncode)

    cli = find_arduino_cli()
    built_files = []
    for key in selected:
        built_files.append(
            (TARGETS[key]["label"], build_target(cli, key, version, debug=ns.debug))
        )

    verify_script = ROOT / "tools" / "recorder_sim" / "verify_firmware.py"
    if verify_script.exists():
        print("\n=== Running Automated Firmware Verification ===")
        res = subprocess.run([sys.executable, str(verify_script)], cwd=str(ROOT))
        if res.returncode != 0:
            sys.exit(res.returncode)

    if ns.release:
        package_release(version)

    print(f"\n=== Build Summary (Capstan v{version}) ===")
    for label, hex_path in built_files:
        print(f"  * {label}: {hex_path}")


if __name__ == "__main__":
    main()
