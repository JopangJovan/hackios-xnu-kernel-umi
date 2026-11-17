/* Root-only, opt-in independent-bag laboratory. No system handle access.
 * Kept outside the default build; see README before staging this experiment.
 */
#ifndef UMI_AKS_LAB_H
#define UMI_AKS_LAB_H
#include <IOKit/IOLocks.h>
#include <pexpert/pexpert.h>
#include <kern/task.h>
#include <sys/systm.h>
extern "C" {
#include <corecrypto/ccmode.h>
#include <corecrypto/cchmac.h>
#include <corecrypto/ccsha2.h>
extern const struct ccmode_ecb mc_aes_ecb_encrypt, mc_aes_ecb_decrypt;
void read_random(void *, u_int);
}
static int lab_aes(int decrypt, const uint8_t key[32], uint8_t block[16]) {
    const struct ccmode_ecb *mode = decrypt ? &mc_aes_ecb_decrypt : &mc_aes_ecb_encrypt;
    return ccecb_one_shot(mode, 32, key, 1, block, block);
}
static void lab_hmac(const uint8_t *key, size_t kn, const uint8_t *data, size_t dn, uint8_t out[32]) {
    cchmac(ccsha256_di(), kn, key, dn, data, out);
}
static int lab_random(uint8_t *out, size_t size) { read_random(out, (u_int)size); return 0; }
#include "umi_aks_crypto.h"

struct UmiLabBag {
    uint32_t handle;
    bool unlocked, everUnlocked;
    uint8_t blob[LAB_BAG_SIZE], master[32];
};
struct UmiAksLab {
    IOLock *lock;
    task_t owner;
    uint32_t sequence;
    bool enabled;
    UmiLabBag bags[4];
};
static UmiAksLab *umi_lab_alloc(task_t owner) {
    UmiAksLab *s = (UmiAksLab *)IOMallocZero(sizeof(*s));
    if (!s) return NULL;
    s->lock = IOLockAlloc();
    if (!s->lock) { IOFree(s, sizeof(*s)); return NULL; }
    s->owner = owner; task_reference(owner); return s;
}
static void umi_lab_free(UmiAksLab *s) {
    if (!s) return;
    IOLockFree(s->lock); task_deallocate(s->owner);
    lab_wipe(s, sizeof(*s)); IOFree(s, sizeof(*s));
}
static IOReturn lab_secret(uint64_t pointer, uint64_t length, uint8_t out[LAB_MAX_SECRET]) {
    if (!pointer || !length || length > LAB_MAX_SECRET) return kIOReturnBadArgument;
    return copyin((user_addr_t)pointer, out, (size_t)length) ? kIOReturnBadArgument : kIOReturnSuccess;
}
static IOReturn lab_answer(IOExternalMethodArguments *a, const uint8_t *data, size_t len) {
    if (a->structureOutputDescriptor) {
        IOMemoryDescriptor *d = a->structureOutputDescriptor;
        if (d->getLength() < len) return kIOReturnNoSpace;
        IOReturn ret = d->prepare();
        if (ret != kIOReturnSuccess) return ret;
        bool copied = d->writeBytes(0, data, len) == len;
        IOReturn complete = d->complete();
        if (!copied) return kIOReturnIOError;
        if (complete != kIOReturnSuccess) return complete;
        a->structureOutputDescriptorSize = len; return kIOReturnSuccess;
    }
    if (!a->structureOutput || a->structureOutputSize < len) return kIOReturnNoSpace;
    memcpy(a->structureOutput, data, len); a->structureOutputSize = len; return kIOReturnSuccess;
}

