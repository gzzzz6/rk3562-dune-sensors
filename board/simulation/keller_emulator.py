#!/usr/bin/env python3
"""KELLER bus emulator on a POSIX PTY. Never opens a physical serial device."""
import argparse
import json
import math
import os
import pty
import select
import signal
import struct
import time
import tty
from pathlib import Path


def crc16(data):
    crc = 0xffff
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xa001 if crc & 1 else 0)
    return crc


def frame(data):
    return data + struct.pack('>H', crc16(data))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--device-link', required=True)
    parser.add_argument('--control', required=True, type=Path)
    parser.add_argument('--log', required=True, type=Path)
    args = parser.parse_args()
    master, slave = pty.openpty()
    tty.setraw(slave)
    # Private link must not replace any existing device or user file.
    os.symlink(os.ttyname(slave), args.device_link)
    stopping = False
    def stop(sig, _):
        nonlocal stopping
        stopping = True
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    buffer = bytearray()
    samples = 0
    try:
        with args.log.open('a', buffering=1) as log:
            def record(**data):
                log.write(json.dumps(dict(mono=time.monotonic(), wall=time.time(), **data))+'\n')
            record(event='ready', device=os.ttyname(slave))
            while not stopping:
                if not select.select([master], [], [], .1)[0]:
                    continue
                buffer.extend(os.read(master, 4096))
                while len(buffer) >= 4:
                    length = 5 if buffer[1] == 73 else 4
                    if len(buffer) < length:
                        break
                    tx = bytes(buffer[:length])
                    del buffer[:length]
                    allowed = tx in (frame(bytes([1, 48])), frame(bytes([1, 73, 1])))
                    record(event='command', hex=tx.hex(), allowed=allowed)
                    if not allowed:
                        continue
                    control = json.loads(args.control.read_text())
                    mode = control.get('mode', 'normal')
                    if control.get('echo', False):
                        os.write(master, tx)
                    if mode == 'silent':
                        continue
                    if tx[1] == 48:
                        response = frame(bytes([1, 48, 5, control.get('group', 21), 26, 1, 20, 0]))
                    else:
                        samples += 1
                        pressure = float(control.get('pressure_bar', 2))
                        status = int(control.get('status', 0))
                        if mode == 'nan':
                            pressure = math.nan
                        elif mode == 'infinity':
                            pressure = math.inf
                        if mode == 'uninitialized':
                            response = frame(bytes([1, 73 | 128, 32]))
                        else:
                            response = frame(bytes([1, 73])+struct.pack('>fB', pressure, status))
                        record(event='sample', sequence=samples, mode=mode,
                               pressure_bar=pressure if math.isfinite(pressure) else str(pressure), status=status,
                               response=response.hex())
                        if mode == 'crc':
                            response = response[:-1]+bytes([response[-1] ^ 1])
                        elif mode == 'wrong-address':
                            response = frame(bytes([2])+response[1:-2])
                    delay = float(control.get('delay', .005))
                    time.sleep(delay)
                    if control.get('noise', False):
                        os.write(master, b'\xff\x00\xaa')
                    chunk = int(control.get('chunk', 100))
                    for offset in range(0, len(response), chunk):
                        if stopping:
                            break
                        os.write(master, response[offset:offset+chunk])
                        if chunk < len(response):
                            time.sleep(.002)
    finally:
        os.close(master)
        os.close(slave)
        os.unlink(args.device_link)


if __name__ == '__main__':
    main()
