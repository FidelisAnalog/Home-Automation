# FakeGPS — ESPHome Module Specification

**Version:** 0.7 (draft)
**Date:** 2026-07-17
**Author:** JP
**Target:** ESPHome external component + YAML package

---

## 1. Overview

FakeGPS is an ESPHome module that impersonates the GPS receiver inside a Nixie
clock. An ESP32 obtains accurate time from Home Assistant, synthesizes valid
NMEA-0183 sentences once per second (stamped in **local time**), and clocks them
out of a GPIO to the clock's GPS serial input. The clock believes it has
satellite lock and displays correct time.

The same module also sits in the clock's motion path. It reads a real PIR
sensor and **strobes** the clock's motion input to keep the display awake. The
clock is configured to its shortest motion interval, so the ESP owns the real
off-delay. Home Automation (Home Assistant) can override motion to force the
display on or off, and can tune the display timeout live.

## 2. Scope and non-goals

**In scope**

- NMEA time stream: GPZDA, GPRMC, GPGGA, GPGSA — each independently selectable.
- Software-generated (bit-banged) serial TX: selectable baud, selectable
  polarity (TTL / inverted "pseudo-RS232"), selectable framing.
- Signed millisecond emission offset to cancel the clock's fixed display lag.
- PIR passthrough with strobe-based keep-alive and Auto / Force-On / Force-Off
  override; ESP owns the off-delay timer.
- HA-injected "tickle" motion events so any external sensor can reset the
  interval — the ESP is the single motion aggregator.
- Diagnostics + dashboard: on-device web dashboard and HA entities/log showing
  last GPS sent, WiFi channel/BSSID/RSSI, and module state.
- Home Assistant control surface for all of the above.

**Non-goals**

- No RS232 voltage levels. Output is **3.3 V TTL only**; polarity inversion is
  logical, done in software, at TTL levels. No MAX3232 / line driver / level
  translation.
- No GPS input parsing (module is a source, not a receiver).
- No satellite ephemeris realism beyond what the clock needs to accept a fix
  (healthy fix quality, plausible sat count, low DOP).

## 3. Architecture

```
                 +---------------------------- ESP32 -----------------------------+
   HA (WiFi) --> | time source (Home Assistant time; SNTP fallback)               |
                 |        |                                                        |
                 |        v                                                        |
                 | NMEA generator (GPZDA/GPRMC/GPGGA/GPGSA + checksum)             |
                 |        |                                                        |
                 |        v                                                        |
                 | emission scheduler (top-of-second minus time_offset_ms)        |
                 |        |                                                        |
                 |        v                                                        |
                 | bit-bang TX (baud, invert, framing) ---- GPIO --> Clock GPS RX  |
                 |                                                                 |
   Real PIR ---> | PIR input (gpio) --> motion timer (off_delay) --> strobe out ---|--> Clock motion in
                 |                          ^                                      |
   Home Asst <-->| control surface: selects/numbers/switches/sensors  (mode)      |
                 +-----------------------------------------------------------------+
```

## 4. Hardware

**Platform:** ESP32-C3. Target board **01Space ESP32-C3 0.42″ OLED
(ceramic-antenna variant)** — ESP32-C3FH4, 4 MB flash, onboard 72×40 SSD1306
OLED. WiFi for HA time. The software bit-bang needs no UART peripheral, so the
C3's single core is fine at these baud rates; a classic dual-core ESP32 also
works and buys timing headroom only if a very high baud is ever required.

**Signal levels:** all I/O is 3.3 V TTL. No 5 V level translation, no RS232
voltage.

**Power:** the board is fed **5 V** (USB-C or the 5V pin); its onboard LDO makes
3.3 V for the ESP. Supply and signal voltages are independent — GPIO stays
3.3 V TTL. The 5 V rail also runs an HC-SR501-class PIR (needs ~5 V; its output
high is ~3.3 V, safe for the C3 input) and can often be taken from the clock.

**GPIO (C3 defaults — all configurable):**

