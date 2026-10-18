/*
 * libredwood-sb.dylib: code of ours inside SpringBoard (see inject.c for how it gets there).
 *
 *   - The phone is "activated". SpringBoard asks mobileactivationd, which is not running (and
 *     could not talk to Apple anyway); without an answer it treats the device as bricked and
 *     shows the first-boot setup ("hello") instead of the lock screen.
 *   - The setup assistant is never needed, whatever the reason: nobody is going to go through
 *     it here.
 *   - SpringBoard starts a few seconds late where the kernel's boot-args say so (sb_delay=N;
 *     "xl run" does for the phone). /private/var is empty at every boot, so the applications
 *     are registered anew each time, one after another; one that is registered after
 *     SpringBoard has laid out the home screen ends up on a later page, and which one that is
 *     differs from boot to boot. On the phone that happens; under QEMU, where everything takes
 *     five times as long, it does not -- and there the delay is harmful: for a reason not yet
 *     found the window server then never takes our touch screen service for its own.
 *     (A wrapper program in the launchd job would be the obvious way to wait; launchd panics
 *     the system when a system job runs anything that is not Apple's.)
 *   - The picture SpringBoard keeps of an application that leaves the screen (for the application
 *     switcher and the next launch) is saved as JPEG or PNG, not as an ASTC texture: see
 *     snapshot_output_format().
 *   - Where CoreAnimation draws with its software renderer (QEMU; the phone without gpu=1) the
 *     "vibrant" views draw their content as it is: see plain_vibrancy().
 *   - A press of the side button that is long over is not taken for a long press (Siri) because
 *     SpringBoard was busy waking the display: see lock_long_press().
 *   - A long press on the lock screen's wallpaper does not open the poster switcher, which the
 *     window server here shows as a black screen: see poster_switcher_allowed().
 *   - Darwin notifications as a remote control, for scripts (rwtouch notify NAME):
 *       com.redwood.unlock      what the lock screen does after "swipe up to open"
 *       com.redwood.layers      write down the layers SpringBoard has on the screen: see layers()
 *     In any other program given this library (INJECT in build.sh) only the last of them is
 *     answered, with that program's layers.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

/* where what is said goes: the standard error, which is the console's for SpringBoard (an application's is nowhere) */
static int say_fd = 2;

static void
say(const char *format, ...)
{
	char line[1280];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(say_fd, line, (size_t)n);
}

/*
 * Boot-arg touch_lat=1: a line for every turn of SpringBoard's main run loop -- when it woke up (a
 * touch, a timer, the display link ...), when CoreAnimation's commit of what that turn changed
 * began and when it was done (our observers just before and after its own, of order 2000000), in
 * microseconds of mach_absolute_time, as the other "tl:" lines (tools/latency.py, tools/frames.py):
 *   tl: a WOKE COMMIT DONE
 * (a commit with nothing in it takes some microseconds; one that sends changes, a good deal more)
 */
#define CA_COMMIT_ORDER 2000000
static uint64_t main_woke, commit_began;
static mach_timebase_info_data_t timebase;

static uint64_t
now_us(void)
{
	return mach_absolute_time() * timebase.numer / timebase.denom / 1000;
}

static void
main_loop_turn(__unused CFRunLoopObserverRef observer, CFRunLoopActivity activity, void *info)
{
	if (activity == kCFRunLoopAfterWaiting) {
		main_woke = now_us();
	} else if (activity == kCFRunLoopBeforeWaiting && info == NULL) {
		commit_began = now_us();
	} else if (activity == kCFRunLoopBeforeWaiting && main_woke != 0) {
		say("tl: a %llu %llu %llu\n", main_woke, commit_began, now_us());
		main_woke = 0;
	}
}

static void
watch_main_loop(void)
{
	char args[1024] = "";
	size_t size = sizeof(args) - 1;
	const char *arg;

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0 || (arg = strstr(args, "touch_lat=1")) == NULL ||
	    (arg != args && arg[-1] != ' ')) {
		return;
	}
	mach_timebase_info(&timebase);
	CFRunLoopAddObserver(CFRunLoopGetMain(), CFRunLoopObserverCreate(NULL, kCFRunLoopAfterWaiting, true, LONG_MIN,
	    main_loop_turn, NULL), kCFRunLoopCommonModes);
	CFRunLoopAddObserver(CFRunLoopGetMain(), CFRunLoopObserverCreate(NULL, kCFRunLoopBeforeWaiting, true, CA_COMMIT_ORDER - 1,
	    main_loop_turn, NULL), kCFRunLoopCommonModes);
	{
		CFRunLoopObserverContext after = { .info = (void *)1 };

		CFRunLoopAddObserver(CFRunLoopGetMain(), CFRunLoopObserverCreate(NULL, kCFRunLoopBeforeWaiting, true,
		    CA_COMMIT_ORDER + 1, main_loop_turn, &after), kCFRunLoopCommonModes);
	}
}

/* ---- methods of SpringBoard's classes, replaced --------------------------------------------------- */

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define CACHE_BASE 0x180000000UL                /* where the cache was built to be, before the slide */

static uintptr_t slide;

/*
 * Replace -[CLASS SELECTOR] (or +[CLASS SELECTOR]: `kind` is that first character), whose code must now
 * be at the unslid address `expected` (iOS 16.5, 20F66: what a replacement does depends on what the
 * original did). Returns the original.
 * (dyld interposing of the C functions underneath does not reach calls made from inside the
 * shared cache: there one library branches straight into the other.)
 */
static IMP
replace_method_of_kind(char kind, const char *class_name, const char *selector, uintptr_t expected, IMP replacement)
{
	Class cls = objc_getClass(class_name);
	Method method = cls == NULL ? NULL : kind == '+' ? class_getClassMethod(cls, sel_registerName(selector)) :
	    class_getInstanceMethod(cls, sel_registerName(selector));
	IMP original = method ? method_getImplementation(method) : NULL;

	if (original == NULL || (uintptr_t)original != expected + slide) {
		say("redwood: %c[%s %s]: found %#lx, expected %#lx; left alone\n", kind, class_name, selector,
		    original ? (uintptr_t)original - slide : 0, expected);
		return NULL;
	}
	method_setImplementation(method, replacement);
	return original;
}

