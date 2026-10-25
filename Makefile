# HakiOS: one Makefile at the top, the way an Android tree has one. Every target is a script
# below it; run the script itself for its options.
#
#   make                 help
#   make fetch           the OS files from the iOS 16.5 IPSW into sysimage/ (sysimage/fetch.sh)
#   make images          userland/root.apfs and userland/system.apfs (userland/build.sh)
#   make kernel          the kernel from source (xnu/apply.sh, xnu-deps/setup.sh, xnu-build.sh)
#   make loader          the boot loader, with identity.h made from your IPSW (xnuloader/build.sh)
#   make firmware        the phone's firmware off the phone over adb (devices/$(DEVICE)/pull-firmware.sh)
#   make qemu            a 20-minute session in QEMU
#   make phone           a session on the phone, then back to Android (tools/modem-cycle.sh)
#   make prebuilt        refresh prebuilt/ from what was built (tools/update-prebuilt.sh)
#   make clean           build output of the kernel, loader and images
#
#   DEVICE=redwood       the device directory under devices/ (default redwood)
#   ARGS="..."           kernel arguments for make phone (default: everything the phone has)
#   SECS=600             the session's length for make phone / make qemu
#   JOBS=8               parallel jobs for the kernel build

DEVICE ?= redwood
JOBS ?= 8
SECS ?= 600
ARGS ?= touchscreen=1 gpu=1 adsp=1 sound=4 wifi=1 haptic=5 modem=1 modem_again=1 modem_buffer=0xa8000000
IMAGE_OPTS ?= REST=1

.PHONY: help fetch images kernel loader firmware qemu phone prebuilt clean all

help:
	@sed -n '2,20p' Makefile | sed 's/^# \{0,1\}//'

all: kernel loader images

fetch:
	sysimage/fetch.sh

images:
	$(IMAGE_OPTS) userland/build.sh

kernel:
	[ -d xnu-src/xnu-8796.121.2 ] || xnu/apply.sh
	[ -x xnu-deps/host/bin/xnu-clang ] || xnu-deps/setup.sh
	./xnu-build.sh -j$(JOBS) -k

loader:
	xnuloader/build.sh

firmware:
	devices/$(DEVICE)/pull-firmware.sh

qemu:
	mkdir -p logs
	qemu/run-ios.sh $(SECS) -display gtk | tee logs/qemu.log

phone:
	XL_SB_DELAY=8 tools/modem-cycle.sh "$(ARGS)" $(SECS)

prebuilt:
	tools/update-prebuilt.sh

clean:
	rm -rf xnu-src/xnu-8796.121.2/BUILD xnuloader/loader.obj xnuloader/loader.efi userland/root.apfs userland/system.apfs userland/obj