| Signal             | Direction | Default pin | Notes                                 |
|--------------------|-----------|-------------|---------------------------------------|
| NMEA TX            | out       | GPIO0       | to clock GPS serial RX (3.3 V TTL)    |
| Real PIR in        | in        | GPIO1       | from HC-SR501 output (~3.3 V); optional |
| Motion strobe out  | out       | GPIO10      | to clock motion input                 |
| OLED I²C (SDA/SCL) | I/O       | GPIO5 / 6   | onboard 0.42″ SSD1306 (fixed)         |
| BOOT button        | in        | GPIO9       | OLED page-cycle / wake                 |

Avoid GPIO2/8 (strapping) and GPIO20/21 (UART console) and GPIO4–7 (JTAG) for
signals. GPIO9 is the BOOT button, reused as the OLED page/wake input. GPIO3 is
spare.

## 5. Functional requirements

### FR1 — Time source
- Primary: **Home Assistant time** (ESPHome `homeassistant` time platform). The
  device already talks to HA, and this works even when the ESP can't reach the
  public internet.
- Fallback: SNTP over WiFi (optional).
- **No RTC.** The ESP32's free-running clock holds time acceptably across brief
  HA outages and re-syncs on reconnect/boot.
- **Time basis:** this clock does **not** apply a timezone offset, so the module
  stamps **local time** directly — it owns the timezone and DST via `timezone`.
  NMEA local-zone fields are set to `00,00` since the stamped time is already
  local. A `time_basis: utc` option is retained for clocks that *do* apply their
  own offset.
- **Valid/void gate:** until time is synced, RMC status = `V`, GGA fix = `0`,
  GSA fix = `1` (no fix). After sync: RMC `A`, GGA `1`, GSA `3`. This prevents
  the clock latching a bogus time at boot.

### FR2 — NMEA generation
- Emit enabled sentences once per second, aligned to the offset-adjusted second
  boundary (see FR4).
- Each sentence: leading `$`, comma-delimited fields, `*`, two-digit uppercase
  XOR checksum, terminated `\r\n`.
- Checksum = XOR of every character between `$` and `*` (exclusive).
- Position/fix fields use configurable but static "healthy" values so the clock
  accepts lock.

Sentence templates (local time; `CS` = checksum):

| Sentence | Template |
|----------|----------|
| GPZDA | `$GPZDA,hhmmss.ss,dd,mm,yyyy,00,00*CS` |
| GPRMC | `$GPRMC,hhmmss.ss,<A|V>,ddmm.mmm,<N|S>,dddmm.mmm,<E|W>,000.0,000.0,ddmmyy,,,<A|N>*CS` |
| GPGGA | `$GPGGA,hhmmss.ss,ddmm.mmm,<N|S>,dddmm.mmm,<E|W>,<0|1>,nn,h.h,alt,M,geoid,M,,*CS` |
| GPGSA | `$GPGSA,A,<1|3>,01,02,03,04,05,06,07,08,,,,,p.p,h.h,v.v*CS` |

Worked example (synced, 17:28:09 local, 17 Jul 2026, default position):

```
$GPZDA,172809.00,17,07,2026,00,00*5A
$GPRMC,172809.00,A,4014.000,N,08251.000,W,000.0,000.0,170726,,,A*XX
$GPGGA,172809.00,4014.000,N,08251.000,W,1,10,0.9,100.0,M,-34.0,M,,*XX
$GPGSA,A,3,01,02,03,04,05,06,07,08,,,,,1.8,0.9,1.5*XX
```

(Checksums computed at runtime; `XX` illustrative.)

### FR3 — Serial output (software bit-bang)
- No hardware UART. TX waveform generated on the GPIO in software so baud,
  polarity, and framing are runtime parameters.
- **Idle level** = `invert ? 0 : 1`. **Start bit** = opposite of idle.
  **Data** = 8 bits LSB-first, each `invert ? !bit : bit`. **Stop** = idle,
  1 or 2 bits. Parity: none (default), even, or odd.
- **Bit period** = 1,000,000 / baud (µs).
- Timing: transmit each byte inside a short critical section to bound jitter;
  yield between sentences to keep WiFi serviced. At the low burst rate
  (~2,800 bits ≈ 290 ms/s at 9600) this is comfortable.
