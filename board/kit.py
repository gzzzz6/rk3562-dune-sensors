#!/usr/bin/env python3
import argparse
import fcntl
import ipaddress
import json
import math
import os
import platform
import re
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
from pathlib import Path

INSTALL = Path('/opt/rk3562-dune-sensors')
SETTINGS = Path('/etc/rk3562-dune-sensors.json')
UNIT = Path('/etc/systemd/system/rk3562-sensor@.service')
LOGS = Path('/var/log/rk3562-dune-sensors')
DB = Path('/var/lib/rk3562-dune-sensors')
MODES = ('ins', 'depth', 'ins-sim', 'depth-sim')
MAX_LOG = 200 * 1024**2
MIN_FREE = 1024**3


def checked(command, **kwargs):
    return subprocess.run(command, check=True, **kwargs)


def as_int(value, low, high, name):
    if type(value) is not int or not low <= value <= high:
        raise ValueError('%s must be an integer in %d..%d' % (name, low, high))
    return value


def number(value, low, high, name):
    if type(value) not in (int, float) or not math.isfinite(value) or not low <= value <= high:
        raise ValueError('%s must be a finite number in %s..%s' % (name, low, high))
    return value


def truth(value, name):
    if type(value) is not bool:
        raise ValueError('%s must be true or false' % name)
    return str(value).lower()


def config(settings, mode, runtime):
    remote = ipaddress.IPv4Address(settings['pc_ipv4'])
    if remote.is_unspecified or remote.is_multicast or int(remote) == 0xffffffff:
        raise ValueError('pc_ipv4 must be a unicast address')
    port = as_int(settings['neptus_udp_port'], 1, 65535, 'neptus_udp_port')
    for key in ('system', 'imc_id', 'udp_port'):
        raw = [settings[name][key] for name in MODES]
        values = [int(v, 0) for v in raw] if key == 'imc_id' else raw
        if len(set(values)) != len(values):
            raise ValueError('duplicate %s' % key)
    entry = settings[mode]
    identity = int(entry['imc_id'], 0)
    if not 1 <= identity <= 0xfffe:
        raise ValueError('imc_id out of range')
    name = entry['system']
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_-]{0,47}', name):
        raise ValueError('invalid system name')
    udp = as_int(entry['udp_port'], 1024, 65535, 'udp_port')
    simulated = mode.endswith('-sim')
    is_ins = mode.startswith('ins')
    suffix = ' - SIMULATED' if simulated else ' - HARDWARE'
    if simulated:
        serial = runtime / 'sensor'
    else:
        raw = entry['serial']
        if not re.fullmatch(r'/dev/[A-Za-z0-9_./:+-]+', raw):
            raise ValueError('configure the actual /dev/ serial port')
        serial = Path(raw).resolve(strict=True)
        if serial.parent != Path('/dev') or not re.fullmatch(r'tty[A-Za-z0-9_-]+', serial.name):
            raise ValueError('configure a physical /dev/tty* port')
        if not stat.S_ISCHR(serial.stat().st_mode):
            raise ValueError('serial port is not a character device')
    sections = {
        'General': {'Vehicle': name, 'Vehicle Type': 'uuv'},
        'IMC Addresses': {name: '0x%04X' % identity},
        'Transports.Announce': {'Enabled': 'Always', 'Entity Label': 'Announce',
                                'System Type': 'uuv', 'Announcement Periodicity': 3,
                                'Enable Loopback': 'false', 'Enable Multicast': 'false',
                                'Enable Broadcast': 'false'},
    }
    messages = 'Announce, Heartbeat, EntityList, EntityInfo, EntityState, DevDataText'
    sensor = {'Enabled': 'Always', 'Active': 'true', 'Serial Port - Device': serial}
    if is_ins:
        sensor.update({'Entity Label': 'MFOSN INS-DVL' + suffix,
                       'Alignment Socket': '',
                       'Confirmed Bottom Track': 'true' if simulated else truth(
                           entry['confirmed_bottom_track'], 'confirmed_bottom_track')})
        sections['Sensors.MFOSN'] = sensor
        messages += ', EulerAngles, GroundVelocity, Distance'
    else:
        baud = 9600 if simulated else entry['baud']
        if type(baud) is not int or baud not in (9600, 115200):
            raise ValueError('baud must be 9600 or 115200')
        sensor.update({'Entity Label': 'KELLER Depth' + suffix,
                       'Serial Port - Baud Rate': baud,
                       'Serial Port - Local Echo': 'false' if simulated else truth(entry['local_echo'], 'local_echo'),
                       'Device Address': 1 if simulated else as_int(entry['device_address'], 1, 249, 'device_address'),
                       'Sampling Period': 0.2, 'Response Timeout': 0.25,
                       'Surface Pressure': 1.0 if simulated else number(entry['surface_pressure_bar'], 0, 2000, 'surface_pressure_bar'),
                       'Water Density': 1000 if simulated else number(entry['water_density_kg_m3'], 900, 1300, 'water_density_kg_m3'),
                       'Gravity': 9.80665,
                       'Maximum Depth': 100 if simulated else number(entry['maximum_depth_m'], 1, 11000, 'maximum_depth_m')})
        sections['Sensors.KellerDepth'] = sensor
        messages += ', Pressure, Depth'
    sections['Transports.UDP'] = {
        'Enabled': 'Always', 'Entity Label': 'UDP', 'Local Port': udp,
        'Static Destinations': '%s:%d' % (remote, port), 'Announce Service': 'true',
        'Dynamic Nodes': 'false', 'Local Messages Only': 'true', 'Ignore Filter': 'true',
        'Underwater Communications': 'false', 'Communication Range': 0, 'Transports': messages,
        'Filtered Entities': '', 'Rate Limiters': ''}
    sections['Transports.Logging'] = {
        'Enabled': 'Always', 'Entity Label': 'Logger', 'LSF Compression Method': 'gzip',
        'LSF Volume Size': 20, 'Flush Interval': 5, 'Transports': messages + ', LoggingControl',
        'Filtered Entities': '', 'Rate Limiters': ''}
    ordered = ('General', 'IMC Addresses', 'Sensors.MFOSN' if is_ins else 'Sensors.KellerDepth',
               'Transports.Announce', 'Transports.UDP', 'Transports.Logging')
    ini = '\n'.join('[%s]\n%s\n' % (section, '\n'.join(
        '%s = %s' % item for item in sections[section].items())) for section in ordered)
    return ini, serial, udp


