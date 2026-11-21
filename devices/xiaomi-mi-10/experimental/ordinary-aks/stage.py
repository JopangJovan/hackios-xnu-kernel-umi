#!/usr/bin/env python3
"""Stage a root + entitlement + boot-flag gated AKS experiment in a disposable tree."""
import argparse
import hashlib
from pathlib import Path
import shutil

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--xnu',type=Path,required=True)
a=p.parse_args();src=Path(__file__).resolve().parent;k=a.xnu/'iokit/Kernel'
if (a.xnu/'BUILD').exists():
    raise SystemExit('Use a fresh source tree without BUILD; generated Makefiles embed source paths')
pins={'MiniAppleKeyStore.cpp':'e3f87b9ca14351a17ff830de8396e1d2571969ebd6fd347304f2905086b5e88e',
      'UmiAksLab.h':'a758fd4196e8f1eeecdf3530f0e911becabb2416d0404934f8fc263a19d7f9c8'}
for name,sha in pins.items():
    if hashlib.sha256((k/name).read_bytes()).hexdigest()!=sha:raise SystemExit('Unknown or already modified input: '+name)
lab=(k/'UmiAksLab.h').read_text().replace('#include "umi_aks_crypto.h"','#include "umi_aks_crypto.h"\n#include "umi_ordinary_keys.h"')
lab=lab.replace('uint8_t blob[LAB_BAG_SIZE], master[32];','uint8_t blob[LAB_BAG_SIZE], master[32];\n    UmiOrdinaryKeys ordinary;')
lab=lab.replace('bool enabled;','bool enabled, ordinaryMode;')
lab=lab.replace('if (selector == 2 || selector == 6) {', '''if (s->ordinaryMode && selector == 0 && !a->scalarInputCount && !n && !a->scalarOutputCount) { ret=kIOReturnSuccess; goto done; }
    if (s->ordinaryMode && selector == 7 && a->scalarInputCount==1 && in[0]==UOK_SCOPE_HANDLE &&
        !n && out && a->scalarOutputCount==1) { out[0]=UOK_SCOPE_REPLY;ret=kIOReturnSuccess;goto done; }
    if (selector == 2 || selector == 6) {''')
lab=lab.replace('memcpy(bag->blob, next, LAB_BAG_SIZE);\n        bag->handle', '''memcpy(bag->blob, next, LAB_BAG_SIZE);
        uok_reset(&bag->ordinary);
        if(s->ordinaryMode&&bag->unlocked)uok_authenticated(&bag->ordinary,bag->master,bag->blob+48);
        bag->handle''')
lab=lab.replace('memcpy(bag->master, master, 32); bag->unlocked = bag->everUnlocked = true; ret = kIOReturnSuccess; break;', '''memcpy(bag->master, master, 32); bag->unlocked = bag->everUnlocked = true;
        if(s->ordinaryMode)uok_authenticated(&bag->ordinary,bag->master,bag->blob+48);
        ret = kIOReturnSuccess; break;''')
lab=lab.replace('case 0x0a: case 0x0b:\n', '''case 0x0a: case 0x0b:
        if(s->ordinaryMode) {
            if(a->scalarInputCount!=2||!a->structureInput||
               (selector==0xa&&(!out||a->scalarOutputCount!=1))||(selector==0xb&&a->scalarOutputCount))break;
            uint8_t ordinaryResult[80]={0};size_t length=sizeof(ordinaryResult);
            int e=uok_transform(&bag->ordinary,bag->master,bag->blob+48,bag->unlocked,selector==0xa,
                                in[1],(const uint8_t*)a->structureInput,n,ordinaryResult,&length);
            if(!e)ret=lab_answer(a,ordinaryResult,length);
            else if(e==UOK_LOCKED)ret=kIOReturnNotPrivileged;
            else if(e==UOK_MIGRATION)ret=kIOReturnNotReady;
            else if(e==UOK_UNSUPPORTED)ret=kIOReturnUnsupported;
            else if(e==UOK_SPACE)ret=kIOReturnNoSpace;
            else if(e==UOK_ARGUMENT||e==UOK_CORRUPT)ret=kIOReturnBadArgument;
            else ret=kIOReturnError;
            if(!ret&&selector==0xa){out[0]=in[1];a->scalarOutputCount=1;}
            lab_wipe(ordinaryResult,sizeof(ordinaryResult));break;
        }
''')
driver=(k/'MiniAppleKeyStore.cpp').read_text()
driver=driver.replace('fLab = umi_lab_alloc(owner);\n    return fLab != NULL;', '''fLab = umi_lab_alloc(owner);
    if(!fLab)return false;
    OSObject *ordinary=copyClientEntitlement(owner,"com.hakios.aks.ordinary-lab");
    if(ordinary==kOSBooleanTrue) {
        int lab=0;
        bool allowed=type==0 && PE_parse_boot_argn("umi_aks_lab",&lab,sizeof(lab)) && lab==1 &&
            clientHasPrivilege((void*)owner,kIOClientPrivilegeAdministrator)==kIOReturnSuccess;
        ordinary->release();
        /* An opted-in process must never silently fall back to the live bag. */
        if(!allowed){umi_lab_free(fLab);fLab=NULL;return false;}
        fLab->enabled=true;fLab->ordinaryMode=true;
        IOLog("UMI ordinary AKS: isolated entitled client opened\\n");
    } else if(ordinary)ordinary->release();
    return true;''')
assert lab.count('if(s->ordinaryMode&&bag->unlocked)')==1
assert lab.count('if(s->ordinaryMode)uok_authenticated')==1
assert driver.count('com.hakios.aks.ordinary-lab')==1
(k/'UmiAksLab.h').write_text(lab);(k/'MiniAppleKeyStore.cpp').write_text(driver)
shutil.copy2(src/'umi_ordinary_keys.h',k/'umi_ordinary_keys.h')
print('Staged only the isolated ordinary AKS gate and class-6/7 key operations')
