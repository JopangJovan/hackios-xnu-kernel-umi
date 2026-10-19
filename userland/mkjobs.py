#!/usr/bin/env python3
"""Copy stock iOS launchd job definitions into the small image, adjusted for the bring-up.

usage: mkjobs.py [--rest] [--root-volume LISTING] IOS_ROOTFS DEST_DIR label...

For every label, IOS_ROOTFS/System/Library/LaunchDaemons/<label>.plist is copied to DEST_DIR with
its standard error sent to the console, which is where a daemon's last words are read (there
is no log viewer or crash reporter yet). For the jobs named in the environment variable OSLOG
("all" for every one) OS_ACTIVITY_DT_MODE is set too, which makes os_log print there as well:
everything the daemon has to say -- SpringBoard alone says a megabyte in half a minute, which
costs time and pushes everybody else out of the log that comes back from the phone.
The program itself must be there at run time; a warning is printed if it will not be.

What "there at run time" means is the recipe, not what happens to be unpacked on this machine, and the
two recipes differ. With the system image as the root, it is what select-system.py chose, and a daemon it
left out as too big really is absent. With the iOS volume on the phone's flash as the root (build.sh
FLASHBOOT=1), the root is Apple's whole volume: --root-volume names the technical listing of its image
(ios165/rootfs.slt) and a program counts as there if Apple ships it. Seven jobs turn on that --
fairplayd, identityservicesd, passd, assistantd, callservicesd, lskdd, searchpartyd -- and were being
refused on a flash boot for a reason true only of the thinned copy here. (fairplayd now runs on the phone
and is never missed from a lookup again, but it does not stay: it exits 0 the moment it starts, says nothing,
and KeepAlive brings it back every ten seconds -- 22 times in a session, not one signal. It wants the IOKit
user client com_apple_driver_FairPlayIOKitUserClient, which this kernel has not got. The empty App Store and
Weather were never its doing: see the network section of README.md.) Eight more, Safari's and
AuthenticationServicesAgent, hang on the App cryptex being unpacked beside it at all (see CRYPTEXES
below); a workspace used to lose those silently, which tools/new-workspace.sh no longer does.

With --rest, the other jobs of iOS follow (their standard error on the console as well, their os_log
only when named in OSLOG), and with them the "launch angels" (System/Library/LaunchAngels: the
applications without an icon that draw parts of the system's own interface in a process of their
own -- the wallpaper, the keyboard, the now playing controls -- and are reached through a Mach
service like a daemon), but strictly on demand: whatever starts them by
itself (RunAtLoad, KeepAlive) is taken out, so that launchd runs one only when a program asks
for one of its Mach services. That is how an application finds the daemons it needs without
all 490 of them being started at boot. Left out: jobs without Mach services (nothing could
ever start them), jobs whose program will not be there at run time, and those in DENY, which
drive hardware that is not there or do things nobody wants here.
"""
import os
import plistlib
import sys