static IMP
replace_method(const char *class_name, const char *selector, uintptr_t expected, IMP replacement)
{
	return replace_method_of_kind('-', class_name, selector, expected, replacement);
}

/*
 * -[SBLockdownManager _setupActivationState] asks MobileActivation and keeps the answer in the
 * int at +8: 2 = activated, 1 = not, 3 = unknown; it stays 0 when there is no answer.
 */
#define LOCKDOWN_STATE(manager)         (*(int *)((char *)(manager) + 8))
#define LOCKDOWN_ACTIVATED              2

static void
setup_activation_state(id self, __unused SEL selector)
{
	LOCKDOWN_STATE(self) = LOCKDOWN_ACTIVATED;
}

/* -[SBSetupManager _setSetupRequiredReason:]: 0 = no need for the setup assistant */
static void (*original_set_setup_required_reason)(id self, SEL selector, long reason);

static void
set_setup_required_reason(id self, SEL selector, long reason)
{
	if (reason != 0) {
		say("redwood: SpringBoard wants the setup assistant (reason %ld); not here\n", reason);
	}
	original_set_setup_required_reason(self, selector, 0);
}

/*
 * +[XBApplicationSnapshotManifestImpl _outputFormatForSnapshot:] chooses the file format of the picture
 * SpringBoard takes of an application that leaves the screen: 0 = PNG, 1 and 2 = ATX (an ASTC texture),
 * 3 = JPEG. A device with a wide colour display that can decode ASTC gets ATX, and iOS compresses it on
 * the processor: fast with the vector code of Apple's cores, but here it takes one core more than ten
 * seconds per picture (under QEMU; seconds on the phone) -- and all that time the application waits for
 * the answer, until the watchdog for scene updates (10 s, there is no setting to scale it) kills it:
 * no application survived going to the background. JPEG takes a fraction of a second; it has no
 * transparency, so a picture that needs it becomes a PNG (which is what iOS itself does on a device
 * without ASTC).
 */
#define SNAPSHOT_FORMAT_PNG             0
#define SNAPSHOT_FORMAT_JPEG            3

static long
snapshot_output_format(__unused id self, __unused SEL selector, id snapshot)
{
	SEL opaque = sel_registerName("isImageOpaque");

	if (snapshot != NULL && class_getInstanceMethod(object_getClass(snapshot), opaque) != NULL &&
	    ((BOOL (*)(id, SEL))objc_msgSend)(snapshot, opaque)) {
		return SNAPSHOT_FORMAT_JPEG;
	}
	return SNAPSHOT_FORMAT_PNG;
}

/* ---- battery state from redwood's Qualcomm charger stack ----------------------------------------- */

/*
 * iOS reads battery level through UIDevice.batteryLevel and UIDevice.batteryState; SpringBoard's
 * status bar reads it the same way via SBBatteryController. Where the battery has no power source
 * of its own in the kernel (battery=1, the loader's way through Qualcomm's UEFI charger protocols),
 * we hook the UIDevice accessors to return what XNU publishes as kern.redwood.battery_soc (0..100)
 * and kern.redwood.battery_charging.
 * When the sysctl is missing (battery=1 not in boot-args, or the driver never started) we hand
 * back the value the real implementation computed; the status bar then shows whatever iOS last
 * knew, which is better than crashing.
 * With adsp=1 the battery is a power source (the service RedwoodBattery, an IOPMPowerSource:
 * iokit/Kernel/RedwoodBattery.cpp), which powerd reads like an iPhone's own, and from it iOS knows
 * everything by itself; once that service is there, the hooks hand everything to iOS's own code.
 */
static float  (*original_battery_level)(id self, SEL sel);
static long   (*original_battery_state)(id self, SEL sel);
static BOOL   (*original_battery_view_shows_percentage)(id self, SEL sel);
static void   (*original_battery_view_set_shows_percentage)(id self, SEL sel, BOOL shows);
static BOOL   (*original_battery_view_should_show_bolt)(id self, SEL sel);
static double (*original_battery_view_charge_percent)(id self, SEL sel);
static void   (*original_battery_view_set_charge_percent)(id self, SEL sel, double p);
static double (*original_ring_charging_level)(id self, SEL sel);

static int
sysctl_int(const char *name)
{
	int value = -1;
	size_t size = sizeof(value);

	sysctlbyname(name, &value, &size, NULL, 0);
	return value;
}

/* Is the battery a power source that iOS reads by itself (see above)? Once it is, it stays one. */
static bool
battery_power_source(void)
{
	static bool found;
	io_service_t service;

	if (!found && (service = IOServiceGetMatchingService(0, IOServiceMatching("RedwoodBattery"))) != 0) {
		IOObjectRelease(service);
		found = true;
	}
	return found;
}

/* UIDevice.batteryLevel: 0.0..1.0; -1.0 if monitoring not enabled */
static float
battery_level(id self, SEL sel)
{
	int soc = sysctl_int("kern.redwood.battery_soc");

	if (soc < 0 || soc > 100 || battery_power_source()) {
		return original_battery_level ? original_battery_level(self, sel) : -1.0f;
	}
	return (float)soc / 100.0f;
}

/* UIDevice.batteryState: 0=unknown 1=unplugged 2=charging 3=full */
static long
battery_state(id self, SEL sel)
{
	int charging = sysctl_int("kern.redwood.battery_charging");
	int present = sysctl_int("kern.redwood.battery_present");
	int plugged = sysctl_int("kern.redwood.battery_charger_present");
	int soc = sysctl_int("kern.redwood.battery_soc");

	if (charging == -1 || present == -1 || plugged == -1 || soc < 0 || soc > 100 || battery_power_source()) {
		return original_battery_state ? original_battery_state(self, sel) : 0;
	}
	if (!present) return 0;
	if (plugged && soc >= 100) return 3;
	return charging ? 2 : 1;
}

/*
 * UIKitCore's status-bar view has its own presentation policy, separate from UIDevice. With the power
 * source it is iOS's own again: the switch Battery Percentage in Settings > Battery, which the image
 * turns on (SBShowBatteryPercentage in userland/var/mobile/Library/Preferences/com.apple.springboard.plist).
 */
static BOOL
battery_view_shows_percentage(id self, SEL sel)
{
	int soc = sysctl_int("kern.redwood.battery_soc");

	if (soc >= 0 && soc <= 100 && !battery_power_source()) return YES;
	return original_battery_view_shows_percentage ? original_battery_view_shows_percentage(self, sel) : NO;
}

