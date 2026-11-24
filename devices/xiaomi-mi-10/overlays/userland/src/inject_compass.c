/*
 * libhakios.compass.dylib: iOS's Compass application, given this phone's magnetometer (see
 * inject.c for how a library of ours gets into a program; build.sh's INJECT names Compass).
 *
 * Compass draws its dial from CoreMotion, not CoreLocation. A timer of
 * CompassPageViewController calls -updateDisplay:, which asks -[CMMotionManager deviceMotion]
 * (through its CompassMotionManager) and takes three things from it:
 *   heading                        -> setCompassToAngle:force:, the dial
 *   gravity                        -> setCrosshairLevelForData:, the level's crosshair
 *   magneticFieldCalibrationLevel  -> whether it is calibrated (non-zero: yes)
 * -- exactly what its own test class, CompassTestDeviceMotion, overrides (its debug console reads
 * magneticField as well). On an iPhone those come from locationd and the motion coprocessor;
 * neither exists here, so deviceMotion is nil and the dial sits on north.
 *
 * The kernel keeps the magnetometer's last sample and its estimate of the phone's own constant
 * field (sysctl kern.hakios.mag, sys/hakios.input.h; the ak0991x behind the sensor DSP,
 * bsd/dev/hakios.input.c). This library answers deviceMotion with Compass's own
 * CompassTestDeviceMotion object and replaces only its heading getter.  The object is returned at
 * CompassMotionManager, the application's wrapper, rather than by globally replacing
 * CMMotionManager's getter: UIKitCore has its own motion managers in this process and asks their
 * device motion for timestamp/attitude/quaternion, which must remain CoreMotion's real objects.
 */
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <math.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define CACHE_BASE 0x180000000UL
#define COREMOTION "/System/Library/Frameworks/CoreMotion.framework/CoreMotion"

/* sys/hakios.input.h's record (the kernel's header is not in this tree's include path) */
#define HAKIOS_MAG_SYSCTL      "kern.hakios.mag"
#define HAKIOS_MAG_VERSION     2
struct hakios_mag_state {
	uint32_t size;
	uint32_t version;
	uint64_t samples;
	uint64_t time;
	int32_t raw[3];
	int32_t center[3];
	int32_t span[3];
	int32_t calibration;
	int32_t status;
	uint64_t orientation_samples;
	uint64_t orientation_time;
	int32_t heading_millidegrees;
	int32_t pitch_millidegrees;
	int32_t roll_millidegrees;
	int32_t orientation_status;
};

typedef struct { double x, y, z; } hk_vector;                           /* CMMagneticField */

static int console = -1;
static uintptr_t slide;
static int mag_mib[CTL_MAXNAME];
static size_t mag_mib_length;
static id motion;                       /* Compass's one test motion, kept for the process's life */
static unsigned headings_asked;
static uint64_t filtered_sample;
static double filtered_x, filtered_y;
static bool filter_ready;

static void
say(const char *format, ...)
{
	char line[256];
	va_list list;
	int n;

	va_start(list, format);
	n = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	if (n < 0) {
		return;
	}
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	if (console < 0) {
		console = open("/dev/console", O_WRONLY);
	}
	(void)write(console >= 0 ? console : 2, line, (size_t)n);
}

/* If this library brings the application down, say where (as inject_app.c does). */
static void
killed(int number, siginfo_t *info, void *context)
{
	void *frames[32];
	char line[900];
	int n = backtrace(frames, 32), k;

	(void)context;
	k = snprintf(line, sizeof(line), "hakios: Compass: SIGNAL %d at %p (the cache's slide %#lx), the stack:", number,
	    info != NULL ? info->si_addr : NULL, (unsigned long)slide);
	for (int i = 0; i < n && k < (int)sizeof(line) - 24; i++) {
		k += snprintf(line + k, sizeof(line) - (size_t)k, " %#lx", (unsigned long)((uintptr_t)frames[i] - slide));
	}
	line[k++] = '\n';
	if (console < 0) {
		console = open("/dev/console", O_WRONLY);
	}
	(void)write(console >= 0 ? console : 2, line, (size_t)k);
	signal(number, SIG_DFL);
	raise(number);
}

static bool
read_mag(struct hakios_mag_state *state)
{
	size_t size = sizeof(*state);

	if (mag_mib_length == 0) {
		mag_mib_length = CTL_MAXNAME;
		if (sysctlnametomib(HAKIOS_MAG_SYSCTL, mag_mib, &mag_mib_length) != 0) {
			mag_mib_length = 0;
			return false;
		}
	}
	memset(state, 0, sizeof(*state));
	return sysctl(mag_mib, (u_int)mag_mib_length, state, &size, NULL, 0) == 0 && size == sizeof(*state) &&
	       state->size == sizeof(*state) && state->version == HAKIOS_MAG_VERSION && state->samples != 0;
}

/* The earth's field in microtesla, in the device frame: the sample less the phone's own field. */
static void
earth_field(hk_vector *field, const struct hakios_mag_state *state)
{
	field->x = (state->raw[0] - state->center[0]) / 1000.0;
	field->y = (state->raw[1] - state->center[1]) / 1000.0;
	field->z = (state->raw[2] - state->center[2]) / 1000.0;
}

/*
 * A phone lying flat, screen up: the top of the phone (+y) points north when the horizontal
 * field lies along +y, east when it lies along -x (north is then on the phone's left).
 * Degrees clockwise from magnetic north, 0 to 360, as CMDeviceMotion's heading.
 */
