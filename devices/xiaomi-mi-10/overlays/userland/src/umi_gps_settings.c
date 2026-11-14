/*
 * In 20F66 PUIPrivacyController hides its native Location Services row when
 * its carrier-availability cache is nil. An unsuccessful subscription query
 * leaves that cache nil; a successful query with no carrier override yields
 * kCFBooleanTrue. UMI's SDX55 data interface does not provide CoreTelephony.
 * This library is packaged together with the native locationd job.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <objc/runtime.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/hakios.input.h>
#include <unistd.h>

/* Public Mach bootstrap ABI; the trimmed iOS SDK omits servers/bootstrap.h. */
extern kern_return_t bootstrap_look_up(mach_port_t, const char *, mach_port_t *);
#define BOOTSTRAP_UNKNOWN_SERVICE 1102

static void
diagnostic(const char *line)
{
    int fd = open("/dev/console", O_WRONLY);
    if (fd >= 0) {
        (void)write(fd, line, strlen(line));
        close(fd);
    }
}

static bool
has_argument(const char *args, const char *want)
{
    size_t wanted = strlen(want);
    while (*args) {
        args += strspn(args, " \t");
        size_t n = strcspn(args, " \t");
        if (n == wanted && !memcmp(args, want, n)) return true;
        args += n;
    }
    return false;
}

static bool
gps_session(void)
{
    char args[1024];
    size_t n = sizeof(args);
    if (sysctlbyname("kern.bootargs", args, &n, NULL, 0) || !n || n > sizeof(args) ||
        !memchr(args, 0, n) || !has_argument(args, "sdx55_step=12") ||
        !has_argument(args, "sdx55_efs=1")) return false;
    struct hakios_sdx55_fix fix = {0};
    n = sizeof(fix);
    return sysctlbyname(HAKIOS_SDX55_FIX_SYSCTL, &fix, &n, NULL, 0) == 0 &&
        n == sizeof(fix) && fix.size == sizeof(fix) && fix.version == HAKIOS_SDX55_FIX_VERSION;
}

static char refreshed_key;
static void (*original_refresh)(id, SEL);
static id (*original_cached)(id, SEL);

static void
refresh_availability(id controller, SEL cmd)
{
    original_refresh(controller, cmd);
    objc_setAssociatedObject(controller, &refreshed_key, (id)kCFBooleanTrue, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

static id
cached_availability(id controller, SEL cmd)
{
    id native = original_cached(controller, cmd);
    if (native || !objc_getAssociatedObject(controller, &refreshed_key)) return native;
    mach_port_t service = MACH_PORT_NULL;
    kern_return_t result = bootstrap_look_up(bootstrap_port, "com.apple.commcenter.coretelephony.xpc", &service);
    if (MACH_PORT_VALID(service)) mach_port_deallocate(mach_task_self(), service);
    if (result != BOOTSTRAP_UNKNOWN_SERVICE) return nil;
    /* Preserve Apple's cache and recheck availability on the next read. */
    static char logged_key;
    if (!objc_getAssociatedObject(controller, &logged_key)) {
        diagnostic("UMI GPS Settings: showing native Location Services row (no CoreTelephony service)\n");
        objc_setAssociatedObject(controller, &logged_key, (id)kCFBooleanTrue, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    return (id)kCFBooleanTrue;
}

/* Install both methods on the main queue before a user opens Privacy. */
static void
install(void *unused)
{
    (void)unused;
    if (!gps_session()) return;
    const char *path = "/System/Library/PrivateFrameworks/Settings/PrivacySettingsUI.framework/PrivacySettingsUI";
    if (!dlopen(path, RTLD_LAZY | RTLD_LOCAL)) return;
    Class cls = objc_getClass("PUIPrivacyController");
    Method refresh = class_getInstanceMethod(cls, sel_registerName("refreshLocationServicesAvailable"));
    Method cached = class_getInstanceMethod(cls, sel_registerName("_cachedLocationServicesAvailableValue"));
    if (!refresh || !cached || method_getNumberOfArguments(refresh) != 2 ||
        method_getNumberOfArguments(cached) != 2) return;
    char rtype[8], ctype[8];
    method_getReturnType(refresh, rtype, sizeof(rtype));
    method_getReturnType(cached, ctype, sizeof(ctype));
    if (strcmp(rtype, "v") || strcmp(ctype, "@")) return;
    original_refresh = (void (*)(id, SEL))method_getImplementation(refresh);
    original_cached = (id (*)(id, SEL))method_getImplementation(cached);
    method_setImplementation(refresh, (IMP)refresh_availability);
    method_setImplementation(cached, (IMP)cached_availability);
    diagnostic("UMI GPS Settings: installed native privacy-row fallback\n");
}

__attribute__((constructor)) static void
start(void)
{
    if (!strcmp(getprogname(), "Preferences")) dispatch_async_f(dispatch_get_main_queue(), NULL, install);
}