static void
battery_view_set_shows_percentage(id self, SEL sel, BOOL shows)
{
	int soc = sysctl_int("kern.redwood.battery_soc");

	original_battery_view_set_shows_percentage(self, sel, (soc >= 0 && soc <= 100 && !battery_power_source()) ? YES : shows);
}

static BOOL
battery_view_should_show_bolt(id self, SEL sel)
{
	int charging = sysctl_int("kern.redwood.battery_charging");
	int present = sysctl_int("kern.redwood.battery_present");
	int plugged = sysctl_int("kern.redwood.battery_charger_present");

	if (charging != -1 && present != -1 && plugged != -1 && !battery_power_source()) return charging && present && plugged;
	return original_battery_view_should_show_bolt ? original_battery_view_should_show_bolt(self, sel) : NO;
}

/*
 * The actual number the status bar draws. _UIBatteryView keeps the fill fraction (0.0..1.0) in
 * chargePercent; it is set through setChargePercent: and read back for layout. iOS feeds it from
 * BatteryCenter / IOPowerSources, which is empty on this phone, so without us it sits near zero
 * (the "1%" the user saw). We override both sides: setChargePercent: so the stored value is ours
 * no matter who writes it, and the getter so any layout path that reads it agrees.
 */
static double
battery_view_charge_percent(id self, SEL sel)
{
	int soc = sysctl_int("kern.redwood.battery_soc");

	if (soc >= 0 && soc <= 100 && !battery_power_source()) return (double)soc / 100.0;
	return original_battery_view_charge_percent ? original_battery_view_charge_percent(self, sel) : 0.0;
}

static void
battery_view_set_charge_percent(id self, SEL sel, double p)
{
	int soc = sysctl_int("kern.redwood.battery_soc");

	original_battery_view_set_charge_percent(self, sel, (soc >= 0 && soc <= 100 && !battery_power_source()) ? (double)soc / 100.0 : p);
}

/* The big charging ring on the lock screen (CoverSheet) reads its fill level the same way. */
static double
ring_charging_level(id self, SEL sel)
{
	int soc = sysctl_int("kern.redwood.battery_soc");

	if (soc >= 0 && soc <= 100 && !battery_power_source()) return (double)soc / 100.0;
	return original_ring_charging_level ? original_ring_charging_level(self, sel) : 0.0;
}

/* Keep SpringBoard's normal battery observers awake while the XNU cache changes. */
static void
battery_post_notifications(void)
{
	Class uidev = objc_getClass("UIDevice");
	Class notification_center = objc_getClass("NSNotificationCenter");
	id device = uidev ? ((id (*)(Class, SEL))objc_msgSend)(uidev, sel_registerName("currentDevice")) : nil;
	id center = notification_center ? ((id (*)(Class, SEL))objc_msgSend)(notification_center,
	    sel_registerName("defaultCenter")) : nil;

	if (center != nil && !battery_power_source()) {
		SEL post = sel_registerName("postNotificationName:object:");
		((void (*)(id, SEL, id, id))objc_msgSend)(center, post,
		    (id)CFSTR("UIDeviceBatteryLevelDidChangeNotification"), device);
		((void (*)(id, SEL, id, id))objc_msgSend)(center, post,
		    (id)CFSTR("UIDeviceBatteryStateDidChangeNotification"), device);
	}
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		battery_post_notifications();
	});
}

/* ---- the lock screen's clock without a GPU ------------------------------------------------------- */

#define REDWOOD_GL "/redwood/lib/libredwood-gl.dylib"

/* Does the window server draw with OpenGL ES? The same question, asked the same way, as opengl_es() in inject.c. */
static bool
gpu_draws(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	const char *argument;
	io_service_t gpu;

	if (access(REDWOOD_GL, R_OK) != 0) {
		return false;
	}
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
		for (argument = arguments; (argument = strstr(argument, "gl=")) != NULL; argument += 3) {
			if (argument == arguments || argument[-1] == ' ') {
				return argument[3] != '0';
			}
		}
	}
	gpu = IOServiceGetMatchingService(0, IOServiceMatching("RedwoodGPU"));
	if (gpu != 0) {
		IOObjectRelease(gpu);
	}
	return gpu != 0;
}

/*
 * The clock of the lock screen is a "vibrant" view (BSUIVibrancyEffectView in BaseBoardUI): its
 * digits are not drawn, they are a mask -- a copy of them through a colour matrix that keeps red
 * as alpha -- through which the filter vibrantColorMatrix lets the wallpaper behind come out
 * lighter; and a gradient over the whole screen, red at the top, tones the wallpaper down the
 * same way. On the GPU that is what is seen (eagl.c draws the two filters). CoreAnimation's
 * software renderer shows neither: the digits come out red, and the top of the wallpaper as it
 * was. Why was not found out; that renderer is in QuartzCore, not here.
 * Such views have a switch, the user default BSUIVibrancyEffectViewDisabled: with it they show
 * their content as it is drawn, the clock in plain white. It is set for this run of SpringBoard
 * alone (nothing is written), and only where the software renderer draws.
 */
static void
plain_vibrancy(void)
{
	if (gpu_draws()) {
		return;
	}
	CFPreferencesSetAppValue(CFSTR("BSUIVibrancyEffectViewDisabled"), kCFBooleanTrue, kCFPreferencesCurrentApplication);
	say("redwood: SpringBoard: no GPU draws, vibrant views are plain (the lock screen's clock is white)\n");
}

/* ---- the side button's long press ---------------------------------------------------------------- */

/*
 * -[SBLockHardwareButton longPress:] is what the side button's long press recognizer calls: Siri
 * (here, its "turn on Siri" sheet, nobody having turned it on). The recognizer is a timer of
 * SpringBoard's own: started when the main thread sees the press, stopped when it sees the release
 * -- and a release it has not got round to is no release. A press of the side button with the
 * display off wakes the display from inside the handling of that very press, which on an iPhone
 * takes no time. Here it takes as long as the software renderer needs for the first picture of the
 * lock screen: a tenth of a second on the phone, a second under QEMU. That is longer than a long
 * press, so there every wake by the button (and by a tap, which is a press of that button made in
 * inject.c) ended with Siri's sheet over the lock screen, however short the press had been.
 * Where the button's events come from -- inject.c, in backboardd -- it is known whether the button
 * is still held; a long press of a button that is not is dropped here.
 */
