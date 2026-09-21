# AirPlay and DLNA Receiver 3.3.0

## Reliability and integration

- Recovery/Safe Mode after three consecutive watchdog or panic resets.
- Additive NVS configuration migration with an exposed schema version.
- Wi-Fi diagnostics including retry count, disconnect reason, RSSI, channel,
  BSSID, address and setup-AP state.
- Transactional Wi-Fi credential tests: new credentials are staged, committed
  only after DHCP succeeds, and rolled back after five failures.
- Runtime MQTT broker, authentication and topic settings with Home Assistant
  discovery and playback commands.
- Idle-only left, right and stereo speaker tests plus persistent balance.
- GitHub Release update checks and matching-target OTA installation.
- OTA product validation and existing bootloader rollback protection.
- New `/reliability` control page and JSON APIs.
- Build fix: increase the Home Assistant MQTT discovery buffer so maximum
  device/topic names compile safely with `-Werror=format-truncation`.
- New Apple-inspired Liquid Glass interface across the main panel, advanced
  controls, reliability, equalizer, logs and speed-test pages. Includes matched
  light/dark modes, mobile layouts and reduced-motion accessibility support.
- Liquid Glass revision 3: improve secondary/disabled button contrast in Light
  and Auto-Light modes and replace all checkboxes with accessible iPhone-style
  on/off switches.

## Compatibility

The AirPlay, DLNA, audio-buffer and timing paths remain unchanged. Safe Mode
deliberately skips those services only after repeated crash-class resets or
when explicitly forced from the control panel.