- **Polarity:** `invert=false` → TTL (non-inverted, idle high). `invert=true` →
  pseudo-RS232 (inverted TTL, idle low). Same code path.

### FR4 — Millisecond emission offset
- Purpose: cancel the clock's fixed pipeline lag (transmit + parse + display
  update) so the displayed second changes at true time.
- Mechanism: stamp the sentence for second `T` but **start emitting** at
  `T − time_offset_ms`. Positive offset advances (pulls display earlier);
  negative retards. This is a **phase shift on emission**, not a change to the
  stamped value — the clock syncs to arrival, not to fractional-seconds fields.
- Resolution: derived from the microsecond clock; ms is the working unit,
  sub-ms achievable. Signed, default 0.
- Calibration: single knob absorbs all fixed latency; tune against a reference
  until the clock matches. Optional fractional-seconds stamping (`hhmmss.sss`)
  is available but secondary (most clocks truncate).

### FR5 — Motion (strobe + override)
- Clock is set to its **shortest** motion interval; the ESP owns the real
  off-delay.
- **Motion event** — a real PIR rising edge **or** an HA-injected *tickle* —
  sets `last_motion = now` and (re)arms the countdown = `off_delay`. The tickle
  makes the ESP the single motion aggregator: any external sensor
  (Zigbee/Z-Wave/mmWave/camera/etc.) can, via an HA automation, press the tickle
  and reset the interval. Tickles are honored in Auto and ignored under
  Force Off.
- While within `off_delay` of last motion (**Auto**) **or** **Force On**:
  emit a **strobe** — a pulse of `strobe_pulse_ms` on the motion output — every
  `restrobe_period_s` to keep the clock awake.
- Countdown expired / **Force Off**: stop strobing; output rests at inactive
  level; clock blanks on its own short fuse.
- `restrobe_period_s` must be shorter than the clock's minimum motion interval.
- Output and input active levels are configurable to match the clock/PIR.
- Modes (HA select): **Auto** (countdown), **Force On** (always strobe),
  **Force Off** (never strobe, ignore PIR).
- **PIR is optional.** With none fitted, `pir_in_pin` is simply unused and motion
  is driven entirely by HA tickles; the timer, strobe, and override behavior are
  unchanged.

### FR6 — Home Assistant control surface
See §7 for entity list. HA can enable/disable the stream and individual
sentences, set baud/polarity/framing, set the ms offset, set the off-delay and
strobe timing live, select the motion mode, inject a tickle, and read
motion/time status.

### FR7 — Diagnostics and dashboard
- **Diagnostic entities** (exposed to HA and the on-device dashboard):
  - Link: WiFi SSID, BSSID, channel, RSSI (dBm), IP, MAC, uptime, reconnect count.
  - GPS stream: last-sentence-sent timestamp, last raw sentence string, TX
    counter (sentences/sec), active baud, polarity, current `time_offset_ms`,
    time-sync status.
  - Motion: current mode, real-PIR state, driven-line state, last-motion source
    (PIR vs tickle), seconds since last motion, countdown remaining.
- **On-device dashboard:** ESPHome `web_server` — live entity states plus a
  streaming log served at the device's own IP, no HA required.
- **Onboard OLED:** compact local status on the 0.42″ SSD1306, at-a-glance, no
  client needed — full layout in **FR8**.
- **HA dashboard:** a supplied Lovelace view arranging the entities above
  (status tiles + history/logbook).
- **Rolling event log:** a text sensor holding the last *N* events, mirrored to
  the HA logbook — logs GPS-sent, WiFi connect/disconnect and channel change,
  mode changes, force on/off, and motion events (tagged PIR or tickle).

### FR8 — Onboard OLED display

**Panel.** 0.42″ SSD1306/SH1106, **72×40 px** visible, I²C `0x3C` on GPIO5 (SDA)
/ GPIO6 (SCL), ~800 kHz. The visible 72×40 window sits inside a 128×64
controller frame, so in ESPHome use the **`SSD1306 128x64` / `SH1106 128x64`**
model with the correct **x/y offset** (≈28–30 / ≈12–24) — the native "72×40"
model is unreliable. Exact offset and `rotation` are confirmed per unit at
bring-up (§10).

