# AirPlay and DLNA Receiver 3.2.0

## Safety, recovery and administration

- Authenticated JSON backup and restore for device, Wi-Fi, volume, LED, DSP,
  radio favourites, access-password digest and maintenance preferences.
- OTA images are validated before boot selection. New OTA images remain in the
  bootloader rollback window until the receiver survives a 30-second stability
  check.
- Warning and error messages are retained on SPIFFS independently of browser
  WebSocket clients, with download/refresh and clear controls.
- Speaker protection adds a configurable 50-98% full-scale ceiling, automatic
  limiting, clipping statistics and resettable protection counters.
- Scheduled restarts use an uptime interval and wait until AirPlay, DLNA and
  radio audio are idle.
- Auto, dark and light control-panel themes.
- Password-gated configuration mutations and an unlock screen on both control
  panels. Only a domain-separated SHA-256 digest is stored by the receiver.
- Factory reset requires both a browser confirmation and the exact phrase
  `ERASE ALL SETTINGS`.

## Compatibility

The AirPlay receive, timing and decoding paths are unchanged. The existing
DLNA renderer and five-band DSP processing remain in place.

## Build outputs

The GitHub Actions workflow reads `version.txt` and creates:

- `airplay2-receiver-esp32s3-v3.2.0-install.bin`
- `airplay2-receiver-esp32s3-v3.2.0-ota.bin`
- `SHA256SUMS-v3.2.0.txt`
