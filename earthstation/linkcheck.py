#!/usr/bin/env python3
"""
linkcheck - isolate where an uplink failure lives.

    python linkcheck.py COM11            # test one port
    python linkcheck.py COM11 COM3       # compare the radio and the cable

For each port it runs four probes:

  1. LISTEN     is anything arriving, and does it decode?
  2. TC PING    a telecommand addressed to the callsign
  3. TC PING *  the same, broadcast - passes even if the callsign is wrong
  4. Help       a bare console line, which the OBC prints back ungated

The pattern of which probes pass tells you what is broken:

  downlink ok, nothing else        -> uplink is not physically reaching the OBC
  console works, TC does not       -> frame/CRC problem, not a wiring problem
  broadcast works, callsign fails  -> the station is addressing the wrong callsign
  everything fails on both ports   -> the OBC is not running this firmware

Read-only apart from the probes it sends; none of them move hardware.
"""

from __future__ import annotations

import sys
import time

import mskproto as msk

BAUD = 115200
LISTEN_S = 12.0
REPLY_S = 6.0


def open_port(port: str):
    import serial
    handle = serial.Serial()
    handle.port = port
    handle.baudrate = BAUD
    handle.timeout = 0.15
    # Do not let opening the port bounce the ESP32 via the CP210x handshake
    # lines - a reset mid-test would look exactly like a dead uplink.
    handle.dtr = False
    handle.rts = False
    handle.open()
    time.sleep(0.3)
    handle.reset_input_buffer()
    return handle


def collect(handle, seconds: float, decoder: msk.Decoder):
    """Reads for `seconds`, returning (frames, console_lines, raw_bytes)."""
    frames, lines, raw = [], [], 0
    end = time.time() + seconds
    while time.time() < end:
        chunk = handle.read(handle.in_waiting or 1)
        if not chunk:
            continue
        raw += len(chunk)
        for kind, item in decoder.feed(chunk):
            (frames if kind == "frame" else lines).append(item)
    return frames, lines, raw


def wait_for(handle, decoder, seconds, predicate):
    """Reads until `predicate(frame_or_line)` is true or the time runs out."""
    end = time.time() + seconds
    seen_frames, seen_lines = [], []
    while time.time() < end:
        chunk = handle.read(handle.in_waiting or 1)
        if not chunk:
            continue
        for kind, item in decoder.feed(chunk):
            (seen_frames if kind == "frame" else seen_lines).append(item)
            if predicate(kind, item):
                return item, seen_frames, seen_lines
    return None, seen_frames, seen_lines


