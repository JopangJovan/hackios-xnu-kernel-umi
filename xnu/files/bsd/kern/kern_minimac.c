/*
 * Stand-ins for the two closed security policies user space cannot do
 * without: AMFI and the Sandbox. On a device they are kexts that register
 * MAC policies of those names; system software talks to them through
 * __mac_syscall() and relies on what they decide.
 *
 * Neither stand-in is a security mechanism. This kernel runs whatever it is
 * given (see libkern/amfi/miniamfi.c); these only give the answers the iOS
 * user space needs in order to run at all:
 *
 *   AMFI     The binaries of the operating system carry ad hoc signatures
 *            (no certificate chain); on a device AMFI finds their hashes
 *            in the trust cache and flags them as platform binaries.
 *            launchd panics if a system daemon is not one. There is no
 *            trust cache here, so every ad hoc signed binary is taken for
 *            a platform binary. Unsigned programs (our own) are not.
 *
 *   Sandbox  Every query (may this process look up that service?) is
 *            answered "allowed", and every request to apply a profile
 *            succeeds without applying anything.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/codesign.h>
#include <sys/errno.h>
#include <sys/kpi_socket.h>
#include <sys/priv.h>
#include <sys/proc_internal.h>
#include <sys/ubc_internal.h>
#include <sys/vnode_internal.h>
#include <libkern/libkern.h>
#include <security/mac_framework.h>
#include <security/mac_policy.h>

void minimac_policy_init(void);

static int
miniamfi_vnode_check_signature(__unused struct vnode *vp, __unused struct label *label, __unused cpu_type_t cpu_type,
    struct cs_blob *cs_blob, unsigned int *cs_flags, __unused unsigned int *signer_type, __unused int flags,
    __unused unsigned int platform, __unused char **fatal_failure_desc, __unused size_t *fatal_failure_desc_len)
{
	if (cs_blob == NULL) {
		return 0;
	}
	/*
	 * Accepting a signature is what marks the process as signed; the
	 * kernel consults the signature of a process only when that is set.
	 */
	*cs_flags |= CS_SIGNED;
	if (*cs_flags & CS_ADHOC) {
		*cs_flags |= CS_PLATFORM_BINARY;
	}
	return 0;
}

/*
 * dyld asks AMFI what it may do for the process it is starting (request 0x5a, "check dyld
 * policy"): the argument is { flags in, pointer to flags out }. Everything is allowed --
 * @paths, the DYLD_* variables, inserted libraries and interposing -- which is what lets
 * our own library be loaded into stock daemons (DYLD_INSERT_LIBRARIES).
 */
#define AMFI_SYSCALL_CHECK_DYLD_POLICY  0x5a
#define AMFI_DYLD_OUTPUT_ALLOW_ALL      0xffULL

/*
 * Request 0x60, "query boot-arg state" (amfi_interface_query_bootarg_state() in libamfi): the
 * argument points to 64 bits of flags, one for each boot-arg that switches a part of AMFI off
 * (amfi_get_out_of_my_way and its kin). ExtensionKit asks, and unless one of the bits 0x86 is set
 * it wants the signature of the system's table of extension points
 * (/System/Library/xpc/extensions.plist.sig) verified by AMFI's IOKit service. There is no such
 * service here ("Unable to find AMFI"); the table is then not read and LaunchServices knows no
 * extension point: every application extension is refused when the applications are registered
 * ("does not correspond to a known extension point"), pkd cannot make a record for any extension
 * point, and Shortcuts dies on a nil one. Nothing is enforced by this kernel, so that is what it
 * says. (Which of the three bits stands for which boot-arg is not known; ExtensionKit takes any
 * of them.)
 */
#define AMFI_SYSCALL_QUERY_BOOTARG_STATE 0x60
#define AMFI_BOOTARG_STATE_NOT_ENFORCING 0x86ULL

