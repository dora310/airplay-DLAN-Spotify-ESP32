# ESP32-S3 AirPlay 2 + DLNA v3.3.0 development source

## v3.3.0 reliability and integration controls

Open `http://DEVICE-IP/reliability` after installation. This release adds:

- crash-triggered Recovery/Safe Mode;
- automatic, additive configuration migration;
- Wi-Fi diagnostics and transactional credential testing with rollback;
- runtime MQTT and Home Assistant discovery settings;
- idle-only left/right/stereo speaker tests and saved balance;
- GitHub Release OTA checks and matching-board installation.

The GitHub updater expects an OTA release asset whose filename contains the
current target (`esp32s3`, `esp32s2`, or `esp32`), the text `ota`, and `.bin`.
Safe Mode intentionally leaves only networking and the web configuration
server active so an unstable optional service cannot block recovery.

## v3.2.0 safety and administration controls

Open `http://DEVICE-IP/advanced` to use:

- authenticated JSON settings backup and restore;
- retained warning/error diagnostics;
- speaker protection, limiter and clipping counters;
- idle-only scheduled restarts;
- automatic, dark and light themes;
- control-panel password management; and
- phrase-confirmed factory reset.

OTA updates uploaded from the main panel are checked as application images and
boot with ESP-IDF rollback enabled. The new image is confirmed only after the
core configuration service remains healthy for 30 seconds.

This branch keeps the working v0.2.00 AirPlay protocol, HAP, RTSP, PTP and
audio-receiver code intact. New services sit behind a central audio-source
owner so AirPlay always has the highest priority.

## Implemented in this source

- Source manager with priorities: AirPlay > DLNA > Snapcast adapter >
  Squeezelite adapter > radio. AirPlay takeover stops DLNA through the
  existing tested path.
- AirPlay lifecycle cleanup remains in the upstream-derived RTSP/audio stream
  code; ownership and playback-control state are now also reset at disconnect.
- Existing sorted PSRAM jitter buffer, timing thresholds, rapid stale-frame
  drain and buffered-stream TCP back-pressure are retained.
- Wi-Fi retry/backoff, setup-AP recovery, stale-BSSID release for mesh roaming,
  serialized scans, and configurable setup-AP channel.
- Internet-radio URL playback through the existing MP3/FLAC/WAV DLNA decoder.
- Eight named Internet-radio favourites stored in NVS and playable from the
  advanced web page.
- A 15-minute to 2-hour sleep timer that safely mutes the active source, plus
  quick DSP profiles and downloadable JSON diagnostics.
- Shared software DSP for AirPlay and DLNA on the I2S build:
  five parametric peak filters, limiter, slow normalization, balance,
  stereo/mono/left/right/swap and high-pass/low-pass crossover. Settings are
  stored in NVS.
- The advanced page exposes the five EQ bands with -12 dB to +12 dB sliders,
  Bass Boost/Loudness/Flat buttons, nine named presets and persistent custom
  settings. Boosted profiles force the limiter on.
- REST API, advanced web page, playback controls, health report and API-key
  protection. The password itself is never stored; NVS contains its SHA-256
  digest. After enabling it, the browser keeps the entered API key in local
  storage and sends it as `X-API-Key`.
- Optional MQTT control and Home Assistant discovery (disabled by default).
- OTA SHA-256 integrity checking plus ESP-IDF rollback confirmation after a
  healthy 30-second network/AirPlay startup.
- Boot count, reset reason, low-memory events, minimum heap, PSRAM, largest
  internal allocation block, limiter-event and source-switch diagnostics.

Open `http://DEVICE-IP/advanced` for the new controls.

## Integration-ready, not bundled

Snapcast, Squeezelite/LMS and BLE provisioning are deliberately represented by
weak adapter contracts in `main/optional_backends.*`. Their full engines are
large independent projects and are **not** included in this archive. Enable
their Kconfig switch only after adding a compatible ESP-IDF component that
overrides the corresponding weak functions. This prevents an unused engine
from consuming the sockets and internal RAM needed by AirPlay.

BLE on ESP32-S3 is configuration/control only; ESP32-S3 has no Bluetooth
Classic A2DP receiver support.

## DLNA scope

The built-in renderer plays HTTP MP3, FLAC and WAV sources and exposes basic
UPnP AVTransport/RenderingControl. Accurate duration/seek, playlist expansion,
album-art caching and gapless playback require a larger media-session layer and
remain future work. The REST radio endpoint reuses this decoder.

## Secure OTA and rollback

Rollback is enabled for the ESP32-S3 defaults. SHA-256 detects corruption but
does not prove who produced an image. For authenticated OTA, generate and keep
your own private signing key offline, then enable ESP-IDF signed-app verification
(`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` and
`CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`) and configure the public-key
verification/signing workflow. No private key is included in this source.

Do not enable Secure Boot or flash encryption on a test board until you have a
recovery plan; eFuse changes may be irreversible.

## REST API

- `GET /api/v1/status`
- `GET /api/v1/health`
- `POST /api/v1/control` with `{"action":"play_pause"}`
- `GET|POST /api/v1/dsp`
- `POST /api/v1/radio` with `{"url":"https://..."}`
- `GET|POST /api/v1/radio/presets`
- `GET|POST /api/v1/sleep` with `{"minutes":30}`; use `0` to cancel
- `POST /api/v1/security/password` with a password of 8–64 characters

When protection is enabled, mutations and detailed health data require the
`X-API-Key` header. An empty password disables protection.

## MQTT/Home Assistant

Enable `CONFIG_MQTT_CONTROL_ENABLE`, set `CONFIG_MQTT_BROKER_URI`, and choose a
topic prefix. It is off by default so the MQTT TCP socket and task cannot affect
AirPlay timing. Broker authentication/TLS credentials should be added through a
deployment-specific configuration rather than committed to this archive.

## Build notes

Use the same ESP-IDF 5.5.x environment and board defaults as v0.2.00. For the
ESP32-S3-WROOM-1 + PCM5102A setup, keep the I2S build and the known-good GPIO
mapping. A clean build should regenerate the managed components from
`main/idf_component.yml`.

This archive is a development source handoff. It has static consistency checks
but was not compiled or tested on hardware in the current environment because
the ESP-IDF Xtensa toolchain is not installed. Do not flash a binary labeled
v3.2.0 unless it was produced from this source and tested first on a recovery-
capable board.

## One-click GitHub build

After pushing this folder to the default branch of a GitHub repository, open
**Actions**, select **Build ESP32-S3 installation BIN**, choose **Run workflow**,
and download the `airplay-esp32s3-v3.2.0-bin-files` artifact. It contains the
merged installation image for address `0x0`, the application-only OTA image,
and SHA-256 checksums.
