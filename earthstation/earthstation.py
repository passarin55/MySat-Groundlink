#!/usr/bin/env python3
"""
MySat Groundlink - Earth Station

Operates a MySat 1U CubeSat trainer over the HC-12 radio link (or over the USB
programmer cable, which carries exactly the same protocol).

    python earthstation.py --port COM7
    python earthstation.py --list
    python earthstation.py --sim                 # no hardware needed
    python earthstation.py --port COM7 --dash    # + web dashboard on :8800

Inside the console, anything that is not a /command is uplinked as a
telecommand:

    MYSAT> ping
    MYSAT> solar deploy
    MYSAT> mode stream
    MYSAT> snap down
    MYSAT> /rcu ping                             # talk to the radio controller
    MYSAT> /status

Requires pyserial for real links (`pip install pyserial`); the simulator and
the replay reader run on the standard library alone.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import queue
import random
import shlex
import sys
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Dict, List, Optional

import mskproto as msk


def _enable_ansi() -> None:
    """Turn on VT escape processing so async output can clear a line on Windows."""
    if os.name != "nt":
        return
    try:
        import ctypes
        kernel32 = ctypes.windll.kernel32
        handle = kernel32.GetStdHandle(-11)          # STD_OUTPUT_HANDLE
        mode = ctypes.c_uint32()
        if kernel32.GetConsoleMode(handle, ctypes.byref(mode)):
            kernel32.SetConsoleMode(handle, mode.value | 0x0004)
    except Exception:                                 # noqa: BLE001
        pass


class Console:
    """
    Terminal output shared between the REPL and the receive thread.

    The receive thread prints whenever a frame lands, which is usually while
    the operator is halfway through typing a command. Printing plainly there
    scrolls the prompt away and makes a perfectly good link look dead, so every
    asynchronous line clears the current row, prints above it, then redraws the
    prompt.
    """

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self.prompt = ""            # set by the REPL while input() is pending
        self.mute = False           # /mute - suppress asynchronous chatter
        _enable_ansi()

    def out(self, text: str) -> None:
        """Asynchronous output: never disturbs the line being typed."""
        if self.mute:
            return
        with self._lock:
            sys.stdout.write("\r\033[2K" + text + "\n")
            if self.prompt:
                sys.stdout.write(self.prompt)
            sys.stdout.flush()

    def say(self, text: str = "") -> None:
        """Synchronous output, in response to something the operator typed."""
        with self._lock:
            sys.stdout.write(text + "\n")
            sys.stdout.flush()


console = Console()

DEFAULT_BAUD = 115200
DEFAULT_CALLSIGN = "MYSAT"
ACK_TIMEOUT = 6.0
# Commands that put the HC-12 into AT mode take the radio off the air for a
# second or so, so their acknowledgement cannot come back as quickly.
SLOW_COMMANDS = {"RFCFG", "RFAT", "RFTEST", "RFSYNC", "RFCH", "RFPWR", "RFFU", "CAL", "SNAP"}
SLOW_ACK_TIMEOUT = 20.0


# ---------------------------------------------------------------------------
# Links
# ---------------------------------------------------------------------------
class Link:
    """A byte pipe to the spacecraft."""

    name = "link"

    def open(self) -> None: ...
    def close(self) -> None: ...
    def read(self) -> bytes: return b""
    def write(self, data: bytes) -> None: ...
    @property
    def connected(self) -> bool: return False


class SerialLink(Link):
    def __init__(self, port: str, baud: int = DEFAULT_BAUD):
        self.port = port
        self.baud = baud
        self._ser = None
        self.name = f"serial {port} @ {baud}"

    def open(self) -> None:
        try:
            import serial
        except ImportError:
            raise SystemExit("pyserial is required for serial links: pip install pyserial")
        # Build the port unopened so DTR/RTS can be deasserted BEFORE the handle
        # is created. Many USB-serial adapters wire those lines to the ESP32's
        # EN/IO0, and pyserial asserts both by default - which resets the
        # spacecraft every time the ground station connects to it.
        handle = serial.Serial()
        handle.port = self.port
        handle.baudrate = self.baud
        handle.timeout = 0.2
        handle.dtr = False
        handle.rts = False
        handle.open()
        self._ser = handle
        time.sleep(0.2)
        self._ser.reset_input_buffer()

    def close(self) -> None:
        if self._ser:
            try:
                self._ser.close()
            finally:
                self._ser = None

    def read(self) -> bytes:
        if not self._ser:
            return b""
        waiting = getattr(self._ser, "in_waiting", 0)
        return self._ser.read(waiting or 1)

    def write(self, data: bytes) -> None:
        if self._ser:
            self._ser.write(data)
            self._ser.flush()

    @property
    def connected(self) -> bool:
        return bool(self._ser and self._ser.is_open)


class TcpLink(Link):
    """For a serial-over-TCP bridge (ser2net, ESP-Link, a remote station)."""

    def __init__(self, host: str, port: int):
        self.host, self.port = host, port
        self._sock = None
        self.name = f"tcp {host}:{port}"

    def open(self) -> None:
        import socket
        self._sock = socket.create_connection((self.host, self.port), timeout=5)
        self._sock.settimeout(0.2)

    def close(self) -> None:
        if self._sock:
            try:
                self._sock.close()
            finally:
                self._sock = None

    def read(self) -> bytes:
        import socket
        if not self._sock:
            return b""
        try:
            return self._sock.recv(4096)
        except socket.timeout:
            return b""

    def write(self, data: bytes) -> None:
        if self._sock:
            self._sock.sendall(data)

    @property
    def connected(self) -> bool:
        return self._sock is not None


class SimLink(Link):
    """
    A spacecraft simulator that speaks real MSK-1, so the whole ground station
    can be exercised without hardware. Frames are byte-identical to what the
    firmware emits.
    """

    name = "simulator"

    def __init__(self, callsign: str = DEFAULT_CALLSIGN):
        self.callsign = callsign
        self._out = bytearray()
        self._lock = threading.Lock()
        self._seq = 0
        self._t0 = time.time()
        self._last_bc = 0.0
        self._last_tm = 0.0
        self._mode = 2
        self._deployed = False
        self._led = False
        self._photos = [1, 2, 3]
        self._open = False

    def open(self) -> None:
        self._open = True

    def close(self) -> None:
        self._open = False

    @property
    def connected(self) -> bool:
        return self._open

    def _emit(self, text: str) -> None:
        with self._lock:
            self._out.extend(text.encode("ascii", "replace"))

    def _next(self) -> int:
        self._seq += 1
        return self._seq

    def read(self) -> bytes:
        now = time.time()
        if self._mode != 0 and now - self._last_bc > 10:
            self._last_bc = now
            self._emit(msk.build(msk.T_BEACON, self._next(), self.callsign,
                                 int(now - self._t0), self._mode,
                                 f"{3.9 + 0.05 * math.sin(now / 30):.2f}",
                                 0x33F | (0x40 if self._deployed else 0),
                                 180000))
        if self._mode >= 2 and now - self._last_tm > 2:
            self._last_tm = now
            self._emit(self._telemetry(now))

        time.sleep(0.05)
        with self._lock:
            data, self._out = bytes(self._out), bytearray()
        return data

    def _telemetry(self, now: float) -> str:
        t = now - self._t0
        spin = t * 12 % 360
        sun = [max(0, int(900 * math.cos(math.radians(spin + 90 * i)))) for i in range(4)]
        return msk.build(
            msk.T_TELEMETRY, self._next(), self.callsign, int(t / 2), int(t),
            datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S"),
            f"{22.5 + 2 * math.sin(t / 40):.2f}", f"{44 + 6 * math.sin(t / 55):.2f}",
            f"{1008 + 3 * math.sin(t / 90):.2f}", f"{112.5 + 20 * math.sin(t / 70):.1f}",
            f"{60 + 30 * math.sin(t / 100):.1f}", 2,
            f"{18 * math.sin(t / 7):.1f}", f"{12 * math.cos(t / 9):.1f}", f"{spin - 180:.1f}",
            sun[0], sun[1], sun[2], sun[3],
            f"{3.90 + 0.06 * math.sin(t / 30):.3f}", f"{-120 + 40 * math.sin(t / 20):.1f}",
            f"{5.02 + 0.2 * math.sin(t / 25):.3f}",
            f"{max(0, 210 * math.cos(math.radians(spin))):.1f}",
            f"{max(0, 210 * math.cos(math.radians(spin + 180))):.1f}",
            1 if self._deployed else 0, 1 if self._led else 0, 0, 178000, 620000)

    def write(self, data: bytes) -> None:
        decoder = msk.Decoder()
        for kind, item in decoder.feed(data):
            if kind != "frame" or item.type != msk.T_TELECMD:
                continue
            seq = item.integer(0, 0)
            dest, cmd = item.get(1), item.get(2).upper()
            arg = item.get(3)
            if dest not in (self.callsign, "*", "RCU"):
                continue
            if dest == "RCU":
                self._ack(seq, msk.ACK_OK, "RCU alive resets=0")
                continue

            if cmd == "PING":
                self._ack(seq, msk.ACK_OK, "PONG v.2.0-GL (sim)")
            elif cmd == "TM":
                self._ack(seq, msk.ACK_OK, "TM")
                self._emit(self._telemetry(time.time()))
            elif cmd == "MODE":
                self._mode = {"OFF": 0, "BEACON": 1, "STREAM": 2, "FULL": 3}.get(arg.upper(), self._mode)
                self._ack(seq, msk.ACK_OK, f"mode={arg.upper()}")
            elif cmd == "SOLAR":
                self._deployed = arg.upper() != "RETRACT"
                self._ack(seq, msk.ACK_OK, "deploying" if self._deployed else "retracting")
            elif cmd == "LED":
                self._led = arg.upper() == "ON"
                self._ack(seq, msk.ACK_OK, f"led={arg.upper()}")
            elif cmd == "IMGLIST":
                self._ack(seq, msk.ACK_OK, " ".join(str(i) for i in self._photos))
            elif cmd in ("IMG", "SNAP"):
                self._ack(seq, msk.ACK_OK, "downlink #1")
                threading.Thread(target=self._send_image, daemon=True).start()
            elif cmd == "RFSTAT":
                self._ack(seq, msk.ACK_OK, "ch 1 pwr 8 FU3 @115200 [on]")
                self._emit(msk.build(msk.T_RADIO, self._next(), self.callsign,
                                     1, 0, 1, 8, 115200, 3, 170, 45, 0, 0, 0, 12, 0, 4, 0))
            elif cmd == "HELP":
                self._ack(seq, msk.ACK_OK, " ".join(sorted(msk.COMMANDS)))
            else:
                self._ack(seq, msk.ACK_OK, f"{cmd} accepted (sim)")

    def _ack(self, ref: int, status: str, text: str) -> None:
        self._emit(msk.build(msk.T_ACK, self._next(), ref, status, text))

    def _send_image(self) -> None:
        # A tiny valid JPEG-ish payload; enough to exercise reassembly + CRC.
        payload = bytes(random.randrange(256) for _ in range(1500))
        payload = b"\xff\xd8\xff\xe0" + payload + b"\xff\xd9"
        chunk = 132
        chunks = [payload[i:i + chunk] for i in range(0, len(payload), chunk)]
        self._emit(msk.build(msk.T_IMG_META, self._next(), self.callsign, 1,
                             len(chunks), len(payload), chunk,
                             datetime.now().strftime("%Y-%m-%d %H:%M:%S")))
        import base64
        for index, part in enumerate(chunks):
            self._emit(msk.build(msk.T_IMG_DATA, self._next(), 1, index,
                                 base64.b64encode(part).decode()))
            time.sleep(0.02)
        self._emit(msk.build(msk.T_IMG_END, self._next(), 1, len(chunks),
                             len(payload), f"{msk.crc32(payload):08X}"))


class ReplayLink(Link):
    """Replays a captured session log so a pass can be reviewed offline."""

    def __init__(self, path: str, speed: float = 1.0):
        self.path, self.speed = path, speed
        self._lines: List[str] = []
        self._index = 0
        self.name = f"replay {os.path.basename(path)}"

    def open(self) -> None:
        with open(self.path, "r", encoding="utf-8", errors="replace") as handle:
            self._lines = [line for line in handle if line.startswith("$")]

    def close(self) -> None:
        self._index = len(self._lines)

    @property
    def connected(self) -> bool:
        return self._index < len(self._lines)

    def read(self) -> bytes:
        if self._index >= len(self._lines):
            time.sleep(0.2)
            return b""
        line = self._lines[self._index]
        self._index += 1
        time.sleep(0.05 / max(self.speed, 0.01))
        return line.encode()

    def write(self, data: bytes) -> None:
        pass


# ---------------------------------------------------------------------------
# Station
# ---------------------------------------------------------------------------
@dataclass
class LinkStats:
    frames_ok: int = 0
    frames_bad: int = 0
    console_lines: int = 0
    telecommands_sent: int = 0
    acks: int = 0
    bytes_rx: int = 0
    bytes_tx: int = 0
    first_frame: Optional[float] = None
    last_frame: Optional[float] = None
    last_beacon: Optional[float] = None
    last_telemetry: Optional[float] = None

    @property
    def frame_error_rate(self) -> float:
        total = self.frames_ok + self.frames_bad
        return (self.frames_bad / total) if total else 0.0


class Station:
    def __init__(self, link: Link, callsign: str = DEFAULT_CALLSIGN,
                 outdir: str = "groundlink_data", quiet: bool = False):
        self.link = link
        self.callsign = callsign
        self.outdir = outdir
        self.quiet = quiet

        self.decoder = msk.Decoder()
        self.images = msk.ImageAssembler()
        self.stats = LinkStats()

        self.telemetry: deque = deque(maxlen=3000)
        self.beacon: Dict = {}
        self.radio: Dict = {}
        self.events: deque = deque(maxlen=400)
        self.console: deque = deque(maxlen=400)

        self._seq = 0
        self._beacon_signature = None
        self._beacon_printed = 0.0
        self.beacon_print_interval = 60.0
        self._pending: Dict[int, queue.Queue] = {}
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None

        os.makedirs(self.outdir, exist_ok=True)
        os.makedirs(os.path.join(self.outdir, "images"), exist_ok=True)
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        self._csv_path = os.path.join(self.outdir, f"telemetry_{stamp}.csv")
        self._raw_path = os.path.join(self.outdir, f"session_{stamp}.log")
        self._csv = None
        self._csv_writer = None
        self._raw = open(self._raw_path, "a", encoding="utf-8")

    # -- lifecycle ----------------------------------------------------------
    def start(self) -> None:
        self.link.open()
        self._thread = threading.Thread(target=self._rx_loop, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2)
        self.link.close()
        if self._csv:
            self._csv.close()
        self._raw.close()

    # -- receive ------------------------------------------------------------
    def _rx_loop(self) -> None:
        while not self._stop.is_set():
            try:
                data = self.link.read()
            except Exception as exc:                      # noqa: BLE001
                self._note("ERROR", f"link read failed: {exc}")
                time.sleep(0.5)
                continue
            if not data:
                time.sleep(0.02)
                continue

            self.stats.bytes_rx += len(data)
            for kind, item in self.decoder.feed(data):
                if kind == "frame":
                    self._on_frame(item)
                else:
                    self.console.append((time.time(), item))
                    if not self.quiet:
                        console.out(f"  \033[2m{item}\033[0m")

    def _on_frame(self, frame: msk.Frame) -> None:
        now = time.time()
        self.stats.frames_ok = self.decoder.frames_ok
        self.stats.frames_bad = self.decoder.frames_bad
        self.stats.last_frame = now
        if self.stats.first_frame is None:
            self.stats.first_frame = now

        self._raw.write(str(frame) + "\n")
        self._raw.flush()

        if frame.type == msk.T_TELEMETRY:
            record = msk.parse_telemetry(frame)
            record["_rx"] = now
            self.telemetry.append(record)
            self.stats.last_telemetry = now
            self._write_csv(record)

        elif frame.type == msk.T_BEACON:
            self.beacon = msk.parse_beacon(frame)
            self.beacon["_rx"] = now
            self.stats.last_beacon = now
            if not self.quiet and self._beacon_worth_printing():
                health = self.beacon.get("health", {})
                down = [k for k, v in health.items()
                        if not v and k in msk.SUBSYSTEM_FLAGS]
                note = f" degraded: {', '.join(down)}" if down else ""
                console.out(f"  \033[36m[BEACON]\033[0m {self.beacon.get('callsign')} "
                            f"up {self.beacon.get('uptime_s')}s "
                            f"{self.beacon.get('battery_v')} V "
                            f"{self.beacon.get('mode_name')}{note}")

        elif frame.type == msk.T_RADIO:
            self.radio = msk.parse_radio_status(frame)
            self.radio["_rx"] = now

        elif frame.type == msk.T_EVENT:
            level, text = frame.get(2), frame.get(3)
            self.events.append((now, level, text))
            if not self.quiet:
                colour = {"WARN": "33", "ERROR": "31"}.get(level, "32")
                console.out(f"  \033[{colour}m[{level}]\033[0m {text}")

        elif frame.type == msk.T_ACK:
            self.stats.acks += 1
            ref = frame.integer(1, -1)
            with self._lock:
                waiter = self._pending.pop(ref, None)
            if waiter:
                waiter.put(frame)
            elif not self.quiet:
                console.out(f"  \033[35m[ACK {frame.get(2)}]\033[0m {frame.get(3)}")

        elif frame.type in (msk.T_IMG_META, msk.T_IMG_DATA, msk.T_IMG_END):
            done = self.images.feed(frame)
            if frame.type == msk.T_IMG_META and not self.quiet:
                console.out(f"  \033[34m[IMG]\033[0m incoming #{frame.get(2)}: "
                            f"{frame.get(4)} bytes in {frame.get(3)} chunks")
            if done:
                self._save_image(done)

    def _write_csv(self, record: Dict) -> None:
        if self._csv is None:
            self._csv = open(self._csv_path, "w", newline="", encoding="utf-8")
            self._csv_writer = csv.DictWriter(self._csv, fieldnames=["rx_utc"] + msk.TM_FIELDS)
            self._csv_writer.writeheader()
        row = {k: record.get(k) for k in msk.TM_FIELDS}
        row["rx_utc"] = datetime.fromtimestamp(record["_rx"], timezone.utc).isoformat(timespec="seconds")
        self._csv_writer.writerow(row)
        self._csv.flush()

    def _save_image(self, transfer: msk.ImageTransfer) -> None:
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        name = f"img{transfer.image_id:03d}_{stamp}.jpg"
        path = os.path.join(self.outdir, "images", name)
        with open(path, "wb") as handle:
            handle.write(transfer.data())

        if transfer.crc_ok:
            verdict = "CRC ok"
        elif transfer.crc_ok is False:
            verdict = f"CRC MISMATCH, {len(transfer.missing)} chunk(s) lost"
        else:
            verdict = "no CRC"
        console.out(f"  \033[34m[IMG]\033[0m saved {path} "
                    f"({transfer.received}/{transfer.chunk_count} chunks, {verdict})")
        if transfer.missing:
            preview = transfer.missing[:12]
            more = "..." if len(transfer.missing) > 12 else ""
            console.out(f"       missing chunks: {preview}{more} - re-request with 'img "
                        f"{transfer.image_id}'")

    def _beacon_worth_printing(self) -> bool:
        """
        A beacon every 10 s scrolls everything else off the screen and buries
        the acks the operator is actually waiting for. Print one when something
        changes, and otherwise only occasionally as a keep-alive.
        """
        signature = (self.beacon.get("mode"), tuple(sorted(self.beacon.get("health", {}).items())))
        now = time.time()
        changed = signature != self._beacon_signature
        due = (now - self._beacon_printed) >= self.beacon_print_interval
        if changed or due:
            self._beacon_signature = signature
            self._beacon_printed = now
            return True
        return False

    def _note(self, level: str, text: str) -> None:
        self.events.append((time.time(), level, text))
        if not self.quiet:
            console.out(f"  \033[31m[{level}]\033[0m {text}")

    # -- transmit -----------------------------------------------------------
    def send(self, command: str, *args, dest: Optional[str] = None,
             wait: bool = True, timeout: Optional[float] = None) -> Optional[msk.Frame]:
        if timeout is None:
            timeout = SLOW_ACK_TIMEOUT if command.upper() in SLOW_COMMANDS else ACK_TIMEOUT
        with self._lock:
            self._seq += 1
            seq = self._seq
            waiter: Optional[queue.Queue] = queue.Queue(maxsize=1) if wait else None
            if waiter:
                self._pending[seq] = waiter

        frame = msk.build_tc(seq, dest or self.callsign, command, *args)
        payload = frame.encode("ascii")
        self.link.write(payload)
        self.stats.bytes_tx += len(payload)
        self.stats.telecommands_sent += 1

        if not waiter:
            return None
        try:
            return waiter.get(timeout=timeout)
        except queue.Empty:
            with self._lock:
                self._pending.pop(seq, None)
            return None

    def send_raw(self, text: str) -> None:
        """Sends a bare console line (the stock human command set)."""
        payload = (text + "\n").encode("ascii", "replace")
        self.link.write(payload)
        self.stats.bytes_tx += len(payload)

    # -- reporting ----------------------------------------------------------
    def snapshot(self) -> Dict:
        latest = self.telemetry[-1] if self.telemetry else {}
        now = time.time()
        return {
            "link": self.link.name,
            "connected": self.link.connected,
            "callsign": self.callsign,
            "stats": {
                "frames_ok": self.stats.frames_ok,
                "frames_bad": self.stats.frames_bad,
                "frame_error_rate": round(self.stats.frame_error_rate, 4),
                "telecommands_sent": self.stats.telecommands_sent,
                "acks": self.stats.acks,
                "bytes_rx": self.stats.bytes_rx,
                "bytes_tx": self.stats.bytes_tx,
                "age_last_frame": round(now - self.stats.last_frame, 1) if self.stats.last_frame else None,
                "age_last_beacon": round(now - self.stats.last_beacon, 1) if self.stats.last_beacon else None,
            },
            "beacon": {k: v for k, v in self.beacon.items() if not k.startswith("_")},
            "radio": {k: v for k, v in self.radio.items() if not k.startswith("_")},
            "telemetry": {k: v for k, v in latest.items() if not k.startswith("_")},
            "history": [
                {"t": round(r["_rx"], 1), "battery_v": r.get("battery_v"),
                 "temperature": r.get("temperature"), "iaq": r.get("iaq"),
                 "roll": r.get("roll"), "pitch": r.get("pitch"), "yaw": r.get("yaw"),
                 "ph1": r.get("ph1"), "ph2": r.get("ph2"),
                 "ph3": r.get("ph3"), "ph4": r.get("ph4")}
                for r in list(self.telemetry)[-180:]
            ],
            "events": [{"t": round(t, 1), "level": lvl, "text": txt}
                       for t, lvl, txt in list(self.events)[-40:]],
            "image": ({
                "id": self.images.current.image_id,
                "progress": round(self.images.current.progress, 3),
                "received": self.images.current.received,
                "total": self.images.current.chunk_count,
            } if self.images.current else None),
        }

    def print_status(self) -> None:
        snap = self.snapshot()
        tm = snap["telemetry"]
        bc = snap["beacon"]
        st = snap["stats"]

        print()
        print(f"  \033[1mEARTH STATION\033[0m  link: {snap['link']}  target: {self.callsign}")
        print(f"  frames {st['frames_ok']} ok / {st['frames_bad']} bad "
              f"({st['frame_error_rate'] * 100:.1f}% FER)   "
              f"TC {st['telecommands_sent']} sent, {st['acks']} acked")
        age = st["age_last_frame"]
        print(f"  last frame: {('%.1f s ago' % age) if age is not None else 'never'}"
              f"   last beacon: "
              f"{('%.0f s ago' % st['age_last_beacon']) if st['age_last_beacon'] is not None else 'never'}")

        if bc:
            # Two kinds of beacon flag: hardware presence, where a missing one is
            # a fault, and ordinary mode state, where it just means "not right
            # now". Reporting "solar_deployed" under "down/absent" made stowed
            # panels look like a broken subsystem.
            health = bc.get("health", {})
            up = [k for k, v in health.items() if v and k in msk.SUBSYSTEM_FLAGS]
            down = [k for k, v in health.items() if not v and k in msk.SUBSYSTEM_FLAGS]
            active = [k for k, v in health.items() if v and k in msk.STATE_FLAGS]
            print(f"  mode {bc.get('mode_name')}   sensors up: {', '.join(up) or 'none'}")
            if down:
                print(f"  \033[33msensors down/absent:\033[0m {', '.join(down)}")
            print(f"  active: {', '.join(active) or 'none'}")

        if tm:
            print(f"  power   {tm.get('battery_v')} V  {tm.get('battery_ma')} mA   "
                  f"solar {tm.get('solar_v')} V  L {tm.get('solar_ma_left')} / "
                  f"R {tm.get('solar_ma_right')} mA")
            print(f"  env     {tm.get('temperature')} C  {tm.get('humidity')} %RH  "
                  f"{tm.get('pressure')} hPa  gas {tm.get('gas_resistance')} kOhm  "
                  f"IAQ {tm.get('iaq')} (acc {tm.get('iaq_accuracy')})")
            print(f"  attitude roll {tm.get('roll')}  pitch {tm.get('pitch')}  yaw {tm.get('yaw')}")
            print(f"  sun     {tm.get('ph1')} {tm.get('ph2')} {tm.get('ph3')} {tm.get('ph4')}")
            print(f"  panels  {'DEPLOYED' if tm.get('solar_deployed') else 'stowed'}   "
                  f"STAR LED {'on' if tm.get('star_led') else 'off'}   "
                  f"heap {tm.get('free_heap')}")

        if snap["radio"]:
            rs = snap["radio"]
            print(f"  radio   ch {rs.get('rf_channel')}  power {rs.get('rf_txpower')}  "
                  f"FU{rs.get('rf_fu_mode')}  {rs.get('rf_baud')} baud   "
                  f"resets {rs.get('rf_resets')}   err: {rs.get('last_error_text')}")

        if not tm:
            mode = bc.get("mode_name")
            if mode in ("BEACON", "OFF"):
                print(f"  \033[33mno telemetry:\033[0m the satellite is in {mode} mode, which "
                      f"sends beacons only.")
                print(f"           send 'mode stream' to get sensor readings.")
            elif self.stats.last_frame:
                print("  no telemetry frame received yet")

        if snap["image"]:
            img = snap["image"]
            print(f"  image   #{img['id']} {img['received']}/{img['total']} chunks "
                  f"({img['progress'] * 100:.0f}%)")
        print(f"  logging telemetry -> {self._csv_path}")
        print(f"          raw frames -> {self._raw_path}")
        print()


# ---------------------------------------------------------------------------
# Console
# ---------------------------------------------------------------------------
HELP = """
  Telecommands (anything not starting with '/'):
     ping | tm | sys | help
     mode off|beacon|stream|full      tmrate <ms>       bcrate <ms>
     solar deploy|retract|toggle      led on|off|blink  cal
     snap [down]   imglist   img <id>   imgabort   imgpace <ms>
     logstart <s>  logstop  loglist  logdel  evlog
     rfstat  rfch <1..100>  rfpwr <1..8>  rffu <1..4>  rfreset
     wdt <s>  emerg on|off  fs  safe  reboot
     time <yyyy> <mm> <dd> <hh> <mm> <ss>     callsign <name>

  Station commands:
     /status              link + spacecraft summary
     /watch [seconds]     live telemetry, Ctrl-C to stop
     /rcu <cmd> [args]    address the radio controller directly (emergency path)
     /raw <text>          send a bare console line (stock command set)
     /images              list received images
     /events              recent spacecraft events
     /callsign <name>     which callsign this station addresses
     /dash [port]         start the web dashboard
     /ports               list serial ports
     /mute  /unmute       silence incoming chatter while you type
     /beacons [seconds]   how often to print a beacon keep-alive (default 60)
     /help  /quit

  Incoming frames print ABOVE the prompt and redraw it, so you can keep typing
  while telemetry arrives. Beacons are summarised - one line a minute, plus one
  immediately whenever a subsystem or the radio mode changes.