static int
miniamfi_policy_syscall(__unused struct proc *p, int call, user_addr_t arg)
{
	struct {
		uint64_t input_flags;
		user_addr_t output_flags;
	} args;
	uint64_t flags = AMFI_DYLD_OUTPUT_ALLOW_ALL;
	uint64_t state = AMFI_BOOTARG_STATE_NOT_ENFORCING;
	int error;

	if (call == AMFI_SYSCALL_QUERY_BOOTARG_STATE) {
		return copyout(&state, arg, sizeof(state));
	}
	if (call != AMFI_SYSCALL_CHECK_DYLD_POLICY) {
		return ENOPOLICY;               /* what user space got before there was a handler */
	}
	error = copyin(arg, &args, sizeof(args));
	if (error == 0) {
		error = copyout(&flags, args.output_flags, sizeof(flags));
	}
	return error;
}

static const struct mac_policy_ops miniamfi_ops = {
	.mpo_vnode_check_signature = miniamfi_vnode_check_signature,
	.mpo_policy_syscall = miniamfi_policy_syscall,
};

SECURITY_READ_ONLY_LATE(static struct mac_policy_conf) miniamfi_conf = {
	.mpc_name = "AMFI",
	.mpc_fullname = "Apple Mobile File Integrity (stand-in)",
	.mpc_ops = &miniamfi_ops,
};
SECURITY_READ_ONLY_LATE(static mac_policy_handle_t) miniamfi_handle;

/* Which request numbers have been seen: one log line each, to learn what user space asks for. */
static uint64_t minisandbox_seen;

static int
minisandbox_policy_syscall(struct proc *p, int call, __unused user_addr_t arg)
{
	if (call >= 0 && call < 64 && (minisandbox_seen & (1ULL << call)) == 0) {
		minisandbox_seen |= 1ULL << call;
		printf("minisandbox: request %d (first seen from %s[%d]): allowed\n", call, p->p_comm, proc_getpid(p));
	}
	return 0;
}

/*
 * Privileges for processes that are not root. On a device the sandbox hands them out by
 * entitlement; here the ones the user interface cannot do without are simply granted:
 *
 *   PRIV_WORK_INTERVAL    telling the scheduler about the work of drawing a frame. UIKit does
 *                         it as soon as anything scrolls, and takes a refusal for a programming
 *                         error ("Failed to create work interval"): SpringBoard died of it on
 *                         the first swipe between pages of the home screen.
 *
 * Whatever else is asked for is refused as before, and said once, to learn what there is.
 */
static int
minisandbox_priv_grant(__unused kauth_cred_t cred, int priv)
{
	static int seen[16];
	static unsigned seen_count;

	if (priv == PRIV_WORK_INTERVAL) {
		return 0;
	}
	for (unsigned i = 0; i < seen_count; i++) {
		if (seen[i] == priv) {
			return EPERM;
		}
	}
	if (seen_count < sizeof(seen) / sizeof(seen[0])) {
		seen[seen_count++] = priv;
		printf("minisandbox: privilege %d refused to %s[%d] (first time)\n", priv, current_proc()->p_comm,
		    proc_getpid(current_proc()));
	}
	return EPERM;
}

static const struct mac_policy_ops minisandbox_ops = {
	.mpo_policy_syscall = minisandbox_policy_syscall,
	.mpo_priv_grant = minisandbox_priv_grant,
};

SECURITY_READ_ONLY_LATE(static struct mac_policy_conf) minisandbox_conf = {
	.mpc_name = "Sandbox",
	.mpc_fullname = "Seatbelt sandbox policy (stand-in)",
	.mpc_ops = &minisandbox_ops,
};
SECURITY_READ_ONLY_LATE(static mac_policy_handle_t) minisandbox_handle;

/* Called from kernel_bootstrap_thread() once the MAC framework is up. */
void
minimac_policy_init(void)
{
	mac_policy_register(&miniamfi_conf, &miniamfi_handle, NULL);
	mac_policy_register(&minisandbox_conf, &minisandbox_handle, NULL);
}