def unit(mode):
    return 'rk3562-sensor@%s.service' % mode


def install():
    if platform.machine() != 'aarch64' or not Path('/run/systemd/system').is_dir():
        raise RuntimeError('requires an aarch64 Linux host running systemd')
    if INSTALL.exists() or UNIT.exists() or INSTALL.is_symlink() or UNIT.is_symlink():
        raise RuntimeError('Existing install found. Stop and uninstall before replacing it.')
    if shutil.disk_usage('/opt').free < MIN_FREE:
        raise RuntimeError('less than 1 GiB free in /opt')
    src = Path(__file__).resolve().parent
    checked(['sha256sum', '-c', 'SHA256SUMS'], cwd=str(src))
    with tempfile.TemporaryDirectory(prefix='.rk3562-sensors-', dir='/opt') as scratch:
        stage = Path(scratch) / 'package'
        stage.mkdir()
        for name in ('kit.py', 'settings.example.json', 'SHA256SUMS', 'rk3562-sensor@.service'):
            shutil.copyfile(src / name, stage / name)
        shutil.copytree(src / 'simulation', stage / 'simulation', ignore=shutil.ignore_patterns('__pycache__'))
        shutil.copytree(src / 'bin', stage / 'bin')
        shutil.copyfile(src.parent / 'LICENCE.md', stage / 'LICENCE.md')
        for mode in MODES:
            dest = stage / 'runtime' / mode
            for name in ('bin', 'etc', 'lib', 'www', 'i18n'):
                (dest / name).mkdir(parents=True)
            binary = 'dune-mfosn' if mode.startswith('ins') else 'dune-keller-depth'
            shutil.copyfile(stage / 'bin' / binary, dest / 'bin/dune')
            (dest / 'bin/dune').chmod(0o755)
            for name, target in (('log', LOGS / mode), ('db', DB / mode)):
                target.mkdir(parents=True, exist_ok=True, mode=0o700)
                (dest / name).symlink_to(target)
        made_settings = False
        stage.rename(INSTALL)
        try:
            if not SETTINGS.exists() and not SETTINGS.is_symlink():
                with SETTINGS.open('x') as output:
                    made_settings = True
                    SETTINGS.chmod(0o600)
                    output.write((INSTALL / 'settings.example.json').read_text())
            shutil.copyfile(INSTALL / UNIT.name, UNIT)
            checked(['systemd-analyze', 'verify', str(UNIT)])
            checked(['systemctl', 'daemon-reload'])
        except BaseException:
            UNIT.unlink(missing_ok=True)
            if made_settings:
                SETTINGS.unlink(missing_ok=True)
            INSTALL.rename(stage)
            subprocess.run(['systemctl', 'daemon-reload'], check=False)
            raise
    print('Installed; no service started. Configure %s' % SETTINGS)


