#ifndef UMI_TXN_AUTH_NOTIFY_H
#define UMI_TXN_AUTH_NOTIFY_H

#include <stdint.h>
#include <stdio.h>

/* A private observer signal. Receivers must never grant access from a signal
 * or from notify_get_state: neither proves authentication to that receiver. */
enum umi_txn_auth_state {
    UMI_TXN_UNAVAILABLE = 1,
    UMI_TXN_LOCKED,
    UMI_TXN_AUTHENTICATED,
    UMI_TXN_FIRST_AUTHENTICATION, /* Separate signal so state updates cannot overwrite it. */
};

static inline void umi_txn_notify_name(char name[64], const char *root, const char *event)
{
    snprintf(name, 64, "com.hakios.umi.txn.%s.%s", root + sizeof("/var/tmp/") - 1, event);
}

#endif
