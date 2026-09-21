#!/usr/bin/env python3
"""Update an ESP32 application version without changing executable code.

This is intended for equal-length version strings such as 0.1.29 -> 0.2.00.
It updates every exact version occurrence, then regenerates the ESP image XOR
checksum and optional appended SHA-256 digest.  A matching merged image can be
updated by replacing its application image at offset 0x20000.
"""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path


ESP_IMAGE_MAGIC = 0xE9
ESP_CHECKSUM_SEED = 0xEF
APP_OFFSET = 0x20000


def parse_image(image: bytes) -> tuple[list[tuple[int, int, int]], int, bool]:
    if len(image) < 24 or image[0] != ESP_IMAGE_MAGIC:
        raise ValueError("input is not an ESP application image")

    segment_count = image[1]
    hash_appended = image[23] == 1
    position = 24
    segments: list[tuple[int, int, int]] = []

    for index in range(segment_count):
        if position + 8 > len(image):
            raise ValueError(f"truncated segment header {index}")
        load_address, data_length = struct.unpack_from("<II", image, position)
        position += 8
        data_end = position + data_length
        if data_end > len(image):
            raise ValueError(f"truncated segment data {index}")
        segments.append((load_address, position, data_length))
        position = data_end

    checksum_offset = ((position + 15) & ~15) - 1
    digest_size = 32 if hash_appended else 0
    expected_size = checksum_offset + 1 + digest_size
    if expected_size != len(image):
        raise ValueError(
            f"unexpected image size: parsed {expected_size}, actual {len(image)}"
        )
    return segments, checksum_offset, hash_appended


def calculate_checksum(image: bytes, segments: list[tuple[int, int, int]]) -> int:
    checksum = ESP_CHECKSUM_SEED
    for _, data_offset, data_length in segments:
        for value in image[data_offset : data_offset + data_length]:
            checksum ^= value
    return checksum


def verify_image(image: bytes) -> None:
    segments, checksum_offset, hash_appended = parse_image(image)
    calculated = calculate_checksum(image, segments)
    if image[checksum_offset] != calculated:
        raise ValueError(
            f"invalid ESP checksum: stored {image[checksum_offset]:#04x}, "
            f"calculated {calculated:#04x}"
        )
    if hash_appended:
        expected_digest = hashlib.sha256(image[:-32]).digest()
        if image[-32:] != expected_digest:
            raise ValueError("invalid appended ESP SHA-256 digest")


def update_application(image: bytes, old: bytes, new: bytes) -> tuple[bytes, list[int]]:
    if len(old) != len(new):
        raise ValueError("old and new versions must have the same byte length")
    verify_image(image)

    offsets: list[int] = []
    search_from = 0
    while True:
        offset = image.find(old, search_from)
        if offset < 0:
            break
        offsets.append(offset)
        search_from = offset + len(old)
    if not offsets:
        raise ValueError(f"version {old.decode()} was not found")

    updated = bytearray(image)
    for offset in offsets:
        updated[offset : offset + len(old)] = new

    segments, checksum_offset, hash_appended = parse_image(updated)
    updated[checksum_offset] = calculate_checksum(updated, segments)
    if hash_appended:
        updated[-32:] = hashlib.sha256(updated[:-32]).digest()

    result = bytes(updated)
    verify_image(result)
    if old in result:
        raise ValueError("an old version occurrence remains after patching")
    return result, offsets


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input_ota", type=Path)
    parser.add_argument("output_ota", type=Path)
    parser.add_argument("old_version")
    parser.add_argument("new_version")
    parser.add_argument("--input-merged", type=Path)
    parser.add_argument("--output-merged", type=Path)
    args = parser.parse_args()

    if bool(args.input_merged) != bool(args.output_merged):
        parser.error("--input-merged and --output-merged must be used together")

    old = args.old_version.encode("ascii")
    new = args.new_version.encode("ascii")
    original_ota = args.input_ota.read_bytes()
    updated_ota, offsets = update_application(original_ota, old, new)
    args.output_ota.write_bytes(updated_ota)

    if args.input_merged:
        merged = bytearray(args.input_merged.read_bytes())
        app_end = APP_OFFSET + len(original_ota)
        if app_end > len(merged):
            raise ValueError("merged image is too small for the OTA application")
        if merged[APP_OFFSET:app_end] != original_ota:
            raise ValueError("merged application does not match the input OTA image")
        merged[APP_OFFSET:app_end] = updated_ota
        args.output_merged.write_bytes(merged)

    print("updated offsets: " + ", ".join(hex(offset) for offset in offsets))
    print(f"ESP checksum and SHA-256 verified for firmware {args.new_version}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
