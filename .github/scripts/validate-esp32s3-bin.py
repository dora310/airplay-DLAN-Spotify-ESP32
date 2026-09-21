#!/usr/bin/env python3

import sys
from pathlib import Path


EXPECTED_SIZE = 0x800000
APP_OFFSET = 0x20000
ESP_IMAGE_MAGIC = 0xE9
SPI_MODE_DIO = 2


def fail(message: str) -> None:
    raise SystemExit(f"Firmware validation failed: {message}")


if len(sys.argv) != 2:
    fail("expected the merged BIN filename")

firmware_path = Path(sys.argv[1])
if not firmware_path.is_file():
    fail(f"file does not exist: {firmware_path}")

firmware = firmware_path.read_bytes()

if len(firmware) != EXPECTED_SIZE:
    fail(
        f"unexpected merged size: {len(firmware)} bytes; "
        f"expected {EXPECTED_SIZE} bytes"
    )

if firmware[0] != ESP_IMAGE_MAGIC:
    fail("bootloader image is missing at address 0x0")

if firmware[2] != SPI_MODE_DIO:
    fail(f"bootloader flash mode is {firmware[2]}; expected DIO mode (2)")

if firmware[APP_OFFSET] != ESP_IMAGE_MAGIC:
    fail("application image is missing at address 0x20000")

print(
    "Validated merged image: 8 MB, DIO mode, "
    "bootloader and application present"
)
