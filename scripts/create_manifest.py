#!/usr/bin/env python3
"""Create the small authenticated-by-HTTPS manifest consumed by the dashboard."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path


VERSION_PATTERN = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")


def build_manifest(version: str, tag: str, repository: str, firmware: Path) -> dict[str, object]:
    if not VERSION_PATTERN.fullmatch(version):
        raise ValueError(f"invalid release version: {version}")
    if tag != f"v{version}":
        raise ValueError(f"tag {tag!r} does not match version {version!r}")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise ValueError(f"invalid repository name: {repository}")
    image = firmware.read_bytes()
    digest = hashlib.sha256(image).hexdigest()
    if not SHA256_PATTERN.fullmatch(digest):
        raise AssertionError("unexpected SHA-256 output")
    return {
        "product": "ebike-dashboard",
        "hardware": "waveshare-esp32-s3-touch-lcd-2",
        "channel": "stable",
        "version": version,
        "idf_version": "5.5.2",
        "size": len(image),
        "sha256": digest,
        "firmware_url": (
            f"https://github.com/{repository}/releases/download/{tag}/ebike-dashboard.bin"
        ),
        "release_notes": f"Stable e-bike dashboard firmware {version}.",
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--firmware", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    manifest = build_manifest(args.version, args.tag, args.repository, args.firmware)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
