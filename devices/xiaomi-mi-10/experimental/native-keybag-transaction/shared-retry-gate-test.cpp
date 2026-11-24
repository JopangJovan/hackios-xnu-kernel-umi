/* Exercise the actual kernel gate with deterministic sleep/wakeup scenarios.
 * This models state transitions, not IOKit scheduling or a hardware reset. */
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <initializer_list>

using IOReturn = int;
enum {
    kIOReturnSuccess, kIOReturnBadArgument, kIOReturnBusy,
    kIOReturnNotPrivileged, kIOReturnNotReady,
    kSecondScale, THREAD_UNINT, THREAD_TIMED_OUT
};
struct UmiAksLab { void *owner; };
struct IOExternalMethodArguments {
    uint64_t *scalarInput = nullptr, *scalarOutput = nullptr;
    unsigned scalarInputCount = 0, scalarOutputCount = 0;
    size_t structureInputSize = 0, structureOutputSize = 0;
    void *structureInputDescriptor = nullptr, *structureOutputDescriptor = nullptr;
};
static struct { void *lock; } umi_native;
static void IOLockLock(void *) {}
static void IOLockUnlock(void *) {}
static void IOLockWakeup(void *, void *, bool) {}
static void clock_interval_to_deadline(unsigned, int, uint64_t *deadline) { *deadline = 1; }
static int (*on_sleep)();
static int IOLockSleepDeadline(void *, void *, uint64_t, int)
{
    assert(on_sleep);
    return on_sleep();
}
#include "UmiAksSharedRetry.h"

