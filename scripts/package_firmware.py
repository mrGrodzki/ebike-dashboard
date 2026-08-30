#!/usr/bin/env python3
"""Collect OTA and first-USB-flash files from an ESP-IDF build directory."""

from __future__ import annotations

import argparse
import json
import shutil
from pathlib import Path


OTA_SLOT_SIZE = 0x400000


def safe_build_file(build_dir: Path, relative_name: str) -> Path:
    candidate = (build_dir / relative_name).resolve()
    build_root = build_dir.resolve()
    if build_root not in candidate.parents and candidate != build_root:
        raise ValueError(f"unsafe build file path: {relative_name}")
    if not candidate.is_file():
        raise FileNotFoundError(candidate)
    return candidate


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("build_dir", type=Path)
    parser.add_argument("output_dir", type=Path)
    args = parser.parse_args()

    build_dir = args.build_dir.resolve()
    output_dir = args.output_dir.resolve()
    flash_json_path = build_dir / "flasher_args.json"
    flash_data = json.loads(flash_json_path.read_text(encoding="utf-8"))

    firmware_source = safe_build_file(build_dir, "ebike_dashboard.bin")
    firmware_size = firmware_source.stat().st_size
    if firmware_size >= OTA_SLOT_SIZE:
        raise RuntimeError(
            f"firmware is {firmware_size} bytes and does not fit the {OTA_SLOT_SIZE}-byte OTA slot"
        )

    output_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(firmware_source, output_dir / "ebike-dashboard.bin")

    first_flash = output_dir / "first-usb-flash"
    first_flash.mkdir(parents=True, exist_ok=True)
    flash_files = flash_data.get("flash_files", {})
    if not flash_files:
        raise RuntimeError("flasher_args.json does not contain flash_files")
    for relative_name in flash_files.values():
        source = safe_build_file(build_dir, relative_name)
        destination = first_flash / relative_name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)

    for name in ("flash_args", "flash_project_args", "flasher_args.json"):
        source = safe_build_file(build_dir, name)
        shutil.copy2(source, first_flash / name)

    (first_flash / "FLASH-WINDOWS.bat").write_text(
        "@echo off\r\n"
        "if \"%1\"==\"\" (echo Usage: FLASH-WINDOWS.bat COM5 & exit /b 2)\r\n"
        "python -m esptool --chip esp32s3 --port %1 write_flash @flash_args\r\n",
        encoding="ascii",
    )
    (first_flash / "flash-linux-macos.sh").write_text(
        "#!/usr/bin/env sh\n"
        "set -eu\n"
        "test $# -eq 1 || { echo 'Usage: ./flash-linux-macos.sh /dev/ttyACM0'; exit 2; }\n"
        "python -m esptool --chip esp32s3 --port \"$1\" write_flash @flash_args\n",
        encoding="utf-8",
    )
    (first_flash / "README.txt").write_text(
        "FIRST OTA-ENABLED USB FLASH\n\n"
        "This package changes the partition table. Flash it once over USB.\n"
        "It preserves the NVS offset, but back up anything important first.\n"
        "Windows: FLASH-WINDOWS.bat COM5\n"
        "Linux/macOS: ./flash-linux-macos.sh /dev/ttyACM0\n"
        "After this first flash, normal releases can be installed over Wi-Fi.\n",
        encoding="utf-8",
    )

    print(f"OTA image: {firmware_size} / {OTA_SLOT_SIZE} bytes")
    print(f"Packaged {len(flash_files)} first-flash binaries in {first_flash}")


if __name__ == "__main__":
    main()
