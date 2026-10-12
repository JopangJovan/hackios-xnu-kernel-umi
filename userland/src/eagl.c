/*
 * libredwood-gl.dylib: an OpenGL ES implementation behind iOS's EAGLContext, inside the window
 * server (backboardd; libredwood.dylib loads this when the image has it: build.sh GL=1).
 *
 * CoreAnimation's render server draws with Metal, or else with OpenGL ES, or else with its own
 * software rasterizer (CA::WindowServer::EAGLServer::renderer()). For OpenGL ES it makes an
 * EAGLContext of API 3 and asks six things of it, all of which are replaced here:
 *
 *   -initWithAPI:properties:   ours makes a context of the OpenGL ES library behind us
 *   -API                       3
 *   -setParameter:to:          noted
 *   -getMacroContextPrivate    a table of functions, called as entry(table[0], arguments...) at
 *                              the offsets of iOS's own OpenGLES (src/eagl_slots.txt); ours is
 *                              made by mkeagl.py: each entry counts the call and passes it on,
 *                              except the ones written by hand below (my_gl...)
 *   -texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:
 *                              the texture bound to `target` is to show what the IOSurface holds
 *   -attachImage:toCoreSurface:invertedRender:
 *                              the texture bound to the target ("image") is to BE the IOSurface:
 *                              what is drawn into it (it becomes a framebuffer's colour
 *                              attachment) is the screen; or it is a picture to be shown
 *
 * A GPU's driver does the last two by using the IOSurface's memory as the texture's. Nothing of
 * the kind is assumed here: the texture is an ordinary one, filled from the IOSurface when that
 * has changed (its seed) and read back into it when a frame ends (glFlush, glFinish), with
 * nothing but OpenGL ES calls. So this file does not know what draws: the library is reached
 * through its gl* functions and the four functions of redwood_gl.h. A driver that can share the
 * memory needs surface_to_texture() and target_to_surface() replaced, and nothing else.
 *
 * Where CoreAnimation relies on iOS's library being what it is, this stands in between:
 * blue-first pixels into a texture called GL_RGBA (internal_format()), and shaders that read
 * textures at coordinates in pixels (my_glShaderSource()).
 *
 * What CoreAnimation asks of OpenGL ES is written down as it goes (the log lines "redwood-gl:"):
 * strings and limits it reads, texture formats and sizes, calls and triangles per frame. More
 * with the kernel's boot-args: gltrace=FIRST-LAST writes down every call of those frames (lines
 * "gl:", with a pixel of the framebuffer after each draw), glshaders=1 the text of every shader,
 * glshow=TEXTURE puts that texture over the corner of the picture.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#include <mach/mach_time.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "redwood_gl.h"

/* ---- OpenGL ES: the types, the few names used here, the library's functions --------------------- */

typedef unsigned int GLenum, GLbitfield, GLuint;
typedef int GLint, GLsizei, GLfixed, GLclampx;
typedef unsigned char GLboolean, GLubyte;
typedef signed char GLbyte;
typedef short GLshort;
typedef unsigned short GLushort, GLhalf;
typedef float GLfloat, GLclampf;
typedef double GLdouble, GLclampd;
typedef char GLchar;
typedef void GLvoid;
typedef long GLintptr, GLsizeiptr;
typedef int64_t GLint64;
typedef uint64_t GLuint64;
typedef struct __GLsync *GLsync;
typedef void *GLeglImageOES;
typedef void (*GLDEBUGPROC)(GLenum, GLenum, GLuint, GLenum, GLsizei, const GLchar *, const void *);
typedef GLDEBUGPROC GLDEBUGPROCKHR;

#define GL_TRIANGLES                    0x0004
#define GL_TRIANGLE_STRIP               0x0005
#define GL_TRIANGLE_FAN                 0x0006
#define GL_UNPACK_ROW_LENGTH            0x0cf2
#define GL_UNPACK_ALIGNMENT             0x0cf5
#define GL_PACK_ROW_LENGTH              0x0d02
#define GL_PACK_ALIGNMENT               0x0d05
#define GL_TEXTURE_2D                   0x0de1
#define GL_UNSIGNED_BYTE                0x1401
#define GL_RGBA                         0x1908
#define GL_VENDOR                       0x1f00
#define GL_RENDERER                     0x1f01
#define GL_VERSION                      0x1f02
#define GL_EXTENSIONS                   0x1f03
#define GL_TEXTURE_BINDING_2D           0x8069
#define GL_BGRA                         0x80e1
#define GL_INFO_LOG_LENGTH              0x8b84
#define GL_COMPILE_STATUS               0x8b81
#define GL_LINK_STATUS                  0x8b82
#define GL_SHADING_LANGUAGE_VERSION     0x8b8c
#define GL_COLOR_ATTACHMENT0            0x8ce0
#define GL_FRAMEBUFFER_COMPLETE         0x8cd5
#define GL_FRAMEBUFFER                  0x8d40