def log_limit(directory):
    return (shutil.disk_usage(directory).free < MIN_FREE or
            sum(p.stat().st_size for p in directory.rglob('*') if p.is_file()) >= MAX_LOG)


def run(mode):
    runtime = Path('/run/rk3562-sensor-' + mode)
    if not runtime.is_dir():
        raise RuntimeError('run this mode via systemctl')
    root = INSTALL / 'runtime' / mode
    logdir = LOGS / mode
    ini, serial, udp = config(json.loads(SETTINGS.read_text()), mode, runtime)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(('0.0.0.0', udp))
    if log_limit(logdir):
        raise RuntimeError('log capacity threshold reached before start')
    stopping = False
    children = []
    lock = None
    def stop(*_):
        nonlocal stopping
        stopping = True
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, stop)
    tag = str(time.time_ns())
    try:
        if not mode.endswith('-sim'):
            # Serialize access by resolved device number within this kit.
            lock = (DB / ('serial-%d.lock' % serial.stat().st_rdev)).open('a')
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        (root / 'etc/sensor.ini').write_text(ini)
        if mode == 'ins-sim':
            children.append(subprocess.Popen([sys.executable, '-B', str(INSTALL / 'simulation/replay.py'),
                                               str(serial), str(logdir / ('input-' + tag + '.jsonl'))]))
        if mode == 'depth-sim':
            children.append(subprocess.Popen([sys.executable, '-B', str(INSTALL / 'simulation/keller_emulator.py'),
                                               '--device-link', str(serial),
                                               '--control', str(INSTALL / 'simulation/scenario.json'),
                                               '--log', str(logdir / ('input-' + tag + '.jsonl'))]))
        if children:
            for _ in range(40):
                if serial.exists() or stopping or children[0].poll() is not None:
                    break
                time.sleep(0.05)
            if stopping:
                return
            if not serial.exists():
                raise RuntimeError('simulation serial port did not appear')
        children.append(subprocess.Popen([str(root / 'bin/dune'), '-d', str(root / 'etc'),
                                          '-c', 'sensor', '-p', 'Hardware'], cwd=str(root)))
        deadline = 0
        while not stopping:
            for child in children:
                if child.poll() is not None:
                    raise RuntimeError('child %s exited %s' % (child.pid, child.returncode))
            if time.monotonic() >= deadline:
                if log_limit(logdir):
                    print('Log capacity threshold reached; stopping', flush=True)
                    break
                deadline = time.monotonic() + 5
            time.sleep(0.2)
    finally:
        for child in reversed(children):
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
        if lock is not None:
            lock.close()


def uninstall():
    if not (INSTALL / 'kit.py').is_file():
        raise RuntimeError('no installation found')
    checked(['systemctl', 'stop'] + [unit(mode) for mode in MODES])
    UNIT.unlink()
    checked(['systemctl', 'daemon-reload'])
    shutil.rmtree(INSTALL)
    print('Removed services and programs. Kept settings, logs and databases.')


def export(mode, output):
    with tarfile.open(output, 'x:gz') as archive:
        archive.add(LOGS / mode, arcname=mode)
        journal = subprocess.check_output(['journalctl', '-u', unit(mode), '-n', '300', '--no-pager'])
        import io
        info = tarfile.TarInfo('journal.txt')
        info.size = len(journal)
        archive.addfile(info, io.BytesIO(journal))


def main():
    parser = argparse.ArgumentParser()
    subs = parser.add_subparsers(dest='action', required=True)
    for action in ('install', 'uninstall', 'check', 'run', 'export'):
        sub = subs.add_parser(action)
        if action in ('check', 'run', 'export'):
            sub.add_argument('mode', choices=MODES)
        if action == 'export':
            sub.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.action == 'install':
        install()
    elif args.action == 'uninstall':
        uninstall()
    elif args.action == 'run':
        run(args.mode)
    elif args.action == 'export':
        export(args.mode, args.output)
    else:
        config(json.loads(SETTINGS.read_text()), args.mode, Path('/run/rk3562-sensor-' + args.mode))
        print('Configuration valid: ' + args.mode)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, TypeError, KeyError, RuntimeError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
