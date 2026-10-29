#!/usr/bin/env python3
"""Check that the native driver selector keeps UMI out of other device builds."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('hakios_config', root / 'tools/hakios-config.py')
cfg = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cfg)
assert not cfg.DRIVERS_ERROR, cfg.DRIVERS_ERROR
assert not cfg.drivers_built({})['HAKIOS_UMI']
with tempfile.TemporaryDirectory() as tmp:
    for device, expected in [('umi', True), ('redwood', False)]:
        output = Path(tmp) / (device + '.config')
        subprocess.run(['python3', str(root / 'tools/hakios-config.py'),
                        'defconfig', device, '-o', str(output)], check=True)
        assert cfg.drivers_built(cfg.load(str(output)))['HAKIOS_UMI'] == expected
    drivers = [d for d in cfg.DRIVERS if d.name == 'UMI']
    assert len(drivers) == 1
    assert len(drivers[0].files) == 13
    for target, _ in drivers[0].files:
        source = root / drivers[0].source(target)
        assert source.is_file(), source
subprocess.run(['python3', str(root / 'devices/xiaomi-mi-10/prepare.py'), 'verify'], check=True)
print('PASS: UMI native drivers opt in only, wrappers present, upstream templates pristine')