struct eagl;
static int eagl_tracing;                /* this frame's calls are written down (boot-arg gltrace=FIRST-LAST) */
static void eagl_trace(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void eagl_trace_floats(const float *values, int count);
static void eagl_enter(struct eagl *e, unsigned offset);
static void eagl_leave(struct eagl *e, unsigned offset);
static long eagl_missing(void *context, unsigned offset);
#define EAGL_ENTER(context, offset)     eagl_enter(context, offset)
#define EAGL_LEAVE(context, offset)     eagl_leave(context, offset)

#define EAGL_GEN_DECLARATIONS
#include "eagl_gen.h"
#undef EAGL_GEN_DECLARATIONS

static void
say(const char *format, ...)
{
	char line[1024];
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

static void
eagl_trace(const char *format, ...)
{
	char line[512] = "gl: ";
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line + 4, sizeof(line) - 4, format, ap) + 4;
	va_end(ap);
	write(2, line, n > (int)sizeof(line) - 1 ? sizeof(line) - 1 : (size_t)n);
}

static void
eagl_trace_floats(const float *values, int count)
{
	char line[512] = "gl:    ";
	int n = 7;

	for (int i = 0; i < count && i < 16 && n < 480; i++) {
		n += snprintf(line + n, sizeof(line) - (size_t)n, " %g", (double)values[i]);
	}
	line[n++] = '\n';
	write(2, line, (size_t)n);
}

static uint64_t
now_us(void)
{
	static mach_timebase_info_data_t base;

	if (base.denom == 0) {
		mach_timebase_info(&base);
	}
	return mach_absolute_time() * base.numer / base.denom / 1000;
}

/* ---- a context -------------------------------------------------------------------------------- */

#define SLOTS           0x440           /* entries of the table: more than iOS's has (checked below) */
#define FRAMEBUFFERS    4096            /* names of framebuffers followed: they are small numbers */

/* A texture that stands for an IOSurface. */
struct image {
	GLuint texture;
	IOSurfaceRef surface;
	bool target;                    /* drawn into (attachImage) rather than shown (texImageIOSurface) */
	bool invert, drawn;
	GLenum to, internal, format, type;
	uint32_t plane, seed;
	GLsizei width, height;
};

struct eagl {
	void *table[SLOTS];             /* [0] is the context itself: what every entry gets as its first argument */
	id object;                      /* the EAGLContext */
	struct redwood_gl *gl;
	unsigned long api;
	struct image images[256];
	unsigned image_count;
	GLuint framebuffer;             /* the one bound */
	GLuint colour[FRAMEBUFFERS];    /* the texture that is each framebuffer's colour attachment */
	GLuint reader;                  /* a framebuffer of ours, to read a texture back */
	bool in_frame;
	/* a frame's worth */
	unsigned calls, draws, triangles, uploads, clears;
	uint64_t upload_bytes, began;
	/* everything so far */
	unsigned frames, shaders, programs, errors;
	uint64_t all_calls, all_draws, all_triangles, readback_us, frame_us;
	unsigned most_calls, most_triangles;
	const char *sources[64];        /* the text of the shaders being made, by name modulo this */
	bool fragment[64];              /* ... and which of the two kinds each is */
};

static unsigned trace_first, trace_last;        /* the frames whose calls are written down */
static int all_shaders;                         /* boot-arg glshaders=1: the text of every shader, not of the first two */
static __thread struct eagl *current;
static struct eagl *contexts[8];
static uint32_t slot_calls[SLOTS];
static int check_errors = 1;            /* ask the library for an error after every call (the first frames) */

static const char *eagl_slot_name(unsigned offset);

static void
eagl_enter(struct eagl *e, unsigned offset)
{
	if (current != e) {
		if (!redwood_gl_make_current(e->gl)) {
			say("redwood-gl: the context cannot be made this thread's\n");
		}
		current = e;
	}
	slot_calls[offset / 8]++;
	e->calls++;
}

static void
eagl_leave(struct eagl *e, unsigned offset)
{
	GLenum error;

	if (check_errors && offset != 0x338 && (error = glGetError()) != 0) {
		if (e->errors++ < 60) {
			say("redwood-gl: %s: error %#x\n", eagl_slot_name(offset), error);
		}
	}
}

/* An entry the library has no function for: say so, once an entry. */
static long
eagl_missing(void *context, unsigned offset)
{
	struct eagl *e = context;

	eagl_enter(e, offset);
	if (slot_calls[offset / 8] == 1) {
		say("redwood-gl: CALLED AND NOT THERE: entry %#x, %s\n", offset, eagl_slot_name(offset));
	}
	return 0;
}

static struct eagl *
context_of(id object)
{
	for (unsigned i = 0; i < sizeof(contexts) / sizeof(contexts[0]); i++) {
		if (contexts[i] != NULL && contexts[i]->object == object) {
			return contexts[i];
		}
	}
	return NULL;
}

/* ---- what CoreAnimation asks for: kept for the summary ------------------------------------------- */

struct format_use {
	GLenum internal, format, type;
	unsigned count, widest, tallest;
	const char *how;
};
static struct format_use formats[48];
static unsigned widest_texture, tallest_texture;

static void
note_format(const char *how, GLenum internal, GLenum format, GLenum type, GLsizei width, GLsizei height)
{
	struct format_use *use = NULL;

	for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
		if (formats[i].count == 0 || (formats[i].how == how && formats[i].internal == internal &&
		    formats[i].format == format && formats[i].type == type)) {
			use = &formats[i];
			break;
		}
	}
	if ((unsigned)width > widest_texture) {
		widest_texture = (unsigned)width;
	}
	if ((unsigned)height > tallest_texture) {
		tallest_texture = (unsigned)height;
	}
	if (use == NULL) {
		return;
	}
	if (use->count++ == 0) {
		use->how = how;
		use->internal = internal;
		use->format = format;
		use->type = type;
		say("redwood-gl: a new kind of texture: %s, internal format %#x, format %#x, type %#x, %d x %d\n", how, internal,
		    format, type, width, height);
	}
	if ((unsigned)width > use->widest) {
		use->widest = (unsigned)width;
	}
	if ((unsigned)height > use->tallest) {
		use->tallest = (unsigned)height;
	}
}

