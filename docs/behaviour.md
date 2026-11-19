# How it works

Wiring is in [wiring.md](wiring.md), the protocol findings in
[protocol.md](protocol.md). The code is in `components/sauna360/`
(`sauna_policy.cpp`, `sauna_stats.cpp`, `sauna_clock.cpp`, the proxy in
`sauna360.cpp`) and `components/sauna_wifi/`; every entity is declared in
`packages/proxy.yaml`.

## 1. The ESP sits between heater and panel

```
heater ==bus A== [ESP32-S3] ==bus B== panel
```

The heater talks to exactly one registered panel, polls it about 30 times a
second, and waits for each answer. The ESP forwards every frame whole in both
directions, so the panel keeps working exactly as before - including its own
registration with the heater.

The ESP only steps in in two places:

- **The panel is late.** If the panel has not answered a heater poll within
  60 ms, the ESP answers for it (keep-alive, the recorded configuration reply,
  or an acknowledgement), so the heater never notices. A panel answer that
  then turns up late is dropped, so the heater never gets two.
- **The ESP has a command to send** (light, heater, temperature, bath time,
  clock). It goes to the heater *in place of* the panel's next "nothing to
  report" reply - the slot the panel itself uses for button presses.

**Bus Mode** (Configuration) decides how much of this happens:

| Mode | Panel wired to | ESP sends anything? |
|---|---|---|
| Listen | the heater directly | never - only reads |
| Forward | the ESP | only stand-in answers for a late panel |
| Forward + Control (default) | the ESP | also its own commands |

Commands made while not in Forward + Control are thrown away, never saved for
later. **All rules below need Forward + Control.**

## 2. The rules

### Sauna Allowed (default on)

- Off: pressing the heater button on the panel does nothing. The ESP passes
  the command on with the heater part removed, so the heater answers it as a
  "no command" and the panel stops repeating the press. The light button still
  works, and switching the heater *off* is always allowed.
- A heater start that arrives within 3 s of a refused one counts as the panel
  repeating the same press and is refused too, even if the sauna has just been
  allowed again - so an old press can never start the heater later. A new
  press works as soon as the panel has stopped repeating.
