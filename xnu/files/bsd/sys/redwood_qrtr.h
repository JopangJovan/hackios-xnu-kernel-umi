/*
 * QRTR, as far as a client inside the kernel needs it: datagrams to and from the services that
 * run on Qualcomm's other processors, each service at a (node, port) that its processor's name
 * service announces. Here the only other processor is the phone's Wi-Fi DSP, reached over GLINK
 * (bsd/dev/redwood_wifi.c), and we are node 1.
 *
 * Callbacks come from one thread, the link's, one at a time; they may sleep and may call
 * anything below. A datagram is at most REDWOOD_QRTR_MAX bytes either way.
 */
#ifndef _SYS_REDWOOD_QRTR_H_
#define _SYS_REDWOOD_QRTR_H_

#ifdef KERNEL
#include <sys/types.h>

#define REDWOOD_QRTR_MAX        8192

struct redwood_qrtr_port;

/* A datagram for the port has arrived. The data is the caller's again on return. */
typedef void (*redwood_qrtr_receive_t)(void *context, uint32_t node, uint32_t port, const void *data, size_t length);
/* A service that was looked up is there, at that node and port (arrived), or is no longer. */
typedef void (*redwood_qrtr_service_t)(void *context, bool arrived, uint32_t node, uint32_t port);

/* A port of ours, with a number of its own. NULL: there are no more. */
struct redwood_qrtr_port *redwood_qrtr_open(redwood_qrtr_receive_t receive, void *context);
/* One datagram to a node's port. May sleep (for seconds, if the other side is slow to take it). 0, or an errno. */
int redwood_qrtr_send(struct redwood_qrtr_port *port, uint32_t node, uint32_t to, const void *data, size_t length);
/*
 * Say when a service appears and disappears: for those already there the callback comes at
 * once (from the link's thread, like every callback). `instance` 0: any. 0, or an errno.
 */
int redwood_qrtr_lookup(struct redwood_qrtr_port *port, uint32_t service, uint32_t version, uint32_t instance,
    redwood_qrtr_service_t callback);
/* No callback for the port comes after this returns. */
void redwood_qrtr_close(struct redwood_qrtr_port *port);
#endif /* KERNEL */

#endif /* _SYS_REDWOOD_QRTR_H_ */
