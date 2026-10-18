"""
mskproto - codec for the MSK-1 link protocol used by MySat Groundlink.

Wire format:  $<TYPE>,<field>,<field>,...*<CRC16>\n

  * CRC16-CCITT-FALSE (poly 0x1021, init 0xFFFF) over the bytes between
    '$' and '*', rendered as four uppercase hex digits.
  * Printable ASCII only; a field never contains '$', '*', ',' or a newline.
  * The decoder tolerates arbitrary non-frame text on the same wire, because
    the satellite's human-readable console shares the radio link.

This module is pure standard library. Only earthstation.py needs pyserial.
"""

from __future__ import annotations

import base64
import binascii
import re
from dataclasses import dataclass, field
from typing import Dict, Iterator, List, Optional

# --- frame types ------------------------------------------------------------
T_BEACON = "BC"
T_TELEMETRY = "TM"
T_EVENT = "EV"
T_ACK = "AK"
T_IMG_META = "IM"
T_IMG_DATA = "ID"
T_IMG_END = "IE"
T_TELECMD = "TC"
T_RADIO = "RS"

ACK_OK = "OK"
ACK_ERR = "ERR"
ACK_BUSY = "BUSY"
ACK_UNKNOWN = "UNK"
ACK_DENIED = "DENY"

MAX_FRAME = 512


# ---------------------------------------------------------------------------
# CRC
# ---------------------------------------------------------------------------
def crc16(data: bytes) -> int:
    """CRC16-CCITT-FALSE, matching mskCrc16() in the firmware."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def crc32(data: bytes) -> int:
    """IEEE CRC32, matching mskCrc32Update() in the firmware."""
    return binascii.crc32(data) & 0xFFFFFFFF


# ---------------------------------------------------------------------------
# Frames
# ---------------------------------------------------------------------------
_UNSAFE = re.compile(r"[,$*\r\n]")


def escape(value: str) -> str:
    """Same substitutions the firmware's mskEscape() performs."""
    out = []
    for ch in str(value):
        if ch == ",":
            out.append(";")
        elif ch == "$":
            out.append("S")
        elif ch == "*":
            out.append("+")
        elif ch == "\r":
            continue
        elif ch == "\n":
            out.append(" ")
        elif " " <= ch <= "~":
            out.append(ch)
        else:
            out.append("?")
    return "".join(out)


@dataclass
class Frame:
    type: str
    fields: List[str] = field(default_factory=list)
    raw: str = ""

    def get(self, index: int, default: str = "") -> str:
        return self.fields[index] if 0 <= index < len(self.fields) else default

    def num(self, index: int, default: Optional[float] = None) -> Optional[float]:
        text = self.get(index).strip()
        if not text or text.lower() == "nan":
            return default
        try:
            return float(text)
        except ValueError:
            return default

    def integer(self, index: int, default: Optional[int] = None) -> Optional[int]:
        value = self.num(index)
        return default if value is None else int(value)

    def __str__(self) -> str:
        return f"${self.type}," + ",".join(self.fields)


def build(frame_type: str, *fields) -> str:
    """Renders a complete frame including the leading '$' and trailing '\\n'."""
    body = frame_type + "".join("," + escape(f) for f in fields)
    return f"${body}*{crc16(body.encode('ascii')):04X}\n"


def build_tc(seq: int, dest: str, command: str, *args) -> str:
    """
    Builds an uplink telecommand.

        build_tc(7, "MYSAT", "SOLAR", "DEPLOY")
        -> '$TC,7,MYSAT,SOLAR,DEPLOY*XXXX\\n'

    `dest` is the satellite callsign, "*" for a broadcast, or "RCU" to address
    the radio controller directly (emergency path, works while the OBC is down).
    """
    if dest == "*":
        # escape() would turn '*' into '+' and the satellite would read the
        # broadcast as a callsign that matches nobody. Emitting it raw is safe:
        # both parsers locate the checksum by scanning for the LAST '*'.
        body = f"{T_TELECMD},{escape(seq)},*,{escape(command.upper())}"
        body += "".join("," + escape(a) for a in args)
        return "$" + body + "*%04X" % crc16(body.encode("ascii")) + "\n"
    return build(T_TELECMD, seq, dest, command.upper(), *args)


