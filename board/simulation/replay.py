"""Emulate COMA feedback and audit each received NMEA group."""
import errno
import json
import os
import pty
import struct
import sys
import time
import tty
from pathlib import Path


def frame(tick, age, fused=True, position=(34.365803, -8.549541)):
    f = bytearray(112)
    f[:3] = b'\x99\x66\x6c'
    lat, lon = (round(value * 2147483647 / 180) for value in position)
    struct.pack_into('<IIiii', f, 3, tick, tick * 5, lat, 0, lon)
    struct.pack_into('<hhhhhh', f, 23, 120, -30, 60, 100, -300, -200)
    struct.pack_into('<I', f, 59, 8 | (8192 if fused else 0))
    struct.pack_into('<hhhh', f, 87, 1280, 128, 64, -96)
    struct.pack_into('<I', f, 107, age)
    f[-1] = sum(f[2:-1]) & 255
    return bytes(f)


def coordinate(value, width, hemisphere):
    result = int(value[:width]) + float(value[width:]) / 60
    return -result if hemisphere in ('S', 'W') else result


def verify(line):
    body, check = line.decode('ascii').strip('\r\n').split('*')
    checksum = 0
    for value in body[1:].encode('ascii'):
        checksum ^= value
    if not body.startswith('$') or check != f'{checksum:02X}' or not line.endswith(b'\r\n'):
        raise ValueError('invalid NMEA line')
    return body[1:].split(',')


def main(link, evidence):
    master, slave = pty.openpty()
    tty.setraw(slave)
    os.set_blocking(master, False)
    link.symlink_to(os.ttyname(slave))
    start = time.monotonic()
    tick = 0
    last_phase = None
    incoming = bytearray()
    gga = None
    position = (34.365803, -8.549541)
    fault = link.parent / 'pause-input'
    ignore = link.parent / 'ignore-alignment'
    with evidence.open('w', buffering=1) as log:
        while True:
            elapsed = time.monotonic() - start
            if elapsed < 1:
                phase = 'starting'
            elif elapsed < 7:
                phase = 'normal'
            elif elapsed < 11:
                phase = 'duplicate_age'
            elif elapsed < 16:
                phase = 'resumed'
            elif elapsed < 18:
                phase = 'no_input'
            elif elapsed < 22:
                phase = 'recovered'
            elif elapsed < 25:
                phase = 'not_fused'
            else:
                phase = 'normal_again'
            if phase != last_phase:
                log.write(json.dumps(dict(phase=phase, elapsed=elapsed, wall=time.time())) + '\n')
                last_phase = phase
            try:
                output = os.read(master, 4096)
            except BlockingIOError:
                output = b''
            if output:
                log.write(json.dumps(dict(serial_writes=output.hex(), elapsed=elapsed)) + '\n')
                incoming.extend(output)
                if len(incoming) > 4096:
                    raise ValueError('oversized simulated COMA write')
                while b'\n' in incoming:
                    end = incoming.index(b'\n') + 1
                    line = bytes(incoming[:end])
                    del incoming[:end]
                    parts = verify(line)
                    if parts[0] == 'GPGGA' and gga is None and parts[1] == '134658.00' and parts[6:] == [
                            '2','09','1.0','433.00','M','0.00','M','08','AAAA']:
                        gga = (coordinate(parts[2], 2, parts[3]), coordinate(parts[4], 3, parts[5]))
                    elif line == b'$GPVTG,15.957,T,,M,0.0062,N,0.0015,K,A*32\r\n' and gga is not None:
                        adopt = not ignore.exists()
                        log.write(json.dumps(dict(alignment_group=gga, adopted=adopt, elapsed=elapsed)) + '\n')
                        if adopt:
                            position = gga
                        gga = None
                    else:
                        raise ValueError('unexpected simulated COMA write')
            if phase not in ('starting', 'no_input') and not fault.exists():
                f = frame(tick * 2, 0 if phase == 'duplicate_age' else (tick % 100) * 10,
                          phase != 'not_fused', position)
                if tick % 151 == 20:
                    bad = bytearray(f)
                    bad[-1] ^= 1
                    os.write(master, b'\x00\x99' + bad)
                try:
                    if tick % 23 == 1:
                        os.write(master, f[:17])
                        time.sleep(0.001)
                        os.write(master, f[17:])
                    else:
                        os.write(master, f)
                except OSError as exc:
                    if exc.errno != errno.EAGAIN:
                        raise
                tick += 1
            target = start + (int(elapsed * 100) + 1) * 0.01
            if target > time.monotonic():
                time.sleep(target - time.monotonic())


if __name__ == '__main__':
    main(Path(sys.argv[1]), Path(sys.argv[2]))