#define GESTURE_BEGAN                   1       /* UIGestureRecognizerState */
#define GESTURE_ENDED                   3       /* and cancelled, failed after it */

static void (*original_lock_long_press)(id self, SEL selector, id recognizer);

static bool
lock_button_held(void)
{
	static int token = NOTIFY_TOKEN_INVALID;
	uint64_t state = 0;

	if (token == NOTIFY_TOKEN_INVALID && notify_register_check("com.redwood.button.lock", &token) != NOTIFY_STATUS_OK) {
		token = NOTIFY_TOKEN_INVALID;
		return true;
	}
	/* 1 = released; held, or not known: as SpringBoard has it */
	return notify_get_state(token, &state) != NOTIFY_STATUS_OK || state != 1;
}

static void
lock_long_press(id self, SEL selector, id recognizer)
{
	static bool dropping;
	SEL state_selector = sel_registerName("state");
	long state = recognizer != NULL && class_getInstanceMethod(object_getClass(recognizer), state_selector) != NULL ?
	    ((long (*)(id, SEL))objc_msgSend)(recognizer, state_selector) : 0;

	if (state == GESTURE_BEGAN) {
		dropping = !lock_button_held();
		if (dropping) {
			say("redwood: SpringBoard: a long press of the side button, which was released long ago: dropped\n");
		}
	}
	if (dropping) {
		if (state >= GESTURE_ENDED) {
			dropping = false;
		}
		return;
	}
	original_lock_long_press(self, selector, recognizer);
}

/* ---- remote control ------------------------------------------------------------------------------ */

#define UNLOCK_SOURCE_COVER_SHEET       24      /* what the lock screen itself passes */

static void
unlock(void)
{
	Class cls = objc_getClass("SBLockScreenManager");
	SEL shared = sel_registerName("sharedInstance"), locked = sel_registerName("isUILocked");
	SEL unlock_ui = sel_registerName("unlockUIFromSource:withOptions:");
	id manager;

	if (cls == NULL || class_getClassMethod(cls, shared) == NULL || class_getInstanceMethod(cls, locked) == NULL ||
	    class_getInstanceMethod(cls, unlock_ui) == NULL) {
		say("redwood: unlock: SBLockScreenManager is not what it was\n");
		return;
	}
	manager = ((id (*)(Class, SEL))objc_msgSend)(cls, shared);
	if (manager == NULL) {
		say("redwood: unlock: no lock screen manager yet\n");
	} else if (!((BOOL (*)(id, SEL))objc_msgSend)(manager, locked)) {
		say("redwood: unlock: not locked\n");
	} else {
		BOOL done = ((BOOL (*)(id, SEL, long, id))objc_msgSend)(manager, unlock_ui, UNLOCK_SOURCE_COVER_SHEET, NULL);

		say("redwood: unlock: asked, answer %d\n", done);
	}
}

/*
 * The poster switcher: on iOS 16 a long press on the lock screen's wallpaper shrinks it into a
 * row of posters to choose from or customize. The row is PosterBoard's, an application scene of
 * that process which the lock screen hosts (CSPosterSwitcherViewController, a CALayerHost of the
 * context of PosterBoard's window over an opaque black layer). PosterBoard does lay it out -- its
 * window has the posters, "Customize" and "+", the hosted context is that window's, the scene is
 * in the foreground -- but the window server draws nothing of that context, with either
 * renderer, so what is left is the black layer under it, until a swipe up takes the switcher
 * away. Why is not known: the display is not in secure mode, SpringBoard counts the user as
 * authenticated, the same nesting of hosted contexts works for the wallpaper itself.
 * Until it is, the switcher is not offered, the way SpringBoard itself withholds it where it has
 * none (an iPad, Guided Access): its policy says the capability is not there, and the long press
 * does nothing.
 */
static BOOL
poster_switcher_allowed(__unused id self, __unused SEL selector, id *explanation)
{
	if (explanation != NULL) {
		*explanation = (id)CFSTR("the poster switcher is not shown here (see inject_sb.c)");
	}
	return NO;
}

/*
 * What SpringBoard has put on the screen, as CoreAnimation has it: every layer of every window, a
 * line each -- class and name, place, opacity, what it shows (an image, a colour, another
 * process's layers), and what is done to it: its filters by type with their inputs, in order,
 * the compositing filter, the mask. That is where a picture that comes out wrong on one renderer
 * and right on another is described: the same description goes to both.
 */
#define SEND(type, object, name)        ((type (*)(id, SEL))objc_msgSend)((id)(object), sel_registerName(name))
#define SEND1(type, object, name, argument_type, argument) \
	((type (*)(id, SEL, argument_type))objc_msgSend)((id)(object), sel_registerName(name), argument)

static const char *layers_of = "";               /* in another program than SpringBoard: " of" and its name */

static const char *
text_of(id object)
{
	id text = object ? SEND(id, object, "description") : NULL;

	return text ? SEND(const char *, text, "UTF8String") : "-";
}

static void
layer_filter(id filter, const char *what, int depth)
{
	static const char *const inputs[] = { "inputColorMatrix", "inputRadius", "inputAmount", "inputColor", "inputAngle",
		                              "inputPremultipliedValues", "inputNormalizeEdges", "inputReversed" };
	char line[1024];
	size_t n;

	if (!class_respondsToSelector(object_getClass(filter), sel_registerName("type"))) {
		say("redwood: layers%s: %*s  %s %s\n", layers_of, 2 * depth, "", what, text_of(filter));     /* a name alone */
		return;
	}
	n = (size_t)snprintf(line, sizeof(line), "%s %s \"%s\"%s", what, text_of(SEND(id, filter, "type")),
	    text_of(SEND(id, filter, "name")), SEND(BOOL, filter, "isEnabled") ? "" : " (disabled)");
	for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]) && n < sizeof(line); i++) {
		id key = SEND1(id, objc_getClass("NSString"), "stringWithUTF8String:", const char *, inputs[i]);
		id value = SEND1(id, filter, "valueForKey:", id, key);
		float m[20];

		if (value == NULL) {
			continue;
		}
		if (i == 0 && class_respondsToSelector(object_getClass(value), sel_registerName("CAColorMatrixValue"))) {
			SEND1(void, value, "getValue:", void *, m);
			n += (size_t)snprintf(line + n, sizeof(line) - n, " %s [%.3g %.3g %.3g %.3g %.3g | %.3g %.3g %.3g %.3g %.3g | "
			    "%.3g %.3g %.3g %.3g %.3g | %.3g %.3g %.3g %.3g %.3g]", inputs[i], m[0], m[1], m[2], m[3], m[4], m[5], m[6],
			    m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15], m[16], m[17], m[18], m[19]);
		} else {
			n += (size_t)snprintf(line + n, sizeof(line) - n, " %s %s", inputs[i], text_of(value));
		}
	}
	say("redwood: layers%s: %*s  %s\n", layers_of, 2 * depth, "", line);
}

