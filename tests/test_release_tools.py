from __future__ import annotations

import hashlib
import importlib.util
import tempfile
import unittest
from pathlib import Path


def load_manifest_module():
    path = Path(__file__).parents[1] / "scripts" / "create_manifest.py"
    spec = importlib.util.spec_from_file_location("create_manifest", path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class ReleaseToolTests(unittest.TestCase):
    def test_manifest_matches_firmware(self) -> None:
        module = load_manifest_module()
        with tempfile.TemporaryDirectory() as temporary_directory:
            firmware = Path(temporary_directory) / "firmware.bin"
            firmware.write_bytes(b"real firmware bytes")
            manifest = module.build_manifest(
                "0.10.0", "v0.10.0", "mrGrodzki/ebike-dashboard", firmware
            )
            self.assertEqual(manifest["size"], firmware.stat().st_size)
            self.assertEqual(
                manifest["sha256"], hashlib.sha256(firmware.read_bytes()).hexdigest()
            )
            self.assertTrue(
                str(manifest["firmware_url"]).endswith("/v0.10.0/ebike-dashboard.bin")
            )


if __name__ == "__main__":
    unittest.main()
