# MySat Groundlink

A complete radio ground-segment for the [MySat](https://www.mysatkit.com/) 1U
CubeSat trainer: new firmware for **both** microcontrollers plus an earth
station that flies the spacecraft over the HC-12 link.

```
firmware/MySat_OBC/     ESP32-CAM  — on-board computer, sensors, camera, radio service
firmware/MySat_RCU/     ATmega328P — radio control unit: owns the HC-12, servo, watchdog
earthstation/           Python     — telemetry, telecommanding, image downlink, dashboard
PROTOCOL.md                        — MSK-1 and RCU-2 wire specifications
```

Both sketches compile clean against `esp32:esp32:esp32cam` (core 3.3.10) and
`arduino:avr:nano` (core 1.8.6). The earth station and its protocol codec were
exercised end to end against the built-in spacecraft simulator: telecommand →
ack, telemetry decode, CSV logging, and a CRC-verified image reassembly.

---

## What the analysis found

Starting point: [`MySatKit/MySatKit-Firmware`](https://github.com/MySatKit/MySatKit-Firmware)
v1.4/v1.4.1, its eight public forks, the
[MySatKit-Ground-Station](https://github.com/MySatKit/MySatKit-Ground-Station)
and [MySatKit-PCB](https://github.com/MySatKit/MySatKit-PCB) repositories, and
your local `MySatKit-Firmware-main` copy.

**1. The radio was never actually implemented.** The stock ATmega328P sketch
declares `RF_TURN` and `RF_SET` in its command enum, then leaves both cases
empty:

```c
case RF_TURN:
case RF_SET:
  break;
```

The whole radio story in the shipped kit is "the HC-12 is a transparent serial
bridge, so whatever the ESP32 prints appears in a Serial Monitor on the
ground". That works, but there is no framing, no error detection, no uplink
command path and no way to configure the module from software. All eight forks
are unmodified copies — nobody had filled this in.

**2. The three UARTs are one bus.** From the published board 1.5.8 schematics,
the ESP32's UART0, the Nano's UART and the HC-12 all sit on a single TX/RX
pair. Two of those are push-pull *transmitters* on the same net. The stock Nano
sketch prints a banner at boot straight into the ESP32's own output. Upstream
v.1.4.1 (2026-08-19) is titled "fixed a bug causing the HC-12 to stop working
during operation" — bus contention is a very plausible cause.

**3. The stock `RF_SET` fix is incomplete.** Upstream added
`digitalWrite(RF_SET_PIN, LOW)` in `setup()` — but never
`pinMode(RF_SET_PIN, OUTPUT)`. On an input pin that write only enables the
internal pull-up.

**4. Your local copy has a stubbed BME680.** `environment_sensor.h` in
`MySatKit-Firmware-main` returns hard-coded constants (25.0 °C, 50 %RH,
1013.25 hPa) with the comment "BSEC library disabled due to linker issues".
Every environment number in your telemetry was fictional.

---

## What this project does about it

| Problem | Fix |
|---|---|
| No radio protocol | **MSK-1**: CRC16-framed printable-ASCII frames for beacon, telemetry, events, acks, image chunks and uplink telecommands. Coexists with the stock human-readable console on the same wire. |
| No uplink | 33 telecommands: platform control, camera, mission logging, radio configuration, safe mode, reboot. |
| Radio unconfigurable from software | The RCU owns the HC-12 — power, AT mode, channel, TX power, FU mode, sleep — with a bus-grant handshake so the AT session never collides with the OBC. |
| Factory HC-12 at 9600 vs sketches at 115200 | `RFSYNC` sweeps the baud table, finds the module, forces it to 115200. |
| No way to interrogate the radio | `RFAT` sends any AT command and returns the reply; `RFCFG` dumps the module's whole configuration. |
| HC-12 wedges mid-operation | The RCU keeps its TX pin high-Z unless granted the bus, and can hard power-cycle the module. |
| A hung OBC takes the link down with it | The RCU hears the ground uplink independently. Arm it with `EMERG ON` and it answers `$TC,...,RCU,...` telecommands and beacons on its own after the OBC misses its watchdog. |
| Fake environment data | A register-level BME680 driver implementing the datasheet's floating-point compensation. No BSEC blob, so no linker problem. |
| Photos invisible without Wi-Fi | The photo index is loaded in `setup()`, not only from `initServer()`. |
| No ground software | A Python earth station: console, live web dashboard, CSV + raw session logging, image reassembly with CRC32 verification, session replay, and a spacecraft simulator for working without hardware. |

Everything from the stock firmware still works: all console commands, the Wi-Fi
Web GUI, mission CSV logging, the photo ring buffer, the event log, plotter
mode. The OBC also still speaks the legacy single-byte I²C commands, so it runs
against an un-upgraded Nano — it just reports the radio controller as `legacy`
and disables the radio-configuration commands.

---

## Hardware caveat — read this first

The bus topology above is read from the **net labels** in
`MySatKit-PCB/Schematic_1.5.8.board-A.pdf` and `board-B.pdf`. Those are
schematics, not a netlist. Before relying on the bus-grant mechanism, confirm
with a continuity check that:

* the Nano's D1 (TX) is on the same net as the ESP32's U0TX and the HC-12's RXD;
* the Nano's D0 (RX) is on the same net as the ESP32's U0RX and the HC-12's TXD.

If your board wires the HC-12 to the Nano only (no ESP32 connection), the RCU
firmware still works but the OBC's frames will not reach the radio — tell me and
I will add a Nano-side relay path instead.

The MOSFET polarity on `EN_HC-12` (D5) and `Arduino_SET_HC-12` (D4) is inverting
on board 1.5.8; the firmware assumes that, matching the stock sketch's own
comment that D5 low "allows HC-12 to always work by default".

---

## Building and flashing

### Libraries

The OBC needs the same libraries as the stock firmware — they are in
`MySatKit-Firmware-main/libraries.zip`, or install via Library Manager:

```
Adafruit ADS1X15, Adafruit BusIO, Adafruit Unified Sensor,
Adafruit NeoPixel, BeastDevices INA3221, Rtc by Makuna, ArduinoJson
```

No BME680 or BSEC library is needed any more. The RCU needs only `Servo`.

### OBC — ESP32-CAM

Board: **AI Thinker ESP32-CAM**, partition scheme with LittleFS
(the stock kit's settings work unchanged).

```bash
arduino-cli compile -b esp32:esp32:esp32cam firmware/MySat_OBC
arduino-cli upload -b esp32:esp32:esp32cam -p COM7 firmware/MySat_OBC
```

Then upload the `data/` folder to LittleFS exactly as with the stock firmware
(the `arduino-littlefs-upload` VS Code extension, already in your Downloads).
Without it the Web GUI has no assets — the radio link works regardless.

### RCU — Arduino Nano

```bash
arduino-cli compile -b arduino:avr:nano firmware/MySat_RCU
arduino-cli upload -b arduino:avr:nano -p COM8 firmware/MySat_RCU
```

Use `arduino:avr:nano:cpu=atmega328old` if your Nano has the old bootloader.

**Flash the Nano before the ESP32.** Both share the programming header, and the
RCU releases the TX line at boot.

### Earth station

```bash
pip install pyserial
python earthstation/earthstation.py --port COM7 --dash
```

Only `pyserial` — the dashboard and codec are standard library.

---

## Operating

Try it without hardware first:

```bash
python earthstation/earthstation.py --sim --dash
```

Then with the radio. Plug the ground-station HC-12 into a USB-TTL adapter:

```bash
python earthstation/earthstation.py --list          # find the port
python earthstation/earthstation.py --port COM7
```

A session:

```
MYSAT> ping
  OK PONG v.2.0-GL
MYSAT> mode stream                 # switch the link to MSK frames only
  OK mode=STREAM
MYSAT> solar deploy
  OK deploying
MYSAT> snap down                   # photograph and downlink in one go
  OK photo #4
  [IMG] incoming #4: 48213 bytes in 366 chunks
  [IMG] saved groundlink_data/images/img004_20260822_1043.jpg (366/366 chunks, CRC ok)
MYSAT> rfstat
  OK ch 1 pwr 8 FU3 @115200 [on]
MYSAT> /status
MYSAT> /rcu ping                   # talk to the radio controller directly
```

`/help` lists everything. Station commands start with `/`; anything else is
uplinked as a telecommand.

### Radio modes

| Mode | On the air |
|---|---|
| `OFF` | nothing — pure stock console behaviour |
| `BEACON` | a `$BC` every 10 s (default) |
| `STREAM` | `$BC` + `$TM`, human console text suppressed — use this for a real pass |
| `FULL` | frames **and** console text interleaved — useful while debugging |

### Link budget reality

HC-12 at 433 MHz, FU3, +20 dBm gives a few hundred metres line of sight with
the stock antennas. A 48 kB XGA photo at 115200 baud with the default 25 ms
chunk pacing takes roughly 20 seconds; over a marginal link, raise `IMGPACE`
and lower the camera resolution. There is no per-chunk retransmission — the
station reports which chunk indices went missing and you re-request the image.

The HC-12 exposes no RSSI, so the station reports **frame error rate** instead.
That is the honest signal-quality number available on this hardware.

### Losing the OBC

```
MYSAT> emerg on
MYSAT> wdt 120
```

If the ESP32 then stops kicking for 120 s, the RCU takes the TX line,
power-cycles the HC-12, and starts beaconing on its own. From the ground:

```
MYSAT> /rcu ping
MYSAT> /rcu reset
MYSAT> /rcu solar retract
```

The Nano's LED tells you which state it is in without a console: slow single
blink = normal, double blink = configuring the HC-12, fast continuous = the OBC
watchdog expired and the RCU is holding the link, long-on = HC-12 powered down.

---

## Output files

```
groundlink_data/
  telemetry_<stamp>.csv     one row per TM frame, all 28 fields + RX time
  session_<stamp>.log       every decoded frame, replayable
  images/img###_<stamp>.jpg reassembled downlinks
```

Replay a pass afterwards:

```bash
python earthstation/earthstation.py --replay groundlink_data/session_20260822_104312.log
```

---

## Known limitations

* **IAQ is an approximation.** Bosch's certified BSEC IAQ needs their pre-built
  blob, which is what broke the build in the first place. The open
  humidity + gas-baseline heuristic here tracks air-quality *trends* correctly
  and reports an accuracy flag, but the absolute number is not BSEC-equivalent.
  The gas baseline is persisted to NVS and needs ~20 minutes of clean air on a
  first run.
* **No uplink authentication.** Anyone on the channel with the callsign can
  command the satellite. That matches the kit's educational scope; a shared-key
  HMAC field in the `TC` frame would be a natural next step.
* **No per-chunk ARQ for images.** Deliberate — it keeps the flight side simple
  and the failure mode visible.
* **`RFBAUD` will break your link** unless every node changes together. The RCU
  refuses anything but 115200 for exactly that reason; use `RF_SYNC` instead.
* **A corrupt `radio.cfg` used to silence the radio permanently.** `toInt()`
  returns 0 for an unparseable line, and 0 is `RADIO_OFF` — so one interrupted
  write could disable the transmitter, recoverable only by a telecommand that
  needs the transmitter. Seen for real after a brownout loop. The file now
  carries a magic header and is discarded if it fails to validate, and
  `RADIO_OFF` is never restored across a reboot: the satellite always comes back
  beaconing. `MODE OFF` now lasts until the next reset.

* **Deploying the panels used to brown the board out.** Root cause, measured:
  `Servo::attach()` starts emitting the library's default 1500 us pulse, which
  is mid-travel (~90 deg). With the panels at 10 or 170 deg that is an ~80 deg
  error the instant the output goes live, so the servo pulls stall current
  before the 1 deg/10 ms ramp ever runs. `reset_reason` reported `BROWNOUT` at
  4.2 V on the charger. Fixed by writing the current position immediately after
  `attach()` (a hold, not a slam) and persisting the angle in the Nano's EEPROM
  so the belief survives a reset. Verified: two full deploy/retract cycles with
  no reboot.

* **The STAR LED never lit.** ESP32 core 3.x made the LEDC API pin-based, but
  the writes still passed a channel number, so PWM went to GPIO 0 - the
  camera's XCLK pin - while every status flag reported the LED as on. Fixed to
  `ledcWrite(LED, ..)`; the channel define is deleted so it cannot come back.

* **Watch the battery.** On cell power alone the ESP32 enters a ~1 Hz brownout
  reset loop once the cell sags (observed around 4.00 V under HC-12 transmit
  load). It looks exactly like a dead uplink and silently invalidates any test
  run with USB power removed. Board A has its own USB-C charging port feeding
  the MH-CD42 — use that to power the satellite without putting anything on the
  serial bus.

* **The ATmega328P's UART is not on the HC-12 bus at all (board 1.5.8).**
  Measured in both directions: `rcu_rx_bytes` stays at 0, and an AT command
  issued through the RCU returns `last_error = 5` (timeout). The Nano can
  neither hear the ground nor talk to the radio module.

  Two features were designed around the schematic being right about this and do
  not work on unmodified hardware: **emergency commanding addressed to `RCU`**
  (`/rcu ...` times out) and the **RCU-side AT engine**.

  Radio configuration was rewritten around the hardware as it actually is — the
  OBC runs the AT session over its own UART0, which demonstrably reaches the
  module since that is how telemetry gets out, and asks the RCU to toggle the
  HC-12's SET pin over I²C. `RFCH`, `RFPWR`, `RFFU` and the new `RFSYNC`,
  `RFAT`, `RFCFG` all work with no rewiring.

  Emergency commanding needs two wires: HC-12 TXD to Nano D0, HC-12 RXD to
  Nano D1. The RCU-side engine and bus-grant handshake stay in the firmware for
  that case.
* **Only one uplink path works at a time, and which one is not deterministic.**
  Measured by A/B on real hardware. The satellite's HC-12 TXD, the ESP32
  programmer's TX and the Nano's USB TX all drive the ESP32's RX net. They
  fight; the winner takes the uplink and the losers go silent. Two runs,
  identical hardware and firmware, opposite outcomes:

  | run | radio uplink | cable uplink |
  |---|---|---|
  | programmer plugged in | worked | dead |
  | programmer replugged  | dead   | worked |

  Downlink is never affected - that is the other net, where the ESP32 is the
  only driver, which is why beacons keep flowing and make the link look healthy.

  **Operating rule: to fly over the radio, unplug every USB adapter from the
  satellite** - both the ESP32 programmer and the Nano - and run it on battery.
  The HC-12 is then the only driver on that net and the uplink is stable. To use
  the cable instead, unplug the HC-12. `linkcheck.py` tells you which path is
  currently live.

---

## Licence

GPL-3.0-or-later. This is a derivative of
[MySatKit-Firmware](https://github.com/MySatKit/MySatKit-Firmware), whose
repository is licensed under the GNU General Public License v3, so this project
inherits that licence. See [LICENSE](LICENSE) and [NOTICE.md](NOTICE.md), which
records what was changed.

Note that the upstream project is internally inconsistent about this: its source
headers say "Open Source (MIT)" while its `license.md` is the full GPL-3.0 text.
The repository-level licence is the one followed here, as the more restrictive
and more clearly stated of the two. If MySatKit confirm they intend MIT, this
project can be relicensed to match.