def probe(port: str, callsign: str = "MYSAT") -> dict:
    print(f"\n{'=' * 66}\n  {port} @ {BAUD}\n{'=' * 66}")
    result = {"port": port, "listen": False, "tc": False, "broadcast": False, "console": False}

    try:
        handle = open_port(port)
    except Exception as exc:                                   # noqa: BLE001
        print(f"  cannot open: {exc}")
        return result

    decoder = msk.Decoder()
    try:
        # -- 1. listen ------------------------------------------------------
        print(f"\n  [1] LISTEN {LISTEN_S:.0f} s ...")
        frames, lines, raw = collect(handle, LISTEN_S, decoder)
        kinds = {}
        for f in frames:
            kinds[f.type] = kinds.get(f.type, 0) + 1
        print(f"      {raw} bytes, {len(frames)} valid frames "
              f"({decoder.frames_bad} rejected), {len(lines)} console lines")
        if kinds:
            print(f"      frame types: {kinds}")
        for f in frames:
            if f.type == msk.T_BEACON:
                bc = msk.parse_beacon(f)
                print(f"      beacon callsign = '{bc['callsign']}'  "
                      f"mode = {bc['mode_name']}  up {bc['uptime_s']} s")
                result["callsign"] = bc["callsign"]
                break
        for line in lines[:4]:
            print(f"      text: {line[:70]}")
        result["listen"] = bool(frames or lines)
        if not result["listen"]:
            print("      NOTHING RECEIVED on this port")

        # -- 2. telecommand to the callsign ---------------------------------
        target = result.get("callsign") or callsign
        print(f"\n  [2] TC PING -> '{target}' ...")
        frame = msk.build_tc(9001, target, "PING")
        print(f"      tx: {frame.strip()}")
        handle.reset_input_buffer()
        handle.write(frame.encode())
        handle.flush()
        ack, _, _ = wait_for(handle, decoder, REPLY_S,
                             lambda k, i: k == "frame" and i.type == msk.T_ACK
                             and i.integer(1) == 9001)
        if ack:
            print(f"      ACK: {ack.get(2)} {ack.get(3)}")
            result["tc"] = True
        else:
            print("      no ack")

        # -- 3. broadcast ----------------------------------------------------
        print("\n  [3] TC PING -> '*' (broadcast) ...")
        frame = msk.build_tc(9002, "*", "PING")
        handle.reset_input_buffer()
        handle.write(frame.encode())
        handle.flush()
        ack, _, _ = wait_for(handle, decoder, REPLY_S,
                             lambda k, i: k == "frame" and i.type == msk.T_ACK
                             and i.integer(1) == 9002)
        if ack:
            print(f"      ACK: {ack.get(2)} {ack.get(3)}")
            result["broadcast"] = True
        else:
            print("      no ack")

        # -- 4. bare console line -------------------------------------------
        # printConsoleHelp() writes with raw Serial.println, so it comes back
        # even in BEACON mode where ordinary console text is suppressed.
        print("\n  [4] console line 'Help' ...")
        handle.reset_input_buffer()
        handle.write(b"Help\n")
        handle.flush()
        hit, _, seen = wait_for(handle, decoder, REPLY_S,
                                lambda k, i: k == "text" and "MYSAT CONSOLE" in i)
        if hit:
            print(f"      got: {hit[:70]}")
            result["console"] = True
        else:
            print("      no console response")
            for line in seen[:3]:
                print(f"      (saw: {line[:60]})")
    finally:
        handle.close()

    return result


def verdict(results):
    print(f"\n{'=' * 66}\n  SUMMARY\n{'=' * 66}")
    print(f"  {'port':8s} {'downlink':>9s} {'TC':>5s} {'bcast':>6s} {'console':>8s}")
    for r in results:
        print(f"  {r['port']:8s} {str(r['listen']):>9s} {str(r['tc']):>5s} "
              f"{str(r['broadcast']):>6s} {str(r['console']):>8s}")

    print()
    for r in results:
        port = r["port"]
        if not r["listen"]:
            print(f"  {port}: nothing arriving at all - wrong port, wrong baud, or the "
                  f"module is unpowered.")
        elif r["tc"] or r["broadcast"]:
            if not r["tc"] and r["broadcast"]:
                cs = r.get("callsign", "?")
                print(f"  {port}: uplink works, but only broadcast. The station must "
                      f"address '{cs}'.")
            else:
                print(f"  {port}: uplink and downlink both fine.")
        elif r["console"]:
            print(f"  {port}: bytes DO reach the OBC (console answered) but MSK frames "
                  f"do not decode there. Suspect a corrupted uplink byte stream - "
                  f"drop the HC-12 to a slower air rate, or check for framing errors.")
        else:
            print(f"  {port}: downlink works, uplink is dead - no byte you send reaches "
                  f"the OBC.")


def main() -> int:
    ports = sys.argv[1:]
    if not ports:
        print(__doc__)
        try:
            from serial.tools import list_ports
            print("Ports on this machine:")
            for p in list_ports.comports():
                print(f"  {p.device:8s} {p.description}")
        except ImportError:
            print("pyserial not installed")
        return 2

    results = [probe(p) for p in ports]
    verdict(results)
    return 0


if __name__ == "__main__":
    sys.exit(main())
