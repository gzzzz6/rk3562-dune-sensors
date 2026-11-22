#!/usr/bin/env python3
"""Read the KELLER P1 surface reference in bar. No zero, reset or write commands."""

import argparse
import fcntl
import math
import os
import select
import statistics
import struct
import termios
import time
import tty
from pathlib import Path

from kit import DB


def crc16(data):
    crc = 0xffff
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xa001 if crc & 1 else 0)
    return crc


def transact(fd, address, function, echo):
    request = bytes([address, function] + ([1] if function == 73 else []))
    request += struct.pack('>H', crc16(request))
    termios.tcflush(fd, termios.TCIFLUSH)
    if os.write(fd, request) != len(request):
        raise OSError('short request')
    deadline = time.monotonic() + 0.5
    pending_echo = request if echo else b''
    buffer = bytearray()
    while time.monotonic() < deadline:
        readable, _, _ = select.select([fd], [], [], max(0, deadline - time.monotonic()))
        if not readable:
            break
        for byte in os.read(fd, 256):
            if pending_echo:
                if byte != pending_echo[0]:
                    raise ValueError('local echo mismatch')
                pending_echo = pending_echo[1:]
                continue
            buffer.append(byte)
            while len(buffer) >= 2:
                if buffer[0] != address or buffer[1] not in (function, function | 0x80):
                    del buffer[0]
                    continue
                size = 5 if buffer[1] & 0x80 else (10 if function == 48 else 9)
                if len(buffer) < size:
                    break
                if crc16(buffer[:size - 2]) == int.from_bytes(buffer[size - 2:size], 'big'):
                    if buffer[1] & 0x80:
                        raise ValueError('KELLER exception %d' % buffer[2])
                    return bytes(buffer[:size])
                del buffer[0]
    raise TimeoutError('no valid response to function %d' % function)


def read_p1(fd, address, echo, count):
    init = transact(fd, address, 48, echo)
    if init[2] != 5 or init[3] not in (20, 21, 24) or init[7] > 1:
        raise ValueError('unsupported KELLER class, group or status')
    for _ in range(count):
        sample = transact(fd, address, 73, echo)
        pressure = struct.unpack('>f', sample[2:6])[0]
        if sample[6] & 0xd2 or not math.isfinite(pressure) or not -1 <= pressure <= 2000:
            raise ValueError('invalid P1 status or pressure')
        yield pressure
        time.sleep(0.2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('serial', type=Path)
    parser.add_argument('--baud', default=9600, type=int, choices=(9600, 115200))
    parser.add_argument('--address', default=1, type=int)
    parser.add_argument('--local-echo', action='store_true')
    args = parser.parse_args()
    if not 1 <= args.address <= 249:
        parser.error('address must be 1..249')
    device = args.serial.resolve(strict=True)
    if device.parent != Path('/dev') or not device.name.startswith('tty') or not device.is_char_device():
        parser.error('select the verified physical /dev/tty* port')
    with (DB / ('serial-%d.lock' % device.stat().st_rdev)).open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            original = termios.tcgetattr(fd)
            try:
                tty.setraw(fd)
                options = termios.tcgetattr(fd)
                options[2] &= ~(termios.PARENB | termios.CSTOPB | termios.CSIZE | termios.CRTSCTS)
                options[2] |= termios.CS8 | termios.CREAD | termios.CLOCAL
                options[4] = options[5] = getattr(termios, 'B%d' % args.baud)
                termios.tcsetattr(fd, termios.TCSANOW, options)
                values = list(read_p1(fd, args.address, args.local_echo, 5))
            finally:
                termios.tcsetattr(fd, termios.TCSANOW, original)
        finally:
            os.close(fd)
    print('P1 range: %.6f to %.6f bar' % (min(values), max(values)))
    print('Median P1: %.6f bar' % statistics.median(values))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, termios.error) as error:
        raise SystemExit(str(error))
