#!/bin/bash
# Fetch the driver of the phone's Wi-Fi radio: ath11k, Qualcomm's driver for the WCN6750 (and its
# kin) as it is in Linux. Its files are BSD-3-Clause-Clear; they are taken from a pinned release of
# Linux, checked, and left where the kernel's build finds them. Nothing of Linux is in the
# repository: what the driver expects of a Linux kernel around it is ours, written from what
# those interfaces are documented to do.
#
#   VERSION   the release. 7.0.14: the one the work on the rest of the driver was begun with (it
#             is to be compiled against FreeBSD's LinuxKPI headers, which follow Linux 7.0). For
#             the WCN6750 the driver asks the same of firmware and platform as in the 6.6 series:
#             the entries for "wcn6750 hw1.0" and the messages of qmi.c are the same.
#
# What comes out, in xnu-deps/build/ath11k:
#   src/ath11k/         drivers/net/wireless/ath/ath11k of that release, untouched
#   src/spectral_common.h  the one file of drivers/net/wireless/ath/ that ath11k's headers include
#   src/.release        the release and the archive's checksum, for whoever asks what this is
# Needs on this machine: curl, tar, xz, sha256sum. The archive is 150 MB and is kept.
set -e
VERSION=7.0.14
SHA256=de9999b784d2293f00d39c62d8f92a08ab8a54bc4e80ffd250a0c09cb07a0f98
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
B=$ROOT/xnu-deps/build/ath11k
ATH=drivers/net/wireless/ath
mkdir -p "$B"
cd "$B"

if [ "$(cat src/.release 2>/dev/null)" != "$VERSION $SHA256" ]; then
	[ -f linux-$VERSION.tar.xz ] || curl -sL --fail -o linux-$VERSION.tar.xz \
		https://cdn.kernel.org/pub/linux/kernel/v${VERSION%%.*}.x/linux-$VERSION.tar.xz
	echo "$SHA256  linux-$VERSION.tar.xz" | sha256sum -c --quiet
	rm -rf src unpacked
	mkdir unpacked
	tar xJf linux-$VERSION.tar.xz -C unpacked --strip-components=1 linux-$VERSION/$ATH/ath11k \
		linux-$VERSION/$ATH/spectral_common.h
	# every file must say itself that it may be used so: the licence is checked, not assumed
	for f in unpacked/$ATH/ath11k/*.[ch] unpacked/$ATH/spectral_common.h; do
		case "$(head -6 "$f")" in
		*"SPDX-License-Identifier: BSD-3-Clause-Clear"*) ;;
		*"Permission to use, copy, modify, and/or distribute this software"*) ;;      # (ISC, spelt out)
		*) echo "mk-ath11k.sh: $f does not carry the licence expected" >&2; exit 1 ;;
		esac
	done
	mkdir src
	mv unpacked/$ATH/ath11k src/ath11k
	mv unpacked/$ATH/spectral_common.h src/
	rm -rf unpacked
	echo "$VERSION $SHA256" > src/.release
fi
echo "mk-ath11k.sh: ath11k of Linux $VERSION: $B/src/ath11k," \
	"$(ls src/ath11k/*.c | wc -l) .c files, $(cat src/ath11k/*.[ch] | wc -l) lines"