static void
summary(struct eagl *e)
{
	unsigned frames = e->frames ? e->frames : 1;

	say("redwood-gl: after %u frames: %llu calls, %llu draws and %llu triangles a frame on average; the most in one: "
	    "%u calls, %u triangles; %llu ms a frame, of which %llu copying the picture out; %u shaders, %u programs; "
	    "biggest texture %u x %u; %u errors\n", e->frames, e->all_calls / frames, e->all_draws / frames,
	    e->all_triangles / frames, e->most_calls, e->most_triangles, e->frame_us / frames / 1000,
	    e->readback_us / frames / 1000, e->shaders, e->programs, widest_texture, tallest_texture, e->errors);
	for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]) && formats[i].count; i++) {
		say("redwood-gl:   textures: %s %#x/%#x/%#x: %u times, up to %u x %u\n", formats[i].how, formats[i].internal,
		    formats[i].format, formats[i].type, formats[i].count, formats[i].widest, formats[i].tallest);
	}
	for (unsigned offset = 8; offset < SLOTS * 8; offset += 8) {
		if (slot_calls[offset / 8]) {
			say("redwood-gl:   %-28s %u\n", eagl_slot_name(offset), slot_calls[offset / 8]);
		}
	}
}

/* ---- IOSurfaces ----------------------------------------------------------------------------------- */

static struct image *
image_of(struct eagl *e, GLuint texture)
{
	for (unsigned i = 0; i < e->image_count; i++) {
		if (e->images[i].texture == texture) {
			return &e->images[i];
		}
	}
	return NULL;
}

static struct image *
image_new(struct eagl *e, GLuint texture, IOSurfaceRef surface)
{
	struct image *image = image_of(e, texture);

	if (image == NULL) {
		if (e->image_count == sizeof(e->images) / sizeof(e->images[0])) {
			say("redwood-gl: more textures with IOSurfaces than there is room for\n");
			return NULL;
		}
		image = &e->images[e->image_count++];
	} else {
		CFRelease(image->surface);
	}
	memset(image, 0, sizeof(*image));
	image->texture = texture;
	image->surface = (IOSurfaceRef)CFRetain(surface);
	return image;
}

static void
image_forget(struct eagl *e, GLuint texture)
{
	struct image *image = image_of(e, texture);

	if (image != NULL) {
		CFRelease(image->surface);
		*image = e->images[--e->image_count];
	}
}

/* Apple's OpenGL ES takes blue-first pixels into a texture called GL_RGBA; everybody else's calls the texture so too. */
static GLenum
internal_format(GLenum internal, GLenum format)
{
	return format == GL_BGRA && internal == GL_RGBA ? GL_BGRA : internal;
}

static unsigned
bytes_a_pixel(GLenum format, GLenum type)
{
	unsigned parts = format == GL_RGBA || format == GL_BGRA ? 4 : format == 0x1907 ? 3 :        /* RGB */
	    format == 0x8227 || format == 0x190a ? 2 : 1;                                           /* RG, luminance + alpha */

	switch (type) {
	case GL_UNSIGNED_BYTE:
		return parts;
	case 0x140b: case 0x8d61: case 0x1403:          /* half floats, 16 bits */
		return 2 * parts;
	case 0x1406:                                    /* floats */
		return 4 * parts;
	case 0x8368:                                    /* 2:10:10:10 in a word */
		return 4;
	default:
		return 2;                               /* the packed 16-bit kinds */
	}
}

/*
 * Fill a texture, which is bound, from its IOSurface. This and target_to_surface() are where a
 * driver that maps the IOSurface's memory would do something else.
 */
static void
surface_to_texture(struct eagl *e, struct image *image)
{
	IOSurfaceRef surface = image->surface;
	bool planar = IOSurfaceGetPlaneCount(surface) > 0;
	size_t row = planar ? IOSurfaceGetBytesPerRowOfPlane(surface, image->plane) : IOSurfaceGetBytesPerRow(surface);
	unsigned pixel = bytes_a_pixel(image->format, image->type);
	GLint row_length = 0, alignment = 4;
	uint8_t *base, *flipped = NULL;

	if (IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL) != 0) {
		say("redwood-gl: an IOSurface cannot be locked\n");
		return;
	}
	base = planar ? IOSurfaceGetBaseAddressOfPlane(surface, image->plane) : IOSurfaceGetBaseAddress(surface);
	if (image->invert && (flipped = malloc(row * (size_t)image->height)) != NULL) {
		for (GLsizei y = 0; y < image->height; y++) {
			memcpy(flipped + row * (size_t)y, base + row * (size_t)(image->height - 1 - y), row);
		}
		base = flipped;
	}
	glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(row / pixel));
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(image->to, 0, (GLint)internal_format(image->internal, image->format), image->width, image->height, 0,
	    image->format, image->type, base);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
	glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
	IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
	free(flipped);
	image->seed = IOSurfaceGetSeed(surface);
	e->uploads++;
	e->upload_bytes += row * (size_t)image->height;
}

/* What has been drawn into a texture, into its IOSurface. */
static void
target_to_surface(struct eagl *e, struct image *image)
{
	IOSurfaceRef surface = image->surface;
	size_t row = IOSurfaceGetBytesPerRow(surface);
	uint8_t *base;

	if (e->reader == 0) {
		glGenFramebuffers(1, &e->reader);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, e->reader);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, image->texture, 0);
	if (IOSurfaceLock(surface, 0, NULL) == 0) {
		base = IOSurfaceGetBaseAddress(surface);
		glPixelStorei(GL_PACK_ROW_LENGTH, (GLint)(row / 4));
		glPixelStorei(GL_PACK_ALIGNMENT, 4);
		if (!image->invert) {
			glReadPixels(0, 0, image->width, image->height, GL_BGRA, GL_UNSIGNED_BYTE, base);
		} else {
			for (GLsizei y = 0; y < image->height; y++) {
				glReadPixels(0, y, image->width, 1, GL_BGRA, GL_UNSIGNED_BYTE, base + row * (size_t)(image->height - 1 - y));
			}
		}
		glPixelStorei(GL_PACK_ROW_LENGTH, 0);
		IOSurfaceUnlock(surface, 0, NULL);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, e->framebuffer);
	image->drawn = false;
}