static void
layer_line(id layer, const char *role, int depth)
{
	struct { double x, y, w, h; } frame = SEND(typeof(frame), layer, "frame");
	const char *class_name = object_getClassName(layer);
	id contents = SEND(id, layer, "contents"), filters = SEND(id, layer, "filters"), mask = SEND(id, layer, "mask");
	id compositing = SEND(id, layer, "compositingFilter"), sublayers = SEND(id, layer, "sublayers");
	id background = SEND(id, layer, "backgroundFilters");
	void *colour = SEND(void *, layer, "backgroundColor");
	char more[512] = "";
	size_t n = 0;

	if (SEND(BOOL, layer, "isHidden")) {
		n += (size_t)snprintf(more + n, sizeof(more) - n, " HIDDEN");
	}
	if (contents != NULL) {
		id text = (id)CFCopyDescription((CFTypeRef)contents);

		n += (size_t)snprintf(more + n, sizeof(more) - n, " contents %.60s", text_of(text));
		CFRelease((CFTypeRef)text);
	}
	if (contents != NULL && class_respondsToSelector(object_getClass(layer), sel_registerName("contentsMultiplyColor")) &&
	    SEND(void *, layer, "contentsMultiplyColor") != NULL) {
		id text = (id)CFCopyDescription(SEND(void *, layer, "contentsMultiplyColor"));
		const char *numbers = strstr(text_of(text), "( ");

		n += (size_t)snprintf(more + n, sizeof(more) - n, " times %.40s", numbers ? numbers : "?");
		CFRelease((CFTypeRef)text);
	}
	if (colour != NULL) {
		id text = (id)CFCopyDescription(colour);
		const char *numbers = strstr(text_of(text), "( ");

		n += (size_t)snprintf(more + n, sizeof(more) - n, " colour %.40s", numbers ? numbers : "?");
		CFRelease((CFTypeRef)text);
	}
	if (strcmp(class_name, "CAPortalLayer") == 0) {
		n += (size_t)snprintf(more + n, sizeof(more) - n, " PORTAL of %p%s%s", (void *)SEND(id, layer, "sourceLayer"),
		    SEND(BOOL, layer, "hidesSourceLayer") ? " (hides it)" : "", SEND(BOOL, layer, "matchesOpacity") ? " (its opacity)" : "");
	} else if (strcmp(class_name, "CABackdropLayer") == 0) {
		n += (size_t)snprintf(more + n, sizeof(more) - n, " BACKDROP group \"%s\" scale %g%s", text_of(SEND(id, layer, "groupName")),
		    SEND(double, layer, "scale"), SEND(BOOL, layer, "captureOnly") ? " (capture only)" : "");
	} else if (strcmp(class_name, "CAGradientLayer") == 0) {
		struct { double x, y; } from = SEND(typeof(from), layer, "startPoint"), to = SEND(typeof(to), layer, "endPoint");
		id colours = SEND(id, layer, "colors");

		n += (size_t)snprintf(more + n, sizeof(more) - n, " GRADIENT %s from %g,%g to %g,%g at %.40s:", text_of(SEND(id, layer, "type")),
		    from.x, from.y, to.x, to.y, text_of(SEND(id, layer, "locations")));
		for (long i = 0; colours != NULL && i < SEND(long, colours, "count") && n < sizeof(more); i++) {
			id text = (id)CFCopyDescription((CFTypeRef)SEND1(id, colours, "objectAtIndex:", long, i));
			const char *numbers = strstr(text_of(text), "( ");

			n += (size_t)snprintf(more + n, sizeof(more) - n, " %.30s", numbers ? numbers : "?");
			CFRelease((CFTypeRef)text);
		}
	} else if (strcmp(class_name, "CAShapeLayer") == 0) {
		void *stroke = SEND(void *, layer, "strokeColor"), *fill = SEND(void *, layer, "fillColor");
		id text_stroke = stroke ? (id)CFCopyDescription(stroke) : NULL, text_fill = fill ? (id)CFCopyDescription(fill) : NULL;
		const char *ns = text_stroke ? strstr(text_of(text_stroke), "( ") : NULL, *nf = text_fill ? strstr(text_of(text_fill), "( ") : NULL;

		n += (size_t)snprintf(more + n, sizeof(more) - n, " SHAPE line %g stroke %.30s fill %.30s ends %g-%g", SEND(double, layer,
		    "lineWidth"), ns ? ns : "-", nf ? nf : "-", SEND(double, layer, "strokeStart"), SEND(double, layer, "strokeEnd"));
		if (text_stroke != NULL) {
			CFRelease((CFTypeRef)text_stroke);
		}
		if (text_fill != NULL) {
			CFRelease((CFTypeRef)text_fill);
		}
	} else if (strcmp(class_name, "CALayerHost") == 0) {
		n += (size_t)snprintf(more + n, sizeof(more) - n, " HOST of context %#x%s%s", SEND(unsigned, layer, "contextId"),
		    SEND(BOOL, layer, "inheritsSecurity") ? " (inherits security)" : "",
		    SEND(BOOL, layer, "rendersAsynchronously") ? " (asynchronously)" : "");
	}
	{
		id keys = SEND(id, layer, "animationKeys");

		if (SEND(BOOL, layer, "shouldRasterize")) {
			n += (size_t)snprintf(more + n, sizeof(more) - n, " RASTERIZED");
		}
		for (long i = 0; keys != NULL && i < SEND(long, keys, "count") && n < sizeof(more); i++) {
			n += (size_t)snprintf(more + n, sizeof(more) - n, "%s%s", i == 0 ? " ANIMATIONS " : ",",
			    text_of(SEND1(id, keys, "objectAtIndex:", long, i)));
		}
	}
	say("redwood: layers%s: %*s%s%s %p \"%s\" %g,%g %gx%g opacity %g%s\n", layers_of, 2 * depth, "", role, class_name, (void *)layer,
	    text_of(SEND(id, layer, "name")), frame.x, frame.y, frame.w, frame.h, (double)SEND(float, layer, "opacity"), more);
	for (long i = 0; filters != NULL && i < SEND(long, filters, "count"); i++) {
		layer_filter(SEND1(id, filters, "objectAtIndex:", long, i), "filter", depth);
	}
	for (long i = 0; background != NULL && i < SEND(long, background, "count"); i++) {
		layer_filter(SEND1(id, background, "objectAtIndex:", long, i), "background filter", depth);
	}
	if (compositing != NULL) {
		layer_filter(compositing, "compositing filter", depth);
	}
	if (mask != NULL) {
		layer_line(mask, "MASK ", depth + 1);
	}
	for (long i = 0; sublayers != NULL && i < SEND(long, sublayers, "count"); i++) {
		layer_line(SEND1(id, sublayers, "objectAtIndex:", long, i), "", depth + 1);
	}
}

