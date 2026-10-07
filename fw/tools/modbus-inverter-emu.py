#!/usr/bin/env python3
"""modbus-inverter-emu.py - bench harness for the solar-ctl RS485 link.

Two roles, one file (bench harness only - never shipped to the target):

  slave (default)  a fake Deye-ish Modbus RTU inverter on the laptop's
                   USB-RS485 dongle: answers FC 03/04 reads from a small
                   animated register map, accepts FC 06 writes, and
                   logs every frame in hex.
  master (--master)  polls a remote slave (the board's echo test today,
                   the future inverter) with FC03 reads and logs replies.

Zero dependencies on purpose (user policy): raw termios + select. RTU
framing is byte-gap based: a gap > t1.5 char-times closes a frame
(t=3.5 char-times is the spec bound; we split slightly tighter).

The USB dongle's DE/DE-enable is assumed auto-direction (typical for
CH34x/FT232 RS485 dongles); nothing here touches TIOCSRS485 host-side.

The register map below is INVENTED, not Deye documentation - just
plausible values that move, to prove framing/CRC/direction control.
Replace with real Deye SUN register addresses when wiring the actual
protocol.
"""

from __future__ import annotations

import argparse
import os
import select
import struct
import sys
import termios
import time

# ------------------------------------------------------------------ CRC16

def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc

def append_crc(pdu: bytes) -> bytes:
    return pdu + struct.pack("<H", crc16(pdu))

# ------------------------------------------------------------------ serial

BAUDS = {v: getattr(termios, f"B{v}") for v in (1200, 4800, 9600, 19200, 38400, 115200)}

def open_serial(dev: str, baud: int) -> int:
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = a[1] = 0                       # raw in/out: no echo, no xlate
    a[2] = BAUDS[baud] | termios.CS8 | termios.CLOCAL | termios.CREAD
    a[3] = 0                              # no icanon/isig
    a[4] = a[5] = BAUDS[baud]
    a[6] = [0] * len(a[6])                # VMIN=0 VTIME=0: pure polling
    termios.tcsetattr(fd, termios.TCSANOW, a)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd

class RtuFrameReader:
    """RTU framing = silence between frames. 3.5 char times at baud
    (11 bits/char) ends a frame; also a hard cap per frame."""

    def __init__(self, fd: int, baud: int):
        self.fd = fd
        self.t_idle = max(0.0015, 4.0 * 11.0 / baud)

    def read(self, timeout: float) -> bytes | None:
        frame = b""
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            r, _, _ = select.select([self.fd], [], [], min(remaining, self.t_idle))
            if r:
                try:
                    chunk = os.read(self.fd, 512)
                except BlockingIOError:
                    chunk = b""
                if chunk:
                    frame += chunk
                    continue
            elif frame:
                break                       # silence after bytes: frame done
        return frame or None

# ------------------------------------------------------------------ slave

