"""Test release verification against copies of the published, known-good binaries."""

import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from release_manifest import APP, FACTORY, FIRMWARE, RADIO, TABLE, make_manifest


class ReleaseManifestTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.folder = Path(temp.name)
        self.saved = json.loads((FIRMWARE / "build-info.json").read_bytes())
        self.commit = self.saved["source_commit"]
        for name in self.saved["artifacts"]:
            shutil.copyfile(FIRMWARE / name, self.folder / name)

    def flip_byte(self, name, offset):
        with (self.folder / name).open("r+b") as target:
            target.seek(offset)
            value = target.read(1)[0]
            target.seek(offset)
            target.write(bytes([value ^ 1]))

    def test_published_manifest(self):
        self.assertEqual(make_manifest(self.folder, self.commit), self.saved)

    def test_wrong_source_and_elf(self):
        with self.assertRaisesRegex(ValueError, "source commit"):
            make_manifest(self.folder, "0" * 40)
        with self.assertRaisesRegex(ValueError, "ELF SHA-256"):
            make_manifest(self.folder, self.commit, self.folder / APP)

    def test_corrupt_app_image(self):
        self.flip_byte(APP, 512)
        with self.assertRaisesRegex(ValueError, "appended SHA-256"):
            make_manifest(self.folder, self.commit)

    def test_corrupt_partition_table(self):
        self.flip_byte(TABLE, 31)
        with self.assertRaisesRegex(ValueError, "partition-table MD5"):
            make_manifest(self.folder, self.commit)

    def test_mismatched_radio_and_merged_image(self):
        self.flip_byte(RADIO, 100)
        with self.assertRaisesRegex(ValueError, "factory image.*disagrees"):
            make_manifest(self.folder, self.commit)

    def test_unexpected_data_in_nvs_gap(self):
        self.flip_byte(FACTORY, 0x9000)
        with self.assertRaisesRegex(ValueError, "unexpected factory image data"):
            make_manifest(self.folder, self.commit)

    def test_wrong_saved_checksum_fails_cli(self):
        self.saved["artifacts"][APP]["sha256"] = "0" * 64
        (self.folder / "build-info.json").write_text(json.dumps(self.saved), encoding="utf-8")
        script = Path(__file__).with_name("release_manifest.py")
        result = subprocess.run([sys.executable, str(script), "--dir", str(self.folder)],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("manifest disagrees", result.stderr)


if __name__ == "__main__":
    unittest.main()
