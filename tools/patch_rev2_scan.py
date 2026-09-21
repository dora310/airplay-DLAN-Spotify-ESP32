#!/usr/bin/env python3
"""Patch only the destructive Wi-Fi scan preamble in the stable r2 image."""

from pathlib import Path
import sys
import tempfile

from esptool.bin_image import LoadFirmwareImage


APP_OFFSET = 0x20000
APP_PARTITION_SIZE = 0x300000
IROM_BASE = 0x42000020
WIFI_SCAN_ADDR = 0x42018B80
PATCH_ADDR = 0x42018B96
SCAN_SETUP_ADDR = 0x42018BE6

# At PATCH_ADDR, r2 loads s_retry_timer before stopping the timer,
# disconnecting the station, delaying, and clearing the BSSID lock.
EXPECTED_OLD = bytes.fromhex("81 99 a2")
# Xtensa `j 0x42018be6`, assembled for PC 0x42018b96.
PATCH_JUMP = bytes.fromhex("06 13 00")

EXPECTED_FUNCTION_PREFIX = bytes.fromhex(
    "36 c1 01 80 f2 40 7d 02 80 85 41 16 12 13 0c 12 "
    "30 28 93 56 92 12"
)
EXPECTED_SCAN_SETUP = bytes.fromhex(
    "9d 01 0c 0a 82 a0 0b 76 88 03 a9 09 4b 99"
)


def load_image(path: Path):
    image = LoadFirmwareImage("esp32s3", str(path))
    # esptool 4.12 loads raw binary segments without the ELF-only `name`
    # attribute, while its save path expects it to exist for diagnostics.
    for segment in image.segments:
        if not hasattr(segment, "name"):
            segment.name = ""
    return image


def find_segment(image, address: int):
    for segment in image.segments:
        if segment.addr <= address < segment.addr + len(segment.data):
            return segment
    raise RuntimeError(f"address {address:#x} is not in an image segment")


def save_image(image, path: Path) -> bytes:
    image.save(str(path))
    return path.read_bytes()


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} INPUT_MERGED.bin OUTPUT_MERGED.bin")
        return 2

    source_path = Path(sys.argv[1])
    output_path = Path(sys.argv[2])
    source = source_path.read_bytes()
    if len(source) != 0x800000:
        raise RuntimeError(f"expected an 8 MiB merged image, got {len(source)} bytes")

    app_partition = source[APP_OFFSET : APP_OFFSET + APP_PARTITION_SIZE]
    with tempfile.TemporaryDirectory(prefix="airplay-scan-patch-") as temp_dir:
        temp = Path(temp_dir)
        original_app_path = temp / "original-app.bin"
        roundtrip_path = temp / "roundtrip-app.bin"
        patched_app_path = temp / "patched-app.bin"
        original_app_path.write_bytes(app_partition)

        # First prove esptool's load/save cycle reproduces this image exactly.
        roundtrip = save_image(load_image(original_app_path), roundtrip_path)
        if app_partition[: len(roundtrip)] != roundtrip:
            raise RuntimeError("esptool round-trip changed the stable r2 application")

        image = load_image(original_app_path)
        function_segment = find_segment(image, WIFI_SCAN_ADDR)
        if function_segment.addr != IROM_BASE:
            raise RuntimeError(
                f"unexpected IROM base {function_segment.addr:#x}; refusing to patch"
            )

        function_offset = WIFI_SCAN_ADDR - function_segment.addr
        patch_offset = PATCH_ADDR - function_segment.addr
        setup_offset = SCAN_SETUP_ADDR - function_segment.addr

        if (
            function_segment.data[
                function_offset : function_offset + len(EXPECTED_FUNCTION_PREFIX)
            ]
            != EXPECTED_FUNCTION_PREFIX
        ):
            raise RuntimeError("wifi_scan prefix does not match stable r2")
        if (
            function_segment.data[
                setup_offset : setup_offset + len(EXPECTED_SCAN_SETUP)
            ]
            != EXPECTED_SCAN_SETUP
        ):
            raise RuntimeError("wifi_scan scan-setup block does not match stable r2")
        if (
            function_segment.data[patch_offset : patch_offset + len(EXPECTED_OLD)]
            != EXPECTED_OLD
        ):
            raise RuntimeError("wifi_scan patch site has unexpected bytes")

        function_segment.data = (
            function_segment.data[:patch_offset]
            + PATCH_JUMP
            + function_segment.data[patch_offset + len(PATCH_JUMP) :]
        )
        patched_app = save_image(image, patched_app_path)

        if len(patched_app) != len(roundtrip):
            raise RuntimeError("patched application length changed")

        patch_file_offset = function_segment.file_offs + 8 + patch_offset
        checksum_offset = len(patched_app) - 33
        digest_start = len(patched_app) - 32
        allowed_changes = set(range(patch_file_offset, patch_file_offset + 3))
        allowed_changes.add(checksum_offset)
        allowed_changes.update(range(digest_start, len(patched_app)))
        actual_changes = {
            index
            for index, (old, new) in enumerate(zip(roundtrip, patched_app))
            if old != new
        }
        unexpected = actual_changes - allowed_changes
        if unexpected:
            raise RuntimeError(
                "unexpected application changes at "
                + ", ".join(hex(index) for index in sorted(unexpected)[:16])
            )
        if not set(range(patch_file_offset, patch_file_offset + 3)).issubset(
            actual_changes
        ):
            raise RuntimeError("not all three scan jump bytes changed")

        output = bytearray(source)
        output[APP_OFFSET : APP_OFFSET + len(patched_app)] = patched_app
        output_path.write_bytes(output)

        # Re-open and verify the application image from the finished merged file.
        verify_app_path = temp / "verify-app.bin"
        verify_app_path.write_bytes(
            output[APP_OFFSET : APP_OFFSET + APP_PARTITION_SIZE]
        )
        verified = load_image(verify_app_path)
        verified.verify()

    print(f"patched scan jump at app offset {patch_file_offset:#x}")
    print(f"application image length {len(patched_app):#x}")
    print(f"changed application bytes {len(actual_changes)}")
    print(f"wrote {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