/*
 * For looking at what is drawn on the way to the picture (boot-arg glshow=TEXTURE): that texture's
 * pixels are put over the top left corner of the picture as it goes out.
 */
#define GL_TEXTURE_WIDTH        0x1000
#define GL_TEXTURE_HEIGHT       0x1001

static GLuint shown_texture;

static void
show_texture(struct eagl *e, struct image *image)
{
	GLint width = 0, height = 0, bound = 0;
	size_t row = IOSurfaceGetBytesPerRow(image->surface);
	uint8_t *pixels, *base;

	glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
	glBindTexture(GL_TEXTURE_2D, shown_texture);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
	glBindTexture(GL_TEXTURE_2D, (GLuint)bound);
	if (width <= 0 || height <= 0 || (pixels = malloc((size_t)width * (size_t)height * 4)) == NULL) {
		return;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, e->reader);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, shown_texture, 0);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glBindFramebuffer(GL_FRAMEBUFFER, e->framebuffer);
	if (glGetError() == 0 && IOSurfaceLock(image->surface, 0, NULL) == 0) {
		base = IOSurfaceGetBaseAddress(image->surface);
		for (GLint y = 0; y < height && y < image->height; y++) {
			memcpy(base + row * (size_t)y, pixels + (size_t)y * (size_t)width * 4,
			    (size_t)(width < image->width ? width : image->width) * 4);
		}
		IOSurfaceUnlock(image->surface, 0, NULL);
	}
	free(pixels);
}

/* Before the first drawing of a frame: the textures whose IOSurfaces somebody has written to since. */
static void
frame_begin(struct eagl *e)
{
	GLint bound = 0;
	bool rebound = false;

	if (e->in_frame) {
		return;
	}
	e->in_frame = true;
	e->began = now_us();
	eagl_tracing = e->frames + 1 >= trace_first && e->frames + 1 <= trace_last;
	if (eagl_tracing) {
		eagl_trace("---- frame %u (framebuffer %u)\n", e->frames + 1, e->framebuffer);
	}
	for (unsigned i = 0; i < e->image_count; i++) {
		struct image *image = &e->images[i];

		if (image->target || IOSurfaceGetSeed(image->surface) == image->seed) {
			continue;
		}
		if (!rebound) {
			glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
			rebound = true;
		}
		glBindTexture(image->to, image->texture);
		surface_to_texture(e, image);
	}
	if (rebound) {
		glBindTexture(GL_TEXTURE_2D, (GLuint)bound);
	}
}

/* glFlush, glFinish: what was drawn must be where the display takes it from. */
static void
frame_end(struct eagl *e)
{
	uint64_t before = now_us(), after;
	unsigned copied = 0;

	for (unsigned i = 0; i < e->image_count; i++) {
		if (e->images[i].target && e->images[i].drawn) {
			target_to_surface(e, &e->images[i]);
			if (shown_texture != 0) {
				show_texture(e, &e->images[i]);
			}
			copied++;
		}
	}
	if (!e->in_frame) {
		return;
	}
	after = now_us();
	e->in_frame = false;
	e->frames++;
	e->all_calls += e->calls;
	e->all_draws += e->draws;
	e->all_triangles += e->triangles;
	e->readback_us += after - before;
	e->frame_us += after - e->began;
	if (e->calls > e->most_calls) {
		e->most_calls = e->calls;
	}
	if (e->triangles > e->most_triangles) {
		e->most_triangles = e->triangles;
	}
	if (e->frames <= 40 || e->frames % 20 == 0) {
		say("redwood-gl: frame %u: %u GL calls, %u draws, %u triangles, %u clears; %u textures refilled from IOSurfaces "
		    "(%llu KB); %u picture%s copied out; %llu ms\n", e->frames, e->calls, e->draws, e->triangles, e->clears,
		    e->uploads, e->upload_bytes >> 10, copied, copied == 1 ? "" : "s", (after - e->began) / 1000);
	}
	if (e->frames == 30 || e->frames % 200 == 0) {
		summary(e);
	}
	if (e->frames == 600 && e->errors == 0) {
		check_errors = 0;
	}
	e->calls = e->draws = e->triangles = e->uploads = e->clears = 0;
	e->upload_bytes = 0;
}

/* Something is about to be drawn into the framebuffer that is bound. */
static void
drawing(struct eagl *e)
{
	struct image *image;

	frame_begin(e);
	if (e->framebuffer != 0 && e->framebuffer < FRAMEBUFFERS && e->colour[e->framebuffer] != 0 &&
	    (image = image_of(e, e->colour[e->framebuffer])) != NULL && image->target) {
		image->drawn = true;
	}
}

/* ---- the entries written by hand ---------------------------------------------------------------- */

static void
my_glBindTexture(void *context, GLenum target, GLuint texture)
{
	struct eagl *e = context;
	struct image *image;

	EAGL_ENTER(e, 0x30);
	if (eagl_tracing) {
		eagl_trace("glBindTexture(%#x, %u)\n", target, texture);
	}
	glBindTexture(target, texture);
	if (texture != 0 && e->image_count != 0 && (image = image_of(e, texture)) != NULL && !image->target &&
	    IOSurfaceGetSeed(image->surface) != image->seed) {
		surface_to_texture(e, image);
	}
	EAGL_LEAVE(e, 0x30);
}

static void
my_glClear(void *context, GLbitfield mask)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x58);
	drawing(e);
	e->clears++;
	if (eagl_tracing) {
		eagl_trace("glClear(%#x)\n", mask);
	}
	glClear(mask);
	EAGL_LEAVE(e, 0x58);
}

/* While calls are written down: what a draw has left in the middle of what it drew into, and near its top. */
#define GL_VIEWPORT             0x0ba2

