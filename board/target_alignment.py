"""Route one SSH stdin request to the verified MFOSN instance on this board."""
import configparser
import json
import os
from pathlib import Path
import socket
import stat
import struct
import sys


def relay(mode, packet):
    if mode not in ('local', 'ins-sim', 'ins'):
        return b'identity_mismatch\n'
    name, identity = {'local': ('rk3562-ins-dvl-test', 0xb0d3),
                      'ins-sim': ('rk3562-ins-dvl-sim', 0xb0d7),
                      'ins': ('rk3562-ins-dvl', 0xb0d5)}[mode]
    local = mode == 'local'
    root = Path('/opt/dune-ins-dvl') if local else Path('/opt/rk3562-dune-sensors/runtime') / mode
    run = Path('/run/dune-ins-dvl' if local else '/run/rk3562-sensor-' + mode)
    if not local:
        entry = json.loads(Path('/etc/rk3562-dune-sensors.json').read_text())[mode]
        if entry['system'] != name or int(entry['imc_id'], 0) != identity:
            return b'identity_mismatch\n'
        if mode == 'ins' and entry.get('allow_position_alignment') is not True:
            return b'disabled\n'
    config = configparser.ConfigParser(interpolation=None)
    if not config.read(root / 'etc' / ('ins-dvl-test.ini' if local else 'sensor.ini')):
        return b'offline\n'
    task = config['Sensors.MFOSN']
    path = run / 'alignment.sock'
    serial = Path(task['Serial Port - Device'])
    if (config['General']['Vehicle'] != name or int(config['IMC Addresses'][name], 0) != identity
            or task['Alignment Socket'] != str(path)
            or task['Entity Label'] != 'MFOSN INS-DVL - ' + ('HARDWARE' if mode == 'ins' else 'SIMULATED')):
        return b'identity_mismatch\n'
    if mode == 'ins':
        if (serial != Path(entry['serial']).resolve() or serial.parent != Path('/dev')
                or not serial.is_char_device()):
            return b'identity_mismatch\n'
    elif serial != run / 'sensor' or not str(serial.resolve()).startswith('/dev/pts/'):
        return b'offline\n'
    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
        client.settimeout(2)
        client.connect(str(path))
        pid, uid, _ = struct.unpack('3i', client.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
        if uid or not os.path.samefile('/proc/%d/exe' % pid, root/'bin/dune'):
            return b'identity_mismatch\n'
        client.sendall(packet)
        return client.recv(256)


if __name__ == '__main__':
    packet = sys.stdin.buffer.readline(256)
    if len(sys.argv) != 2 or len(packet) >= 256 or not packet.endswith(b'\n'):
        sys.exit(2)
    try:
        reply = relay(sys.argv[1], packet)
    except (OSError, KeyError, ValueError, configparser.Error):
        reply = b'offline\n'
    sys.stdout.buffer.write(reply)