class InverterEmu:
    """Small animated holding/input register map + the RTU glue."""

    def __init__(self, fd: int, addr: int):
        self.fd = fd
        self.addr = addr
        self.regs = {                       # invented placeholders only
            0x0000: 4800,   # "PV1 voltage" 0.1 V
            0x0001: 8500,   # "PV1 power"   W
            0x0002: 2300,   # "grid V"      0.1 V
            0x0003: 5000,   # "grid Hz"     0.01 Hz
            0x0004: 87,     # "battery SOC" %
        }
        self.tick = 0

    def animate(self) -> None:
        self.tick += 1
        self.regs[0x0000] = 4800 + 300 * ((self.tick // 5) % 3)
        self.regs[0x0001] = 8500 + 1200 * ((self.tick // 7) % 2)
        self.regs[0x0004] = min(100, 87 + (self.tick // 60) % 13)

    def _reply(self, resp: bytes, log: bool) -> None:
        if log:
            print(f"[tx] {resp.hex(' ')}", flush=True)
        os.write(self.fd, append_crc(resp))

    def _exception(self, fc: int, code: int, log: bool) -> None:
        self._reply(bytes([self.addr, fc | 0x80, code]), log)

    def handle(self, frame: bytes, log: bool) -> None:
        if len(frame) < 4:
            return                          # not even addr+fc+crc: ignore
        body, crc = frame[:-2], struct.unpack("<H", frame[-2:])[0]
        if crc != crc16(body):
            print(f"[rx] CRC mismatch, ignored: {frame.hex(' ')}", flush=True)
            return
        addr, fc = body[0], body[1]
        if log:
            print(f"[rx] {frame.hex(' ')}", flush=True)
        if addr != self.addr and addr != 0x00:   # broadcast: no reply
            return
        pdu = body[2:]

        if fc in (0x03, 0x04):
            if len(pdu) < 4:
                self._exception(fc, 0x03, log); return
            start, count = struct.unpack(">HH", pdu[:4])
            if count == 0 or count > 125:
                self._exception(fc, 0x03, log); return
            if any(start + i not in self.regs for i in range(count)):
                self._exception(fc, 0x02, log); return
            data = b"".join(struct.pack(">H", self.regs[start + i]) for i in range(count))
            self._reply(bytes([self.addr, fc, len(data)]) + data, log)
        elif fc == 0x06:
            if len(pdu) < 4:
                self._exception(fc, 0x03, log); return
            start, val = struct.unpack(">HH", pdu[:4])
            if start not in self.regs:
                self._exception(fc, 0x02, log); return
            self.regs[start] = val
            self._reply(body[0:6], log)     # echo addr+fc+addr+value
        elif fc == 0x08:                    # diagnostics echo
            if len(pdu) < 2:
                self._exception(fc, 0x03, log); return
            self._reply(body[0:2] + pdu[:2], log)
        else:
            self._exception(fc, 0x01, log)  # illegal function

# ------------------------------------------------------------------ loops

def run_slave(dev: str, baud: int, addr: int, log: bool) -> None:
    fd = open_serial(dev, baud)
    emu = InverterEmu(fd, addr)
    reader = RtuFrameReader(fd, baud)
    print(f"slave: {dev} @{baud} addr={addr} (Ctrl-C to stop)", flush=True)
    while True:
        frame = reader.read(timeout=0.2)
        if frame is None:
            emu.animate()   # advances on any short idle, even while polled
            continue
        emu.handle(frame, log)

def run_master(dev: str, baud: int, addr: int, start: int, count: int,
               interval: float, log: bool) -> None:
    fd = open_serial(dev, baud)
    reader = RtuFrameReader(fd, baud)
    req = append_crc(bytes([addr, 0x03]) + struct.pack(">HH", start, count))
    print(f"master: polling {dev} @{baud} slave {addr} "
          f"regs {start}+{count} every {interval}s (Ctrl-C to stop)", flush=True)
    n = 0
    while True:
        n += 1
        if log:
            print(f"[tx] {req.hex(' ')}", flush=True)
        os.write(fd, req)
        reply = reader.read(timeout=0.5)
        if reply is None:
            print(f"[{n:4d}] -- no reply (timeout)", flush=True)
            continue
        body, crc = reply[:-2], struct.unpack("<H", reply[-2:])[0]
        if len(reply) < 4 or crc != crc16(body):
            print(f"[{n:4d}] BAD CRC/frame: {reply.hex(' ')}", flush=True)
            continue
        if log:
            print(f"[rx] {reply.hex(' ')}", flush=True)
        if body[1] & 0x80:
            print(f"[{n:4d}] exception fc={body[1] & 0x7F:02X} code={body[2]}", flush=True)
        else:
            data = body[3:3 + 2 * count]
            if len(data) != 2 * count:
                # valid CRC but not our register reply (e.g. a raw echo test)
                print(f"[{n:4d}] echo/short data, {len(reply)} B: {reply.hex(' ')}", flush=True)
                continue
            vals = struct.unpack(f">{count}H", data)
            print(f"[{n:4d}] " + " ".join(f"{start + i}={v}" for i, v in enumerate(vals)), flush=True)
        time.sleep(interval)

# ------------------------------------------------------------------ CLI

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dev", help="serial device, e.g. /dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=9600, choices=sorted(BAUDS))
    ap.add_argument("--addr", type=lambda s: int(s, 0), default=1,
                    help="slave address (default 1)")
    ap.add_argument("--master", action="store_true",
                    help="poll a remote slave instead of emulating one")
    ap.add_argument("--interval", type=float, default=1.0,
                    help="master poll interval, s (default 1)")
    ap.add_argument("--start", type=lambda s: int(s, 0), default=0,
                    help="master: first register (default 0)")
    ap.add_argument("--count", type=int, default=5, help="master: register count (default 5)")
    ap.add_argument("--quiet", action="store_true", help="hide per-frame hex logs")
    args = ap.parse_args()

    try:
        if args.master:
            run_master(args.dev, args.baud, args.addr, args.start, args.count,
                       args.interval, not args.quiet)
        else:
            run_slave(args.dev, args.baud, args.addr, not args.quiet)
    except KeyboardInterrupt:
        print("bye", flush=True)
        return 0
    except OSError as e:
        print(f"error: {args.dev}: {e}", file=sys.stderr)
        return 1

if __name__ == "__main__":
    sys.exit(main())
