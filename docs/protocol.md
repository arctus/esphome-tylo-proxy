# Protocol notes, and what differs from upstream

`components/sauna360/` is [f-io/esphome-tylo](https://github.com/f-io/esphome-tylo)'s
`sauna360` component (upstream commit `83c10b8`, 2026-08-29), extended. All
frame decoding - temperatures, relays, bath time, error and status codes -
is upstream's. Read its README for the protocol itself; this page covers
only what was added and the bench findings behind it.

## Why a proxy, not a second talker

Measured on a Tylo Pure heater with a PC on the bus:

- **The heater registers exactly one panel.** It searches (`98 7F 00 ..`),
  the panel announces itself with its ID (`7F 01` / `7F 03`), the heater
  assigns it address 0x40 (`40 05`) and ignores any further announce.
- **There is no free slot on the bus.** The panel answers each poll the
  moment its stop bit ends. A command sent in that slot by a second device
  collides with the panel's answer (tried twice, garbled both times).
- **The heater waits for late replies.** Replies held back anywhere from 2 to
  150 ms were accepted; the heater simply polls more slowly. So a
  store-and-forward hop costs nothing but poll rate.
- **A command in place of a keep-alive works.** A PC acting as the only
  panel had a command sent in place of its keep-alive reply accepted within
  40 ms.

Hence the design: the ESP owns both buses, forwards everything, and slips its
own commands into the panel's keep-alive slot.

## Changes against upstream

1. **Real UART port.** Upstream reads the IDF driver on `UART_NUM_0`
   directly, which is only right while its UART is the first one ESPHome
   hands out. The port now comes from `IDFUARTComponent::get_hw_serial_number()`.
   Needed as soon as there are two UARTs; harmless with one.
2. **RS485 proxy mode**, enabled by `panel_uart_id` (plus optional
   `panel_reply_timeout`, 5-150 ms, default 60 ms). The heater is on
   `uart_id` (bus A), the panel on `panel_uart_id` (bus B), and one task on
   core 1 replaces upstream's receive task:
   - every frame is forwarded whole in both directions, registration included;
   - a heater poll (`06`) or data frame (`08`) opens a reply window; the
     panel's `07`/`09` closes it and is forwarded; one that arrives after the
     window is dropped and counted as late;
   - if the window times out the ESP answers the heater itself: a keep-alive
     for a plain poll, the captured panel reply for a configuration poll
     (`18xx`/`1Axx`/`38xx`), a `09` echo for a data frame;
   - bus mode (`set_bus_mode()`): **Listen** never transmits and decodes bus
     A as upstream would; **Forward** proxies; **Forward + Control** also
     sends a queued command in place of the panel's keep-alive reply. Outside
     Control the transmit queue is drained and discarded;
   - counters for the diagnostics: frame rates, reply latency, missed and
     late replies, panel commands, commands sent and dropped.

   Without `panel_uart_id` the component behaves as upstream (apart from 1).
   In proxy mode upstream's boot-time defaults (bath time, bath and maximum
   temperature) are **not** sent: they re-programmed the heater on every
   reboot, and the usage policy owns those settings instead.
3. **Usage policy** (`sauna_policy.cpp`), proxy mode only, acting only in
   Forward + Control - see [behaviour.md](behaviour.md):
   - *Sauna Allowed* off: a heater start from the panel (`07 7000`, bit 0,
     while `0x3400` bit 4 says off) is forwarded with bit 0 cleared, so the
     heater clears its command register and the panel stops resending. Simply
     swallowing it does not work: the panel then retries indefinitely, and
     its first retry after the sauna is allowed again starts the heater. A
     start within 3 s of a refused one is refused too. A heater on anyway is
     toggled off after 10 s.
   - *Temperature Limit*: written into `0x4002`'s maximum bath temperature
     bits, and a `0x6000` target above it is set back.
   - *Session Time Limit*: counts from "within 10 C of target", or after 60
     min of warm-up, then toggles the heater off; the heater's own bath time
     is held at 60 min + the limit as a backstop if the ESP dies.
   - *Restart Cooldown* after a limit stop, an optional two-blink light
     warning 5 min before one, and *Opening Hours* with *Relax Today*.

   `0x4002` is written with the proper bath-time encoding
   (`write_bath_settings_()`); upstream's `set_max_bath_temperature_number()`
   puts raw minutes into the low bits, which is off by 4-32 min above 63 min.
4. **Session statistics** (`sauna_stats.cpp`): sessions of a minute or more
   from `0x3400` bit 4 (in any bus mode), with count, limit stops, total
   hours, last / average / median, and the last 64 sessions with start time
   and how they ended. Saved to NVS once per completed session.
5. **Clock and history page** (`sauna_clock.cpp`): keeps the heater's
   `0x4200` clock, writes it from a phone (`07 4200`, date and time fields
   replaced, other bits preserved), and serves `/sauna` (clock, *Sync clock
   from this phone*, full session list) and
   `POST /sauna/clock?value=YYYY-MM-DD HH:MM:SS`.
6. **`sauna_wifi`** (a separate component): the Wi-Fi fallback described in
   [behaviour.md](behaviour.md#7-network).

## Findings worth knowing

- **Relay frames on the Pure.** Upstream documents the Pure's `0x7180` relay
  bits as high (light bit 17, coils 16..14) and Combi/Elite as low (light
  bit 5, coils 2..0). Every `0x7180` frame seen on the Pures this was tested
  on lives in the low byte (`0x20` light on, `0x00` off, `0xA7` heating and
  light). That is why the package defaults to `model: elite`. If the Light
  switch reads backwards or never changes on your heater, try `pure`.
- **The heater's command `0x7000` bit 0 is a toggle,** not "on". A stop is
  only sent while the heater reports on, and at most every 10 s, until it
  confirms off.
- **The basic panel has no clock**, so the heater's `0x4200` clock is
  usually unset (it read 2001). Set it from `/sauna`; it does not follow
  daylight saving.
- **At `log_level: INFO` undecoded frames are silent.** Set DEBUG before
  concluding the bus is quiet.