static void
trace_pixels(struct eagl *e)
{
	GLint view[4] = { 0, 0, 0, 0 };
	uint8_t middle[4] = { 0, 0, 0, 0 }, low[4] = { 0, 0, 0, 0 };

	glGetIntegerv(GL_VIEWPORT, view);
	glReadPixels(view[0] + view[2] / 2, view[1] + view[3] / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, middle);
	glReadPixels(view[0] + view[2] / 2, view[1] + view[3] / 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, low);
	eagl_trace("    framebuffer %u (%d x %d): middle %u %u %u %u, an eighth up %u %u %u %u (red green blue alpha)%s\n",
	    e->framebuffer, view[2], view[3], middle[0], middle[1], middle[2], middle[3], low[0], low[1], low[2], low[3],
	    glGetError() ? " (not readable)" : "");
}

static unsigned
triangles_of(GLenum mode, GLsizei count)
{
	return mode == GL_TRIANGLES ? (unsigned)count / 3 : (mode == GL_TRIANGLE_STRIP || mode == GL_TRIANGLE_FAN) &&
	       count > 2 ? (unsigned)count - 2 : 0;
}

static void
my_glDrawArrays(void *context, GLenum mode, GLint first, GLsizei count)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x210);
	drawing(e);
	e->draws++;
	e->triangles += triangles_of(mode, count);
	if (eagl_tracing) {
		eagl_trace("glDrawArrays(%#x, %d, %d)\n", mode, first, count);
	}
	glDrawArrays(mode, first, count);
	EAGL_LEAVE(e, 0x210);
	if (eagl_tracing) {
		trace_pixels(e);
	}
}

static void
my_glDrawElements(void *context, GLenum mode, GLsizei count, GLenum type, const void *indices)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x220);
	drawing(e);
	e->draws++;
	e->triangles += triangles_of(mode, count);
	if (eagl_tracing) {
		eagl_trace("glDrawElements(%#x, %d, %#x, %p)\n", mode, count, type, indices);
	}
	glDrawElements(mode, count, type, indices);
	EAGL_LEAVE(e, 0x220);
	if (eagl_tracing) {
		trace_pixels(e);
	}
}

static void
my_glFinish(void *context)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x2d0);
	if (eagl_tracing) {
		eagl_trace("glFinish()\n");
	}
	glFinish();
	frame_end(e);
	EAGL_LEAVE(e, 0x2d0);
}

static void
my_glFlush(void *context)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x2d8);
	if (eagl_tracing) {
		eagl_trace("glFlush()\n");
	}
	glFlush();
	frame_end(e);
	EAGL_LEAVE(e, 0x2d8);
}

/* APPLE_fence: wait for one object to be done with. Everything is done with after glFinish. */
static void
my_glFinishObjectAPPLE(void *context, GLenum object, GLint name)
{
	struct eagl *e = context;

	(void)object;
	(void)name;
	EAGL_ENTER(e, 0xeb8);
	glFinish();
	frame_end(e);
	EAGL_LEAVE(e, 0xeb8);
}

static void
my_glDeleteTextures(void *context, GLsizei n, const GLuint *textures)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x1e0);
	for (GLsizei i = 0; i < n; i++) {
		image_forget(e, textures[i]);
	}
	glDeleteTextures(n, textures);
	EAGL_LEAVE(e, 0x1e0);
}

static const GLubyte *
my_glGetString(void *context, GLenum name)
{
	struct eagl *e = context;
	const GLubyte *string;

	EAGL_ENTER(e, 0x3b0);
	/* 0x8a1e is no name of OpenGL ES's; iOS's library answers it (the constructor of CA::OGL::GLESContext asks) */
	string = name == 0x8a1e ? (const GLubyte *)"" : glGetString(name);
	say("redwood-gl: asked for string %#x: %.900s\n", name, string ? (const char *)string : "(none)");
	EAGL_LEAVE(e, 0x3b0);
	return string;
}

static void
my_glGetIntegerv(void *context, GLenum name, GLint *values)
{
	struct eagl *e = context;
	static unsigned said;

	EAGL_ENTER(e, 0x348);
	glGetIntegerv(name, values);
	if (said < 40) {
		said++;
		say("redwood-gl: asked for number %#x: %d\n", name, values[0]);
	}
	EAGL_LEAVE(e, 0x348);
}

static void
my_glTexImage2D(void *context, GLenum target, GLint level, GLint internal, GLsizei width, GLsizei height, GLint border,
    GLenum format, GLenum type, const void *pixels)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x970);
	if (level == 0) {
		note_format(pixels ? "filled" : "empty", (GLenum)internal, format, type, width, height);
	}
	if (eagl_tracing) {
		eagl_trace("glTexImage2D(%#x, %d, %#x, %d, %d, %d, %#x, %#x, %p)\n", target, level, internal, width, height, border,
		    format, type, pixels);
		if (pixels != NULL && type == 0x140b && format == GL_RGBA) {            /* half floats: the first and the last pixel */
			const uint16_t *half = pixels;

			eagl_trace("    half floats: %#x %#x %#x %#x ... %#x %#x %#x %#x\n", half[0], half[1], half[2], half[3],
			    half[4 * (width - 1)], half[4 * (width - 1) + 1], half[4 * (width - 1) + 2], half[4 * (width - 1) + 3]);
		}
	}
	glTexImage2D(target, level, (GLint)internal_format((GLenum)internal, format), width, height, border, format, type, pixels);
	EAGL_LEAVE(e, 0x970);
}

static void
my_glCompressedTexImage2D(void *context, GLenum target, GLint level, GLenum internal, GLsizei width, GLsizei height,
    GLint border, GLsizei size, const void *data)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0xbe8);
	if (level == 0) {
		note_format("compressed", internal, 0, 0, width, height);
	}
	glCompressedTexImage2D(target, level, internal, width, height, border, size, data);
	EAGL_LEAVE(e, 0xbe8);
}