- If the heater is on anyway (switched off while running, or started through
  the heater's external-switch input, which the ESP cannot see), the ESP
  switches it off after **10 s**.

### Temperature Limit (default on) and Max Allowed Temperature (default level 9, 102 C)

- The limit is written into the heater's **own** maximum temperature, so the
  heater enforces it even without the ESP.
- If the target is raised above the limit (on the panel or here), the ESP sets
  it back to the limit within about 15 s.
- Switching the feature off gives the heater its maximum of 110 C back, once.

### Session Time Limit (default 60 min; Off, 5 for testing, 30 ... 240)

A **session** is the heater being on, from switch-on to switch-off.

```
heater on --> Warming up --> Counting --> (limit reached) --> Stopping --> heater off
               |               ^
               | room within   |
               | 10 C of target|
               +---------------+
               or 60 min of warm-up, whichever comes first
```

- **Warming up is free**, but for at most 60 minutes; counting starts when the
  room is within 10 C of the target, or after 60 minutes, whichever is first.
  So the longest possible session is 60 min + the limit.
- Once counting has started it keeps counting, even if the room cools while
  the door is open.
- At the limit the ESP switches the heater off. The heater command is a
  toggle, so it is only sent while the heater reports on, at most every 10 s,
  until the heater confirms it is off.
- **Starting again begins a new session** with a fresh warm-up and limit -
  unless a Restart Cooldown is set.
- **Backstop:** the heater's own bath time is held at **60 min + the limit**
  (for 60 min: 120 min). If the ESP dies mid-session, the heater still
  switches itself off then. With the limit Off, the heater's bath time is left
  alone and can be chosen under Heater > Bath Time.

### Opening Hours (default on, 07:00 to 22:00)

- Outside the window a start is refused exactly like *Sauna Allowed* off, and
  a heater that comes on anyway is switched off after 10 s.
- **A session that began while the sauna was open runs to its end**, including
  its warm-up allowance and time limit. Closing time shuts the door to new
  users; it does not throw anyone out.
- **Only enforced while the clock is set** (see section 5). With no clock the
  rule does nothing at all and says so in the log and in *Opening Hours*.
- *Opening Time* and *Closing Time* are time fields, to the minute. A window
  that crosses midnight (e.g. 20:00 to 02:00) works; open equal to close means
  all day.

### Relax Today (default off)

Skips the opening hours until midnight, then clears itself and the switch goes
back to off. It survives a reboot on the same day, so restarting the ESP in
the middle of a relaxed evening does not close the sauna again. With no clock
set it stays on until switched off or the device reboots.

### Restart Cooldown (default 5 min; Off, 5, 10, 15, 30, 60 min)

After a session was **stopped by the time limit**, heater starts are refused
(like Sauna Allowed off) for that long. Not after a session somebody ended
themselves.

### Blink Light Before Stop (default on)

5 minutes before a limit stop the light blinks twice (1.5 s steps) and ends
as it was. With the 5-minute test limit it blinks as soon as counting starts.

### Settings the rules hold

While a rule is active it overrides the matching heater setting, and puts it
back if someone changes it on the panel:

| Heater setting | Held at | By |
|---|---|---|
| Maximum temperature | Max Allowed Temperature | Temperature Limit |
| Target temperature | at most the limit | Temperature Limit |
| Bath time | 60 min + Session Time Limit | Session Time Limit |

The check needs the heater's own bath-time / max-temperature frame (`0x4002`),
which the heater sends only when one of them changes - so after a restart of
the ESP alone (an OTA update) it never comes by itself. Since 1.1.1, if it
has not arrived a minute after boot, the ESP writes the values the rules
prescribe (bath time 60 min + limit, maximum = the limit); the heater answers
with its own frame and the check, the display and the last-known settings
take it from there. With a rule off, the matching value is only written if it
is known from flash; otherwise nothing is written.

*Session State* reads **Closed now** while the opening hours block a start,
and *Opening Hours* shows `Open until 22:00`, `Closed until 07:00`, `Relaxed
for today ...`, `Clock not set - not enforced` or `Not enforced`.

A setting the heater keeps refusing is retried every 15 s three times, then
only every 5 min, so a stubborn heater is not flooded with writes.

## 3. Temperature levels

The basic panel has levels, not degrees, and both temperature drop-downs
offer the same ten:

| Level | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|---|---|
| C | 40 | 48 | 56 | 63 | 69 | 79 | 87 | 94 | 102 | 110 |

From the Pure manual's own table for the 110 C program. **Heater >
Temperature Level** shows the level the heater is on (blank if its target is
between levels) and sets it; a level above the limit is lowered to the limit.

## 4. Statistics and history

- Every heater session of **1 minute or more** is counted, whoever started or
  ended it and in any bus mode. Shorter ones - a refused start that ran for
  10 s - are ignored.
- Kept: number of sessions, how many the time limit stopped, total heating
  time, and the **last 64 sessions** with start time, length and how they
  ended (turned off / time limit / not allowed). Average and median are over
  those 64; the median ignores the one session somebody forgot to end.
- Saved to flash **once per finished session**, so they survive reboots and
  power cuts. A session running during a reboot is lost.
- **Reset Statistics** clears them but never touches a running session or its
  limit, so it cannot be used to buy more time. **Factory Reset** also erases
  every setting.
- The full list is at **`/sauna`** on the device's address:
  http://192.168.4.1/sauna on its own access point, its DHCP address on a
  network (shown under Diagnostics -> Network).

## 5. Clock

The ESP keeps no clock of its own (a sauna often has no internet). The
heater has one, which it broadcasts about once a minute and keeps across
ESP reboots - but on the heaters this was tested on it had never been set
(it read 2001), because the basic panel has no clock.

- **Internet time (1.1.0):** when the network has internet, the ESP gets the
  time over NTP every 15 min. When the sauna's clock is unset or 2 min or
  more off, it is written into the heater exactly like the phone sync below,
  so the clock sets itself after a reboot and follows daylight saving within
  15 min. It is local time, in the zone set on `sntp_time`.
- On **/sauna**, *Sync clock from this phone* sends the phone's local time;
  the ESP writes it into the heater's clock and stamps sessions from it.
- *Heater Clock* shows where the time comes from: **(heater)** means the
  heater took it; **(phone, until reboot)** means it did not, and the ESP is
  using the phone's time until it restarts.
- The heater clock does not follow daylight saving by itself: without
  internet, sync again in spring and autumn. Sessions from before the first sync show "clock not set".

## 6. When things go wrong

| Situation | What happens |
|---|---|
| ESP reboots or loses power | Panel shows a connection error and re-registers within ~30 s of the ESP coming back. Whether the heater keeps heating without a panel is **not tested**; at worst it runs until its own bath time (60 min + limit). Settings and statistics are kept; the running session's timer restarts. The heater only reports a value when it changes, so after a restart of the ESP alone (an OTA update) the current temperature stays unknown until it moves; target, max temperature, bath time and temperature level show their last-known values from flash (1.1.0) until the heater reports. |
| ESP dead for good | Move the panel's A/B wires onto the heater (see [wiring.md](wiring.md)) and set Bus Mode to Listen once it is back. |
| Panel unplugged | The ESP answers the heater's polls in its place, so a running heater should carry on and the rules still apply (designed, **not tested** on the real heater). |
| Heater's own safety (door, overheat) | Untouched - the ESP never interferes with it. Its messages appear as *Heater Message*. |

## 7. Network

Two network packages, pick one:

- **`network-fallback.yaml`** joins your Wi-Fi when it is there, and Home
  Assistant through the native API when HA is up - neither is required.
  Without the network the device starts its own access point after a minute
  (ESPHome's `ap_timeout`). While that access point is up it looks for the
  network for `search_time` (2 min), then **pauses the search**, and tries
  again every `retry_interval` (15 min) - waiting while a phone is joined to
  the access point, but never longer than `max_defer` (60 min).
- **`network-ap-only.yaml`** runs the access point only, for a sauna with no
  Wi-Fi at all.

Why the pause: on an ESP32, ESPHome keeps scanning for the network the whole
time its fallback access point is up, and every scan takes the radio off the
access point's channel - which is what a phone on the sauna page notices.
ESPHome only slows the scans down while a captive portal runs, and a captive
portal would take over `/`, the sauna's own page. `sauna_wifi` instead takes
the network out of ESPHome's list while paused (the access point keeps
running untouched) and puts it back for each retry, the same way improv
hands over new credentials.

*Diagnostics -> Network* shows `MyNetwork, 192.168.1.50`, `Own access point
(looking for network)` or `Own access point (network not found, retrying
later)`. *Full History and Clock Sync* always links to the address in use.

`api: reboot_timeout: 0s` and `wifi: reboot_timeout: 0s` are set, so neither
a missing Home Assistant nor a missing network ever reboots the device.
