# ESP32-S3 AirPlay 2 + DLNA v0.2.00 — Wi-Fi Scan r3 source

This is the complete ESP-IDF source package for the latest custom receiver
revision prepared for an ESP32-S3-WROOM-1 with 16 MB flash and a PCM5102A I2S
DAC. It keeps the upstream AirPlay 2 structure intact and adds DLNA beside it.

The exact merged and OTA images supplied to the device are included in
`release/`.

Firmware version **0.2.00** updates the application descriptor, DLNA server
identifier and DLNA model number while preserving the tested r3 executable
code and Wi-Fi scan repair.

## r3 behaviour

- AirPlay 2 receiver based on upstream `rbouteiller/airplay-esp32` v0.1.29.
- DLNA/UPnP MediaRenderer discovery and SOAP control.
- DLNA HTTP playback for MP3, FLAC and 16-bit PCM WAV.
- AirPlay has exclusive priority whenever an AirPlay client owns a session.
- DLNA playback stops before the AirPlay I2S writer resumes.
- SSDP discovery pauses during an AirPlay session to reduce socket and Wi-Fi
  contention.
- Browser live logs remain available.
- **Scan Networks no longer disconnects the active STA connection.** The scan
  runs while the receiver remains associated with the router, preserving the
  web request and DHCP address.

## Important source/release note

The released r3 image was made from the hardware-tested r2 merged image by
replacing the three Xtensa bytes that entered the destructive Wi-Fi scan
preamble. ESP image checksum and appended SHA-256 data were then regenerated.
The exact guarded patcher is included as `tools/patch_rev2_scan.py`.

In this source tree, the same repair is expressed normally in
`main/network/wifi.c`: `wifi_scan()` goes directly to `esp_wifi_scan_start()`
and does not call `esp_wifi_disconnect()` or clear the selected BSSID.

A fresh source build will not be byte-for-byte identical to the released image
because ESP-IDF embeds build metadata and the linker layout can change. Use the
included release image when you want the exact version already tested on the
board.

## Hardware defaults

| Function | ESP32-S3 GPIO |
| --- | ---: |
| PCM5102A BCK | 11 |
| PCM5102A DIN | 12 |
| PCM5102A LRCK/WS | 13 |
| Optional software GND | 14 |
| On-board RGB LED | 48 |

The source targets the generic ESP32-S3 board, octal PSRAM and 16 MB flash.

## Build with ESP-IDF 5.5

```bash
source /path/to/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
```

The project contains `sdkconfig` from the r3 target configuration as well as
the corresponding defaults. MP3, FLAC and WAV decoder options are enabled.

The download ZIP intentionally omits the generated `managed_components/`
cache and non-build documentation/tooling from the large u8g2 submodule.
`idf.py build` restores the exact managed-component versions recorded in
`dependencies.lock`; the u8g2 C sources required by the build are included.

Create an 8 MB merged image:

```bash
esptool --chip esp32s3 merge-bin \
  --flash-mode qio --flash-size 16MB \
  -o airplay2-receiver-esp32s3-v0.2.00-r3-source-build.bin \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x19000 build/ota_data_initial.bin \
  0x20000 build/airplay2-receiver.bin \
  0x620000 build/storage.bin
```

## Flashing

For a full USB installation, flash the tested merged image at address `0x0`:

```bash
esptool --chip esp32s3 erase-flash
esptool --chip esp32s3 --baud 460800 write-flash 0x0 \
  release/airplay2-receiver-esp32s3-v0.2.00-dlna-wifi-scan-r3.bin
```

Erasing flash removes saved Wi-Fi credentials and AirPlay pairing data. The OTA
image is intended for the firmware update page and must not be written at
address `0x0`.

## Main custom files

- `main/network/dlna_renderer.c`
- `main/network/dlna_renderer.h`
- `main/network/wifi.c`
- `main/network/web_server.c`
- `main/network/web_server.h`
- `main/main.c`
- `main/playback_control.c`
- `main/playback_control.h`
- `main/CMakeLists.txt`

## Release checksums

```text
69445b696c854351e4e67fabc92cfd6156cbad76726fc83f551fb41445be8e8d  airplay2-receiver-esp32s3-v0.2.00-dlna-wifi-scan-r3.bin
25364ff8a916336593dbeadebff0c987afe94c93c8945330230f56897ec338f7  airplay2-receiver-esp32s3-v0.2.00-dlna-wifi-scan-r3-ota.bin
```

The upstream project licence remains in `LICENSE`. Licences for managed
components are supplied with those components when ESP-IDF restores them.