static void
my_glBindFramebuffer(void *context, GLenum target, GLuint framebuffer)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x1508);
	e->framebuffer = framebuffer;
	if (eagl_tracing) {
		eagl_trace("glBindFramebuffer(%#x, %u)\n", target, framebuffer);
	}
	glBindFramebuffer(target, framebuffer);
	EAGL_LEAVE(e, 0x1508);
}

static void
my_glFramebufferTexture2D(void *context, GLenum target, GLenum attachment, GLenum to, GLuint texture, GLint level)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x1530);
	if (attachment == GL_COLOR_ATTACHMENT0 && e->framebuffer < FRAMEBUFFERS) {
		e->colour[e->framebuffer] = texture;
	}
	if (eagl_tracing) {
		eagl_trace("glFramebufferTexture2D(%#x, %#x, %#x, %u, %d)\n", target, attachment, to, texture, level);
	}
	glFramebufferTexture2D(target, attachment, to, texture, level);
	EAGL_LEAVE(e, 0x1530);
}

static void
my_glDeleteFramebuffers(void *context, GLsizei n, const GLuint *framebuffers)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x1510);
	for (GLsizei i = 0; i < n; i++) {
		if (framebuffers[i] < FRAMEBUFFERS) {
			e->colour[framebuffers[i]] = 0;
		}
		if (framebuffers[i] == e->framebuffer) {
			e->framebuffer = 0;
		}
	}
	glDeleteFramebuffers(n, framebuffers);
	EAGL_LEAVE(e, 0x1510);
}

/* A private entry of iOS's library (CA::OGL::GLESContext::set_surface calls it); nobody else has it. */
static void
my_glFramebufferParameteriAPPLE(void *context, GLenum target, GLenum name, GLint value)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x2008);
	if (slot_calls[0x2008 / 8] <= 4) {
		say("redwood-gl: glFramebufferParameteriAPPLE(%#x, %#x, %d): nothing done\n", target, name, value);
	}
}

/* EXT_debug_marker: names for a debugger's list of calls. */
static void
my_glInsertEventMarkerEXT(void *context, GLsizei length, const GLchar *marker)
{
	(void)length;
	(void)marker;
	EAGL_ENTER(context, 0x1988);
}

static void
my_glPushGroupMarkerEXT(void *context, GLsizei length, const GLchar *marker)
{
	(void)length;
	(void)marker;
	EAGL_ENTER(context, 0x1990);
}

static void
my_glPopGroupMarkerEXT(void *context)
{
	EAGL_ENTER(context, 0x1998);
}

/*
 * CoreAnimation's shaders are in the first version of the shading language, with one thing of
 * Apple's own in them: sampler2DRect and texture2DRect(), a texture read at coordinates counted in
 * its pixels (what it shows of IOSurfaces is read so). The language has no such thing, and in
 * its first version no way to make one, since a shader cannot ask how big a texture is. In
 * version 3.00 it can. So every shader is handed on as a 3.00 one: the words of the first
 * version that 3.00 dropped are defined back, and texture2DRect() divides by textureSize().
 * (All of a program's shaders must be of one version, so all are treated alike.)
 */
#define GL_FRAGMENT_SHADER      0x8b30

static const char fragment_start[] =
    "#define varying in\n"
    "#define texture2D texture\n"
    "#define texture2DProj textureProj\n"
    "#define texture2DLod textureLod\n"
    "#define textureCube texture\n"
    "#define sampler2DRect sampler2D\n"
    "#define texture2DRect(s, c) texture(s, (c) / vec2(textureSize(s, 0)))\n"
    "layout(location = 0) out highp vec4 redwood_colour[1];\n"
    "#define gl_FragData redwood_colour\n"
    "#define gl_FragColor redwood_colour[0]\n"
    "#line 1\n";
static const char vertex_start[] =
    "#define attribute in\n"
    "#define varying out\n"
    "#define texture2D texture\n"
    "#define texture2DLod textureLod\n"
    "#line 1\n";

static GLuint
my_glCreateShader(void *context, GLenum type)
{
	struct eagl *e = context;
	GLuint shader;

	EAGL_ENTER(e, 0x1298);
	shader = glCreateShader(type);
	e->fragment[shader % 64] = type == GL_FRAGMENT_SHADER;
	EAGL_LEAVE(e, 0x1298);
	return shader;
}

/*
 * CoreAnimation does not ask whether its shaders compiled. Here it is asked, and what the
 * compiler said is written down with the shader's text.
 */
static void
my_glShaderSource(void *context, GLuint shader, GLsizei count, const GLchar *const *strings, const GLint *lengths)
{
	struct eagl *e = context;
	const char *start = e->fragment[shader % 64] ? fragment_start : vertex_start;
	const GLchar *parts[3] = { "#version 300 es\n", start, NULL };
	size_t size = 1, extensions = 0;
	char *text, *line, *head;

	EAGL_ENTER(e, 0x12a0);
	for (GLsizei i = 0; i < count; i++) {
		size += lengths != NULL && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
	}
	if ((text = calloc(1, 2 * size + sizeof(fragment_start) + 64)) == NULL) {
		glShaderSource(shader, count, strings, lengths);
		return;
	}
	for (GLsizei i = 0; i < count; i++) {
		strncat(text, strings[i], lengths != NULL && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]));
	}
	/* "#extension" lines must stay ahead of everything that is not the preprocessor's: they move to the top */
	head = text + size;
	strcpy(head, parts[0]);
	for (line = text; (line = strstr(line, "#extension")) != NULL; line += 2) {
		size_t length = strcspn(line, "\n");

		strncat(head, line, length);
		strcat(head, "\n");
		line[0] = line[1] = '/';
		extensions++;
	}
	parts[0] = head;
	parts[2] = text;
	free((void *)e->sources[shader % 64]);
	e->sources[shader % 64] = text;
	glShaderSource(shader, 3, parts, NULL);
	EAGL_LEAVE(e, 0x12a0);
}

