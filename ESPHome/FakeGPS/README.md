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
| `fakegps-device.yaml` | Remote package: everything project-side — component source, fakegps config, time source, OLED, fonts, web server |
| `fakegps-entities.yaml` | Remote package: all the HA switches/selects/numbers/sensors |

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

## Fresh install (new board, no ESPHome yet)

1. **Flash generic ESPHome from the browser.** Plug the board in over USB-C
   and go to [web.esphome.io](https://web.esphome.io) (Chrome/Edge). Click
   **Connect**, pick the board's serial port, then **Prepare for first use**.
   When it finishes, use the same dialog to join the board to your WiFi.
2. **Adopt it.** Open the ESPHome Builder in Home Assistant — the device shows
   up as discovered (`esphome-web-xxxxxx`). Click **Adopt**, give it a name.
   The builder writes a base config with your own keys and WiFi secrets.
3. **Add FakeGPS.** Edit the adopted config, paste this at the bottom, and
   hit **Install**:

   ```yaml
   packages:
     fakegps:
       url: https://github.com/FidelisAnalog/Home-Automation
       ref: main
       files:
         - ESPHome/FakeGPS/fakegps-device.yaml
         - ESPHome/FakeGPS/fakegps-entities.yaml
       refresh: 0s
   ```

   If the adopted config's `esp32:` section shows `framework: type: arduino`,
   change it to `esp-idf` — that's what this project is built and tested on.

Then wire per the table above. Signal pins are HA dropdowns after boot, so
wiring differences need no YAML changes.

## Other boards (classic ESP32 / WROOM-32)

The packages default to the ESP32-C3 0.42" OLED board. Any classic ESP32
(ESP-WROOM-32) board — e.g. the ELEGOO ESP-32 DevKit (EL-SM-012) or a
D1 Mini ESP32, both USB-C — works with the same firmware, but the safe
GPIOs differ per chip, so the builder config must override the pin
substitutions. **This is not optional**: the C3 values land on the WROOM-32's
serial console and flash pins.

Builder config for a WROOM-32 board — `esp32:` section uses
`variant: esp32` (the wizard/adoption sets this from the detected chip),
plus:

```yaml
substitutions:
  # Safe, fully usable GPIOs on WROOM-32 dev boards
  fakegps_pin_a: GPIO16
  fakegps_pin_b: GPIO17
  fakegps_pin_c: GPIO18
  fakegps_pin_d: GPIO19
  fakegps_tx_pin_default: GPIO16
  fakegps_pir_pin_default: GPIO17
  fakegps_motion_pin_default: GPIO18
  # No OLED fitted, but the I2C bus still initializes — it must not sit on
  # the C3 defaults (GPIO6 is a flash pin on WROOM-32; the board won't boot)
  oled_sda: GPIO21
  oled_scl: GPIO22
```

Default wiring on these boards: GPIO16 → clock GPS RX, GPIO17 ← PIR,
GPIO18 → clock motion input. The missing OLED logs one init failure at
boot and is otherwise ignored.

Most WROOM-32 dev boards (ELEGOO included) have a spare blue LED on GPIO2.
To use it as a health indicator — off when everything is fine, slow blink
when WiFi/HA is down, fast blink on error — add to the builder config:

```yaml
status_led:
  pin: GPIO2
```

## Standalone (no Home Assistant)

The component doesn't care where time comes from — swap Home Assistant time
for NTP and the device is fully self-sufficient. In your device config
(after the fresh-install steps):

1. **Time from NTP instead of HA** — the package's HA time source is replaced
   with SNTP under the same id. The timezone must be stated explicitly (HA
   normally supplies it); DST rules are baked in at build, so changeovers
   still handle themselves:

   ```yaml
   time:
     - id: !remove ha_time
     - platform: sntp
       id: ha_time
       timezone: America/New_York   # your IANA timezone
   ```

2. **Stop the no-HA reboot** — ESPHome reboots after 15 min without an HA
   connection unless told otherwise:

   ```yaml
   api:
     reboot_timeout: 0s
   ```

   (Or delete the `api:` block entirely if HA will never be involved.)

Control and calibration happen on the device's own web page (its IP, port
80) — every switch, select, and number from the entity table is there, and
set values persist across reboots. The network must offer NTP (internet
access or a local NTP server). What you lose without HA: external-sensor
tickles via automations (use the HTTP API below instead) and long-term
history.

## HTTP API (no HA required)

The on-device web server is also a REST API — anything that can make an
HTTP request can drive the device. Entity URLs use the entity name,
lowercased with underscores:

```sh
# Inject a motion event (what an HA automation would do)
curl -X POST http://<device-ip>/button/tickle_motion/press

# Force the display on / off / back to motion control
curl -X POST "http://<device-ip>/select/motion_output/set?option=Force%20On"
curl -X POST "http://<device-ip>/select/motion_output/set?option=Motion"

# Tune numbers, e.g. display off-delay to 10 min
curl -X POST "http://<device-ip>/number/display_off_delay/set?value=10"

# Read state (JSON)
curl http://<device-ip>/binary_sensor/time_synced
```

## Usage

The device YAML in the builder is minimal: identity, secrets, network, and a
`packages:` block pulling `fakegps-device.yaml` + `fakegps-entities.yaml` from
this repo (the block shown in Fresh install above). After a push to `main`,
hit Update/Install in the ESPHome builder — `refresh: 0s` makes it re-fetch
every build.

Runtime tuning from HA (no reflash): per-sentence enables, baud, output type,
`time_offset_ms` (cancels the clock's display lag — see spec FR4), sentence
interval, motion output mode, off-delay, restrobe period, fake
position/satellites. The motion strobe pulse is fixed at 1 s.

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
| Sentence GPZDA / GPRMC / GPGGA / GPGSA | switch ×4 | config | Enable each sentence type individually (only GPRMC on by default) |
| GPS Baud | select | config | 4800–115200; takes effect next burst |
| Output Type | select | config | TTL or Pseudo-RS232 (inverted TTL) |
| GPS TX Pin | select | config | GPIO0 (default) /1/3/10 — live remap |
| PIR Input Pin | select | config | None or GPIO0/1 (default)/3/10 — live remap |
| Motion Output Pin | select | config | GPIO0/1/3/10 (default 10) — live remap |
| Motion Output | select | — | Force On / Force Off / Motion (follows PIR + tickles) |
| Time Offset | number (ms, −1000…1000) | — | Emission phase shift; positive = display earlier |
| Sentence Interval | number (s, 1–60) | config | Seconds between NMEA bursts |
| Display Off Delay | number (min, 0.1–1440) | — | ESP-owned display timeout after last motion (default 15 min) |
| Restrobe Period | number (s, 1–15) | config | Keep-alive pulse spacing; must be < clock's min motion interval. Pulse width fixed at 1 s; default 1 s = line held high while display-on |
| Fake Latitude / Longitude | number ×2 | config | Position reported in RMC/GGA |
| Fake Satellites | number (4–12) | config | Sat count in GGA/GSA |
| Tickle Motion | button | — | Inject a motion event (for HA automations from any sensor) |
| Restart | button | diagnostic | Reboot the ESP |

### Status

| Entity | Type | Category | What it shows |
|--------|------|----------|---------------|
| Time Synced | binary | diagnostic | System clock has real time; the TX line is completely silent until then |
| Display Should Be On | binary | — | Result of mode + countdown (what the ESP wants) |
| Seconds Since Motion | sensor (s) | — | Time since last PIR/tickle event (unknown until first) |
| Motion Countdown | sensor (s) | — | Remaining off-delay time (0 outside Auto) |
| NMEA Sentences Sent | sensor | diagnostic | Total sentences transmitted since boot |
| Uptime | sensor | diagnostic | Since boot |
| FakeGPS WiFi RSSI | sensor (dBm) | — | From device package; also drawn on the OLED |
| Last Motion Source | text | diagnostic | `PIR` / `tickle` / `none` |
| Last Reboot Reason | text | diagnostic | Power-on / software restart / panic / brownout / watchdog |
| IP Address / SSID / BSSID / MAC Address | text ×4 | diagnostic | WiFi link details |

Transmitted NMEA sentences, raw PIR state, and strobe pulses deliberately have
**no HA entity** — their churn would flood the HA logbook, and the PIR is an
internal input, not an HA motion sensor. All three appear in the device log
instead: open the device's web page (its IP) or the builder's log viewer.

### Calibrating the time offset

1. Watch the clock next to a reference (phone NTP clock app).
2. Adjust the **Time Offset** number in HA (positive = display earlier).
3. The stamped time never changes — only the emission instant shifts.

## Status

- **Phase 1 (done):** board online, OLED time + RSSI, web dashboard.
- **Phase 2 (done):** full component — NMEA + bit-bang TX + motion + HA controls.
- **Phase 3 (this):** OLED pages — Home (time, WiFi bars, TX blink),
  Motion (PIR/strobe/mode), Network (BSSID/channel/RSSI). BOOT button
  cycles pages (no auto-rotate); screen blanks after 5 min, BOOT wakes.
- **Remaining:** bench bring-up with the clock; offset calibration.
