#!/usr/bin/env python3
"""Hand off a running temporary UMI boot to iOS USB after native authentication."""

import argparse
from pathlib import Path
import re
import subprocess
import tempfile
import time


def authenticated(console):
    last = None
    for line in console.splitlines():
        _, marker, state = line.partition('UMI passcode observer: UI_locked=')
        if not marker:
            continue
        fields = dict(field.split('=', 1) for field in ('UI_locked=' + state).split()
                      if '=' in field)
        last = fields
    return last is not None and all(last.get(key) == value for key, value in {
        'UI_locked': '0', 'has_passcode': '1', 'authenticated': '1',
        'effectively_locked': '0',
    }.items())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('serial', help='Fastboot serial of the selected UMI')
    parser.add_argument('--timeout', default=60, type=int, help='Maximum wait in seconds')
    args = parser.parse_args()
    if not 1 <= args.timeout <= 85:
        parser.error('Timeout must be within the temporary image handoff window (1–85 seconds)')

    def command(*arguments):
        return subprocess.run(['fastboot', '-s', args.serial, *arguments], check=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                              timeout=8).stdout

    try:
        if not re.search(r'^product: xnuloader\s*$', command('getvar', 'product'), re.MULTILINE):
            raise SystemExit('The selected device is not running the temporary loader')
        with tempfile.TemporaryDirectory(prefix='umi-usb-handoff-') as directory:
            capture = Path(directory) / 'console.log'
            deadline = time.monotonic() + args.timeout
            while time.monotonic() < deadline:
                status = command('getvar', 'result')
                if not re.search(r'^result: menu-ios-running\b', status, re.MULTILINE):
                    raise SystemExit('The temporary iOS boot is no longer running')
                capture.unlink(missing_ok=True)
                command('oem', 'log')
                command('get_staged', str(capture))
                if authenticated(capture.read_text(errors='replace')):
                    command('oem', 'pair-usb')
                    print('USB handoff requested after native unlock')
                    return
                time.sleep(1)
    except (OSError, subprocess.SubprocessError) as error:
        raise SystemExit(f'Cannot confirm and hand off the selected temporary boot: {error}') from error
    raise SystemExit('No authenticated iOS session observed before the deadline')


if __name__ == '__main__':
    main()
