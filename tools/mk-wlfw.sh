#!/bin/bash
# Give the kernel's build ath11k's descriptions of the messages a WCN6750's firmware speaks, for
# bsd/dev/redwood_wlfw.c (a stand-in for ath11k's own qmi.c: see there). They are taken out of
# the driver's sources as they are -- the structures of its qmi.h and the tables of its qmi.c --
# and written as one header into the kernel's build directory, where the compiler finds it and
# git does not: ath11k's text is BSD-3-Clause-Clear and is not kept in the repository.
# Without that header redwood_wlfw.c compiles to a line that says so.
#
# usage: tools/mk-wlfw.sh          then build the kernel (./xnu-build.sh); again after a clean build
set -e
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
source "$ROOT/xnu-env.sh"
ATH=$ROOT/xnu-deps/build/ath11k/src/ath11k
[ -f "$ATH/qmi.c" ] || "$ROOT/tools/mk-ath11k.sh"
OUT=$XNU_SRC/BUILD/obj/DEVELOPMENT_ARM64_${MACHINE:-BCM2837}/bsd/DEVELOPMENT
[ -d "$OUT" ] || { echo "mk-wlfw.sh: no build directory $OUT: build the kernel once first" >&2; exit 1; }
python3 - "$ATH" "$(cat "$ATH/../.release" | cut -d' ' -f1)" > "$OUT/redwood_wlfw_ath11k.h.new" <<'PY'
import re
import sys

src, release = sys.argv[1], sys.argv[2]
c = open(src + '/qmi.c').read()
h = open(src + '/qmi.h').read()
print('/* ath11k\'s qmi.h and the message tables of its qmi.c (Linux %s, BSD-3-Clause-Clear), put together by' % release)
print(' * tools/mk-wlfw.sh: not to be edited, not to be kept. */')
# the names qmi.h uses of a Linux kernel around it, outside its message definitions: nothing here does anything
print('''#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
typedef uint64_t dma_addr_t;
#define __iomem
struct list_head { struct list_head *next, *prev; };
struct work_struct { int unused; };
struct workqueue_struct;
typedef struct { int unused; } spinlock_t;
typedef struct { int unused; } wait_queue_head_t;
''')
print(re.sub(r'^#include <linux/[^>]*>\n', '', h, flags=re.M))
# every table, in the file's order (a table is used by the ones after it)
print('\n'.join(re.findall(r'static const struct qmi_elem_info \w+\[\] = \{.*?\n\};\n', c, re.S)))
PY
cmp -s "$OUT/redwood_wlfw_ath11k.h.new" "$OUT/redwood_wlfw_ath11k.h" 2>/dev/null && rm "$OUT/redwood_wlfw_ath11k.h.new" ||
	mv "$OUT/redwood_wlfw_ath11k.h.new" "$OUT/redwood_wlfw_ath11k.h"
echo "mk-wlfw.sh: $OUT/redwood_wlfw_ath11k.h ($(grep -c 'qmi_elem_info .*\[\] = {' "$OUT/redwood_wlfw_ath11k.h") tables)"