/* The caller holds the client mutex; no global/system bag can be selected. */
static IOReturn umi_lab_method(UmiAksLab *s, uint32_t selector, IOExternalMethodArguments *a) {
    uint8_t oldpass[LAB_MAX_SECRET] = {0}, newpass[LAB_MAX_SECRET] = {0};
    uint8_t master[32] = {0}, key[32] = {0}, next[LAB_BAG_SIZE] = {0}, result[72] = {0};
    IOReturn ret = kIOReturnBadArgument;
    UmiLabBag *bag = NULL;
    const uint64_t *in = a->scalarInput;
    uint64_t *out = a->scalarOutput;
    size_t n = a->structureInputSize;
    if (a->scalarInputCount && !in) goto done;
    if (selector == 2 || selector == 6) {
        if (!out || a->scalarOutputCount != 1 || s->sequence >= 0xfffe) goto done;
        for (unsigned i = 0; i < 4; ++i) if (!s->bags[i].handle) { bag = &s->bags[i]; break; }
        if (!bag) { ret = kIOReturnNoResources; goto done; }
        if (selector == 2) {
            /* Native aks_create_bag(secret, length, type=0, &handle). */
            if (a->scalarInputCount != 6 || in[0] != 0 || in[1] != UINT64_MAX || in[4] || in[5] || n) goto done;
            ret = lab_secret(in[2], in[3], oldpass); if (ret) goto done;
            uint8_t uuid[16];
            lab_random(master, sizeof(master)); lab_random(uuid, sizeof(uuid));
            int failed = lab_seal_bag(oldpass, in[3], uuid, master, next);
            lab_wipe(uuid, sizeof(uuid));
            if (failed) { ret = kIOReturnError; goto done; }
            memcpy(bag->master, master, sizeof(master)); bag->unlocked = bag->everUnlocked = true;
        } else {
            if (a->scalarInputCount || !a->structureInput || !lab_bag_format((const uint8_t *)a->structureInput, n)) goto done;
            memcpy(next, a->structureInput, LAB_BAG_SIZE);
            bag->unlocked = bag->everUnlocked = false;
        }
        memcpy(bag->blob, next, LAB_BAG_SIZE);
        bag->handle = 0x554d0000 | ++s->sequence;
        out[0] = bag->handle; a->scalarOutputCount = 1; ret = kIOReturnSuccess; goto done;
    }
    if (!a->scalarInputCount || !in[0]) goto done;
    for (unsigned i = 0; i < 4; ++i) if (s->bags[i].handle && s->bags[i].handle == in[0]) { bag = &s->bags[i]; break; }
    if (!bag) { ret = kIOReturnNotFound; goto done; }
    switch (selector) {
    case 3: /* save: serialize only the authenticated encrypted envelope */
        if (a->scalarInputCount != 1 || n) break;
        ret = lab_answer(a, bag->blob, LAB_BAG_SIZE); break;
    case 4: /* unload */
        if (a->scalarInputCount != 1 || n) break;
        lab_wipe(bag, sizeof(*bag)); ret = kIOReturnSuccess; break;
    case 7: /* public lock flags */
        if (a->scalarInputCount != 1 || !out || a->scalarOutputCount != 1 || n) break;
        out[0] = (bag->unlocked ? 0 : 1) | (bag->everUnlocked ? 4 : 0);
        a->scalarOutputCount = 1; ret = kIOReturnSuccess; break;
    case 0x0d: /* lock: destroy the unwrapped master */
        if (a->scalarInputCount != 1 || n) break;
        lab_wipe(bag->master, sizeof(bag->master)); bag->unlocked = false; ret = kIOReturnSuccess; break;
    case 0x0c: /* unlock */
        if (a->scalarInputCount != 3 || n) break;
        ret = lab_secret(in[1], in[2], oldpass); if (ret) break;
        if (lab_open_bag(bag->blob, LAB_BAG_SIZE, oldpass, in[2], master)) { ret = kIOReturnNotPermitted; break; }
        memcpy(bag->master, master, 32); bag->unlocked = bag->everUnlocked = true; ret = kIOReturnSuccess; break;
    case 0x0f: /* change-secret: reject unsupported SE/escrow extensions */
        if (a->scalarInputCount != 11 || !out || a->scalarOutputCount != 1 || n || in[1] || in[2] != 0xff ||
            in[7] || in[8] || in[9] || in[10]) break;
        ret = lab_secret(in[3], in[4], oldpass); if (ret) break;
        ret = lab_secret(in[5], in[6], newpass); if (ret) break;
        if (lab_open_bag(bag->blob, LAB_BAG_SIZE, oldpass, in[4], master)) { ret = kIOReturnNotPermitted; break; }
        if (lab_seal_bag(newpass, in[6], bag->blob + 48, master, next)) { ret = kIOReturnError; break; }
        /* Commit only after both authentication and encryption succeed. */
        memcpy(bag->blob, next, LAB_BAG_SIZE); out[0] = 0; a->scalarOutputCount = 1;
        ret = kIOReturnSuccess; break;
    case 0x0a: case 0x0b:
        if (a->scalarInputCount != 2 || in[1] < 1 || in[1] > 12 || !a->structureInput) break;
        if (!bag->unlocked) { ret = kIOReturnNotPermitted; break; }
        lab_class_key(bag->master, bag->blob + 48, (uint32_t)in[1], key);
        if (selector == 0x0a) {
            if (!out || a->scalarOutputCount != 1 || n < 16 || n > 64 || n % 8) break;
            if (lab_wrap(key, (const uint8_t *)a->structureInput, n, result)) { ret = kIOReturnError; break; }
            ret = lab_answer(a, result, n + 8);
            if (!ret) { out[0] = in[1]; a->scalarOutputCount = 1; }
        } else {
            if (n < 24 || n > 72 || n % 8) break;
            if (lab_unwrap(key, (const uint8_t *)a->structureInput, n, result)) { ret = kIOReturnNotPermitted; break; }
            ret = lab_answer(a, result, n - 8);
        }
        break;
    default: ret = kIOReturnUnsupported; break;
    }
done:
    lab_wipe(oldpass, sizeof(oldpass)); lab_wipe(newpass, sizeof(newpass));
    lab_wipe(master, sizeof(master)); lab_wipe(key, sizeof(key));
    lab_wipe(next, sizeof(next)); lab_wipe(result, sizeof(result));
    return ret;
}
#endif
