# FakeGPS — Nixie clock GPS impersonator + motion keeper

An ESPHome external component that impersonates the GPS receiver inside a
Nixie clock. The ESP32-C3 gets accurate time from Home Assistant, synthesizes
valid NMEA-0183 sentences once per second (stamped in local time), and
bit-bangs them out a GPIO to the clock's GPS serial input. It also owns the
clock's motion path: a real PIR (optional) or HA "tickle" events keep the
display awake via strobed pulses, with Auto / Force On / Force Off override.

Full requirements: [fakegps_esphome_spec.md](fakegps_esphome_spec.md).

## Files

| File | What it is |
|------|------------|
| `components/fakegps/` | The external component (NMEA generator, bit-bang TX, emission scheduler, motion logic) |
| `fakegps-device.yaml` | Remote package: everything project-side — component source, fakegps config/pins, time source, OLED, fonts, web server |
| `fakegps-entities.yaml` | Remote package: all the HA switches/selects/numbers/sensors |
| `spectrum18-masterbed.yaml` | Reference device config: identity + secrets + network + the packages block (live copy is in the HAOS builder) |
| `nixie-fakegps-phase1.yaml` | Historical: phase-1 (OLED + time) paste-in blocks |

## Wiring (ESP32-C3 0.42" OLED board)

| Signal | Default pin | Notes |
|--------|-------------|-------|
| NMEA TX | GPIO0 | → clock GPS RX (3.3 V TTL) |
| PIR in | GPIO1 | ← HC-SR501 out (~3.3 V); optional (set to None) |
| Motion strobe | GPIO10 | → clock motion input |
| OLED I²C | GPIO5/6 | substitution defaults `oled_sda`/`oled_scl` |
| Power | 5 V | USB-C or 5V pin; PIR shares the 5 V rail |

The three signal pins are **HA config dropdowns** (immediate effect, survive
reboot) — rewiring never requires a YAML edit. The dropdowns offer only the
safe GPIOs (0/1/3/10); 2/8/9 are strapping/BOOT, 4–7 are JTAG/I²C, 20/21 are
the UART console. The OLED pins are build-time (I²C driver), overridable via
`substitutions:` in the builder config. A board with no OLED fitted logs one
display-init failure at boot and runs normally otherwise.

All signals are fixed active-high (HC-SR501-style PIR, rising-edge strobe).

## Usage

The device YAML in the builder is minimal: identity, secrets, network, and a
`packages:` block pulling `fakegps-device.yaml` + `fakegps-entities.yaml` from
this repo (see `spectrum18-masterbed.yaml`). After a push to `main`, hit
Update/Install in the ESPHome builder — `refresh: 0s` makes it re-fetch every
build.

Runtime tuning from HA (no reflash): per-sentence enables, baud, output type,
`time_offset_ms` (cancels the clock's display lag — see spec FR4), sentence
interval, motion output mode, off-delay, restrobe period, fake
position/satellites. The motion strobe pulse is fixed at 250 ms.

Note: the spec (FR2) says one burst per second; the working default is now a
burst every **5 s** (`sentence_interval`, 1–3600 s in YAML, 1–60 s from the HA
number). Each burst is still aligned to a top-of-second minus the time offset
and stamped with that second.

## Home Assistant entities

Everything the device exposes, including entities from the device YAML (not
just the package). Some of these are of debatable value — documented as-is,
prune/rename as the project settles.

### Controls

| Entity | Type | Category | What it does |
|--------|------|----------|--------------|
| Sentence GPZDA / GPRMC / GPGGA / GPGSA | switch ×4 | config | Enable each sentence type individually |
| GPS Baud | select | config | 4800–115200; takes effect next burst |
| Output Type | select | config | TTL or Pseudo-RS232 (inverted TTL) |
| GPS TX Pin | select | config | GPIO0 (default) /1/3/10 — live remap |
| PIR Input Pin | select | config | None or GPIO0/1 (default)/3/10 — live remap |
| Motion Output Pin | select | config | GPIO0/1/3/10 (default 10) — live remap |
| Motion Output | select | — | Force On / Force Off / Motion (follows PIR + tickles) |
| Time Offset | number (ms, −1000…1000) | — | Emission phase shift; positive = display earlier |
| Sentence Interval | number (s, 1–60) | config | Seconds between NMEA bursts |
| Display Off Delay | number (min, 0.1–1440) | — | ESP-owned display timeout after last motion |
| Restrobe Period | number (s, 1–15) | config | Keep-alive pulse spacing; must be < clock's min motion interval. Pulse width fixed at 250 ms |
| Fake Latitude / Longitude | number ×2 | config | Position reported in RMC/GGA |
| Fake Satellites | number (4–12) | config | Sat count in GGA/GSA |
| Tickle Motion | button | — | Inject a motion event (for HA automations from any sensor) |
| Restart | button | diagnostic | Reboot the ESP |

### Status

| Entity | Type | Category | What it shows |
|--------|------|----------|---------------|
| Time Synced | binary | diagnostic | System clock has real time (gates A/V validity) |
| PIR Motion | binary | — | Raw state of the physical PIR input |
| Display Should Be On | binary | — | Result of mode + countdown (what the ESP wants) |
| Motion Line Driven | binary | diagnostic | Strobe pulse currently active on the output pin |
| Seconds Since Motion | sensor (s) | — | Time since last PIR/tickle event (unknown until first) |
| Motion Countdown | sensor (s) | — | Remaining off-delay time (0 outside Auto) |
| NMEA Sentences Sent | sensor | diagnostic | Total sentences transmitted since boot |
| Uptime | sensor | diagnostic | Since boot |
| FakeGPS WiFi RSSI | sensor (dBm) | — | From device package; also drawn on the OLED |
| Last Motion Source | text | diagnostic | `PIR` / `tickle` / `none` |
| IP Address / SSID / BSSID / MAC Address | text ×4 | diagnostic | WiFi link details |

Transmitted NMEA sentences deliberately have **no HA entity** — per-burst
state churn floods the HA logbook. They're printed to the device log instead:
open the device's web page (its IP) or the builder's log viewer.

### Calibrating the time offset

1. Watch the clock next to a reference (phone NTP clock app).
2. Adjust the **Time Offset** number in HA (positive = display earlier).
3. The stamped time never changes — only the emission instant shifts.

## Status

- **Phase 1 (done):** board online, OLED time + RSSI, web dashboard.
- **Phase 2 (this):** full component — NMEA + bit-bang TX + motion + HA controls.
- **Later:** OLED status glyphs/detail pages (spec FR8), rolling event log
  text sensor, Lovelace dashboard YAML.