"""


def list_ports() -> List[str]:
    try:
        from serial.tools import list_ports as lp
    except ImportError:
        return []
    return [f"{p.device:10s} {p.description}" for p in lp.comports()]


def watch(station: Station, seconds: float) -> None:
    end = time.time() + seconds
    print("  watching telemetry - Ctrl-C to stop")
    last_seq = None
    try:
        while time.time() < end:
            if station.telemetry:
                tm = station.telemetry[-1]
                if tm.get("seq") != last_seq:
                    last_seq = tm.get("seq")
                    print(f"  {tm.get('utc')}  {tm.get('battery_v')} V  "
                          f"{tm.get('temperature')} C  IAQ {tm.get('iaq')}  "
                          f"rpy {tm.get('roll')}/{tm.get('pitch')}/{tm.get('yaw')}  "
                          f"sun {tm.get('ph1')},{tm.get('ph2')},{tm.get('ph3')},{tm.get('ph4')}")
            time.sleep(0.3)
    except KeyboardInterrupt:
        print("\n  watch stopped")


def repl(station: Station, dash_port: Optional[int] = None) -> None:
    if dash_port:
        start_dashboard(station, dash_port)

    print(HELP)
    station.print_status()

    while True:
        prompt = f"{station.callsign}> "
        try:
            # Telling the console what the prompt looks like lets the receive
            # thread redraw it after printing, so incoming frames never leave
            # the operator typing into an invisible line.
            console.prompt = prompt
            line = input(prompt).strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break
        finally:
            console.prompt = ""
        if not line:
            continue

        if line.startswith("/"):
            parts = shlex.split(line[1:])
            if not parts:
                continue
            cmd, args = parts[0].lower(), parts[1:]

            if cmd in ("quit", "exit", "q"):
                break
            elif cmd == "help":
                print(HELP)
            elif cmd == "status":
                station.print_status()
            elif cmd == "watch":
                watch(station, float(args[0]) if args else 60.0)
            elif cmd == "ports":
                ports = list_ports()
                print("  " + ("\n  ".join(ports) if ports else "no serial ports found"))
            elif cmd == "raw":
                station.send_raw(" ".join(args))
                print("  sent as console text")
            elif cmd == "rcu":
                if not args:
                    print("  usage: /rcu <PING|RESET|BEACON|SOLAR DEPLOY|RFCH n|KICK|WDTOFF>")
                    continue
                ack = station.send(args[0], *args[1:], dest="RCU")
                print(f"  {('RCU ' + ack.get(2) + ': ' + ack.get(3)) if ack else 'no answer from the RCU'}")
            elif cmd == "callsign":
                if args:
                    station.callsign = args[0]
                print(f"  addressing {station.callsign}")
            elif cmd == "images":
                if not station.images.completed:
                    print("  no images received yet")
                for t in station.images.completed:
                    print(f"  #{t.image_id}  {t.byte_count} bytes  "
                          f"{t.received}/{t.chunk_count} chunks  "
                          f"crc={'ok' if t.crc_ok else ('BAD' if t.crc_ok is False else '?')}")
            elif cmd == "events":
                for t, level, text in list(station.events)[-25:]:
                    print(f"  {datetime.fromtimestamp(t).strftime('%H:%M:%S')} [{level}] {text}")
            elif cmd == "dash":
                start_dashboard(station, int(args[0]) if args else 8800)
            elif cmd in ("mute", "unmute"):
                console.mute = (cmd == "mute")
                print(f"  asynchronous output {'muted' if console.mute else 'live'} "
                      f"(telemetry is still received and logged)")
            elif cmd == "beacons":
                if args:
                    station.beacon_print_interval = float(args[0])
                print(f"  beacon keep-alive line every "
                      f"{station.beacon_print_interval:.0f} s "
                      f"(and immediately on any health or mode change)")
            else:
                print(f"  unknown station command '/{cmd}' - try /help")
            continue

        parts = line.split()
        command, args = parts[0], parts[1:]
        if command.upper() not in msk.COMMANDS:
            print(f"  '{command}' is not a known telecommand. /help lists them, "
                  f"or use /raw to send it as console text anyway.")
            continue

        ack = station.send(command, *args)
        if ack is None:
            print("  \033[33mno acknowledgement\033[0m - the frame may not have "
                  "reached the satellite (check channel, power and antenna)")
        else:
            status = ack.get(2)
            colour = "32" if status == msk.ACK_OK else "33"
            print(f"  \033[{colour}m{status}\033[0m {ack.get(3)}")


# ---------------------------------------------------------------------------
def start_dashboard(station: Station, port: int) -> None:
    try:
        import dashboard
    except ImportError:
        print("  dashboard.py not found next to earthstation.py")
        return
    dashboard.serve(station, port)


def main() -> int:
    parser = argparse.ArgumentParser(description="MySat Groundlink earth station")
    parser.add_argument("--port", help="serial port, e.g. COM7 or /dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument("--tcp", help="host:port of a serial-over-TCP bridge")
    parser.add_argument("--sim", action="store_true", help="run against the built-in simulator")
    parser.add_argument("--replay", help="replay a session_*.log file")
    parser.add_argument("--speed", type=float, default=1.0, help="replay speed factor")
    parser.add_argument("--callsign", default=DEFAULT_CALLSIGN)
    parser.add_argument("--outdir", default="groundlink_data")
    parser.add_argument("--dash", nargs="?", const=8800, type=int,
                        help="also serve the web dashboard (default port 8800)")
    parser.add_argument("--list", action="store_true", help="list serial ports and exit")
    parser.add_argument("--quiet", action="store_true", help="do not echo frames as they arrive")
    args = parser.parse_args()

    if args.list:
        ports = list_ports()
        print("\n".join(ports) if ports else
              "No serial ports found (install pyserial: pip install pyserial)")
        return 0

    if args.sim:
        link: Link = SimLink(args.callsign)
    elif args.replay:
        link = ReplayLink(args.replay, args.speed)
    elif args.tcp:
        host, _, tcp_port = args.tcp.partition(":")
        link = TcpLink(host, int(tcp_port or 23))
    elif args.port:
        link = SerialLink(args.port, args.baud)
    else:
        ports = list_ports()
        print("No link specified. Use --port, --tcp, --replay or --sim.")
        if ports:
            print("\nAvailable serial ports:\n  " + "\n  ".join(ports))
        return 2

    station = Station(link, callsign=args.callsign, outdir=args.outdir, quiet=args.quiet)
    try:
        station.start()
    except Exception as exc:                              # noqa: BLE001
        print(f"Could not open {link.name}: {exc}")
        return 1

    print(f"\n  MySat Groundlink earth station - {link.name}")
    print(f"  data directory: {os.path.abspath(args.outdir)}")

    try:
        repl(station, args.dash)
    finally:
        station.stop()
        print("  link closed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
