# Attribution and changes

MySat Groundlink is a derivative work of
**[MySatKit-Firmware](https://github.com/MySatKit/MySatKit-Firmware)** by the
MySat development team ([mysatkit.com](https://www.mysatkit.com/)), starting
from release v1.4 / v1.4.1.

The upstream repository carries the GNU General Public License v3 in
`license.md`, so this project is distributed under **GPL-3.0-or-later**. See
[LICENSE](LICENSE).

> Upstream is internally inconsistent about licensing: individual source
> headers read `license: Open Source (MIT) – github.com/mysatkit`, while the
> repository's `license.md` is the complete GPL-3.0 text. This project follows
> the repository-level licence as the more restrictive and more clearly stated
> of the two. If the MySat team confirm MIT was intended, this project can be
> relicensed to match.

Hardware schematics referenced during development come from
[MySatKit-PCB](https://github.com/MySatKit/MySatKit-PCB) (board 1.5.8).

---

## Files taken from upstream

Carried over, some modified:

```
firmware/MySat_OBC/  ADC.h  RTC.h  camera.h  camera_pins.h  console.h
                     control.h  data_logger.h  environment_sensor.h
                     event_log.h  index.html  position_sensor.h
                     power_measure.h  sensors_data.h  web_server.h
                     data/*            (MySat artwork, Bootstrap CSS/JS)
firmware/MySat_RCU/  derived from ino/MySat_Nano_ATmega328p/
```

Bootstrap (`data/bootstrap.css`, `data/bootstrap.js`) is third-party, MIT
licensed, and redistributed unmodified from upstream.

---

## Changes made in this work

As required by GPL-3.0 §5(a), the modifications are:

### New files

| File | Purpose |
|---|---|
| `firmware/MySat_OBC/msk_link.h` | MSK-1 frame codec: CRC16-CCITT framing, builder, incremental parser |
| `firmware/MySat_OBC/msk_radio.h` | Beacon, telemetry, event, ack and chunked image downlink; telecommand dispatch |
| `firmware/MySat_OBC/msk_rcu_proto.h` | I²C protocol shared with the radio controller |
| `firmware/MySat_OBC/rcu_link.h` | OBC-side driver for the radio controller |
| `firmware/MySat_OBC/hc12_at.h` | OBC-driven HC-12 AT configuration |
| `firmware/MySat_OBC/web_ui.h` | Self-contained 3D mission-control page |
| `firmware/MySat_RCU/hc12.h` | HC-12 power, SET and AT engine, bus arbitration |
| `firmware/MySat_RCU/msk_uplink.h` | Passive uplink listener for the ATmega328P |
| `earthstation/` | Python ground station, protocol codec, dashboard, link diagnostic |

### Modifications to upstream files

- **`environment_sensor.h`** — replaced entirely. Upstream shipped a stub
  returning constants (25.0 °C, 50 %RH, 1013.25 hPa) because the Bosch BSEC
  blob would not link. Now a register-level BME680 driver implementing the
  datasheet's floating-point compensation, with an open IAQ approximation whose
  gas baseline persists to NVS.
- **`control.h`** — fixed the STAR LED: ESP32 core 3.x made the LEDC API
  pin-based, but the writes still passed a channel number, so PWM went to
  GPIO 0 (the camera's XCLK pin) and the LED never lit. Added a battery guard
  and LED load-shed before servo moves, and a deferred boot-time panel restore.
- **`console.h`** — refactored `handleCommands()` into `handleConsoleLine()` so
  a single reader owns the UART; added `Help` and the radio command set.
- **`web_server.h`** — serves the new page; responses sent in 1 kB chunks
  because `WebServer::send()` silently truncates large bodies; telemetry JSON
  extended with per-panel currents, housekeeping counters, `att_valid` and
  `reset_reason`. Original page retained at `/classic`.
- **`sensors_data.h`** — output gating for the shared UART, `resetReasonName()`.
- **`camera.h`** — photo index load made idempotent so it works without Wi-Fi.
- **`MySat_Nano_ATmega328p.ino` → `MySat_RCU.ino`** — the stock sketch declared
  `RF_TURN`/`RF_SET` and left both cases empty. Rewritten as a radio
  controller; also fixed `RF_SET_PIN` never being configured as an output, and
  the `Servo::attach()` default-pulse slam that browned the board out on every
  panel deployment.

Removed: `bsec.h`, `inc/`, `bme68x/` — unused once BSEC was dropped.