static void
say_long(const char *what, const char *text)
{
	say("redwood-gl: %s:\n", what);
	for (size_t at = 0, length = strlen(text); at < length; at += 900) {
		write(2, text + at, length - at < 900 ? length - at : 900);
	}
	say("\n");
}

static void
my_glCompileShader(void *context, GLuint shader)
{
	struct eagl *e = context;
	GLint good = 0;
	char said[1024] = "";

	EAGL_ENTER(e, 0x12a8);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &good);
	e->shaders++;
	if (!good) {
		glGetShaderInfoLog(shader, sizeof(said), NULL, said);
		say("redwood-gl: A SHADER DOES NOT COMPILE (shader %u): %s\n", shader, said);
		if (e->sources[shader % 64] != NULL) {
			say_long("its text", e->sources[shader % 64]);
		}
	} else if ((e->shaders <= 2 || all_shaders) && e->sources[shader % 64] != NULL) {
		say("redwood-gl: shader %u:\n", shader);
		say_long(all_shaders ? "a shader" : "the first shaders, for the record", e->sources[shader % 64]);
	}
	EAGL_LEAVE(e, 0x12a8);
}

static void
my_glLinkProgram(void *context, GLuint program)
{
	struct eagl *e = context;
	GLint good = 0;
	char said[1024] = "";

	EAGL_ENTER(e, 0x12c0);
	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &good);
	e->programs++;
	if (!good) {
		glGetProgramInfoLog(program, sizeof(said), NULL, said);
		say("redwood-gl: A PROGRAM DOES NOT LINK (program %u): %s\n", program, said);
	}
	EAGL_LEAVE(e, 0x12c0);
}

/* iOS's library has one entry for deleting a program and deleting a shader. */
static void
my_glDeleteProgram(void *context, GLuint name)
{
	struct eagl *e = context;

	EAGL_ENTER(e, 0x1280);
	if (glIsProgram(name)) {
		glDeleteProgram(name);
	} else {
		glDeleteShader(name);
	}
	EAGL_LEAVE(e, 0x1280);
}

#define EAGL_GEN_TABLE
#include "eagl_gen.h"
#undef EAGL_GEN_TABLE

_Static_assert(EAGL_TABLE_SIZE <= SLOTS * 8, "the table has grown");

/* ---- EAGLContext ---------------------------------------------------------------------------------- */

static id
my_initWithAPI(id self, SEL selector, unsigned long api, id properties)
{
	struct eagl *e;
	unsigned place;

	(void)selector;
	(void)properties;
	for (place = 0; place < sizeof(contexts) / sizeof(contexts[0]) && contexts[place] != NULL; place++) {
	}
	if (api < 2 || api > 3 || place == sizeof(contexts) / sizeof(contexts[0]) || (e = calloc(1, sizeof(*e))) == NULL) {
		say("redwood-gl: no context of API %lu\n", api);
		return nil;
	}
	if ((e->gl = redwood_gl_create((int)api)) == NULL || !redwood_gl_make_current(e->gl)) {
		say("redwood-gl: %s gives no OpenGL ES %lu context\n", redwood_gl_library(), api);
		free(e);
		return nil;
	}
	current = e;
	e->object = self;
	e->api = api;
	e->table[0] = e;
	eagl_fill_table(e->table);
	contexts[place] = e;
	say("redwood-gl: an OpenGL ES %lu context for CoreAnimation, by %s: %s; %s; shaders %s\n", api, redwood_gl_library(),
	    (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER),
	    (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));
	return self;
}

static unsigned long
my_API(id self, SEL selector)
{
	struct eagl *e = context_of(self);

	(void)selector;
	return e ? e->api : 0;
}

static unsigned long
my_setParameter(id self, SEL selector, unsigned parameter, const int32_t *values)
{
	(void)self;
	(void)selector;
	say("redwood-gl: -setParameter:%#x to:%d: noted\n", parameter, values ? values[0] : 0);
	return 0;
}

static void *
my_getMacroContextPrivate(id self, SEL selector)
{
	struct eagl *e = context_of(self);

	(void)selector;
	return e ? e->table : NULL;
}

