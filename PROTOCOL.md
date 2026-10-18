# MSK-1 — MySat Groundlink link protocol

Two protocols are defined here:

* **MSK-1** — the over-the-air / over-the-cable protocol between the earth station and the spacecraft.
* **RCU-2** — the I²C protocol between the ESP32 OBC and the ATmega328P radio controller.

---

## 1. The physical situation

On MySat board 1.5.8 the ESP32's UART0, the ATmega328P's UART and the HC-12
transceiver share **one TX/RX pair**:

```
     net TX ──┬── ESP32 U0TX (out)          net RX ──┬── HC-12 TXD (out)
              ├── HC-12 RXD (in)                     ├── USB programmer TX (out)
              ├── Nano D1/TX (out, gated)            ├── ESP32 U0RX (in)
              └── USB programmer RX (in)             └── Nano D0/RX (in)
```

Three consequences drive every design decision below:

1. **Everything the OBC prints is transmitted.** The stock firmware's
   human-readable telemetry dump already goes over the air — that is how the
   kit is meant to work. MSK-1 must coexist with it rather than replace it.
2. **Two devices can drive net TX.** The ESP32 and the Nano. Contention here is
   a plausible cause of the "HC-12 stops working during operation" bug fixed in
   upstream v.1.4.1, so the RCU keeps its TX pin high-Z unless it is granted the
   bus (`RCU_CMD_BUS_GRANT`).
3. **The uplink reaches both processors.** The RCU hears the ground station
   directly, which is what makes independent emergency commanding possible when
   the OBC is wedged.

