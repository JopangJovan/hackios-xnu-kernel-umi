#!/usr/bin/env python3
"""Copy stock iOS launchd job definitions into the small image, adjusted for the bring-up.

usage: mkjobs.py [--rest] IOS_ROOTFS DEST_DIR label...

For every label, IOS_ROOTFS/System/Library/LaunchDaemons/<label>.plist is copied to DEST_DIR with
its standard error sent to the console, which is where a daemon's last words are read (there
is no log viewer or crash reporter yet). For the jobs named in the environment variable OSLOG
("all" for every one) OS_ACTIVITY_DT_MODE is set too, which makes os_log print there as well:
everything the daemon has to say -- SpringBoard alone says a megabyte in half a minute, which
costs time and pushes everybody else out of the log that comes back from the phone.
The program itself must be in the system image; a warning is printed if it was not extracted.

With --rest, the other jobs of iOS follow (their standard error on the console as well, their os_log
only when named in OSLOG), but strictly on demand: whatever starts them by
itself (RunAtLoad, KeepAlive) is taken out, so that launchd runs one only when a program asks
for one of its Mach services. That is how an application finds the daemons it needs without
all 490 of them being started at boot. Left out: jobs without Mach services (nothing could
ever start them), jobs whose program is not in the system image, and those in DENY, which
drive hardware that is not there or do things nobody wants here.
"""
import os
import plistlib
import sys

# substrings of labels
DENY = ('watchdog', 'CommCenter', 'wifi', 'WiFi', 'bluetooth', 'BTServer', 'locationd', 'softwareupdate',
        'datamigrator', 'backupd', 'thermalmonitord', 'keybagd', 'nfcd', 'biometrickitd', 'pearld',
        'obliterat', 'storage_mounter', 'com.apple.fud', 'sepd', 'seputil', 'camera', 'ReportCrash',
        'osanalytics', 'lockdown', 'mediaserverd', 'audio', 'jetsamproperties', 'mobileactivationd',
        'mobile.installd', 'installcoordinationd', 'purplebuddy', 'restore', 'usb', 'accessoryd',
        'mobileassetd', 'nsurlsessiond', 'apsd', 'itunesstored', 'appstored', 'cloudd', 'bird',
        # these start, crash and are asked for again, over and over
        'aggregated', 'homed', 'routined', 'duetexpertd', 'com.apple.dmd', 'syncdefaultsd')

args = sys.argv[1:]
rest = args[:1] == ['--rest']
if rest:
    args = args[1:]
src_root, dest, labels = args[0], args[1], args[2:]
oslog = os.environ.get('OSLOG', '').split()
src = f'{src_root}/System/Library/LaunchDaemons'
os.makedirs(dest, exist_ok=True)
for label in labels:
    with open(f'{src}/{label}.plist', 'rb') as f:
        job = plistlib.load(f)
    job['StandardErrorPath'] = '/dev/console'
    if 'all' in oslog or label in oslog:
        job.setdefault('EnvironmentVariables', {})['OS_ACTIVITY_DT_MODE'] = 'YES'
    with open(f'{dest}/{label}.plist', 'wb') as f:
        plistlib.dump(job, f, fmt=plistlib.FMT_BINARY)
    program = job.get('Program') or job['ProgramArguments'][0]
    missing = '' if os.path.isfile(f'{src_root}{program}') else '   (NOT in the system image!)'
    print(f'  job {label}: {program}{missing}')

if rest:
    count = 0
    for name in sorted(os.listdir(src)):
        label = name[:-len('.plist')]
        if not name.endswith('.plist') or label in labels or any(word in label for word in DENY):
            continue
        with open(f'{src}/{name}', 'rb') as f:
            job = plistlib.load(f)
        program = job.get('Program') or (job.get('ProgramArguments') or [''])[0]
        if not job.get('MachServices') or not os.path.isfile(f'{src_root}{program}'):
            continue
        for key in ('RunAtLoad', 'KeepAlive', 'StartInterval', 'StartCalendarInterval', 'WatchPaths',
                    'QueueDirectories', 'StartOnMount', 'LaunchEvents'):
            job.pop(key, None)
        job['StandardErrorPath'] = '/dev/console'
        if label in oslog:
            job.setdefault('EnvironmentVariables', {})['OS_ACTIVITY_DT_MODE'] = 'YES'
        with open(f'{dest}/{name}', 'wb') as f:
            plistlib.dump(job, f, fmt=plistlib.FMT_BINARY)
        count += 1
    print(f'  ... and {count} more jobs, started on demand only')
