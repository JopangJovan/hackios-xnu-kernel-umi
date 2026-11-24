/* Included by the opt-in system-bag implementation, with its lock held. */
#ifndef UMI_AKS_SHARED_RETRY_H
#define UMI_AKS_SHARED_RETRY_H

#include "UmiAksRetryProtocol.h"

static struct {
    UmiAksLab *owner;
    uint64_t serial, request, lockEpoch;
    unsigned phase;
    IOReturn result;
    bool enabled, ready, changing, unlocking, replied;
} umi_retry_gate;

static void umi_retry_close(UmiAksLab *client)
{
    if (!umi_retry_gate.enabled || !umi_native.lock) return;
    IOLockLock(umi_native.lock);
    if (umi_retry_gate.owner == client) {
        umi_retry_gate.owner = NULL;
        umi_retry_gate.ready = umi_retry_gate.changing = false;
        IOLockWakeup(umi_native.lock, &umi_retry_gate, false);
    }
    IOLockUnlock(umi_native.lock);
}

static IOReturn umi_retry_rpc(uint32_t selector, UmiAksLab *client, IOExternalMethodArguments *args)
{
    auto &gate = umi_retry_gate;
    const uint64_t *in = args->scalarInput;
    if (args->structureInputSize || args->structureOutputSize ||
        args->structureInputDescriptor || args->structureOutputDescriptor)
        return kIOReturnBadArgument;
    if (selector == UMI_RETRY_CONTROL) {
        if (!in || args->scalarInputCount != 1 || args->scalarOutputCount) return kIOReturnBadArgument;
        if (in[0] == UMI_RETRY_REGISTER_OWNER) {
            if (gate.owner || gate.unlocking) return kIOReturnBusy;
            gate.owner = client;
            return kIOReturnSuccess;
        }
        if (gate.owner != client) return kIOReturnNotPrivileged;
        switch (in[0]) {
        case UMI_RETRY_MARK_READY:
            /* Recovery must not re-enable admission while the previous
             * authentication or password change still owns the gate. */
            if (gate.unlocking || gate.changing) return kIOReturnBusy;
            gate.ready = true;
            return kIOReturnSuccess;
        case UMI_RETRY_BEGIN_CHANGE:
            if (!gate.ready || gate.changing || gate.unlocking) return kIOReturnBusy;
            gate.changing = true;
            return kIOReturnSuccess;
        case UMI_RETRY_END_CHANGE:
            if (!gate.changing) return kIOReturnNotReady;
            gate.changing = false;
            return kIOReturnSuccess;
        default:
            return kIOReturnBadArgument;
        }
    }
    if (!gate.owner || gate.owner == client || gate.owner->owner != client->owner)
        return kIOReturnNotPrivileged;
    if (selector == UMI_RETRY_POLL) {
        if (args->scalarInputCount || args->scalarOutputCount != 2 || !args->scalarOutput)
            return kIOReturnBadArgument;
        args->scalarOutput[0] = gate.replied ? 0 : gate.request;
        args->scalarOutput[1] = gate.replied ? 0 : gate.phase;
        return kIOReturnSuccess;
    }
    if (!in || args->scalarInputCount != 2 || args->scalarOutputCount ||
        in[1] > 1 || !in[0] || in[0] != gate.request || gate.replied)
        return kIOReturnBadArgument;
    gate.result = in[1] ? kIOReturnSuccess : kIOReturnNotReady;
    gate.replied = true;
    IOLockWakeup(umi_native.lock, &gate, false);
    return kIOReturnSuccess;
}

static IOReturn umi_retry_exchange(unsigned phase)
{
    auto &gate = umi_retry_gate;
    if (!gate.owner || !gate.ready || gate.serial == UINT64_MAX) return kIOReturnNotReady;
    uint64_t deadline;
    clock_interval_to_deadline(15, kSecondScale, &deadline);
    gate.request = ++gate.serial;
    gate.phase = phase;
    gate.replied = false;
    while (gate.owner && !gate.replied) {
        if (IOLockSleepDeadline(umi_native.lock, &gate, deadline, THREAD_UNINT) == THREAD_TIMED_OUT)
            break;
    }
    IOReturn result = gate.owner && gate.replied ? gate.result : kIOReturnNotReady;
    /* A timeout can race a durable debit/reset. Require daemon recovery to
     * reconcile it before a later caller can authenticate. */
    if (!gate.replied) gate.ready = false;
    gate.request = 0;
    gate.phase = 0;
    return result;
}

#endif
