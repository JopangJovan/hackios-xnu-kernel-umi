/* Private isolated-volume ABI. Caller holds umi_native.lock for the entire
 * operation. This never authenticates a user or changes the system bag. */
#ifndef UMI_SYSTEM_VOLUME_H
#define UMI_SYSTEM_VOLUME_H
static IOReturn umi_system_volume_key(IOExternalMethodArguments *a, bool authorized) {
    if (!authorized) return kIOReturnNotPrivileged;
    if (!umi_native.enabled || !umi_native.installed) return kIOReturnNotReady;
    if (!umi_native.bag.unlocked) return kIOReturnNotPermitted;
    if (a->scalarInputCount != 2 || !a->scalarInput ||
        a->scalarInput[0] != 0x554d564b ||
        (a->scalarInput[1] != 1 && a->scalarInput[1] != 2) ||
        a->scalarOutputCount || a->structureInputDescriptor ||
        a->structureOutputDescriptor || !a->structureInput || !a->structureOutput)
        return kIOReturnBadArgument;
    bool wrap = a->scalarInput[1] == 1;
    size_t inputSize = wrap ? 32 : 40, outputSize = wrap ? 40 : 32;
    if (a->structureInputSize != inputSize || a->structureOutputSize != outputSize)
        return kIOReturnBadArgument;
    /* The stable system master survives a PIN change. A newly created bag has
     * a different master/UUID and cannot unwrap this volume's random key. */
    uint8_t info[48] = {0}, kek[32] = {0}, result[40] = {0};
    memcpy(info, "UMI-SYSTEM-VOLUME-v1", 20);
    memcpy(info + 32, umi_native.bag.blob + 48, 16);
    lab_hmac(umi_native.bag.master, 32, info, sizeof(info), kek);
    int failed = wrap ? lab_wrap(kek, (const uint8_t *)a->structureInput, inputSize, result)
                      : lab_unwrap(kek, (const uint8_t *)a->structureInput, inputSize, result);
    if (!failed) memcpy(a->structureOutput, result, outputSize);
    else memset(a->structureOutput, 0, outputSize);
    lab_wipe(info, sizeof(info)); lab_wipe(kek, sizeof(kek)); lab_wipe(result, sizeof(result));
    return failed ? kIOReturnNotPermitted : kIOReturnSuccess;
}
#endif
