/* Isolated diagnostic policy, authenticated by a PUBLIC fixture key through
 * journal.c. Not a hardware counter or an offline-guessing defense. */
#ifndef UMI_NATIVE_RETRY_POLICY_H
#define UMI_NATIVE_RETRY_POLICY_H
#include "journal.h"
#include <stdbool.h>
enum { RP_HEADER=64, RP_MAX=TJ_MAX-RP_HEADER };
enum rp_operation { RP_NONE, RP_INITIALIZE, RP_DEBIT, RP_RESET, RP_CHANGE };
typedef uint64_t (*rp_clock_fn)(void *);
struct retry_policy {
    struct tj_store *store;
    rp_clock_fn clock;void *clock_arg;
    uint64_t sequence,generation,deadline,last_clock;
    uint32_t failures,delay;
    size_t length;uint8_t payload[RP_MAX];
    bool loaded,poisoned,attempt,authorized,time_saturated;
    enum rp_operation operation;
};
int rp_open(struct retry_policy *,struct tj_store *,rp_clock_fn,void *);
void rp_clear(struct retry_policy *);
int rp_initialize(struct retry_policy *,const void *,size_t);
int rp_begin(struct retry_policy *);
int rp_finish(struct retry_policy *,bool correct);
int rp_change(struct retry_policy *,const void *,size_t);
uint64_t rp_wait_ms(struct retry_policy *);
uint32_t rp_delay_for(uint32_t failures);
#endif