/* Every WKWebView under a view: whether WebKit takes it for one in the background, its web process and whether that answers. */
static void
web_views(id view, int depth)
{
	Class web = objc_getClass("WKWebView");
	id subviews;
	bool is_web = false;

	if (view == NULL || depth > 40) {
		return;
	}

	for (Class c = object_getClass(view); web != NULL && c != NULL && !is_web; c = class_getSuperclass(c)) {
		is_web = c == web;
	}
	if (is_web) {
		SEL background = sel_registerName("_isBackground"), pid = sel_registerName("_webProcessIdentifier");
		SEL responsive = sel_registerName("_webProcessIsResponsive");

		say("redwood: layers%s: a WKWebView %p: in the background %s, web process %d, responsive %s, in a window %s, hidden %s\n",
		    layers_of, (void *)view, class_respondsToSelector(object_getClass(view), background) ?
		    (SEND(BOOL, view, "_isBackground") ? "YES" : "no") : "?", class_respondsToSelector(object_getClass(view), pid) ?
		    SEND(int, view, "_webProcessIdentifier") : -1, class_respondsToSelector(object_getClass(view), responsive) ?
		    (SEND(BOOL, view, "_webProcessIsResponsive") ? "yes" : "NO") : "?", SEND(id, view, "window") ? "yes" : "NO",
		    SEND(BOOL, view, "isHidden") ? "YES" : "no");
	}
	subviews = SEND(id, view, "subviews");
	for (long i = 0; subviews != NULL && i < SEND(long, subviews, "count"); i++) {
		web_views(SEND1(id, subviews, "objectAtIndex:", long, i), depth + 1);
	}
}

static void
layers(void)
{
	Class window = objc_getClass("UIWindow");
	SEL all = sel_registerName("allWindowsIncludingInternalWindows:onlyVisibleWindows:");
	id windows = window && class_getClassMethod(window, all) ? ((id (*)(Class, SEL, BOOL, BOOL))objc_msgSend)(window, all, 1, 1) : NULL;
	id application = SEND(id, objc_getClass("UIApplication"), "sharedApplication");
	id authentication = application && class_respondsToSelector(object_getClass(application),
	    sel_registerName("authenticationController")) ? SEND(id, application, "authenticationController") : NULL;

	say("redwood: layers%s: %ld windows\n", layers_of, windows ? SEND(long, windows, "count") : 0L);
	if (authentication != NULL) {
		say("redwood: layers: the user is %sauthenticated, %s passcode, the display %sin secure mode\n",
		    SEND(BOOL, authentication, "isAuthenticated") ? "" : "NOT ", SEND(BOOL, authentication, "hasPasscodeSet") ? "a" : "no",
		    SEND(BOOL, authentication, "inSecureDisplayMode") ? "" : "not ");
	}
	for (long i = 0; windows != NULL && i < SEND(long, windows, "count"); i++) {
		id w = SEND1(id, windows, "objectAtIndex:", long, i);
		id scene = class_respondsToSelector(object_getClass(w), sel_registerName("windowScene")) ? SEND(id, w, "windowScene") : NULL;
		id context = class_respondsToSelector(object_getClass(w), sel_registerName("_boundContext")) ? SEND(id, w, "_boundContext") : NULL;

		say("redwood: layers%s: window %s level %g, context %#x (%ssecure, %u commits), scene %s (activation state %ld)\n",
		    layers_of, object_getClassName(w), SEND(double, w, "windowLevel"), context ? SEND(unsigned, context, "contextId") : 0,
		    context && SEND(BOOL, context, "isSecure") ? "" : "not ", context ? (unsigned)SEND(unsigned long long, context, "commitId") : 0,
		    scene ? text_of(SEND(id, scene, "_persistenceIdentifier")) : "none", scene ? SEND(long, scene, "activationState") : -1L);
		layer_line(SEND(id, w, "layer"), "", 1);
	}
	if (application != NULL) {
		say("redwood: layers%s: the application's state %ld (0 active, 1 inactive, 2 in the background)\n", layers_of,
		    SEND(long, application, "applicationState"));
	}
	for (long i = 0; windows != NULL && i < SEND(long, windows, "count"); i++) {
		web_views(SEND1(id, windows, "objectAtIndex:", long, i), 0);
	}
	say("redwood: layers%s: end\n", layers_of);
}

/*
 * Control Center's glyphs whole from its first frames. Its modules and their parts fade in as it
 * comes down: opacity animations, mostly springs that take up to a second to settle at 1. While a
 * layer's opacity is below 1, the window server's OpenGL ES renderer draws the vibrant glyphs in it
 * from a flattened copy that lacks the strokes composited against what is behind them, and draws them
 * whole only at full opacity: the strokes of the media controls, orientation lock, screen mirroring
 * and Focus came in only when the springs had settled, at the end of the presentation. (On an iPhone
 * they are whole all along.) So in Control Center's window an opacity animation is replaced with an
 * eased-out fade of cc_fade=MS between the same values: the modules still fade in and out, and are at
 * full opacity, glyphs whole, after a few frames. Off by default (iOS's own animations stay as they
 * are; the real fix belongs in the drawing, not in the animations): a workaround, for checks only.
 */