static int task_a, task_b, lock_object;
static UmiAksLab daemon{&task_a}, worker{&task_a}, stranger{&task_b};
static unsigned sleeps;
static IOReturn control(UmiAksLab *client, uint64_t operation)
{
    IOExternalMethodArguments args;
    args.scalarInput = &operation; args.scalarInputCount = 1;
    return umi_retry_rpc(UMI_RETRY_CONTROL, client, &args);
}
static IOReturn reply(UmiAksLab *client, uint64_t request, uint64_t accepted)
{
    uint64_t input[] = {request, accepted};
    IOExternalMethodArguments args;
    args.scalarInput = input; args.scalarInputCount = 2;
    return umi_retry_rpc(UMI_RETRY_REPLY, client, &args);
}
static void setup()
{
    umi_retry_gate = {};
    umi_retry_gate.enabled = true;
    umi_native.lock = &lock_object;
    sleeps = 0; on_sleep = nullptr;
    assert(control(&daemon, UMI_RETRY_REGISTER_OWNER) == kIOReturnSuccess);
}
static int answer()
{
    ++sleeps;
    uint64_t out[2] = {};
    IOExternalMethodArguments args;
    args.scalarOutput = out; args.scalarOutputCount = 2;
    assert(umi_retry_rpc(UMI_RETRY_POLL, &worker, &args) == kIOReturnSuccess);
    assert(out[0] == umi_retry_gate.request && out[1] == umi_retry_gate.phase);
    assert(out[0] && out[1]);
    assert(reply(&stranger, out[0], 1) == kIOReturnNotPrivileged);
    assert(reply(&daemon, out[0], 1) == kIOReturnNotPrivileged);
    assert(reply(&worker, out[0] + 1, 1) == kIOReturnBadArgument);
    assert(reply(&worker, out[0], 2) == kIOReturnBadArgument);
    assert(reply(&worker, out[0], 1) == kIOReturnSuccess);
    assert(reply(&worker, out[0], 1) == kIOReturnBadArgument);
    assert(umi_retry_rpc(UMI_RETRY_POLL, &worker, &args) == kIOReturnSuccess);
    assert(!out[0] && !out[1]);
    return 0;
}
static int rejected()
{
    ++sleeps;
    assert(reply(&worker, umi_retry_gate.request, 0) == kIOReturnSuccess);
    return 0;
}
static int timed_out() { ++sleeps; return THREAD_TIMED_OUT; }
static int disconnected()
{
    ++sleeps;
    umi_retry_close(&daemon);
    assert(!umi_retry_gate.owner && !umi_retry_gate.ready && !umi_retry_gate.changing);
    // An in-flight authenticator must finish before a new daemon can restore.
    assert(control(&stranger, UMI_RETRY_REGISTER_OWNER) == kIOReturnBusy);
    return 0;
}
static int spurious()
{
    ++sleeps;
    on_sleep = answer;
    return 0;
}
int main()
{
    setup();
    assert(control(&worker, UMI_RETRY_REGISTER_OWNER) == kIOReturnBusy);
    assert(control(&stranger, UMI_RETRY_MARK_READY) == kIOReturnNotPrivileged);
    assert(control(&daemon, UMI_RETRY_BEGIN_CHANGE) == kIOReturnBusy);
    assert(control(&daemon, UMI_RETRY_END_CHANGE) == kIOReturnNotReady);
    assert(umi_retry_exchange(UMI_RETRY_DEBIT) == kIOReturnNotReady);
    assert(!sleeps);
    assert(control(&daemon, UMI_RETRY_MARK_READY) == kIOReturnSuccess);
    assert(control(&daemon, UMI_RETRY_BEGIN_CHANGE) == kIOReturnSuccess);
    assert(control(&daemon, UMI_RETRY_MARK_READY) == kIOReturnBusy);
    assert(control(&daemon, UMI_RETRY_BEGIN_CHANGE) == kIOReturnBusy);
    assert(control(&daemon, UMI_RETRY_END_CHANGE) == kIOReturnSuccess);
    umi_retry_gate.unlocking = true;
    assert(control(&daemon, UMI_RETRY_MARK_READY) == kIOReturnBusy);
    assert(control(&daemon, UMI_RETRY_BEGIN_CHANGE) == kIOReturnBusy);
    on_sleep = answer;
    for (unsigned phase : {UMI_RETRY_DEBIT, UMI_RETRY_REJECT, UMI_RETRY_ACCEPT}) {
        uint64_t previous = umi_retry_gate.serial;
        assert(umi_retry_exchange(phase) == kIOReturnSuccess);
        assert(umi_retry_gate.serial == previous + 1);
        assert(!umi_retry_gate.request && !umi_retry_gate.phase);
        assert(reply(&worker, previous + 1, 1) == kIOReturnBadArgument);
    }
    on_sleep = rejected;
    assert(umi_retry_exchange(UMI_RETRY_DEBIT) == kIOReturnNotReady);
    assert(umi_retry_gate.ready); // A policy denial is not an unavailable service.
    on_sleep = spurious;
    unsigned before = sleeps;
    assert(umi_retry_exchange(UMI_RETRY_DEBIT) == kIOReturnSuccess);
    assert(sleeps == before + 2);
    on_sleep = timed_out;
    assert(umi_retry_exchange(UMI_RETRY_ACCEPT) == kIOReturnNotReady);
    assert(!umi_retry_gate.ready && !umi_retry_gate.request);
    assert(reply(&worker, umi_retry_gate.serial, 1) == kIOReturnBadArgument);
    before = sleeps;
    assert(umi_retry_exchange(UMI_RETRY_DEBIT) == kIOReturnNotReady && sleeps == before);
    assert(control(&daemon, UMI_RETRY_MARK_READY) == kIOReturnBusy);
    assert(!umi_retry_gate.ready);
    umi_retry_gate.unlocking = false;
    assert(control(&daemon, UMI_RETRY_MARK_READY) == kIOReturnSuccess);
    umi_retry_gate.unlocking = true;
    on_sleep = disconnected;
    assert(umi_retry_exchange(UMI_RETRY_ACCEPT) == kIOReturnNotReady);
    umi_retry_gate.unlocking = false;
    assert(control(&stranger, UMI_RETRY_REGISTER_OWNER) == kIOReturnSuccess);
    assert(!umi_retry_gate.ready);
    assert(reply(&worker, 1, 1) == kIOReturnNotPrivileged);
    setup();
    assert(control(&daemon, UMI_RETRY_MARK_READY) == kIOReturnSuccess);
    umi_retry_gate.serial = std::numeric_limits<uint64_t>::max();
    assert(umi_retry_exchange(UMI_RETRY_DEBIT) == kIOReturnNotReady && !sleeps);
    IOExternalMethodArguments malformed;
    malformed.scalarOutputCount = 2;
    assert(umi_retry_rpc(UMI_RETRY_POLL, &worker, &malformed) == kIOReturnBadArgument);
    malformed.structureInputSize = 1;
    assert(umi_retry_rpc(UMI_RETRY_POLL, &worker, &malformed) == kIOReturnBadArgument);
    puts("PASS shared retry gate: ownership, reservation, stale/duplicate replies, denial, spurious wake, timeout, disconnect, recovery and serial exhaustion");
}
