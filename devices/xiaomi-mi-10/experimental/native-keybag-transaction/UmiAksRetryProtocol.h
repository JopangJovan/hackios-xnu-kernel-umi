/* Shared kernel/daemon wire values. Keep numeric values stable. */
#ifndef UMI_AKS_RETRY_PROTOCOL_H
#define UMI_AKS_RETRY_PROTOCOL_H

enum {
    UMI_RETRY_CONTROL = 0x80,
    UMI_RETRY_POLL = 0x81,
    UMI_RETRY_REPLY = 0x82,
    UMI_RETRY_DEBIT = 1,
    UMI_RETRY_REJECT = 2,
    UMI_RETRY_ACCEPT = 3,
};

enum UmiRetryControlCommand {
    UMI_RETRY_REGISTER_OWNER = 0, /* Register the daemon connection. */
    UMI_RETRY_MARK_READY = 1,     /* Recovery finished; admit requests. */
    UMI_RETRY_BEGIN_CHANGE = 2,   /* Reserve the gate for a password change. */
    UMI_RETRY_END_CHANGE = 3,     /* Release the password-change reservation. */
};

#endif
