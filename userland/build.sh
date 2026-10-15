#!/bin/bash
# Build the user-space side of the bring-up.
#
#   hello            one raw-syscall program; as a RAM disk on its own it boots through mockfs
#   root.packfs      the small image: our init and test programs, etc/rc, and the launchd jobs
#                    (LaunchDaemons/). It travels in the payload next to the kernel (first RAM
#                    disk, /dev/md0) and is mounted on /redwood. Rebuilt every time.
#                      RC=<file>    etc/rc: rc.launchd (default) hands process 1 to the iOS launchd
#                                   after init's checks; rc.ios runs a few iOS programs and halts
#                      JOBS="..."   labels of the stock iOS launchd jobs to load (default: IOS_JOBS)
#                      REST=1       ... and the rest of them, started only when asked for (see mkjobs.py)
#                      WIFID=1      ... and wifid, iOS's Wi-Fi daemon (see below at IOS_JOBS)
#                      OSLOG="..."  labels of the jobs whose os_log output goes to the console ("all": every
#                                   one of JOBS; default: only the window server, which says little)
#                      INJECT="..." the programs started with a library of ours, "LIBRARY NAME..." per line
#                                   (default: src/inject.c in backboardd, src/inject_sb.c in SpringBoard,
#                                   src/inject_media.c in mediaserverd);
#                                   init hands the list to the kernel (etc/inject.conf -> sysctl kern.exec_inject)
#                      EXTRA="..."  more programs for /redwood/bin (files on this machine): one-off
#                                   diagnostics, which a file given as RC_JOBS can then run
#                      RC_JOBS=<file> what the job com.redwood.demo runs: rc.jobs (default), or
#                                   rc.jobs.unlock, which also swipes the lock screen away (for the phone,
#                                   where nothing else can touch the screen yet)
#                      GL=1         with an OpenGL ES implementation for the window server: Mesa
#                                   (tools/mk-mesa.sh fetches and builds it) behind src/eagl.c, as
#                                   lib/libOSMesa.8.dylib (15 MB) and lib/libredwood-gl.dylib. Where the
#                                   kernel has started the phone's GPU, CoreAnimation then draws with its
#                                   OpenGL ES renderer, by that GPU (boot-arg gpu=1); anywhere else -- QEMU,
#                                   no boot-arg, no firmware in ../fw-gpu -- the library is not even loaded
#                                   and nothing changes (src/inject.c, opengl_es()).
#                                   GL=test: only the library and bin/gl_test (a triangle, src/gl_test.c;
#                                   RC_JOBS=rc.jobs.gltest runs it); the window server stays as it is.
#                      ADSP=1       with the audio DSP's firmware from ../fw-audio in fw/ (20 MB; for the
#                                   kernel's adsp_step=N, bsd/dev/redwood_adsp.c, which runs before the root is
#                                   mounted). Not beside GL=1: the two do not fit the 64 MB the loader takes with
#                                   the kernel. adsp=1 needs none of it: the system image has the firmware in /fw.
#   system.packfs    the system image, the root filesystem (second RAM disk, /dev/md1): the part of
#                    iOS 16.5 chosen by ../ios165/select-system.py at its real paths, the dyld
#                    shared cache, and src/stage0.c as /sbin/launchd (the real one is
#                    /sbin/launchd.ios). 3 GB; upload once per loader session with
#                    `xl ramdisk userland/system.packfs`. Rebuilt when missing, when its inputs
#                    (stage0, the manifest, the cryptex files) are newer, or with SYSTEM=1 (after
#                    changing the recipe below).
#
#   PREBUILT=1       do not compile anything: take our programs from ../prebuilt/userland (this is also
#                    what happens when the cross toolchain is not set up, see ../xnu-deps/setup.sh);
#                    the images are then made of them and of the iOS files alone (python3 + liblz4)
set -e
cd "$(dirname "$(readlink -f "$0")")"
source ../xnu-env.sh

