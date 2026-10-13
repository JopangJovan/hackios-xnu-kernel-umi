/*
 * libredwood-media.dylib: code of ours inside mediaserverd (see inject.c for how it gets there).
 *
 * mediaserverd is the audio server, and also the camera's: the capture stack of CMCapture lives
 * in the same process. Soon after it starts, that stack "prewarms" the Core Image filters of the
 * portrait mode, for which Core Image wants an OpenGL ES context; there is no GPU, the context
 * has no functions behind it, and the daemon jumps to address 0 -- every time launchd restarts
 * it. Nobody is going to take a portrait here: the prewarming is skipped.
 *
 * What the daemon is wanted for meanwhile is neither sound nor the camera: it is where
 * VideoToolbox decodes for everybody else (the service com.apple.coremedia.decompressionsession),
 * and a HEIC picture is HEVC inside. Without the daemon ImageIO has "no way to decode a HEIC",
 * and the stock wallpaper of iOS 16, whose layers are four such pictures, is its gradient alone.
 * There is no hardware decoder here; VideoToolbox takes its software one (VCPHEVC.videocodec).
 *
 * CMCapture is not loaded yet when this library is: the method is replaced when its image
 * arrives.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/dyld.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void
say(const char *format, ...)
{
	char line[256];
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

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define CACHE_BASE 0x180000000UL                /* where the cache was built to be, before the slide */

static uintptr_t slide;

/* +[BWCoreImageFilterRenderer prewarmPortraitFilterVersion:semanticStyleFilters:] */
static void
prewarm_portrait_filter(__unused id self, __unused SEL selector, __unused int version, __unused BOOL styles)
{
}

/*
 * The class methods replaced, each as soon as its class exists. `expected` is the unslid address
 * of the code in iOS 16.5 (20F66): a method that is not what it was is left alone.
 */
static struct replacement {
	const char *class_name, *selector;
	uintptr_t expected;
	IMP replacement;
	bool done;
} replacements[] = {
	{ "BWCoreImageFilterRenderer", "prewarmPortraitFilterVersion:semanticStyleFilters:", 0x1a2115c0cUL,
	  (IMP)prewarm_portrait_filter, false },
};

static void
image_added(__unused const struct mach_header *header, __unused intptr_t image_slide)
{
	for (size_t i = 0; i < sizeof(replacements) / sizeof(replacements[0]); i++) {
		struct replacement *r = &replacements[i];
		Class cls;
		Method method;
		IMP original;

		if (r->done || (cls = objc_getClass(r->class_name)) == NULL) {
			continue;
		}
		r->done = true;
		method = class_getClassMethod(cls, sel_registerName(r->selector));
		original = method ? method_getImplementation(method) : NULL;
		if (original == NULL || (uintptr_t)original != r->expected + slide) {
			say("redwood: +[%s %s]: found %#lx, expected %#lx; left alone\n", r->class_name, r->selector,
			    original ? (uintptr_t)original - slide : 0, r->expected);
			continue;
		}
		method_setImplementation(method, r->replacement);
		say("redwood: mediaserverd: +[%s %s] replaced\n", r->class_name, r->selector);
	}
}

__attribute__((constructor))
static void
redwood_media_init(void)
{
	const char *name = getprogname();
	size_t length = 0;

	if (name == NULL || strcmp(name, "mediaserverd") != 0) {
		return;
	}
	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
	_dyld_register_func_for_add_image(image_added);         /* also called for every image already there */
}
