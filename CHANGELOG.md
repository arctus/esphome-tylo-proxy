# Changelog

## 1.1.0 - 2026-11-19

- **Builds on ESPHome 2026.11 again.** 2026.11 removed `format_hex_pretty()`,
  which the component's frame logging used; 1.0.0 no longer compiles there.
  The log now formats the bytes itself (same output, builds on old and new
  ESPHome).
- **Clock from the internet.** `proxy.yaml` adds an SNTP clock (`sntp_time`):
  with internet it sets the heater's clock when it is unset or 2 min or more
  off, every 15 min, so daylight saving is followed and nobody has to press
  the phone sync. Set your time zone on `sntp_time` (see the example).
- **Last-known heater settings after a restart.** The heater reports a value
  only when it changes, so after an OTA update the target, max temperature,
  PCB limit, Temperature Level and Bath Time read unknown until the next use.
  They are now kept in flash and shown after a restart until the heater
  reports - for display only, never written to the heater. The current
  temperature still waits for the heater.
- **Optional web UI login:** `packages/web-auth.yaml` (digest auth, user
  `sauna` by default, password from `web_password`). Existing configs are
  unaffected unless they include it.

## 1.0.0 - 2026-11-14

First public release.

- `sauna360` component from f-io/esphome-tylo (`83c10b8`), extended with an
  RS485 proxy between heater and panel (Listen / Forward / Forward + Control),
  usage rules (Sauna Allowed, Temperature Limit, Session Time Limit, Restart
  Cooldown, Blink Light Before Stop, Opening Hours, Relax Today), session
  statistics with the last 64 sessions, and a `/sauna` page with the full
  history and a phone-to-heater clock sync.
- `sauna_wifi` component: join Wi-Fi when available, run the device's own
  access point when not, and pause the network search while the access point
  is up so its clients are not disturbed.
- Packages: `proxy.yaml` (everything above, as web UI sections), and one of
  `network-fallback.yaml` / `network-ap-only.yaml`.
- OTA is encrypted with the API key; no OTA password.