#define CC_FADE_MS      0
static void (*original_add_animation_cc)(id self, SEL selector, id animation, id key);
static double cc_fade_s = CC_FADE_MS / 1000.0;
static unsigned cc_fades;

static bool
in_control_center(id layer)
{
	for (int depth = 0; layer != NULL && depth < 64; depth++) {
		id delegate = SEND(id, layer, "delegate");

		if (delegate != NULL && strcmp(object_getClassName(delegate), "SBControlCenterWindow") == 0) {
			return true;
		}
		layer = SEND(id, layer, "superlayer");
	}
	return false;
}

static void
add_animation_cc(id self, SEL selector, id animation, id key)
{
	Class basic = objc_getClass("CABasicAnimation");
	id path = animation != NULL && class_respondsToSelector(object_getClass(animation), sel_registerName("keyPath")) ?
	    SEND(id, animation, "keyPath") : NULL;

	if (path != NULL && strcmp(text_of(path), "opacity") == 0 &&
	    class_respondsToSelector(object_getClass(animation), sel_registerName("fromValue")) && in_control_center(self)) {
		id fade = SEND1(id, basic, "animationWithKeyPath:", id, path);
		id ease = SEND1(id, objc_getClass("CAMediaTimingFunction"), "functionWithName:", id,
		    SEND1(id, objc_getClass("NSString"), "stringWithUTF8String:", const char *, "easeOut"));

		SEND1(void, fade, "setFromValue:", id, SEND(id, animation, "fromValue"));
		SEND1(void, fade, "setToValue:", id, SEND(id, animation, "toValue"));
		SEND1(void, fade, "setByValue:", id, SEND(id, animation, "byValue"));
		SEND1(void, fade, "setAdditive:", BOOL, SEND(BOOL, animation, "isAdditive"));
		SEND1(void, fade, "setBeginTime:", double, SEND(double, animation, "beginTime"));
		SEND1(void, fade, "setFillMode:", id, SEND(id, animation, "fillMode"));
		SEND1(void, fade, "setRemovedOnCompletion:", BOOL, SEND(BOOL, animation, "isRemovedOnCompletion"));
		SEND1(void, fade, "setDelegate:", id, SEND(id, animation, "delegate"));
		SEND1(void, fade, "setDuration:", double, cc_fade_s);
		SEND1(void, fade, "setTimingFunction:", id, ease);
		if (cc_fades++ < 3) {
			say("redwood: Control Center: an opacity animation of %.3f s (%s) made a fade of %.3f s\n",
			    SEND(double, animation, "duration"), object_getClassName(animation), cc_fade_s);
		}
		animation = fade;
	}
	original_add_animation_cc(self, selector, animation, key);
}

static void
control_center_fades(const char *arguments)
{
	const char *setting = strstr(arguments, "cc_fade=");
	Method add = class_getInstanceMethod(objc_getClass("CALayer"), sel_registerName("addAnimation:forKey:"));

	if (setting != NULL) {
		cc_fade_s = atof(setting + 8) / 1000.0;
	}
	if (cc_fade_s <= 0 || add == NULL) {
		return;
	}
	original_add_animation_cc = (void (*)(id, SEL, id, id))method_getImplementation(add);
	method_setImplementation(add, (IMP)add_animation_cc);
}

/*
 * Open the tweak loader of the package manager's prefix, when there is one. Nothing here depends on
 * it: without a prefix (or without ElleKit in it) the file is not there and this does nothing.
 */
#define REDWOOD_TWEAK_LOADER    "/redwood/lib/libredwood-tweaks.dylib"

static void
load_tweaks(void)
{
	struct stat st;

	if (stat("/var/jb/usr/lib/TweakInject", &st) != 0) {
		return;                 /* no package manager's prefix here, so no tweaks */
	}
	if (dlopen(REDWOOD_TWEAK_LOADER, RTLD_NOW | RTLD_GLOBAL) == NULL) {
		say("redwood: the tweak loader %s: %s\n", REDWOOD_TWEAK_LOADER, dlerror());
	} else {
		say("redwood: tweaks: %s opened\n", REDWOOD_TWEAK_LOADER);
	}
}