class Decoder:
    """
    Incremental frame decoder. Feed it whatever bytes arrive; it yields valid
    frames and hands everything else back as console text.
    """

    def __init__(self) -> None:
        self._buf: List[str] = []
        self._active = False
        self._text: List[str] = []
        self.frames_ok = 0
        self.frames_bad = 0
        self.console_lines = 0

    def feed(self, data: bytes) -> Iterator[tuple]:
        """
        Yields ("frame", Frame) and ("text", str) tuples in arrival order.
        """
        for byte in data:
            ch = chr(byte) if byte < 128 else "?"

            if ch == "$":
                self._text = []            # drop any partial console line
                self._buf = []
                self._active = True
                continue

            if ch in "\r\n":
                if self._active:
                    self._active = False
                    frame = self._finish()
                    if frame is not None:
                        yield ("frame", frame)
                    self._buf = []
                elif self._text:
                    line = "".join(self._text).strip()
                    self._text = []
                    if line:
                        self.console_lines += 1
                        yield ("text", line)
                continue

            if self._active:
                if len(self._buf) >= MAX_FRAME:
                    self._active = False
                    self.frames_bad += 1
                    self._buf = []
                else:
                    self._buf.append(ch)
            elif len(self._text) < 400:
                self._text.append(ch)

    def _finish(self) -> Optional[Frame]:
        raw = "".join(self._buf)
        if len(raw) < 6 or raw[-5] != "*":
            self.frames_bad += 1
            return None
        body, checksum = raw[:-5], raw[-4:]
        try:
            want = int(checksum, 16)
        except ValueError:
            self.frames_bad += 1
            return None
        if crc16(body.encode("ascii", "replace")) != want:
            self.frames_bad += 1
            return None

        parts = body.split(",")
        self.frames_ok += 1
        return Frame(type=parts[0], fields=parts[1:], raw=raw)


# ---------------------------------------------------------------------------
# Telemetry mapping. The order MUST match radioSendTelemetry() in msk_radio.h.
# ---------------------------------------------------------------------------
TM_FIELDS = [
    "seq", "callsign", "frame", "uptime_s", "utc",
    "temperature", "humidity", "pressure", "gas_resistance", "iaq", "iaq_accuracy",
    "roll", "pitch", "yaw",
    "ph1", "ph2", "ph3", "ph4",
    "battery_v", "battery_ma", "solar_v", "solar_ma_left", "solar_ma_right",
    "solar_deployed", "star_led", "logging_state", "free_heap", "fs_free",
]

_TM_TEXT = {"callsign", "utc"}
_TM_INT = {
    "seq", "frame", "uptime_s", "iaq_accuracy", "solar_deployed",
    "star_led", "logging_state", "free_heap", "fs_free",
}

BC_FIELDS = ["seq", "callsign", "uptime_s", "mode", "battery_v", "flags", "free_heap"]

RS_FIELDS = [
    "seq", "callsign", "rf_state", "at_state", "rf_channel", "rf_txpower",
    "rf_baud", "rf_fu_mode", "servo_angle", "wdt_remaining_s", "last_error",
    "rf_resets", "ground_tc_count",
    "rcu_rx_bytes", "rcu_rx_frames_ok", "rcu_rx_frames_bad",
    "frames_ok", "frames_bad", "tc_received", "tc_rejected",
]

# Beacon health bits, mirroring radioSendBeacon().
BEACON_FLAGS = [
    (0, "bme680"), (1, "mpu"), (2, "ads1015"), (3, "ina3221"), (4, "ds3231"),
    (5, "camera"), (6, "solar_deployed"), (7, "logging"), (8, "wifi"), (9, "rcu"),
]