static double
my_heading(id self, SEL cmd)
{
	struct hakios_mag_state state;
	hk_vector b;
	double heading;

	(void)self;
	(void)cmd;
	if (!read_mag(&state)) {
		if (headings_asked++ == 0) {
			say("hakios: Compass: no magnetometer yet (sysctl %s); the dial stays on north\n", HAKIOS_MAG_SYSCTL);
		}
		return 0;
	}
	earth_field(&b, &state);
	if (state.orientation_samples != 0) {
		heading = state.heading_millidegrees / 1000.0;
	} else {
		/* The App asks much faster than the 10 Hz sensor.  Update once per
		 * physical sample and smooth the vector (not the wrapped angle), so
		 * north cannot jump through 359/0 and a still phone does not twitch. */
		if (!filter_ready || state.samples < filtered_sample) {
			filtered_x = b.x;
			filtered_y = b.y;
			filter_ready = true;
		} else if (state.samples != filtered_sample) {
			filtered_x += 0.2 * (b.x - filtered_x);
			filtered_y += 0.2 * (b.y - filtered_y);
		}
		filtered_sample = state.samples;
		heading = atan2(-filtered_x, filtered_y) * 180.0 / M_PI;
		if (heading < 0) {
			heading += 360.0;
		}
	}
	if (headings_asked < 3 || headings_asked % 120 == 0) {
		say("hakios: Compass: heading %.1f (%s; earth field %.1f %.1f %.1f uT; raw %d %d %d, center %d %d %d nT; "
		    "calibration %d; mag sample %llu, orientation sample %llu)\n", heading,
		    state.orientation_samples != 0 ? "SLPI fusion" : "raw magnetic fallback", b.x, b.y, b.z,
		    state.raw[0], state.raw[1], state.raw[2],
		    state.center[0], state.center[1], state.center[2], state.calibration,
		    (unsigned long long)state.samples, (unsigned long long)state.orientation_samples);
	}
	headings_asked++;
	return heading;
}

static void
set_test_gravity(id object, hk_vector gravity)
{
	((void (*)(id, SEL, hk_vector))objc_msgSend)(object, sel_registerName("setGravity:"), gravity);
}

static id
my_device_motion(id self, SEL cmd)
{
	(void)self;
	(void)cmd;
	return motion;
}

static void
my_start_motion(id self, SEL cmd, long frame)
{
	(void)self;
	(void)cmd;
	say("hakios: Compass: device motion started (reference frame %ld): served from %s, not locationd\n", frame,
	    HAKIOS_MAG_SYSCTL);
}

static void
my_stop_motion(id self, SEL cmd)
{
	(void)self;
	(void)cmd;
}

static BOOL
my_yes(id self, SEL cmd)
{
	(void)self;
	(void)cmd;
	return YES;
}

static bool
replace(Class cls, const char *name, IMP replacement, const char *types)
{
	SEL sel = sel_registerName(name);
	Method method = class_getInstanceMethod(cls, sel);

	if (method == NULL) {
		return class_addMethod(cls, sel, replacement, types);
	}
	method_setImplementation(method, replacement);
	return true;
}

__attribute__((constructor))
static void
hakios_compass_init(void)
{
	struct hakios_mag_state state;
	size_t length = 0;
	const void *cache = _dyld_get_shared_cache_range(&length);
	struct sigaction action = { 0 };
	Class manager, app_manager, test_motion;

	if (getprogname() == NULL || strcmp(getprogname(), "Compass") != 0) {
		return;
	}
	slide = cache != NULL ? (uintptr_t)cache - CACHE_BASE : 0;
	action.sa_sigaction = killed;
	action.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &action, NULL);
	sigaction(SIGBUS, &action, NULL);
	sigaction(SIGABRT, &action, NULL);
	sigaction(SIGILL, &action, NULL);
	sigaction(SIGTRAP, &action, NULL);

	(void)dlopen(COREMOTION, RTLD_NOW);
	manager = objc_getClass("CMMotionManager");
	app_manager = objc_getClass("CompassMotionManager");
	test_motion = objc_getClass("CompassTestDeviceMotion");
	if (manager == NULL || app_manager == NULL || test_motion == NULL) {
		say("hakios: Compass: motion classes are not there (CompassTestDeviceMotion %p, CompassMotionManager %p, "
		    "CMMotionManager %p)\n", (void *)test_motion, (void *)app_manager, (void *)manager);
		return;
	}
	replace(test_motion, "heading", (IMP)my_heading, "d16@0:8");
	/* The class has only scalar/vector ivars and its own accessors; a zeroed instance is its test default. */
	motion = class_createInstance(test_motion, 0);
	set_test_gravity(motion, (hk_vector){ 0, 0, -1 });

	replace(app_manager, "deviceMotion", (IMP)my_device_motion, "@16@0:8");
	replace(manager, "startDeviceMotionUpdatesUsingReferenceFrame:", (IMP)my_start_motion, "v24@0:8Q16");
	replace(manager, "startDeviceMotionUpdates", (IMP)my_stop_motion, "v16@0:8");
	replace(manager, "stopDeviceMotionUpdates", (IMP)my_stop_motion, "v16@0:8");
	replace(manager, "isDeviceMotionAvailable", (IMP)my_yes, "B16@0:8");
	replace(manager, "isDeviceMotionActive", (IMP)my_yes, "B16@0:8");
	replace(manager, "isMagnetometerAvailable", (IMP)my_yes, "B16@0:8");

	if (read_mag(&state)) {
		say("hakios: Compass: magnetometer present (%llu samples so far, calibration %d)\n",
		    (unsigned long long)state.samples, state.calibration);
	} else {
		say("hakios: Compass: %s has no samples yet\n", HAKIOS_MAG_SYSCTL);
	}
}