__attribute__((constructor))
static void
redwood_springboard_init(void)
{
	const char *name = getprogname();
	size_t length = 0;
	IMP activation, snapshot_format, battery_lvl, battery_st, poster_switcher;
	int token;

	if (name == NULL) {
		return;
	}
	if (strcmp(name, "SpringBoard") != 0) {
		static char who[64];

		snprintf(who, sizeof(who), " of %s", name);
		layers_of = who;
		if ((say_fd = open("/dev/console", O_WRONLY | O_NOCTTY)) < 0) {
			say_fd = 2;
		}
		say("redwood: in %s[%d]: its layers are said on the notification com.redwood.layers\n", name, getpid());
		notify_register_dispatch("com.redwood.layers", &token, dispatch_get_main_queue(), ^(__unused int t) {
			layers();
		});
		return;
	}
	/*
	 * The tweaks of the package manager's prefix, if one is installed: ElleKit's loader
	 * (/var/jb/usr/lib/TweakLoader.dylib) reads /var/jb/usr/lib/TweakInject and opens the tweaks
	 * whose Filter names this process. The kernel could insert it as well (etc/inject.conf), but
	 * only one inserted library per program is reliable here, and SpringBoard's is this one; so
	 * this one opens it, the way a jailbreak's own hook does.
	 */
	load_tweaks();
	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
	activation = replace_method("SBLockdownManager", "_setupActivationState", 0x1c4bc8dfcUL, (IMP)setup_activation_state);
	original_set_setup_required_reason = (void (*)(id, SEL, long))replace_method("SBSetupManager",
	    "_setSetupRequiredReason:", 0x1c4cdd3a4UL, (IMP)set_setup_required_reason);
	snapshot_format = replace_method_of_kind('+', "XBApplicationSnapshotManifestImpl", "_outputFormatForSnapshot:",
	    0x1f1c91480UL, (IMP)snapshot_output_format);
	plain_vibrancy();
	original_lock_long_press = (void (*)(id, SEL, id))replace_method("SBLockHardwareButton", "longPress:", 0x1c4f6ab44UL,
	    (IMP)lock_long_press);
	poster_switcher = replace_method("SBMainDisplayPolicyAggregator", "_allowsCapabilityPosterSwitcherWithExplanation:",
	    0x1c507e0fcUL, (IMP)poster_switcher_allowed);
	/*
	 * Battery hooks on UIDevice: these are the methods every app and SpringBoard itself calls, so
	 * hooking them here in the SpringBoard process covers both the status bar and any app that
	 * reads [UIDevice currentDevice].batteryLevel. The addresses are for iOS 16.5 (20F66).
	 */
	original_battery_level = (float (*)(id, SEL))replace_method("UIDevice", "batteryLevel", 0x1897021b0UL, (IMP)battery_level);
	original_battery_state = (long (*)(id, SEL))replace_method("UIDevice", "batteryState", 0x189209794UL, (IMP)battery_state);
	original_battery_view_shows_percentage = (BOOL (*)(id, SEL))replace_method("_UIBatteryView", "showsPercentage",
	    0x189a9370cUL, (IMP)battery_view_shows_percentage);
	original_battery_view_set_shows_percentage = (void (*)(id, SEL, BOOL))replace_method("_UIBatteryView", "setShowsPercentage:",
	    0x18926e150UL, (IMP)battery_view_set_shows_percentage);
	original_battery_view_should_show_bolt = (BOOL (*)(id, SEL))replace_method("_UIBatteryView", "_shouldShowBolt",
	    0x189203e28UL, (IMP)battery_view_should_show_bolt);
	original_battery_view_charge_percent = (double (*)(id, SEL))replace_method("_UIBatteryView", "chargePercent",
	    0x189a935d8UL, (IMP)battery_view_charge_percent);
	original_battery_view_set_charge_percent = (void (*)(id, SEL, double))replace_method("_UIBatteryView", "setChargePercent:",
	    0x18928d310UL, (IMP)battery_view_set_charge_percent);
	original_ring_charging_level = (double (*)(id, SEL))replace_method("CSBatteryChargingRingView", "batteryChargingLevel",
	    0x1c48c2cf4UL, (IMP)ring_charging_level);
	battery_lvl = (IMP)original_battery_level;
	battery_st  = (IMP)original_battery_state;
	say("redwood: SpringBoard: activation %s, setup assistant %s, application snapshots %s, the side button's long press %s, "
	    "battery level %s, battery state %s, poster switcher %s\n",
	    activation ? "answered here" : "NOT replaced", original_set_setup_required_reason ? "refused" : "NOT replaced",
	    snapshot_format ? "as JPEG" : "NOT changed", original_lock_long_press ? "checked" : "NOT checked",
	    battery_lvl ? "from QG" : "NOT replaced", battery_st ? "from QG" : "NOT replaced",
	    poster_switcher ? "not offered" : "NOT withheld");
	say("redwood: battery UI: shows%% %s, set-shows%% %s, bolt %s, chargePercent %s, setChargePercent %s, lockscreen ring %s\n",
	    original_battery_view_shows_percentage ? "ok" : "NO",
	    original_battery_view_set_shows_percentage ? "ok" : "NO",
	    original_battery_view_should_show_bolt ? "ok" : "NO",
	    original_battery_view_charge_percent ? "ok" : "NO",
	    original_battery_view_set_charge_percent ? "ok" : "NO",
	    original_ring_charging_level ? "ok" : "NO");
	{
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		const char *delay;

		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && (delay = strstr(args, "sb_delay=")) != NULL &&
		    (delay == args || delay[-1] == ' ')) {
			usleep((useconds_t)(atof(delay + 9) * 1e6));
		}
	}
	/*
	 * UIDevice won't call batteryLevel/batteryState unless monitoring is enabled. Flip it on once
	 * SpringBoard's run loop is running (the dispatch_after fires on the main queue, which means
	 * we are past UIApplication startup).
	 */
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		Class uidev = objc_getClass("UIDevice");
		SEL current = sel_registerName("currentDevice");
		SEL monitoring = sel_registerName("setBatteryMonitoringEnabled:");
		id device = uidev ? ((id (*)(Class, SEL))objc_msgSend)(uidev, current) : nil;

		if (device && class_getInstanceMethod(object_getClass(device), monitoring)) {
			((void (*)(id, SEL, BOOL))objc_msgSend)(device, monitoring, YES);
		}
		watch_main_loop();
		{
			char arguments[1024] = "";
			size_t size = sizeof(arguments) - 1;

			if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
				control_center_fades(arguments);
			}
		}
		say("redwood: battery: SOC=%d%% battery=%d charger=%d charging=%d\n",
		    sysctl_int("kern.redwood.battery_soc"), sysctl_int("kern.redwood.battery_present"),
		    sysctl_int("kern.redwood.battery_charger_present"), sysctl_int("kern.redwood.battery_charging"));
		battery_post_notifications();
	});
	notify_register_dispatch("com.redwood.unlock", &token, dispatch_get_main_queue(), ^(__unused int t) {
		unlock();
	});
	notify_register_dispatch("com.redwood.layers", &token, dispatch_get_main_queue(), ^(__unused int t) {
		layers();
	});
	/*
	 * Belt-and-suspenders unlock. The user can always swipe up to open the home screen -- that
	 * works fine when the touch driver is doing its job. But if touch is still probing, has
	 * failed, or the panel is a FocalTech and the firmware upload is still chewing through
	 * ~114 KB of SPI (that takes a hot second during bring-up), the user ends up staring at a
	 * dark lock screen with nothing to swipe. So we just forcibly dismiss it after 10 seconds
	 * so iOS always lands on the home screen even in the worst case. Harmless if touch is
	 * already working: the user just doesn't need to swipe themselves.
	 * With autounlock=0 among the kernel's arguments the lock screen stays until it is swiped away
	 * (to show the phone starting the way an iPhone does).
	 */
	{
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		const char *arg;

		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && (arg = strstr(args, "autounlock=0")) != NULL &&
		    (arg == args || arg[-1] == ' ')) {
			say("redwood: the lock screen stays until it is swiped away (autounlock=0)\n");
			return;
		}
	}
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		unlock();
	});
}
