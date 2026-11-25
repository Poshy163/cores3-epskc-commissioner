"""Generate or verify the manifest for the CoreS3 release images."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys


FIRMWARE = Path(__file__).resolve().parents[1] / "firmware"
BOOT = "bootloader.bin"
TABLE = "partition-table.bin"
APP = "epskc-commissioner-app.bin"
RADIO = "rcp-fw.bin"
FACTORY = "cores3-epskc-commissioner.bin"
TABLE_OFFSET = 0x8000


def partition_ranges(table):
    # ESP-IDF 32-byte partition entries, followed by their MD5 record.
    ranges = {}
    for offset in range(0, len(table) - 31, 32):
        entry = table[offset:offset + 32]
        if entry[:16] == b"\xeb\xeb" + b"\xff" * 14:
            if entry[16:] != hashlib.md5(table[:offset], usedforsecurity=False).digest():
                raise ValueError("partition-table MD5 mismatch")
            if not {"factory", "rcp_fw"} <= ranges.keys():
                raise ValueError("missing factory or rcp_fw partition")
            return ranges
        magic, kind, subtype, start, size, label, _ = struct.unpack("<HBBII16sI", entry)
        if magic != 0x50AA:
            break
        name = label.rstrip(b"\0").decode("ascii")
        if (name == "factory" and (kind, subtype) != (0, 0)) or (
            name == "rcp_fw" and (kind, subtype) != (1, 0x82)
        ):
            raise ValueError(f"unexpected partition type for {name}")
        if name in ranges:
            raise ValueError(f"duplicate partition {name}")
        ranges[name] = (start, size)
    raise ValueError("invalid or missing partition-table MD5 record")


def image_metadata(data, offset):
    return {"offset": hex(offset), "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def make_manifest(folder, source_commit, elf=None):
    images = {name: (folder / name).read_bytes() for name in (BOOT, TABLE, APP, RADIO, FACTORY)}
    for name in (BOOT, APP):
        image = images[name]
        # ESP image header, chip ID ESP32-S3, and appended image digest.
        if (len(image) < 208 or image[0] != 0xE9 or image[12:14] != b"\x09\0"
                or image[23] != 1 or hashlib.sha256(image[:-32]).digest() != image[-32:]):
            raise ValueError(f"invalid ESP32-S3 image or appended SHA-256 in {name}")
    app = images[APP]
    # The first segment contains esp_app_desc_t after the 24 + 8 byte headers.
    if app[32:36] != b"\x32\x54\xcd\xab" or app[80:112].rstrip(b"\0") != b"epskc_commissioner":
        raise ValueError("unexpected app descriptor")
    version = app[48:80].rstrip(b"\0").decode("ascii")
    sdk = app[144:176].rstrip(b"\0").decode("ascii")
    if (not re.fullmatch(r"[0-9a-f]{40}", source_commit)
            or not re.fullmatch(r"[0-9a-f]{7,40}", version)
            or not source_commit.startswith(version)):
        raise ValueError(f"release source commit disagrees with embedded app version {version!r}")
    elf_hash = app[176:208].hex()
    if elf is not None:
        with elf.open("rb") as file:
            hasher = hashlib.sha256()
            for chunk in iter(lambda: file.read(1024 * 1024), b""):
                hasher.update(chunk)
        if hasher.hexdigest() != elf_hash:
            raise ValueError("build ELF SHA-256 disagrees with app descriptor")

    ranges = partition_ranges(images[TABLE])
    app_start, app_size = ranges["factory"]
    radio_start, radio_size = ranges["rcp_fw"]
    if len(app) > app_size or len(images[RADIO]) != radio_size:
        raise ValueError("app or radio image size disagrees with its partition")
    previous_end = TABLE_OFFSET + len(images[TABLE])
    for start, size in sorted(ranges.values()):
        if start < previous_end or size == 0:
            raise ValueError("invalid or overlapping partition ranges")
        previous_end = start + size

    merged = images[FACTORY]
    artifacts = {}
    end = 0
    for name, offset in ((BOOT, 0), (TABLE, TABLE_OFFSET), (APP, app_start), (RADIO, radio_start)):
        data = images[name]
        if end > offset or merged[end:offset].count(b"\xff") != offset - end:
            raise ValueError(f"overlap or unexpected factory image data before {name}")
        end = offset + len(data)
        if merged[offset:end] != data:
            raise ValueError(f"factory image at {offset:#x} disagrees with {name}")
        artifacts[name] = image_metadata(data, offset)
    if len(merged) != end:
        raise ValueError("unexpected factory image length")
    artifacts[FACTORY] = image_metadata(merged, 0)
    return {"schema": 2, "source_commit": source_commit, "version": version,
            "sdk": sdk, "app_elf_sha256": elf_hash, "artifacts": artifacts}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dir", type=Path, default=FIRMWARE, help="published image directory")
    parser.add_argument("--write", metavar="SOURCE_COMMIT", help="write manifest for a full Git commit")
    parser.add_argument("--elf", type=Path, help="also verify the build ELF against the descriptor")
    args = parser.parse_args()
    manifest_path = args.dir / "build-info.json"
    try:
        if args.write:
            manifest = make_manifest(args.dir, args.write, args.elf)
            manifest_path.write_bytes((json.dumps(manifest, indent=2) + "\n").encode("utf-8"))
        else:
            saved = json.loads(manifest_path.read_bytes())
            manifest = make_manifest(args.dir, saved["source_commit"], args.elf)
            if saved != manifest:
                raise ValueError("manifest disagrees with published images")
    except (KeyError, TypeError, OSError, ValueError) as error:
        sys.exit(f"release verification failed: {error}")
    print(f"Verified five release images for {manifest['version']} ({manifest['sdk']})")


if __name__ == "__main__":
    main()
