/* Temporary system-bag ABI, explicit diagnostic boot/root opt-in only.
 * Original MKB state events are implemented. Ordinary item-key wrapping and
 * filesystem protection remain unchanged; this is not full data protection.
 */
#ifndef UMI_AKS_NATIVE_H
#define UMI_AKS_NATIVE_H
static struct {
    IOLock *lock;
    task_t controller;
    bool enabled, installed, awaitingLoad, firstEventSent;
    UmiLabBag bag;
} umi_native;
static bool umi_native_init(void) {
    umi_native.lock=IOLockAlloc();
    if(!umi_native.lock)return false;
    int lab=0,boot=0;
    if(PE_parse_boot_argn("umi_aks_lab",&lab,sizeof(lab)) && lab==1 &&
       PE_parse_boot_argn("umi_aks_system_boot",&boot,sizeof(boot)) && boot==1) {
        /* The service is locked before registerService: there is no interval
         * in which early clients see a disabled passcode while loading waits. */
        umi_native.enabled=true;umi_native.awaitingLoad=true;
        IOLog("UMI AKS native: boot starts locked awaiting persisted system bag\n");
    }
    return true;
}
static UmiLabBag *umi_native_local(UmiAksLab *s,uint64_t handle) {
    for(unsigned i=0;i<4;i++)if(s->bags[i].handle && s->bags[i].handle==handle)return &s->bags[i];
    return NULL;
}
static uint32_t umi_native_flags(void) {
    if(!umi_native.enabled)return 6;
    if(!umi_native.installed)return umi_native.awaitingLoad?1:6;
    return (umi_native.bag.unlocked?0:1)|(umi_native.bag.everUnlocked?4:0);
}
static size_t umi_native_state(uint8_t der[128]) {
    if(umi_native.installed)return lab_state_der(&umi_native.bag,der);
    size_t n=2;
    n=lab_der_field(der,n,"sb",0);n=lab_der_field(der,n,"sr",0);
    n=lab_der_field(der,n,"ss",umi_native.awaitingLoad?1:6);n=lab_der_field(der,n,"sas",0);
    n=lab_der_field(der,n,"sfa",0);n=lab_der_field(der,n,"sgs",0);
    n=lab_der_field(der,n,"sls",umi_native.awaitingLoad?1:3);n=lab_der_field(der,n,"sgpe",0);
    n=lab_der_field(der,n,"srcd",0);der[0]=0x31;der[1]=(uint8_t)(n-2);return n;
}
/* Called with the per-client lock held. Lock order: client, then system. */
static bool umi_native_method(UmiAksLab *s,uint32_t sel,IOExternalMethodArguments *a,
                              bool admin,bool deviceEntitled,IOReturn *answer,uint32_t *events) {
    if(!umi_native.lock)return false;
    IOLockLock(umi_native.lock);
    bool handled=true;
    uint32_t before=umi_native_flags();
    *events=0;
    IOReturn ret=kIOReturnBadArgument;
    const uint64_t *v=a->scalarInput;uint64_t *out=a->scalarOutput;
    uint8_t oldpass[LAB_MAX_SECRET]={0},newpass[LAB_MAX_SECRET]={0};
    uint8_t master[32]={0},uuid[16]={0},blob[LAB_BAG_SIZE]={0},der[128]={0};
    UmiLabBag *bag=NULL,*candidate=NULL;
    if(sel==0x7c) {
        int boot=0;
        if(!admin||current_task()!=s->owner||!PE_parse_boot_argn("umi_aks_lab",&boot,sizeof(boot))||boot!=1){ret=kIOReturnNotPrivileged;goto done;}
        if(a->scalarInputCount!=2||!v||v[0]!=0x554d4e41||v[1]>2||a->structureInputSize||a->scalarOutputCount)goto done;
        if(v[1]) {
            if(umi_native.enabled) {
                if(v[1]!=2 || umi_native.controller || !umi_native.awaitingLoad || umi_native.installed) {
                    ret=kIOReturnBusy;goto done;
                }
                /* A root controller may adopt the pending boot, never reset it
                 * to fresh/passwordless mode when persistence is missing. */
                task_reference(s->owner);umi_native.controller=s->owner;
                ret=kIOReturnSuccess;goto done;
            }
            task_reference(s->owner);umi_native.controller=s->owner;umi_native.enabled=true;
            umi_native.awaitingLoad=v[1]==2;umi_native.firstEventSent=false;
            ret=kIOReturnSuccess;goto done;
        }
        if(!umi_native.enabled||umi_native.controller!=s->owner){ret=kIOReturnNotPrivileged;goto done;}
        task_deallocate(umi_native.controller);umi_native.controller=NULL;
        umi_native.enabled=false;umi_native.installed=false;umi_native.awaitingLoad=false;lab_wipe(&umi_native.bag,sizeof(umi_native.bag));
        ret=kIOReturnSuccess;goto done;
    }
    if(!umi_native.enabled){handled=false;goto done;}
    /* Deliberately retain the prior ordinary item-key path. This mode tests
     * system-bag management, not migration of the existing system keychain. */
    if((sel==0xa||sel==0xb) && (!a->scalarInputCount||!v||
        ((v[0]&0xffff0000)!=0x554d0000&&(v[0]&0xffff0000)!=0x554e0000))) {handled=false;goto done;}
    if(!(sel==2||sel==3||sel==4||sel==5||sel==6||sel==7||sel==8||sel==9||sel==0xa||sel==0xb||sel==0xc||
         sel==0xd||sel==0xe||sel==0xf||sel==0x11||sel==0x17||sel==0x20||sel==0x23||sel==0x69)) {handled=false;goto done;}
    if(current_task()!=s->owner){ret=kIOReturnNotPrivileged;goto done;}
    if(a->scalarInputCount&&!v)goto done;
    if(sel==2 && a->scalarInputCount==6 && v[0]!=0){handled=false;goto done;}
    /* SpringBoard's original entitlement permits device lock/unlock only.
     * It does not permit controlling this experiment, replacing a bag, or
     * exporting its serialization. Never trust a process name or client input. */
    if(!(sel==7||sel==0xe||sel==0x11||sel==0x17||sel==0x23) && !admin &&
       !(deviceEntitled && a->scalarInputCount && v[0]==0 && (sel==8||sel==9||sel==0x20))) {
        ret=kIOReturnNotPrivileged;goto done;
    }
    if(sel==2||sel==6){ret=umi_lab_method(s,sel,a);goto done;}
    if(sel==0xe) {
        if(a->scalarInputCount>1||(a->scalarInputCount&&v[0])||a->structureInputSize||!out||a->scalarOutputCount!=1)goto done;
        /* Original keybagd --init loads only on 0xe00002f0 (NotFound). */
        if(umi_native.awaitingLoad){ret=kIOReturnNotFound;goto done;}
        out[0]=0;ret=kIOReturnSuccess;goto done;
    }
    if(!a->scalarInputCount)goto done;
    if(v[0]) {
        bag=umi_native_local(s,v[0]);
        if(!bag){ret=kIOReturnNotFound;goto done;}
    } else if(umi_native.installed)bag=&umi_native.bag;
    if(sel==0xf) {
        /* Missing persisted credentials must never become a fresh passwordless bag. */
        if(umi_native.awaitingLoad){ret=kIOReturnNotReady;goto done;}
        if(a->scalarInputCount!=11||!out||a->scalarOutputCount!=1||a->structureInputSize||
           (v[1]!=0&&v[1]!=4)||v[2]!=0xff||v[7]||v[8]||v[9]||v[10])goto done;
        /* Stock non-xART keybagd requests two-phase replacement with flag 4.
         * Both supported forms return a candidate; neither mutates the active bag. */
        ret=lab_secret(v[5],v[6],newpass);if(ret)goto done;
        if(bag) {
            ret=lab_secret(v[3],v[4],oldpass);if(ret)goto done;
            if(lab_open_bag(bag->blob,LAB_BAG_SIZE,oldpass,v[4],master)){ret=kIOReturnNotPermitted;goto done;}
            memcpy(uuid,bag->blob+48,16);
        } else {
            if(v[0]||v[3]||v[4]){ret=kIOReturnNotPermitted;goto done;}
            lab_random(master,32);lab_random(uuid,16);
        }
        for(unsigned i=0;i<4;i++)if(!s->bags[i].handle){candidate=&s->bags[i];break;}
        if(!candidate||s->sequence>=0xfffe){ret=kIOReturnNoResources;goto done;}
        if(lab_seal_bag(newpass,v[6],uuid,master,blob)){ret=kIOReturnError;goto done;}
        candidate->handle=0x554e0000|++s->sequence;
        candidate->unlocked=candidate->everUnlocked=true;
        memcpy(candidate->blob,blob,sizeof(blob));memcpy(candidate->master,master,32);
        out[0]=candidate->handle;ret=kIOReturnSuccess;
        IOLog("UMI AKS native: staged password candidate; active bag unchanged\n");goto done;
    }
    if(sel==5) {
        if(a->scalarInputCount!=4||!v[0]||!bag||v[1]||a->structureInputSize||a->scalarOutputCount)goto done;
        bool unlocked=v[3]!=0;
        if(unlocked) {
            ret=lab_secret(v[2],v[3],newpass);if(ret)goto done;
            if(lab_open_bag(bag->blob,LAB_BAG_SIZE,newpass,v[3],master)){ret=kIOReturnNotPermitted;goto done;}
        } else if(v[2])goto done;
        UmiLabBag replacement={};memcpy(replacement.blob,bag->blob,LAB_BAG_SIZE);
        replacement.unlocked=replacement.everUnlocked=unlocked;
        if(unlocked)memcpy(replacement.master,master,32);
        lab_wipe(&umi_native.bag,sizeof(umi_native.bag));umi_native.bag=replacement;
        lab_wipe(&replacement,sizeof(replacement));umi_native.installed=true;umi_native.awaitingLoad=false;ret=kIOReturnSuccess;
        IOLog("UMI AKS native: installed system candidate; locked=%d\n",!unlocked);goto done;
    }
    if(sel==0x69) {
        /* Verify the installed secret after save/install. This is not an
         * effaceable-storage roll or protection against old serialized bags. */
        if(a->scalarInputCount!=3||a->structureInputSize||a->scalarOutputCount||!bag)goto done;
        ret=lab_secret(v[1],v[2],newpass);if(ret)goto done;
        ret=lab_open_bag(bag->blob,LAB_BAG_SIZE,newpass,v[2],master)?kIOReturnNotPermitted:kIOReturnSuccess;
        goto done;
    }
    if(v[0]) {ret=umi_lab_method(s,sel,a);goto done;}
    if(sel==7) {
        if(a->scalarInputCount!=1||a->structureInputSize||!out||a->scalarOutputCount!=1)goto done;
        out[0]=umi_native_flags();ret=kIOReturnSuccess;goto done;
    }
    if(sel==0x11||sel==0x23) {
        if(a->scalarInputCount!=1||a->structureInputSize||a->scalarOutputCount)goto done;
        ret=lab_answer(a,der,umi_native_state(der));goto done;
    }
    if(sel==4){ret=kIOReturnNotPermitted;goto done;}
    if(sel==3||sel==0x17) {
        if(a->scalarInputCount!=1||a->structureInputSize||a->scalarOutputCount)goto done;
        if(!bag){ret=kIOReturnNotReady;goto done;}
        ret=lab_answer(a,sel==3?bag->blob:bag->blob+48,sel==3?LAB_BAG_SIZE:16);goto done;
    }
    if(sel==8||sel==0x20||sel==0xd) {
        if(a->scalarInputCount!=1||a->structureInputSize||a->scalarOutputCount)goto done;
        if(bag){lab_wipe(bag->master,32);bag->unlocked=false;}ret=kIOReturnSuccess;goto done;
    }
    if(sel==9||sel==0xc) {
        if(a->scalarInputCount!=3||a->structureInputSize||a->scalarOutputCount)goto done;
        if(!bag){ret=umi_native.awaitingLoad?kIOReturnNotReady:(!v[1]&&!v[2])?kIOReturnSuccess:kIOReturnNotPermitted;goto done;}
        ret=lab_secret(v[1],v[2],newpass);if(ret)goto done;
        if(lab_open_bag(bag->blob,LAB_BAG_SIZE,newpass,v[2],master)){ret=kIOReturnNotPermitted;goto done;}
        memcpy(bag->master,master,32);bag->unlocked=bag->everUnlocked=true;ret=kIOReturnSuccess;goto done;
    }
done:
    /* The original MKB event ABI is state-changed and FIRST unlock, not a
     * lock/unlock pair. Deliver after dropping both locks; observers re-query
     * the current state. No notification on rejected authentication. */
    if(handled && ret==kIOReturnSuccess) {
        if(before!=umi_native_flags())*events|=1;
        if(umi_native.enabled && umi_native.installed && umi_native.bag.everUnlocked && !umi_native.firstEventSent) {
            umi_native.firstEventSent=true;*events|=2;
        }
    }
    lab_wipe(oldpass,sizeof(oldpass));lab_wipe(newpass,sizeof(newpass));lab_wipe(master,sizeof(master));
    lab_wipe(uuid,sizeof(uuid));lab_wipe(blob,sizeof(blob));lab_wipe(der,sizeof(der));
    IOLockUnlock(umi_native.lock);*answer=ret;return handled;
}
#endif