# substrings of labels. (nsurlsessiond is not among them either, taken out in the sileo2 copy: it is the
# daemon that carries out background URLSession downloads, and without it an application's download never
# starts and never fails -- Sileo sits on "Queued" for ever. It matters here for a second reason, below:
# cloudd spins on it when it is missing.
# mediaserverd is not among them, for all that there is neither sound nor a camera:
# it is where VideoToolbox decodes for every other process, a HEIC picture included; see src/inject_media.c.
# apsd is not among them either: it was left out from the first day without ever being tried, and it is the
# service the rest of the system misses most -- near a third of every "failed lookup" line in the run logs is
# one of 22 daemons asking for com.apple.apsd, and the whole account stack asks before it will do anything.
# It needs no hardware we have not got, and it serves its clients whether or not it reaches a push courier,
# which it never will: the courier wants a device identity rooted in the Secure Enclave this phone has not
# got (apsd -> mobileactivationd, "Device is probably unactivated"), so the push itself stays out of reach.)
# CommCenter stays, and this is what it cost to find out. Shipping both of its plists is the stock
# arrangement -- they carry the same label and launchd picks by the device tree, so with no "baseband" node
# it takes com.apple.CommCenter-noncellular, the variant iOS has for a device with no modem -- and it does
# pick it. It then gets a long way: it answers com.apple.commcenter.xpc and .coretelephony.xpc (85 and 18
# failed lookups in a session become none), serves geod its policy, loads the network-extension
# configurations. And then it dies, every time, on one of two IOKit drivers of the cellular path that this
# kernel has not got: "Could not find AppleIPAppender service" (matched by name, user client
# ApplePDPHelperInterface -- PDP contexts, traffic-class flows, dedicated bearers) and SIGSEGV 50 ms later,
# or "Unable to communicate with AppleBaseband driver" (com.apple.driver.AppleBasebandPCI) and SIGABRT.
# It respawns and does it again: 18 crashes in a seven-minute session where the same image without it has
# none. The boot is not held up -- it reaches the same 118 processes against 119 -- so the trade is 103
# failed lookups a session against a process dying every fifteen seconds for ever, which is a bad one.
# Stubbing the first driver is the kind of work RedwoodBattery already is, but the second is an Apple
# baseband on PCIe, and this phone's modem is on the SoC and speaks QMI to a Qualcomm subsystem -- so what
# is behind the driver is missing too, not just the driver.
# cloudd is out, and the reason is not what it first looked like. Neither daemon crashes, and between them
# 42 failed lookups a session go away (com.apple.cloudd 26, com.apple.mobileassetd.v2 16) -- but with the two
# of them in, the home screen never arrives: the boot reaches 76-78 processes against 119, and not one widget
# or poster process starts, where the same image without them has Spotlight, PosterBoard and a dozen widgets
# inside 420 s. I first put that down to the emulated CPU and guessed the phone would not care. The phone was
# measured and cares more: the same recipe, the same four and a half minutes, 42 processes against 128, no
# home screen, and the applications the scenario taps never open at all.
# What it actually is: cloudd retries com.apple.nsurlsessiond without ever stopping -- 2351 times on the
# phone, 120 to 406 in QEMU, against a dozen in a session without it -- because nsurlsessiond was itself in
# this tuple until a few lines above took it out. So this is two daemons' problem with a one-word fix, and
# the word is already gone: the sileo2 copy removed nsurlsessiond for Sileo's downloads and it is removed
# here too. On its own it costs nothing -- that copy's phone session reaches 118 processes and 20 widgets
# with nsurlsessiond in and cloudd still denied -- so cloudd deserves another try now that it is there.
# Two measurements outstanding: mobileassetd alone (it is not the one spinning, 2 retries against cloudd's
# 2351), and cloudd with nsurlsessiond actually present.
# One thing still unexplained: ALLOW="syncdefaultsd appstored" loses the home screen the same way (77
# processes, no widgets) with no retry storm at all -- a dozen nsurlsessiond lookups, nothing spinning. So
# the stall has at least one other cause, and whoever picks this up should not assume nsurlsessiond is all of
# it.
# syncdefaultsd (com.apple.kvsd) is out on its own merits. It serves cloudkeychainproxy3 its store, then
# aborts inside changeTokenForStoreWithConfiguration:reply: with
#   *** Terminating app due to uncaught exception 'NSInvalidArgumentException',
#       reason: '*** -[NSURL initFileURLWithPath:isDirectory:]: nil string parameter'
# -- it is building a file URL for a directory it was given as nil. The likely one is the SyncedPreferences
# directory it asks for by name ("uncontainerizedSyncedPreferencesDirectoryURL"). That was tried --
# build.sh can make an empty directory on the data volume with a d line in var.skel, as it does for
# /var/root/.ssh -- and it is not the answer: with /private/var/mobile/Library/SyncedPreferences there and
# created ("create the directories of the data volume" ok), syncdefaultsd aborts in exactly the same place
# with exactly the same exception. So the nil comes from somewhere else, and the next guess should be the
# container path for com.apple.security.cloudkeychainproxy3 rather than a directory of mobile's.
# bird and duetexpertd were shipped as well and nothing ever asked for them in a bare boot (no spawn, no
# lookup), so they are still untested rather than known good; they need a session with applications.
DENY = ('watchdog', 'CommCenter', 'wifi', 'WiFi', 'bluetooth', 'BTServer', 'locationd', 'softwareupdate',
        'datamigrator', 'backupd', 'thermalmonitord', 'keybagd', 'nfcd', 'biometrickitd', 'pearld',
        'obliterat', 'storage_mounter', 'com.apple.fud', 'sepd', 'seputil', 'camera', 'ReportCrash',
        'osanalytics', 'lockdown', 'audio', 'jetsamproperties', 'mobileactivationd',
        'installcoordinationd', 'purplebuddy', 'restore', 'usb', 'accessoryd',
        'mobileassetd', 'itunesstored', 'appstored', 'cloudd', 'bird',
        # these start, crash and are asked for again, over and over. (aggregated, which was among them, is where
        # iOS 16 keeps its power log -- com.apple.powerlog.plxpclogger.xpc and the rest -- that Settings > Battery
        # waits for; it now runs through a session.)
        'homed', 'routined', 'duetexpertd', 'com.apple.dmd', 'syncdefaultsd', 'photoanalysisd', 'avconferenced')

