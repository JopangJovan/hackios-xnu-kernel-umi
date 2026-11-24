#!/usr/bin/env python3
"""Bind handle zero only inside the already isolated ordinary-AKS client."""
import argparse
import hashlib
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--xnu', type=Path, required=True)
a = p.parse_args()
target = a.xnu / 'iokit/Kernel/UmiAksLab.h'
old = target.read_bytes()
if hashlib.sha256(old).hexdigest() != 'a0899e8cf00e94176eaf3432c4bb4a6ada6a3fba0c51941b4857818d793ecaab':
    raise SystemExit('Expected the exact ordinary-AKS v15 laboratory source')
text = old.decode()
def replace(before, after):
    global text
    assert text.count(before) == 1, before
    text = text.replace(before, after)
replace('uint32_t sequence;', 'uint32_t sequence, systemHandle;')
replace('UmiLabBag *bag = NULL;', 'UmiLabBag *bag = NULL;\n    uint64_t selected = 0;')
replace('''if (!a->scalarInputCount || !in[0]) goto done;
    for (unsigned i = 0; i < 4; ++i) if (s->bags[i].handle && s->bags[i].handle == in[0])''', '''if (!a->scalarInputCount) goto done;
    selected = in[0];
    if (!selected && s->ordinaryMode) selected = s->systemHandle;
    if (!selected) goto done;
    for (unsigned i = 0; i < 4; ++i) if (s->bags[i].handle && s->bags[i].handle == selected)''')
replace('''lab_wipe(bag, sizeof(*bag)); ret = kIOReturnSuccess; break;
    case 7:''', '''if (s->systemHandle == bag->handle) s->systemHandle = 0;
        lab_wipe(bag, sizeof(*bag)); ret = kIOReturnSuccess; break;
    case 5: /* aks_set_system(h, 0): this client only, never the real system bag */
        if (!s->ordinaryMode || a->scalarInputCount != 4 || !in[0] || in[1] || in[2] || in[3] ||
            n || a->scalarOutputCount || a->structureOutputSize || a->structureOutputDescriptor) break;
        if (!bag->unlocked) { ret = kIOReturnNotPrivileged; break; }
        s->systemHandle = bag->handle;
        IOLog("UMI ordinary AKS: client-local default bag bound\\n");
        ret = kIOReturnSuccess; break;
    case 7:''')
target.write_text(text)
print('Staged explicit client-local default bag; real system dispatch unchanged')