**Capacity.** Tiny — roughly three readable lines (~12 chars) at a small pixel
font. Layout is a compact home screen plus optional auto-rotating detail pages.

**Home screen (default, always shown):**
- Headline: local time **HH:MM:SS** currently being emitted (larger font).
- Status row (glyphs / short text): sync state (lock when synced, `--` until),
  WiFi strength (bars or RSSI), motion (display-on dot + mode letter `A`/`O`/`F`).

**Rotating detail pages** (auto-cycle every `oled_page_s`; `0` = home only;
optionally advanced by the BOOT button):
- **GPS:** last sentence type sent, TX counter, active baud, `time_offset_ms`.
- **Net:** SSID (scrolling), RSSI dBm, IP.
- **Motion:** mode, seconds since last motion, countdown remaining, last source
  (PIR / tickle).

**Special states.** Boot splash (name + version); `WAIT SYNC` until time is
valid; optional brief `MOTION` flash on each event.

**Burn-in mitigation** (always-on OLED): reduced contrast by default, and an
optional idle blank after `oled_timeout_s`, woken **only by the BOOT button**
(GPIO9). Motion never wakes the display — otherwise every motion event would
flash the screen on. Optionally a slow 1-px content shift. All configurable.

**Fonts/assets.** A small pixel TTF (~6–8 px) for text and a small icon glyph
subset (WiFi / lock / motion) bundled with the firmware.

## 6. Configuration parameters

| Parameter | Type | Default | Range | Description |
|-----------|------|---------|-------|-------------|
| `tx_pin` | pin | GPIO0 | — | NMEA output pin |
| `pir_in_pin` | pin | GPIO1 | — | Real PIR input |
| `pir_in_active` | enum | high | high/low | Real PIR active level |
| `motion_out_pin` | pin | GPIO10 | — | Strobe output to clock |
| `motion_out_active` | enum | high | high/low | Clock motion-input active level |
| `baud` | int | 9600 | 300–115200 | Bit-bang baud |
| `invert` | bool | false | — | false=TTL, true=pseudo-RS232 (inverted TTL) |
| `data_bits` | int | 8 | 7/8 | Framing data bits |
| `stop_bits` | int | 1 | 1/2 | Framing stop bits |
| `parity` | enum | none | none/even/odd | Framing parity |
| `en_gpzda` | bool | true | — | Emit GPZDA |
| `en_gprmc` | bool | true | — | Emit GPRMC |
| `en_gpgga` | bool | true | — | Emit GPGGA |
| `en_gpgsa` | bool | true | — | Emit GPGSA |
| `latitude` | float | 40.2333 | ±90 | Fake position (decimal deg) |
| `longitude` | float | -82.85 | ±180 | Fake position (decimal deg) |
| `sats` | int | 10 | 4–12 | Satellites in use (GGA/GSA) |
| `altitude_m` | float | 100.0 | — | GGA altitude |
| `hdop` | float | 0.9 | — | Horizontal DOP |
| `timezone` | tz string | (system) | IANA TZ | Local timezone incl. DST rules |
| `time_basis` | enum | local | local/utc | Time stamped into sentences |
| `time_offset_ms` | int | 0 | ±1000 | Signed emission phase offset |
| `time_valid_gate` | bool | true | — | Void sentences until synced |
| `off_delay_min` | float | 5 | 0.1–1440 | ESP-owned display timeout (min) |
| `strobe_pulse_ms` | int | 200 | 20–2000 | Motion strobe pulse width |
| `restrobe_period_s` | int | 20 | 1–3600 | Keep-alive strobe period (< clock min interval) |
| `motion_mode` | enum | auto | auto/on/off | Motion override |
| `web_server` | bool | true | — | Enable on-device dashboard + live log |
| `event_log_depth` | int | 50 | 10–200 | Rolling event-log entries retained |
| `oled_enabled` | bool | true | — | Drive the onboard 0.42″ OLED |
| `oled_page_s` | int | 0 | 0–60 | Detail-page auto-cycle (0 = home only) |
| `oled_rotation` | enum | 0 | 0/180 | Display orientation for mounting |
| `oled_contrast` | int | 40 | 0–100 | Brightness (dimmed to limit burn-in) |
| `oled_timeout_s` | int | 0 | 0–3600 | Idle blank after N s (0 = always on) |