# ALLOW="word ..." in the environment lets jobs through that DENY would stop: for trying one out
DENY = tuple(word for word in DENY if word not in os.environ.get('ALLOW', '').split())

args = sys.argv[1:]
rest = '--rest' in args
root_volume = None              # the listing of the whole iOS volume, when that volume is the root
if '--root-volume' in args:
    i = args.index('--root-volume')
    root_volume = args[i + 1]
    del args[i:i + 2]
args = [a for a in args if a != '--rest']
src_root, dest, labels = args[0], args[1], args[2:]
oslog = os.environ.get('OSLOG', '').split()
src = f'{src_root}/System/Library/LaunchDaemons'
# A program in a cryptex -- on a device a volume of its own, grafted at /System/Cryptexes/NAME (eight of
# Safari's daemons are in the App cryptex) -- is looked for where fetch.sh unpacked that cryptex, beside
# the root filesystem; the images have it at the same place (build.sh).
CRYPTEXES = {'/System/Cryptexes/OS/': 'cryptex', '/System/Cryptexes/App/': 'cryptex-app'}


def volume_paths(listing, _cache={}):
    """Every path in a 7z technical listing of the iOS root filesystem image, without a leading slash."""
    if listing not in _cache:
        _cache[listing] = {line[len('Path = '):].strip()
                           for line in open(listing, errors='replace') if line.startswith('Path = ')}
    return _cache[listing]


def present(program):
    for prefix, unpacked in CRYPTEXES.items():
        if program.startswith(prefix):
            return os.path.isfile(os.path.join(os.path.dirname(os.path.abspath(src_root)), unpacked,
                                               program[len(prefix):]))
    if root_volume:
        return program.lstrip('/') in volume_paths(root_volume)
    return os.path.isfile(f'{src_root}{program}')


angels = f'{src_root}/System/Library/LaunchAngels'
os.makedirs(dest, exist_ok=True)
for label in labels:
    with open(f'{src}/{label}.plist', 'rb') as f:
        job = plistlib.load(f)
    job['StandardErrorPath'] = '/dev/console'
    # a job that is named is wanted, whatever hardware it says it is for: wifid's asks for a node "wlan" in the
    # device tree, which the loader's tree does not have yet
    job.pop('_LimitLoadToDeviceTree', None)
    if 'all' in oslog or label in oslog:
        job.setdefault('EnvironmentVariables', {})['OS_ACTIVITY_DT_MODE'] = 'YES'
    with open(f'{dest}/{label}.plist', 'wb') as f:
        plistlib.dump(job, f, fmt=plistlib.FMT_BINARY)
    program = job.get('Program') or job['ProgramArguments'][0]
    missing = '' if present(program) else ('   (NOT on the iOS volume!)' if root_volume
                                           else '   (NOT in the system image!)')
    print(f'  job {label}: {program}{missing}')

if rest:
    count = 0
    rest_jobs = [(src, name) for name in sorted(os.listdir(src))]
    if os.path.isdir(angels):
        rest_jobs += [(angels, name) for name in sorted(os.listdir(angels))]
    for directory, name in rest_jobs:
        label = name[:-len('.plist')]
        if not name.endswith('.plist') or label in labels or any(word in label for word in DENY):
            continue
        with open(f'{directory}/{name}', 'rb') as f:
            job = plistlib.load(f)
        program = job.get('Program') or (job.get('ProgramArguments') or [''])[0]
        if not job.get('MachServices') or not present(program):
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
