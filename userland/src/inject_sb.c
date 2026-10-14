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
 *   - Darwin notifications as a remote control, for scripts (rwtouch notify NAME):
 *       com.redwood.unlock      what the lock screen does after "swipe up to open"
 *       com.redwood.layers      write down the layers SpringBoard has on the screen: see layers()
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

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
	write(2, line, (size_t)n);
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
 * What SpringBoard has put on the screen, as CoreAnimation has it: every layer of every window, a
 * line each -- class and name, place, opacity, what it shows (an image, a colour, another
 * process's layers), and what is done to it: its filters by type with their inputs, in order,
 * the compositing filter, the mask. That is where a picture that comes out wrong on one renderer
 * and right on another is described: the same description goes to both.
 */
#define SEND(type, object, name)        ((type (*)(id, SEL))objc_msgSend)((id)(object), sel_registerName(name))
#define SEND1(type, object, name, argument_type, argument) \
	((type (*)(id, SEL, argument_type))objc_msgSend)((id)(object), sel_registerName(name), argument)

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
		say("redwood: layers: %*s  %s %s\n", 2 * depth, "", what, text_of(filter));     /* a name alone */
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
	say("redwood: layers: %*s  %s\n", 2 * depth, "", line);
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
	} else if (strcmp(class_name, "CALayerHost") == 0) {
		n += (size_t)snprintf(more + n, sizeof(more) - n, " HOST of context %#x", SEND(unsigned, layer, "contextId"));
	}
	say("redwood: layers: %*s%s%s %p \"%s\" %g,%g %gx%g opacity %g%s\n", 2 * depth, "", role, class_name, (void *)layer,
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

static void
layers(void)
{
	Class window = objc_getClass("UIWindow");
	SEL all = sel_registerName("allWindowsIncludingInternalWindows:onlyVisibleWindows:");
	id windows = window && class_getClassMethod(window, all) ? ((id (*)(Class, SEL, BOOL, BOOL))objc_msgSend)(window, all, 1, 1) : NULL;

	say("redwood: layers: %ld windows\n", windows ? SEND(long, windows, "count") : 0L);
	for (long i = 0; windows != NULL && i < SEND(long, windows, "count"); i++) {
		id w = SEND1(id, windows, "objectAtIndex:", long, i);

		say("redwood: layers: window %s level %g\n", object_getClassName(w), SEND(double, w, "windowLevel"));
		layer_line(SEND(id, w, "layer"), "", 1);
	}
	say("redwood: layers: end\n");
}

__attribute__((constructor))
static void
redwood_springboard_init(void)
{
	const char *name = getprogname();
	size_t length = 0;
	IMP activation, snapshot_format;
	int token;

	if (name == NULL || strcmp(name, "SpringBoard") != 0) {
		return;
	}
	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
	activation = replace_method("SBLockdownManager", "_setupActivationState", 0x1c4bc8dfcUL, (IMP)setup_activation_state);
	original_set_setup_required_reason = (void (*)(id, SEL, long))replace_method("SBSetupManager",
	    "_setSetupRequiredReason:", 0x1c4cdd3a4UL, (IMP)set_setup_required_reason);
	snapshot_format = replace_method_of_kind('+', "XBApplicationSnapshotManifestImpl", "_outputFormatForSnapshot:",
	    0x1f1c91480UL, (IMP)snapshot_output_format);
	plain_vibrancy();
	original_lock_long_press = (void (*)(id, SEL, id))replace_method("SBLockHardwareButton", "longPress:", 0x1c4f6ab44UL,
	    (IMP)lock_long_press);
	say("redwood: SpringBoard: activation %s, setup assistant %s, application snapshots %s, the side button's long press %s\n",
	    activation ? "answered here" : "NOT replaced", original_set_setup_required_reason ? "refused" : "NOT replaced",
	    snapshot_format ? "as JPEG" : "NOT changed", original_lock_long_press ? "checked" : "NOT checked");
	{
		char args[1024] = "";
		size_t size = sizeof(args) - 1;
		const char *delay;

		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && (delay = strstr(args, "sb_delay=")) != NULL &&
		    (delay == args || delay[-1] == ' ')) {
			usleep((useconds_t)(atof(delay + 9) * 1e6));
		}
	}
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
	 */
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		unlock();
	});
}
