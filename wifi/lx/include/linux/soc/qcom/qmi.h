/*
 * QMI, the protocol in which the driver talks to the radio's firmware before the radio's own
 * registers exist. The helpers are the kernel's (bsd/sys/redwood_qmi.h, bsd/dev/redwood_qmi.c),
 * under Linux's names; a kernel tree that does not have them yet gets LinuxKPI's header, in
 * which every call fails.
 */
#ifndef _LX_LINUX_SOC_QCOM_QMI_H
#define _LX_LINUX_SOC_QCOM_QMI_H

#ifdef LX_REDWOOD_QMI_H
#include <linux/jiffies.h>
#define REDWOOD_QMI_JIFFIES_TO_MS(j)	jiffies_to_msecs(j)
#include LX_REDWOOD_QMI_H
#else
#include <linux/socket.h>
#include_next <linux/soc/qcom/qmi.h>
#endif

#endif