## 7. Home Assistant entities

- **Switches:** stream enable; per-sentence enable (GPZDA/GPRMC/GPGGA/GPGSA).
- **Selects:** baud; polarity (TTL/inverted); motion mode (Auto/On/Off).
- **Numbers:** `time_offset_ms`; `off_delay_min`; `strobe_pulse_ms`;
  `restrobe_period_s`; latitude; longitude; sats.
- **Buttons:** **tickle motion** (inject a motion event — pressed by HA
  automations from any external sensor).
- **Binary sensors:** time synced; real PIR; driven motion line;
  display-should-be-on.
- **Sensors:** seconds since last motion; countdown remaining; WiFi RSSI;
  TX counter.
- **Text sensors (diagnostic):** last sentence sent; last-sent timestamp;
  last-motion source (PIR/tickle); SSID; BSSID; WiFi channel; IP; MAC; uptime;
  active baud/polarity; rolling event log.

## 8. Implementation notes

- Delivered as an ESPHome **external component** (C++) plus a YAML package.
  The bit-bang TX, NMEA assembly, precise emission scheduling, and motion timer
  live in the component; HA entities are exposed via template/native entities in
  the package.
- Position stored in decimal degrees; converted to NMEA `ddmm.mmmm` /
  `dddmm.mmmm` at build time.
- Emission scheduler runs off the microsecond clock referenced to synced epoch,
  firing the burst at `second_boundary − time_offset_ms` and stamping second `T`.
- Runtime baud/polarity/framing changes take effect on the next second's burst
  (no driver reconfig — they're just variables the bit-bang reads).

## 9. Acceptance tests

1. **NMEA validity** — capture TX on a USB-serial adapter / logic analyzer;
   parse with `pynmea2`/gpsd. All enabled sentences present, checksums valid,
   RMC status `A`, GGA fix `1`, sats/DOP sane.
2. **Polarity & baud** — verify idle level and bit timing on an analyzer for
   both `invert=false` (idle high) and `invert=true` (idle low), at the
   configured baud.
3. **Valid/void gate** — on cold boot before sync, sentences carry `V`/fix `0`;
   after sync they flip to `A`/fix `1`.
4. **Offset** — measure clock display vs a reference; adjust `time_offset_ms`
   and confirm the display rollover shifts by the commanded amount.
5. **Motion Auto** — a PIR event lights the display; it stays on for
   `off_delay_min` after the last event, then blanks. Repeated motion re-arms.
6. **Motion override** — Force On holds the display on regardless of PIR;
   Force Off blanks it and ignores PIR; returning to Auto restores normal.
7. **Tickle** — pressing the tickle button (or an HA automation doing so) resets
   the countdown exactly like a real PIR event in Auto, and is ignored under
   Force Off. Last-motion source reports `tickle`.
8. **Dashboard** — on-device web dashboard and HA entities show live RSSI,
   channel, BSSID, last-sent timestamp/sentence, and the rolling event log;
   values update as the stream runs.
9. **OLED** — display initializes with the correct offset (72×40 centered, no
   wrap), shows time matching the emitted stream and updates each second; status
   glyphs track sync/WiFi/motion; pages rotate if enabled; idle blank + BOOT-button
   wake work if configured (motion does not wake the display).

## 10. Open items (clock-specific — confirm before/at bring-up)

- Default **baud** the clock's GPS expects (assumed 9600).
- Which **sentence** the clock actually consumes for time (ensure enabled and
  used as the offset-calibration reference).
- Clock's **minimum motion interval** (bounds `restrobe_period_s`).
- Whether the clock needs any sentence outside the four (e.g., GPGSV) — out of
  scope unless required.
- Final **GPIO** assignment on the chosen board.
- OLED **offset / rotation** for the specific unit (72×40 window position varies
  slightly between batches).