/* (the types are the method's own: the last three arrive on the stack, where sizes matter) */
static BOOL
my_texImageIOSurface(id self, SEL selector, IOSurfaceRef surface, unsigned long target, unsigned long internal,
    GLsizei width, GLsizei height, unsigned long format, unsigned long type, uint32_t plane, BOOL invert)
{
	static unsigned shown;
	struct eagl *e = context_of(self);
	struct image *image;
	GLint texture = 0;

	(void)selector;
	if (e == NULL || surface == NULL) {
		return NO;
	}
	eagl_enter(e, 0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
	if (target != GL_TEXTURE_2D || texture == 0 || (image = image_new(e, (GLuint)texture, surface)) == NULL) {
		say("redwood-gl: -texImageIOSurface: target %#lx, texture %d: not done\n", target, texture);
		return NO;
	}
	image->to = (GLenum)target;
	image->internal = (GLenum)internal;
	image->format = (GLenum)format;
	image->type = (GLenum)type;
	image->plane = plane;
	image->invert = invert;
	image->width = width;
	image->height = height;
	note_format(invert ? "IOSurface shown, inverted" : "IOSurface shown", image->internal, image->format, image->type, width,
	    height);
	if (shown++ < 24) {
		say("redwood-gl: -texImageIOSurface: surface %u ('%.4s' %zu x %zu, %zu planes) as texture %d: %d x %d, plane %u%s\n",
		    IOSurfaceGetID(surface), (const char *)&(uint32_t){ __builtin_bswap32(IOSurfaceGetPixelFormat(surface)) },
		    IOSurfaceGetWidth(surface), IOSurfaceGetHeight(surface), IOSurfaceGetPlaneCount(surface), texture, width,
		    height, plane, invert ? ", inverted" : "");
	}
	surface_to_texture(e, image);
	eagl_leave(e, 0x970);
	return YES;
}

/* (the "image" is the target, GL_TEXTURE_2D, and means the texture bound to it) */
static BOOL
my_attachImage(id self, SEL selector, unsigned long target, IOSurfaceRef surface, BOOL inverted)
{
	struct eagl *e = context_of(self);
	static unsigned attached;
	struct image *image;
	GLint texture = 0;
	uint32_t pixels;

	(void)selector;
	if (e == NULL || surface == NULL) {
		return NO;
	}
	eagl_enter(e, 0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
	pixels = IOSurfaceGetPixelFormat(surface);
	if (target != GL_TEXTURE_2D || texture == 0 || (pixels != 'BGRA' && pixels != 0) ||
	    IOSurfaceGetBytesPerElement(surface) != 4 || IOSurfaceGetPlaneCount(surface) != 0 ||
	    (image = image_new(e, (GLuint)texture, surface)) == NULL) {
		say("redwood-gl: -attachImage: target %#lx, texture %d, an IOSurface of '%.4s': not done\n", target, texture,
		    (const char *)&(uint32_t){ __builtin_bswap32(pixels) });
		return NO;
	}
	image->target = true;
	image->to = GL_TEXTURE_2D;
	image->invert = inverted;
	image->width = (GLsizei)IOSurfaceGetWidth(surface);
	image->height = (GLsizei)IOSurfaceGetHeight(surface);
	if (attached++ < 24) {
		say("redwood-gl: -attachImage: texture %d is IOSurface %u ('%.4s' %d x %d, %zu bytes a row)%s\n", texture,
		    IOSurfaceGetID(surface), (const char *)&(uint32_t){ __builtin_bswap32(pixels) }, image->width, image->height,
		    IOSurfaceGetBytesPerRow(surface), inverted ? ", drawn inverted" : "");
	}
	note_format("IOSurface attached", GL_BGRA, GL_BGRA, GL_UNSIGNED_BYTE, image->width, image->height);
	/*
	 * What is attached is as often read as drawn into (a picture CoreAnimation has made a surface
	 * around, to be shown): the texture starts with what the surface holds.
	 */
	image->internal = image->format = GL_BGRA;
	image->type = GL_UNSIGNED_BYTE;
	surface_to_texture(e, image);
	eagl_leave(e, 0x970);
	return YES;
}

static void
my_dealloc(id self, SEL selector)
{
	struct objc_super super = { self, class_getSuperclass(objc_getClass("EAGLContext")) };
	struct eagl *e = context_of(self);

	if (e != NULL) {
		say("redwood-gl: the context goes\n");
		summary(e);
		for (unsigned i = 0; i < sizeof(contexts) / sizeof(contexts[0]); i++) {
			if (contexts[i] == e) {
				contexts[i] = NULL;
			}
		}
		if (current == e) {
			redwood_gl_make_current(NULL);
			current = NULL;
		}
		redwood_gl_destroy(e->gl);
		free(e);
	}
	((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&super, selector);
}

__attribute__((constructor)) static void
install(void)
{
	static const struct {
		const char *name;
		IMP ours;
	} methods[] = {
		{ "initWithAPI:properties:", (IMP)my_initWithAPI },
		{ "API", (IMP)my_API },
		{ "setParameter:to:", (IMP)my_setParameter },
		{ "getMacroContextPrivate", (IMP)my_getMacroContextPrivate },
		{ "texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:", (IMP)my_texImageIOSurface },
		{ "attachImage:toCoreSurface:invertedRender:", (IMP)my_attachImage },
		{ "dealloc", (IMP)my_dealloc },
	};
	Class class = objc_getClass("EAGLContext");
	unsigned replaced = 0;
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	const char *argument;

	/* the kernel's boot-args are where a run is told things */
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
		if ((argument = strstr(arguments, "gltrace=")) != NULL) {
			trace_last = trace_first = (unsigned)strtoul(argument + 8, (char **)&argument, 10);
			if (*argument == '-') {
				trace_last = (unsigned)strtoul(argument + 1, NULL, 10);
			}
		}
		all_shaders = strstr(arguments, "glshaders=1") != NULL;
		if ((argument = strstr(arguments, "glshow=")) != NULL) {
			shown_texture = (GLuint)strtoul(argument + 7, NULL, 10);
		}
	}

	if (class == Nil) {
		say("redwood-gl: this program has no EAGLContext: nothing to do\n");
		return;
	}
	for (unsigned i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
		SEL selector = sel_registerName(methods[i].name);
		Method method = class_getInstanceMethod(class, selector);

		if (method == NULL) {
			say("redwood-gl: EAGLContext has no -%s\n", methods[i].name);
			continue;
		}
		say("redwood-gl: -[EAGLContext %s] (%s) replaced\n", methods[i].name, method_getTypeEncoding(method));
		if (method == class_getInstanceMethod(class_getSuperclass(class), selector)) {
			/* inherited: the class gets one of its own, the one it inherits is everybody's */
			class_addMethod(class, selector, methods[i].ours, method_getTypeEncoding(method));
		} else {
			method_setImplementation(method, methods[i].ours);
		}
		replaced++;
	}
	say("redwood-gl: %u of %zu methods of EAGLContext are ours; OpenGL ES by %s\n", replaced,
	    sizeof(methods) / sizeof(methods[0]), redwood_gl_library());
}
