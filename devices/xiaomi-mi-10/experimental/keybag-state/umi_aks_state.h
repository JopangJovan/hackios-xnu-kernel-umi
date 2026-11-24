/* Public state encoding for an explicitly opted-in, independent test bag.
 * Does not change system bag selection, notifications or protection policy.
 */
#ifndef UMI_AKS_STATE_H
#define UMI_AKS_STATE_H
static size_t lab_der_field(uint8_t *out, size_t at, const char *key, uint8_t value) {
    size_t n = strlen(key);
    out[at++] = 0x30; out[at++] = (uint8_t)(n + 5);
    out[at++] = 0x0c; out[at++] = (uint8_t)n;
    memcpy(out + at, key, n); at += n;
    out[at++] = 2; out[at++] = 1; out[at++] = value;
    return at;
}
/* Matches the existing iOS 16.5 state schema; all bags here have a password.
 * Zero backoff/attempt fields mean no retry policy is implemented by this lab.
 */
static size_t lab_state_der(const UmiLabBag *bag, uint8_t out[128]) {
    size_t n = 2;
    n = lab_der_field(out, n, "sb", 0);
    n = lab_der_field(out, n, "sr", 0);
    n = lab_der_field(out, n, "ss", (bag->unlocked ? 0 : 1) | (bag->everUnlocked ? 4 : 0));
    n = lab_der_field(out, n, "sas", 0);
    n = lab_der_field(out, n, "sfa", 0);
    n = lab_der_field(out, n, "sgs", 0);
    n = lab_der_field(out, n, "sls", bag->unlocked ? 0 : 1);
    n = lab_der_field(out, n, "sgpe", 0);
    n = lab_der_field(out, n, "srcd", 0);
    out[0] = 0x31; out[1] = (uint8_t)(n - 2);
    return n;
}
#endif
