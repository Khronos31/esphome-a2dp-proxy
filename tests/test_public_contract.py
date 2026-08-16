"""Checks for the source-only public release contract."""

from __future__ import annotations

import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class PublicReleaseContractTests(unittest.TestCase):
    def test_required_public_files_exist(self):
        required = (
            "VERSION",
            "LICENSE",
            "README.md",
            "components/a2dp_source/__init__.py",
            "components/a2dp_source/a2dp_source_speaker.cpp",
            "examples/esp32-devkitc-ve.yaml",
            "tools/release_version.py",
        )
        for relative_path in required:
            with self.subTest(path=relative_path):
                self.assertTrue((ROOT / relative_path).is_file())

    def test_release_metadata_is_consistent(self):
        result = subprocess.run(
            ["python", "tools/release_version.py", "check"],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(0, result.returncode, result.stderr)

    def test_wrong_release_tag_is_rejected(self):
        result = subprocess.run(
            ["python", "tools/release_version.py", "check", "--tag", "v9.9.9"],
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(0, result.returncode)
        self.assertIn("release tag", result.stderr)

    def test_example_uses_stable_shared_radio_profile(self):
        example = (ROOT / "examples/esp32-devkitc-ve.yaml").read_text(
            encoding="utf-8"
        )
        self.assertIn("max_bitpool: 32", example)
        self.assertIn("buffer_duration: 750ms", example)
        self.assertIn("buffer_size: 400000", example)
        self.assertIn("sample_rate: 22050", example)
        self.assertIn("num_channels: 1", example)
        self.assertNotIn("task_stack_in_psram", example)

    def test_example_owns_network_and_authentication(self):
        example = (ROOT / "examples/esp32-devkitc-ve.yaml").read_text(
            encoding="utf-8"
        )
        self.assertIn("ssid: !secret wifi_ssid", example)
        self.assertIn("password: !secret wifi_password", example)
        self.assertIn("key: !secret api_encryption_key", example)
        self.assertIn("password: !secret ota_password", example)
        self.assertNotIn("build-test", example)

    def test_repository_does_not_contain_firmware_artifacts(self):
        forbidden_suffixes = {".bin", ".elf", ".map"}
        artifacts = [
            str(path.relative_to(ROOT))
            for path in ROOT.rglob("*")
            if path.is_file()
            and ".esphome" not in path.parts
            and path.suffix in forbidden_suffixes
        ]
        self.assertEqual([], artifacts)


if __name__ == "__main__":
    unittest.main()