# Which beacon bits are hardware presence (a missing one is a fault) and which
# are ordinary mode state (a missing one is just "not right now").
SUBSYSTEM_FLAGS = {"bme680", "mpu", "ads1015", "ina3221", "ds3231", "camera", "rcu"}
STATE_FLAGS = {"solar_deployed", "logging", "wifi"}

RADIO_MODES = {0: "OFF", 1: "BEACON", 2: "STREAM", 3: "FULL", 9: "RCU-HOLDING"}

RCU_ERRORS = {
    0: "none", 1: "bad i2c frame", 2: "bad command", 3: "bad argument",
    4: "no bus grant", 5: "AT timeout", 6: "AT rejected", 7: "HC-12 unresponsive",
}


def _decode(frame: Frame, names: List[str], ints: set, texts: set) -> Dict:
    out: Dict[str, object] = {}
    for index, name in enumerate(names):
        if name in texts:
            out[name] = frame.get(index)
        elif name in ints:
            out[name] = frame.integer(index)
        else:
            out[name] = frame.num(index)
    return out


def parse_telemetry(frame: Frame) -> Dict:
    data = _decode(frame, TM_FIELDS, _TM_INT, _TM_TEXT)
    data["solar_deployed"] = bool(data.get("solar_deployed"))
    data["star_led"] = bool(data.get("star_led"))
    return data


def parse_beacon(frame: Frame) -> Dict:
    data = _decode(frame, BC_FIELDS, {"seq", "uptime_s", "mode", "flags", "free_heap"}, {"callsign"})
    flags = int(data.get("flags") or 0)
    data["health"] = {name: bool(flags & (1 << bit)) for bit, name in BEACON_FLAGS}
    data["mode_name"] = RADIO_MODES.get(int(data.get("mode") or 0), "?")
    return data


def parse_radio_status(frame: Frame) -> Dict:
    data = _decode(frame, RS_FIELDS, set(RS_FIELDS) - {"callsign"}, {"callsign"})
    data["last_error_text"] = RCU_ERRORS.get(int(data.get("last_error") or 0), "?")
    return data


# ---------------------------------------------------------------------------
# Image reassembly
# ---------------------------------------------------------------------------
@dataclass
class ImageTransfer:
    image_id: int
    chunk_count: int
    byte_count: int
    chunk_size: int
    timestamp: str = ""
    chunks: Dict[int, bytes] = field(default_factory=dict)
    complete: bool = False
    crc_ok: Optional[bool] = None

    @property
    def received(self) -> int:
        return len(self.chunks)

    @property
    def missing(self) -> List[int]:
        return [i for i in range(self.chunk_count) if i not in self.chunks]

    @property
    def progress(self) -> float:
        return (self.received / self.chunk_count) if self.chunk_count else 0.0

    def data(self) -> bytes:
        return b"".join(self.chunks[i] for i in sorted(self.chunks))


class ImageAssembler:
    """
    Collects $IM / $ID / $IE frames into whole JPEGs.

    A chunk lost to a fade leaves a hole; `missing` lists the indices so the
    operator can re-request the image (the link is one-way per frame - there is
    no per-chunk ARQ, which keeps the satellite side simple).
    """

    def __init__(self) -> None:
        self.current: Optional[ImageTransfer] = None
        self.completed: List[ImageTransfer] = []

    def feed(self, frame: Frame) -> Optional[ImageTransfer]:
        if frame.type == T_IMG_META:
            self.current = ImageTransfer(
                image_id=frame.integer(2, 0) or 0,
                chunk_count=frame.integer(3, 0) or 0,
                byte_count=frame.integer(4, 0) or 0,
                chunk_size=frame.integer(5, 0) or 0,
                timestamp=frame.get(6),
            )
            return None

        if frame.type == T_IMG_DATA:
            if self.current is None:
                return None
            image_id = frame.integer(1, -1)
            if image_id != self.current.image_id:
                return None
            index = frame.integer(2, -1)
            try:
                payload = base64.b64decode(frame.get(3), validate=True)
            except (binascii.Error, ValueError):
                return None
            if index is not None and index >= 0:
                self.current.chunks[index] = payload
            return None

        if frame.type == T_IMG_END:
            if self.current is None:
                return None
            transfer = self.current
            transfer.complete = True
            want = frame.get(4).strip()
            if want:
                try:
                    transfer.crc_ok = crc32(transfer.data()) == int(want, 16)
                except ValueError:
                    transfer.crc_ok = None
            self.completed.append(transfer)
            self.current = None
            return transfer

        return None


