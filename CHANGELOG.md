# Changelog

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