CC="xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra"
LD="-Wl,-e,_start -Wl,-pagezero_size,0x100000000"
DYN="xnu-clang -arch arm64 -isysroot ${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk -O1"
IOS=../ios165
CACHE=$IOS/20F66__iPhone10,3_6
# The stock iOS jobs given to launchd, roughly in the order they were brought up.
IOS_JOBS=${JOBS-"
	com.apple.notifyd com.apple.cfprefsd.xpc.daemon com.apple.cfprefsd.xpc.daemon.system com.apple.logd
	com.apple.syslogd com.apple.distnoted.xpc.daemon com.apple.diagnosticd com.apple.fseventsd
	com.apple.containermanagerd com.apple.containermanagerd.system com.apple.runningboardd
	com.apple.backboardd com.apple.mobile.usermanagerd com.apple.mobilegestalt.xpc com.apple.lsd
	com.apple.powerd com.apple.analyticsd com.apple.logd_helper
	com.apple.iconservices.iconservicesagent com.apple.pluginkit.pkd
	com.apple.SpringBoard
"}
# WIFID=1: iOS's own Wi-Fi daemon as well, for Settings to have a Wi-Fi to show: the kernel answers it in
# bsd/dev/redwood_apple80211.c. With the radio (wifi=1) Settings then shows Wi-Fi on, the network joined and the
# networks the radio hears. NOT FINISHED: a network cannot be joined from Settings yet.
[ -n "$WIFID" ] && IOS_JOBS="$IOS_JOBS com.apple.wifid"
# The programs (file names) started with a library of ours inside them: "LIBRARY NAME..." per line
# (src/inject.c is libredwood.dylib, src/inject_sb.c is libredwood-sb.dylib, src/inject_media.c is
# libredwood-media.dylib).
INJECT=${INJECT-"/redwood/lib/libredwood.dylib backboardd containermanagerd containermanagerd_system
/redwood/lib/libredwood-sb.dylib SpringBoard
/redwood/lib/libredwood-media.dylib mediaserverd"}
mkdir -p obj

if [ -z "$PREBUILT" ] && ! command -v ld64 >/dev/null; then PREBUILT=1; fi
if [ -n "$PREBUILT" ]; then
	echo "build.sh: not compiling, using the programs in prebuilt/userland"
	for f in ../prebuilt/userland/*; do
		# keep a file that did not change (and its time stamp): system.packfs depends on stage0
		cmp -s "$f" "obj/$(basename "$f")" || cp "$f" obj/
	done
	cp obj/hello hello
else
	$CC $LD -o hello src/hello.c
	$CC -Ilibmini -c libmini/mini.c -o obj/mini.o
	$CC -c libmini/start.S -o obj/start.o
	for prog in init child stage0; do
		$CC -Ilibmini $LD -o obj/$prog.new src/$prog.c obj/mini.o obj/start.o
		# keep the old file (and its time stamp) when nothing changed: system.packfs depends on stage0
		cmp -s obj/$prog.new obj/$prog && rm obj/$prog.new || mv obj/$prog.new obj/$prog
	done
fi

# --- the small image ------------------------------------------------------------------------------
rm -rf root
mkdir -p root/sbin root/bin root/etc root/share root/LaunchDaemons
cp obj/init root/sbin/init
cp obj/child root/bin/child
echo "Darwin on redwood: image built $(date '+%Y-%m-%d %H:%M')" > root/etc/motd
# 300000 bytes, byte i = (i*31 + (i>>8)) & 0xff; init verifies it through read() and mmap()
python3 -c "import sys; sys.stdout.buffer.write(bytes((i*31 + (i>>8)) & 0xff for i in range(300000)))" > root/share/pattern.bin
if [ -d $IOS/rootfs/usr/lib ]; then
	if [ -z "$PREBUILT" ]; then
		# ordinary dynamically linked programs of our own, built against the iOS SDK
		$DYN -o obj/hello_dyn src/hello_dyn.c
		$DYN -framework CoreFoundation -o obj/svc_test src/svc_test.c
		$DYN -Wno-four-char-constants -framework CoreFoundation -framework IOSurface -o obj/surf_test src/surf_test.c
		$DYN -o obj/rwtouch src/rwtouch.c
		$DYN -framework CoreFoundation -framework AudioToolbox -framework AVFoundation -lobjc -o obj/sound_test src/sound_test.c
		$DYN -framework CoreFoundation -lMobileGestalt -o obj/mg_test src/mg_test.c
		$DYN -o obj/cpu_test src/cpu_test.c
		$DYN -framework CoreFoundation -framework IOSurface -o obj/cow_test src/cow_test.c
		$DYN -framework CoreFoundation -framework CoreGraphics -framework ImageIO -o obj/snap_test src/snap_test.c
		$DYN -fobjc-arc -framework Foundation -framework SystemConfiguration -o obj/net_test src/net_test.m
		# (the keychain answers only a program that has a keychain access group: signed with one, if ldid is there)
		$DYN -framework CoreFoundation -framework Security -o obj/keychain_test src/keychain_test.c &&
			{ ${LDID:-ldid} -Ssrc/keychain_test.entitlements obj/keychain_test 2>/dev/null ||
			  echo "build.sh: no ldid: keychain_test is unsigned, and the keychain will refuse it (-34018)"; }
		# (and wifid's way of keeping a network's password, by wifid's name: src/wifipw_test.c)
		$DYN -framework CoreFoundation -framework Security -o obj/wifipw_test src/wifipw_test.c &&
			{ ${LDID:-ldid} -Ssrc/wifipw_test.entitlements obj/wifipw_test 2>/dev/null ||
			  echo "build.sh: no ldid: wifipw_test is unsigned, and the keychain will refuse it (-34018)"; }
		# our library for inside stock iOS programs (the jobs in INJECT are started with it)
		$DYN -dynamiclib -install_name /redwood/lib/libredwood.dylib -framework CoreFoundation -framework IOKit -lobjc \
			-o obj/libredwood.dylib src/inject.c
		$DYN -dynamiclib -install_name /redwood/lib/libredwood-sb.dylib -framework CoreFoundation -framework IOKit -lobjc \
			-o obj/libredwood-sb.dylib src/inject_sb.c
		$DYN -dynamiclib -install_name /redwood/lib/libredwood-media.dylib -framework CoreFoundation -lobjc \
			-o obj/libredwood-media.dylib src/inject_media.c
		# the output device of the audio server: a plug-in for CoreAudio's HAL
		$DYN -bundle -framework CoreFoundation -o obj/RedwoodAudio src/audio_plugin.c
		if [ -n "$GL" ]; then
			# the OpenGL ES library, and the table of functions that hands CoreAnimation's calls to it
			# (Mesa says beside the library what it was built from: one from other patches is built again)
			M=../xnu-deps/build/mesa/out
			[ "$(cat $M/built-from 2>/dev/null)" = "$(cat ../tools/mesa/*.patch ../tools/mesa/redwood-drm/* | sha256sum)" ] ||
				../tools/mk-mesa.sh
			./mkeagl.py src/eagl_slots.txt $M/gl.xml $M/exported-gl.txt src/eagl.c > obj/eagl_gen.h
			$DYN -dynamiclib -install_name /redwood/lib/libredwood-gl.dylib -Iobj -framework CoreFoundation \
				-framework IOKit -framework IOSurface -lobjc $M/libOSMesa.8.dylib -o obj/libredwood-gl.dylib \
				src/eagl.c src/gl_osmesa.c
			$DYN -framework CoreFoundation -framework IOKit $M/libOSMesa.8.dylib -o obj/gl_test src/gl_test.c
			cp $M/libOSMesa.8.dylib obj/
		fi
	fi
	cp obj/hello_dyn obj/svc_test obj/surf_test obj/rwtouch obj/mg_test root/bin/
	[ -f obj/cpu_test ] && cp obj/cpu_test root/bin/        # (a prebuilt/ from before this program has none)
	[ -f obj/cow_test ] && cp obj/cow_test root/bin/
	[ -f obj/snap_test ] && cp obj/snap_test root/bin/
	[ -f obj/net_test ] && cp obj/net_test root/bin/
	[ -f obj/keychain_test ] && cp obj/keychain_test root/bin/
	[ -f obj/wifipw_test ] && cp obj/wifipw_test root/bin/
	[ -f obj/sound_test ] && cp obj/sound_test root/bin/
	for f in $EXTRA; do cp "$f" root/bin/; done
	mkdir -p root/lib
	cp obj/libredwood.dylib obj/libredwood-sb.dylib root/lib/
	[ -f obj/libredwood-media.dylib ] && cp obj/libredwood-media.dylib root/lib/     # (as with cpu_test)
	if [ -f obj/RedwoodAudio ]; then
		mkdir -p root/lib/RedwoodAudio.driver
		cp obj/RedwoodAudio root/lib/RedwoodAudio.driver/
		cp src/audio_plugin.plist root/lib/RedwoodAudio.driver/Info.plist
	fi
	if [ -n "$GL" ]; then
		cp obj/libOSMesa.8.dylib root/lib/
		cp obj/gl_test root/bin/
		[ "$GL" = test ] || cp obj/libredwood-gl.dylib root/lib/
	fi
	# a program is not started with a library that is not there: it would not start at all
	echo "$INJECT" | while read -r library names; do
		[ -z "$library" ] || [ ! -f "root/lib/${library#/redwood/lib/}" ] || echo "$library $names"
	done > root/etc/inject.conf
	cp $IOS/var.skel root/etc/var.skel              # the directories init creates in /private/var
	# time zones: the template's links go in a circle until the time zone daemon has run (/usr/share/zoneinfo
	# points back into /var); point them at the data that ships with the system, and use this machine's zone
	zone=$(timedatectl show -p Timezone --value 2>/dev/null); zone=${zone:-UTC}
	sed -i -e 's|^l\t/usr/share/zoneinfo\t/private/var/db/timezone/zoneinfo$|l\t/usr/share/zoneinfo.default\t/private/var/db/timezone/zoneinfo|' \
		-e "s|^l\t/var/db/timezone/zoneinfo/[^\t]*\t/private/var/db/timezone/localtime\$|l\t/var/db/timezone/zoneinfo/$zone\t/private/var/db/timezone/localtime|" root/etc/var.skel
	# ... and the files of ours it puts there (var/): those under mobile/ belong to the user mobile
	cp -r var root/var
	(cd var && find . -type f | sed 's|^\./||' | sort) | while read -r f; do
		case $f in mobile/*) owner=501 ;; *) owner=0 ;; esac
		printf 'f\t644\t%s\t%s\t/private/var/%s\t/var/%s\n' $owner $owner "$f" "$f"
	done >> root/etc/var.skel
	cp ${RC:-rc.launchd} root/etc/rc
	cp ${RC_JOBS:-rc.jobs} root/etc/rc.jobs
	# the jobs the iOS launchd will see (/System/Library/LaunchDaemons is a symlink to this directory)
	cp com.redwood.demo.plist com.redwood.wifi.plist root/LaunchDaemons/
	[ -n "$IOS_JOBS" ] && OSLOG=${OSLOG-com.apple.backboardd} ./mkjobs.py ${REST:+--rest} $IOS/rootfs root/LaunchDaemons $IOS_JOBS
fi
# firmware of the phone's own hardware, where somebody has pulled it from the phone's Android into ../fw-gpu
# (it is the vendor's and never in the repository): the kernel's GPU bring-up, bsd/dev/redwood_adreno.c, finds it in fw/
if [ -d ../fw-gpu ]; then
	mkdir -p root/fw
	cp ../fw-gpu/* root/fw/
fi
# ... and the image of the Wi-Fi DSP, from /vendor/firmware_mnt/image of the same Android into ../fw-wifi
# (bsd/dev/redwood_wifi.c; 7.6 MB), with what the radio's firmware is sent once it runs: this phone's
# board data and the regulatory database (image/qca6750 there; 60 and 24 KB)
if [ -f ../fw-wifi/wpss.mdt ]; then
	mkdir -p root/fw
	install -m 0644 ../fw-wifi/wpss.mdt ../fw-wifi/wpss.b0[2-9] root/fw/
	for f in bd_m20gl.elf regdb_xiaomi.bin; do
		[ -f ../fw-wifi/qca6750/$f ] && install -m 0644 ../fw-wifi/qca6750/$f root/fw/
	done
	# the network the radio is to join: its name on the first line, its passphrase on the second. Yours, and
	# so not in the repository; it goes where the firmware goes and the kernel reads it there (wifi_join=1)
	rm -f root/fw/network
	[ -f ../fw-wifi/network ] && install -m 0600 ../fw-wifi/network root/fw/network
fi
# ... and with ADSP=1 the image of the audio DSP, from the same place into ../fw-audio (bsd/dev/redwood_adsp.c):
# adsp.mdt and adsp.b02 to adsp.b36, 20 MB. Only when asked for: beside GL=1 the payload would be more than the 64 MB
# the loader takes.
if [ -n "$ADSP" ] && [ -f ../fw-audio/adsp.mdt ]; then
	mkdir -p root/fw
	install -m 0644 ../fw-audio/adsp.mdt ../fw-audio/adsp.b0[2-9] ../fw-audio/adsp.b[1-9][0-9] root/fw/
fi
../mkpackfs.py -o root.packfs root

# --- the system image -----------------------------------------------------------------------------
if [ -f $IOS/system.manifest ] && [ -f $CACHE/dyld_shared_cache_arm64 ] &&
	{ [ ! -f system.packfs ] || [ obj/stage0 -nt system.packfs ] || [ $IOS/system.manifest -nt system.packfs ] ||
	  [ $IOS/cryptex -nt system.packfs ] || [ -n "$SYSTEM" ]; }; then
	rm -rf sysroot
	cp -al $IOS/rootfs sysroot                      # hard links: no copies of the 0.5 GB
	# launchd must load our choice of jobs, not all 500 that iOS has
	rm -rf sysroot/System/Library/LaunchDaemons sysroot/System/Library/NanoLaunchDaemons \
		sysroot/System/Library/LaunchAngels sysroot/System/Library/xpc/launchd.plist
	ln -s /redwood/LaunchDaemons sysroot/System/Library/LaunchDaemons
	mkdir -p sysroot/redwood sysroot/sbin
	# the applications iOS installs on its first boot are system applications here (see select-system.py)
	if [ -d sysroot/private/var/staged_system_apps ]; then
		mv sysroot/private/var/staged_system_apps/* sysroot/Applications/
		rm -rf sysroot/private/var
	fi
	mv sysroot/sbin/launchd sysroot/sbin/launchd.ios
	cp obj/stage0 sysroot/sbin/launchd
	# the audio DSP's firmware, the phone's own from ../fw-audio (never in the repository), where the kernel reads
	# it once the root is mounted (bsd/dev/redwood_adsp.c, adsp=1): 20 MB that do not fit beside GL=1 in the small image
	if [ -f ../fw-audio/adsp.mdt ]; then
		mkdir -p sysroot/fw
		cp ../fw-audio/adsp.mdt ../fw-audio/adsp.b0[2-9] ../fw-audio/adsp.b[1-9][0-9] sysroot/fw/
	fi
	# ... and the speaker amplifiers' configuration, the phone's /vendor/etc/aw882xx_acf.bin (bsd/dev/redwood_speaker.c,
	# for sound=3): Android's own register values, which are not in the repository either
	if [ -f ../fw-audio/aw882xx_acf.bin ]; then
		mkdir -p sysroot/fw
		cp ../fw-audio/aw882xx_acf.bin sysroot/fw/
	fi
	# our audio device, where the audio server looks for such plug-ins (and for no others: Apple's are left out)
	mkdir -p sysroot/System/Library/Audio/Plug-Ins/HAL
	ln -s /redwood/lib/RedwoodAudio.driver sysroot/System/Library/Audio/Plug-Ins/HAL/RedwoodAudio.driver
	# the OS cryptex: on a device a separate volume grafted at /System/Cryptexes/OS. It holds the dyld
	# shared cache (dyld looks there first) and a few libraries and resources outside the cache.
	mkdir -p sysroot/System/Cryptexes
	if [ -d $IOS/cryptex ]; then cp -al $IOS/cryptex sysroot/System/Cryptexes/OS; fi
	D=sysroot/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld
	mkdir -p $D
	for f in $CACHE/dyld_shared_cache_arm64*; do
		case "$f" in *.symbols) continue ;; esac     # symbolication data, never mapped
		ln "$f" $D/
	done
	# thousands of small files: 4K blocks for them, the kernel's 16K pages for everything bigger
	# compressed (read in place by packfs): half the size with LZ4, which is what fits into the phone's RAM;
	# ZSTD=1 compresses with Zstandard instead (a quarter smaller again, slower to build and to read),
	# PLAIN=1 builds the image uncompressed
	if [ -n "$PLAIN" ]; then pack="-b 4096 -a 16384"; elif [ -n "$ZSTD" ]; then pack="--zstd"; else pack="-z"; fi
	../mkpackfs.py $pack -m $IOS/system.manifest -o system.packfs sysroot
	rm -rf sysroot
fi