# ---------------------------------------------------------------------------
# Command catalogue - what the OBC accepts (see radioDispatchTc()).
# ---------------------------------------------------------------------------
COMMANDS = {
    "PING":     "liveness check",
    "TM":       "request one telemetry frame",
    "SYS":      "heap / filesystem / uptime summary",
    "MODE":     "OFF|BEACON|STREAM|FULL - what the radio transmits",
    "TMRATE":   "<ms> telemetry period (250..600000)",
    "BCRATE":   "<ms> beacon period (1000..3600000)",
    "SOLAR":    "DEPLOY|RETRACT|TOGGLE the solar panels",
    "LED":      "ON|OFF|BLINK the STAR LED",
    "CAL":      "zero the MPU gyro (hold the satellite still)",
    "TIME":     "<yyyy> <mm> <dd> <hh> <mi> <ss> set the RTC",
    "CALLSIGN": "<name> change the satellite callsign",
    "SNAP":     "[DOWN] take a photo, optionally downlink it at once",
    "IMGLIST":  "list stored photo ids",
    "IMG":      "<id> downlink a stored photo",
    "IMGABORT": "abort the image downlink in progress",
    "IMGPACE":  "<ms> gap between image chunks (0..2000)",
    "LOGSTART": "<seconds> start CSV mission logging",
    "LOGSTOP":  "stop mission logging",
    "LOGLIST":  "list mission log files",
    "LOGDEL":   "delete all mission logs",
    "EVLOG":    "dump the on-board event log as $EV frames",
    "RFSTAT":   "radio controller status",
    "RFCH":     "<1..100> HC-12 channel (the ground station must follow)",
    "RFPWR":    "<1..8> HC-12 TX power, -1 dBm .. +20 dBm",
    "RFFU":     "<1..4> HC-12 FU mode",
    "RFSYNC":   "find the HC-12's baud rate and force it to 115200",
    "RFAT":     "<AT+...> send a raw AT command to the HC-12 and show the reply",
    "RFCFG":    "read the HC-12's full configuration back (AT+RX)",
    "RFTEST":   "ask the HC-12 for an AT reply - checks its TXD reaches the OBC",
    "RFRESET":  "power-cycle the HC-12",
    "WDT":      "<seconds> RCU watchdog timeout, 0 disables",
    "EMERG":    "ON|OFF arm RCU emergency commanding",
    "WIFI":     "<ssid> <password> join a network and start the web UI",
    "WIFISTAT": "wi-fi state, IP address and RSSI",
    "WIFIOFF":  "disable wi-fi (the radio link is unaffected)",
    "FS":       "filesystem summary",
    "SAFE":     "safe mode: beacon only at 30 s, logging off",
    "REBOOT":   "restart the OBC",
    "HELP":     "command list from the satellite itself",
}

# Commands the radio controller itself answers, addressed to "RCU".
RCU_COMMANDS = {
    "PING":   "is the radio controller alive",
    "RESET":  "power-cycle the HC-12",
    "BEACON": "force an emergency beacon",
    "SOLAR":  "DEPLOY|RETRACT - servo without the OBC",
    "RFCH":   "<1..100> change channel without the OBC",
    "KICK":   "reset the OBC watchdog timer",
    "WDTOFF": "disable the OBC watchdog",
}
