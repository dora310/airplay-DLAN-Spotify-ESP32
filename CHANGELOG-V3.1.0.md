# AirPlay and DLNA Receiver 3.1.0

## New options

- Eight named Internet-radio favourites stored in NVS.
- Play saved favourites directly from the Advanced control page.
- Sleep timer choices from 15 minutes to 2 hours, with cancellation and
  remaining-time status. Expiry deterministically mutes AirPlay or DLNA/radio.
- Quick DSP selections for DSP off, night mode, mono and speech filtering.
- One-click download of the current diagnostic report as JSON.
- Accurate muted status for both AirPlay and DLNA sources.
- The provisioning Wi-Fi network is named `AirPlay and DLNA Setup`.
- The control panel heading is `AirPlay 2 and DLNA+ Receiver`, and interface
  descriptions no longer use the ESP/ESP32 product wording.
- Five visible EQ sliders at 80 Hz, 250 Hz, 1 kHz, 4 kHz and 12 kHz, each with
  a -12 dB to +12 dB range.
- Flat, Bass Boost, Loudness, Vocal, Rock, Pop, Jazz, Classical and Night EQ
  presets, plus a persistent custom preset action.
- Positive EQ boosts automatically enable the limiter to reduce clipping.
- Playback controls now include recognisable play, track, volume and mute icons.

## Compatibility and stability

- Firmware, application descriptor, DLNA server identification and GitHub
  artifact names now use version 3.1.0.
- The AirPlay RTSP, HAP, PTP, timing, decoder and audio-receiver paths are
  unchanged.
- Radio favourites use small NVS strings and do not allocate background tasks.
- The sleep timer uses the existing ESP-IDF timer service and remains inactive
  until explicitly started.

## Build

Run the GitHub workflow **Build ESP32-S3 installation BIN**. The workflow reads
`version.txt` and produces:

- `airplay2-receiver-esp32s3-v3.1.0-install.bin`
- `airplay2-receiver-esp32s3-v3.1.0-ota.bin`
- `SHA256SUMS-v3.1.0.txt`

Use the installation BIN at address `0x0` for a fresh flash. Use only the OTA
BIN in the firmware-update page.
