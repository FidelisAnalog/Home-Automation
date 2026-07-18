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
| `fakegps-entities.yaml` | Remote package: all the HA switches/selects/numbers/sensors |
| `spectrum18-masterbed.yaml` | Reference device config (secrets redacted — live copy is in the HAOS builder) |
| `nixie-fakegps-phase1.yaml` | Historical: phase-1 (OLED + time) paste-in blocks |

## Wiring (ESP32-C3 0.42" OLED board)

| Signal | Pin | Notes |
|--------|-----|-------|
| NMEA TX | GPIO0 | → clock GPS RX (3.3 V TTL) |
| PIR in | GPIO1 | ← HC-SR501 out (~3.3 V); optional |
| Motion strobe | GPIO10 | → clock motion input |
| OLED I²C | GPIO5/6 | onboard, fixed |
| Power | 5 V | USB-C or 5V pin; PIR shares the 5 V rail |

Active levels: set `inverted: true` on `pir_in_pin` / `motion_out_pin` in the
device YAML if the PIR or clock uses active-low.

## Usage

The device YAML pulls both the component and the entities package from this
repo over GitHub (see `spectrum18-masterbed.yaml`, sections
`external_components:` and `packages:`). After a push to `main`, hit
Update/Install in the ESPHome builder — `refresh: 0s` makes it re-fetch every
build.

Runtime tuning from HA (no reflash): stream + per-sentence enables, baud,
polarity, `time_offset_ms` (cancels the clock's display lag — see spec FR4),
motion mode, off-delay, strobe timing, fake position/satellites.

### Calibrating the time offset

1. Watch the clock next to a reference (phone NTP clock app).
2. Adjust the **Time Offset** number in HA (positive = display earlier).
3. The stamped time never changes — only the emission instant shifts.

## Status

- **Phase 1 (done):** board online, OLED time + RSSI, web dashboard.
- **Phase 2 (this):** full component — NMEA + bit-bang TX + motion + HA controls.
- **Later:** OLED status glyphs/detail pages (spec FR8), rolling event log
  text sensor, Lovelace dashboard YAML.