> **Measured on real hardware (2026-08-22) — the diagram above is WRONG for
> board 1.5.8.** The ATmega328P's UART is not on this bus at all:
>
> * `rcu_rx_bytes = 0` — its RX pin has never seen a single byte;
> * `RFPWR` routed through the RCU's AT engine returns `last_error = 5`
>   (AT timeout) — its TX never reaches the HC-12 either.
>
> So on that board the RCU can neither hear the ground (no independent
> emergency commanding) nor run an AT session. What it *can* do is drive the
> HC-12's SET and power pins, which are plain GPIOs reachable over I²C — so
> **the OBC runs the AT session over its own UART0 and uses the RCU as a GPIO
> expander for SET** (see `hc12_at.h`). Bus arbitration becomes a no-op, since
> there is no second transmitter to arbitrate against.
>
> Adding two wires — HC-12 TXD to Nano D0, HC-12 RXD to Nano D1 — restores the
> original design; the RCU-side AT engine and the bus-grant handshake are still
> in the firmware for that case.
>
> The topology above is read from the net labels in
> [MySatKit-PCB](https://github.com/MySatKit/MySatKit-PCB) `Schematic_1.5.8.board-A.pdf`
> and `board-B.pdf`. The PDFs are a schematic, not a netlist — confirm the three
> TX/RX connections with a continuity check before relying on the bus-grant
> mechanism on your board.

---

## 2. MSK-1 frame format

```
$<TYPE>,<field>,<field>,...*<CRC16>\n
```

| Element | Rule |
|---|---|
| `$` | Start of frame. A `$` always restarts the parser, so a truncated frame cannot swallow the next one. |
| `TYPE` | Two uppercase letters. |
| fields | Printable ASCII, comma separated. `,` → `;`, `$` → `S`, `*` → `+`, control bytes → `?`. Empty means "sensor absent". |
| `*` | Start of checksum. |
| `CRC16` | CRC16-CCITT-FALSE (poly `0x1021`, init `0xFFFF`, no reflection, no final XOR) over the bytes **between** `$` and `*`, four uppercase hex digits. |
| `\n` | End of frame. |

Max frame length 512 bytes. Frames are printable, so a plain Serial Monitor
still shows something readable.

A decoder must **ignore anything that is not a valid frame** — the same wire
carries the satellite's free-form console text.

### Downlink frames

| Type | Fields |
|---|---|
| `BC` beacon | `seq, callsign, uptime_s, mode, battery_v, flags, free_heap` |
| `TM` telemetry | 28 fields, see below |
| `EV` event | `seq, callsign, level, text` |
| `AK` ack | `seq, ref_seq, status, text` |
| `IM` image meta | `seq, callsign, image_id, chunk_count, byte_count, chunk_size, timestamp` |
| `ID` image chunk | `seq, image_id, chunk_index, base64` |
| `IE` image end | `seq, image_id, chunk_count, byte_count, crc32_hex` |
| `RS` radio status | `seq, callsign, rf_state, at_state, channel, txpower, baud, fu_mode, servo_angle, wdt_remaining_s, last_error, rf_resets, ground_tc_count, rcu_rx_bytes, rcu_rx_frames_ok, rcu_rx_frames_bad, frames_ok, frames_bad, tc_received, tc_rejected` |

`status` is one of `OK`, `ERR`, `BUSY`, `UNK`, `DENY`.

### `TM` field order

```
 0 seq              7 pressure hPa      14 ph1            21 solar_ma_left
 1 callsign         8 gas kOhm          15 ph2            22 solar_ma_right
 2 frame_counter    9 iaq               16 ph3            23 solar_deployed
 3 uptime_s        10 iaq_accuracy      17 ph4            24 star_led
 4 utc ISO-8601    11 roll deg          18 battery_v      25 logging_state
 5 temperature C   12 pitch deg         19 battery_mA     26 free_heap
 6 humidity %RH    13 yaw deg           20 solar_v        27 fs_free
```

Sun sensors are `ph1` left (−X), `ph2` back (−Y), `ph3` right (+X), `ph4` front
(+Y), matching the stock console labels. `iaq_accuracy`: 0 warming up,
1 settling, 2 calibrated, 3 calibrated from a stored baseline.

### `BC` health flags

| Bit | Meaning | Bit | Meaning |
|---|---|---|---|
| 0 | BME680 present | 5 | camera present |
| 1 | MPU present | 6 | solar panels deployed |
| 2 | ADS1015 present | 7 | mission logging active |
| 3 | INA3221 present | 8 | Wi-Fi connected |
| 4 | DS3231 present | 9 | radio controller present |

`mode`: 0 OFF, 1 BEACON, 2 STREAM, 3 FULL, 9 the RCU is holding the link
because the OBC missed its watchdog.

### Uplink

```
$TC,<seq>,<dest>,<CMD>[,<arg>...]*<CRC16>\n
```

`dest` is the satellite callsign, `*` for broadcast, or `RCU` to address the
radio controller directly. A telecommand addressed to somebody else is counted
and **silently dropped** — the satellite does not transmit a rejection, so two
stations on one channel do not talk over each other.

Every telecommand is answered with exactly one `AK` carrying the same `seq` in
`ref_seq`. There is no retransmission: MSK-1 is a *detect-and-report* protocol,
not a reliable one. A missing ack means "send it again".

### Image downlink

`IM` → `ID` × N → `IE`. Chunks are 132 raw bytes (176 base64 characters) and
are paced by `IMGPACE` milliseconds so the main flight loop stays responsive.
There is no per-chunk ARQ: the receiver reports which chunk indices are missing
and the operator re-requests the whole image. `IE` carries a CRC32 of the whole
JPEG so a silently corrupted image is never mistaken for a good one.

---

## 3. Radio telecommands (`dest` = callsign)

| Command | Args | Effect |
|---|---|---|
| `PING` | — | liveness + firmware version |
| `TM` | — | one telemetry frame now |
| `SYS` | — | heap, filesystem, uptime, RCU summary |
| `MODE` | `OFF\|BEACON\|STREAM\|FULL` | what the radio transmits |
| `TMRATE` | ms (250–600000) | telemetry period |
| `BCRATE` | ms (1000–3600000) | beacon period |
| `SOLAR` | `DEPLOY\|RETRACT\|TOGGLE` | solar panel servo |
| `LED` | `ON\|OFF\|BLINK` | STAR LED |
| `CAL` | — | zero the MPU gyro |
| `TIME` | y m d h mi s | set the DS3231 |
| `CALLSIGN` | name | rename the spacecraft |
| `SNAP` | `[DOWN]` | capture, optionally downlink immediately |
| `IMGLIST` | — | stored photo ids |
| `IMG` | id | downlink a stored photo |
| `IMGABORT` | — | stop the transfer in progress |
| `IMGPACE` | ms (0–2000) | inter-chunk gap |
| `LOGSTART` | seconds | start CSV mission logging |
| `LOGSTOP` / `LOGLIST` / `LOGDEL` | — | mission log control |
| `EVLOG` | — | dump the on-board event log as `EV` frames |
| `RFSTAT` | — | radio controller status + `RS` frame |
| `RFCH` | 1–100 | HC-12 channel |
| `RFPWR` | 1–8 | HC-12 TX power, −1 dBm … +20 dBm |
| `RFFU` | 1–4 | HC-12 FU mode |
| `RFSYNC` | — | sweep baud rates, find the HC-12, force it to 115200 |
| `RFAT` | `AT+...` | send a raw AT command, return the module's reply |
| `RFCFG` | — | read the HC-12's whole configuration back (`AT+RX`) |
| `RFRESET` | — | power-cycle the HC-12 |
| `WDT` | seconds (0 = off) | RCU watchdog timeout |
| `EMERG` | `ON\|OFF` | arm RCU emergency commanding |
| `FS` | — | filesystem summary |
| `SAFE` | — | beacon only at 30 s, logging off, LED off |
| `REBOOT` | — | restart the OBC |
| `HELP` | — | command list from the spacecraft itself |

`RFCH`, `RFPWR`, `RFFU` and `RFBAUD` **ack before they take effect**, because
after the change the satellite is on different link parameters. The ground
station's own HC-12 has to be reconfigured to match — it is a separate module
with its own AT interface.

### Emergency telecommands (`dest` = `RCU`)

Executed by the ATmega328P, and only when armed with `EMERG ON`. The RCU
answers only while it legitimately owns the TX line, i.e. after the OBC has
missed its watchdog — otherwise it would collide with a healthy OBC.

| Command | Effect |
|---|---|
| `PING` | is the radio controller alive |
| `RESET` | power-cycle the HC-12 |
| `BEACON` | force an emergency beacon |
| `SOLAR DEPLOY\|RETRACT` | drive the servo without the OBC |
| `RFCH <n>` | change channel without the OBC |
| `KICK` | reset the OBC watchdog timer |
| `WDTOFF` | disable the watchdog |

---

## 4. RCU-2 — I²C, OBC to radio controller

Slave address `0x08`.

**Legacy write** (stock MySat firmware, still accepted):

```
[cmd]        cmd < 0x08
```

**Extended write:**

```
[0xA5][len][cmd][arg0..argN][xor]
  len = bytes after `len`, including cmd and the xor byte
  xor = XOR of `len` through the last arg
```

**Status read:** `Wire.requestFrom(0x08, sizeof(RcuStatus))` returns a packed
struct — magic `0x5A`, protocol version, firmware version, RF state, AT state,
channel, TX power, baud index, FU mode, servo state and angle, bus owner,
emergency armed, watchdog action, last error, watchdog timeout and remaining,
uptime, HC-12 reset count, ground telecommand count, and the RCU's own
receive counters. A stock Nano returns
nothing valid here, which is how the OBC detects an un-upgraded radio
controller and falls back to legacy single-byte commands.

### Command set

| Code | Command | Args |
|---|---|---|
| `0x00`/`0x01` | `MOTOR_OPEN` / `MOTOR_CLOSE` | — |
| `0x02`/`0x03` | `RF_TURN` / `RF_SET` | — (legacy) |
| `0x10` | `RF_POWER` | 0 off / 1 on |
| `0x11` | `RF_AT_MODE` | 0 transparent / 1 AT |
| `0x12` | `RF_CHANNEL` | 1–100 |
| `0x13` | `RF_TXPOWER` | 1–8 |
| `0x14` | `RF_BAUD` | table index |
| `0x15` | `RF_FU_MODE` | 1–4 |
| `0x16` | `RF_RESET` | — |
| `0x17` | `RF_SLEEP` | 0 wake / 1 sleep |
| `0x18` | `RF_APPLY` | run the queued AT batch |
| `0x19` | `RF_SYNC` | sweep bauds, find the HC-12, force it to 115200 |
| `0x20` | `SERVO_ANGLE` | 0–180 |
| `0x21` | `SERVO_STOP` | — |
| `0x22` | `SERVO_LIMITS` | opened, closed |
| `0x30` | `WDT_SET` | timeout LSB, MSB (seconds) |
| `0x31` | `WDT_KICK` | — |
| `0x32` | `WDT_ACTION` | 0 none, 1 beacon, 2 rf reset, 3 both |
| `0x40`/`0x41` | `BUS_GRANT` / `BUS_RELEASE` | — |
| `0x42` | `SET_CALLSIGN` | up to 6 ASCII bytes |
| `0x50` | `EMERGENCY_ARM` | 0 / 1 |
| `0x51` | `PING` | — |
| `0x52` | `CLEAR_ERROR` | — |

### Reconfiguring the HC-12 — the handshake

Changing a radio parameter needs the shared TX line, so it is bracketed:

```
OBC                                  RCU
 |-- obcRadioSilent = true            |     stop printing entirely
 |-- BUS_GRANT --------------------> |     enable USART TX (PD1 leaves high-Z)
 |-- RF_CHANNEL 42 ---------------->  |     queue "AT+C042"
 |-- RF_APPLY --------------------->  |     assert SET, send AT, wait for OK,
 |                                    |     release SET
 |<-- poll status until AT_IDLE ----- |
 |-- BUS_RELEASE ------------------> |     TX back to high-Z
 |-- obcRadioSilent = false           |
```

`RF_SYNC` extends this: the RCU walks the baud table, sends `AT` at each rate
until the module answers `OK`, issues `AT+B115200`, and returns its own UART to
115200. That makes a factory HC-12 (9600 baud) usable without a manual AT
session.

### Board pin polarity

Both HC-12 control lines are driven through inverting MOSFETs:

| Nano pin | Net | Low | High |
|---|---|---|---|
| D5 | `EN_HC-12` → Q4 (P-ch) | HC-12 **powered** | HC-12 off |
| D4 | `Arduino_SET_HC-12` → Q3 (N-ch) | SET released → transparent | SET asserted → **AT mode** |

The stock sketch wrote `RF_SET_PIN` low without ever calling
`pinMode(RF_SET_PIN, OUTPUT)`, so that write only enabled the internal pull-up.
Both pins are configured as real outputs in `MySat_RCU`.
