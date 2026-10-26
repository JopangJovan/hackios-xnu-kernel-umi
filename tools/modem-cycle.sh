#!/bin/bash
# tools/modem-cycle.sh "KERNEL ARGS" [SECS] [LOG]: wait for the phone (up to 6 h), enter the loader, upload the
# system image, run the kernel with those args, summarize the modem's log, and put the phone back into Android.
# The phone is never flashed: the kernel is booted with "fastboot boot"-like upload through xl.
cd "$(dirname "$0")/.." || exit 1
PATH=$PWD/tools/macos-bin/platform-tools:$PWD/tools/macos-bin:$PATH
ARGS=$1; SECS=${2:-300}
for i in $(seq 1 2160); do s=$(./xl state); [ "$s" != absent ] && break; sleep 10; done
echo "$(date +%T) phone: $s"; [ "$s" = absent ] && { echo "NO PHONE"; exit 1; }
./xl enter 2>&1 | tail -1
./xl ramdisk userland/system.apfs 2>&1 | tail -1
XL_SB_DELAY=${XL_SB_DELAY:-600} XL_CMDLINE="debug=0x8 rd=md0 hv_wdt_pet=1 bootcpu=7 cpus=8 cpufreq=1 ufs=2 $ARGS" XL_SECS=$SECS XL_FOLLOW=1 XL_FOLLOW_KEEP=1 ./xl run 2>&1 | tail -2
echo "$(date +%T) state after run: $(./xl state)"
L=$(ls -t logs/*-run.log.live | head -1); F=${L%.live}; echo "LOG=$F"
cat $L $F 2>/dev/null | grep -a "mpss: \(7: \(THE\|the watch\)\|11\|rmtfs\|tftp\|servreg\|dms\|nas\|ipa: \(THE\|from node\|the modem\|a probe\)\|link: \(THE\|tftp\)\|LAST WORDS\|step 1[01]\)\|serving system\|panic(\|ipa: \|cell: \|net: en1\|wda: \|wds: \(THE\|the client\|about\)" | grep -v "about to ask\|about to tell" | sed 's/^\[ *[0-9.]*\]: //' | awk '!seen[$0]++' | cut -c1-230 | head -80
echo "== back to Android"
case "$(./xl state)" in loader|fastboot) XL_FORCE=1 ./xl reboot >/dev/null 2>&1 ;; esac
for i in $(seq 1 40); do s=$(./xl state); [ "$s" = android ] && break; sleep 5; done
echo "$(date +%T) phone: $s"; echo DONE
