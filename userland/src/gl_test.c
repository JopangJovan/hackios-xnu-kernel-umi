/*
 * gl_test: one triangle through the OpenGL ES library of the image (lib/libOSMesa.8.dylib: Mesa,
 * with its Adreno driver when the kernel has kept the GPU on -- boot-arg adreno_keep=1 -- and its
 * software one when not), read back and put on the panel. It says which driver drew, how long
 * it took and what a few pixels are, so that the picture need not be seen to be believed.
 *
 *   gl_test [SECONDS [WIDTH HEIGHT]]   keep the picture on the panel for so long (default 6: the
 *                                      window server draws over it with its next frame)
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* GL/osmesa.h and GLES2/gl2.h of Mesa, as far as they are used here */
typedef struct osmesa_context *OSMesaContext;
extern OSMesaContext OSMesaCreateContextAttribs(const int *attributes, OSMesaContext share);
extern unsigned char OSMesaMakeCurrent(OSMesaContext context, void *buffer, unsigned type, int width, int height);

extern const unsigned char *glGetString(unsigned name);
extern unsigned glGetError(void);
extern void glGenTextures(int n, unsigned *names);
extern void glBindTexture(unsigned target, unsigned texture);
extern void glTexImage2D(unsigned target, int level, int internal, int width, int height, int border, unsigned format,
    unsigned type, const void *pixels);
extern void glTexParameteri(unsigned target, unsigned name, int value);
extern void glGenFramebuffers(int n, unsigned *names);
extern void glBindFramebuffer(unsigned target, unsigned framebuffer);
extern void glFramebufferTexture2D(unsigned target, unsigned attachment, unsigned textarget, unsigned texture, int level);
extern unsigned glCheckFramebufferStatus(unsigned target);
extern void glViewport(int x, int y, int width, int height);
extern void glClearColor(float red, float green, float blue, float alpha);
extern void glClear(unsigned mask);
extern unsigned glCreateShader(unsigned type);
extern void glShaderSource(unsigned shader, int count, const char *const *strings, const int *lengths);
extern void glCompileShader(unsigned shader);
extern void glGetShaderiv(unsigned shader, unsigned name, int *value);
extern unsigned glCreateProgram(void);
extern void glAttachShader(unsigned program, unsigned shader);
extern void glBindAttribLocation(unsigned program, unsigned index, const char *name);
extern void glLinkProgram(unsigned program);
extern void glGetProgramiv(unsigned program, unsigned name, int *value);
extern void glUseProgram(unsigned program);
extern void glVertexAttribPointer(unsigned index, int size, unsigned type, unsigned char normalized, int stride,
    const void *pointer);
extern void glEnableVertexAttribArray(unsigned index);
extern void glDrawArrays(unsigned mode, int first, int count);
extern void glFinish(void);
extern void glReadPixels(int x, int y, int width, int height, unsigned format, unsigned type, void *pixels);

#define OSMESA_RGBA                     0x1908
#define OSMESA_FORMAT                   0x22
#define OSMESA_PROFILE                  0x33
#define OSMESA_CONTEXT_MAJOR_VERSION    0x36
#define OSMESA_ES_PROFILE               0x38
#define GL_TRIANGLES                    0x0004
#define GL_TEXTURE_2D                   0x0de1
#define GL_UNSIGNED_BYTE                0x1401
#define GL_FLOAT                        0x1406
#define GL_RGBA                         0x1908
#define GL_VENDOR                       0x1f00
#define GL_RENDERER                     0x1f01
#define GL_VERSION                      0x1f02
#define GL_NEAREST                      0x2600
#define GL_TEXTURE_MAG_FILTER           0x2800
#define GL_TEXTURE_MIN_FILTER           0x2801
#define GL_COLOR_BUFFER_BIT             0x4000
#define GL_FRAGMENT_SHADER              0x8b30
#define GL_VERTEX_SHADER                0x8b31
#define GL_COMPILE_STATUS               0x8b81
#define GL_LINK_STATUS                  0x8b82
#define GL_FRAMEBUFFER_COMPLETE         0x8cd5
#define GL_COLOR_ATTACHMENT0            0x8ce0
#define GL_FRAMEBUFFER                  0x8d40

/* the kernel's GPU service takes pixels for the panel (iokit/Kernel/RedwoodGPU.cpp: kRedwoodShow) */
struct show {
	uint64_t pixels;
	uint32_t width, height, row_bytes, pad;
};

static uint64_t
now_ms(void)
{
	static mach_timebase_info_data_t base;

	if (base.denom == 0) {
		mach_timebase_info(&base);
	}
	return mach_absolute_time() * base.numer / base.denom / 1000000;
}

static unsigned
shader(unsigned type, const char *text)
{
	unsigned name = glCreateShader(type);
	int good = 0;

	glShaderSource(name, 1, &text, NULL);
	glCompileShader(name);
	glGetShaderiv(name, GL_COMPILE_STATUS, &good);
	if (!good) {
		printf("gl_test: a shader does not compile\n");
	}
	return name;
}

int
main(int argc, char **argv)
{
	static const int attributes[] = { OSMESA_FORMAT, OSMESA_RGBA, OSMESA_PROFILE, OSMESA_ES_PROFILE,
		                          OSMESA_CONTEXT_MAJOR_VERSION, 2, 0 };
	static const float corners[] = {
		/* where, and what colour */
		-0.8f, -0.6f, 1, 0, 0,
		0.8f, -0.6f, 0, 1, 0,
		0.0f, 0.7f, 0, 0, 1,
	};
	static unsigned char window[16 * 16 * 4];
	int seconds = argc > 1 ? atoi(argv[1]) : 6, width = argc > 3 ? atoi(argv[2]) : 1080;
	int height = argc > 3 ? atoi(argv[3]) : 2400;
	unsigned texture = 0, framebuffer = 0, program, coloured = 0;
	uint8_t *drawn, *shown;
	uint64_t began, drew, read;
	io_service_t service;
	io_connect_t connection = 0;
	OSMesaContext context;
	int good = 0;

	context = OSMesaCreateContextAttribs(attributes, NULL);
	if (context == NULL || !OSMesaMakeCurrent(context, window, GL_UNSIGNED_BYTE, 16, 16)) {
		printf("gl_test: no OpenGL ES context\n");
		return 1;
	}
	printf("gl_test: %s; %s; %s\n", (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER),
	    (const char *)glGetString(GL_VERSION));

	began = now_ms();
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		printf("gl_test: the framebuffer is not complete\n");
		return 1;
	}
	program = glCreateProgram();
	glAttachShader(program, shader(GL_VERTEX_SHADER,
	    "attribute vec2 where; attribute vec3 colour; varying vec3 c;\n"
	    "void main() { gl_Position = vec4(where, 0.0, 1.0); c = colour; }\n"));
	glAttachShader(program, shader(GL_FRAGMENT_SHADER,
	    "precision mediump float; varying vec3 c;\n"
	    "void main() { gl_FragColor = vec4(c, 1.0); }\n"));
	glBindAttribLocation(program, 0, "where");
	glBindAttribLocation(program, 1, "colour");
	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &good);
	if (!good) {
		printf("gl_test: the program does not link\n");
		return 1;
	}
	glUseProgram(program);
	glViewport(0, 0, width, height);
	glClearColor(0.1f, 0.1f, 0.2f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glVertexAttribPointer(0, 2, GL_FLOAT, 0, 5 * sizeof(float), corners);
	glVertexAttribPointer(1, 3, GL_FLOAT, 0, 5 * sizeof(float), corners + 2);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glFinish();
	drew = now_ms();

	drawn = malloc((size_t)width * (size_t)height * 4);
	shown = malloc((size_t)width * (size_t)height * 4);
	if (drawn == NULL || shown == NULL) {
		return 1;
	}
	memset(drawn, 0x55, (size_t)width * (size_t)height * 4);
	glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, drawn);
	read = now_ms();
	/* the panel's rows go down and its pixels are blue first */
	for (int y = 0; y < height; y++) {
		const uint8_t *from = drawn + (size_t)(height - 1 - y) * (size_t)width * 4;
		uint8_t *to = shown + (size_t)y * (size_t)width * 4;

		for (int x = 0; x < width; x++) {
			to[4 * x] = from[4 * x + 2];
			to[4 * x + 1] = from[4 * x + 1];
			to[4 * x + 2] = from[4 * x];
			to[4 * x + 3] = 255;
			coloured += from[4 * x] > 60 || from[4 * x + 1] > 60 || from[4 * x + 2] > 60;
		}
	}
#define PIXEL(x, y) drawn[((size_t)(y) * (size_t)width + (size_t)(x)) * 4], drawn[((size_t)(y) * (size_t)width + (size_t)(x)) * 4 + 1], \
	    drawn[((size_t)(y) * (size_t)width + (size_t)(x)) * 4 + 2], drawn[((size_t)(y) * (size_t)width + (size_t)(x)) * 4 + 3]
	printf("gl_test: %d x %d drawn in %llu ms, read back in %llu ms; error %#x; red green blue alpha: a corner %u %u %u %u, "
	    "the middle %u %u %u %u, near the red corner %u %u %u %u, near the green %u %u %u %u, near the blue %u %u %u %u; "
	    "%u of %u pixels inside the triangle (%u%%; 26%% by geometry)\n", width, height, drew - began, read - drew,
	    glGetError(), PIXEL(2, 2), PIXEL(width / 2, height / 2), PIXEL(width * 15 / 100, height * 22 / 100),
	    PIXEL(width * 85 / 100, height * 22 / 100), PIXEL(width / 2, height * 80 / 100), coloured,
	    (unsigned)(width * height), (unsigned)((uint64_t)coloured * 100 / (uint64_t)(width * height)));

	service = IOServiceGetMatchingService(0, IOServiceMatching("RedwoodGPU"));
	if (service == 0 || IOServiceOpen(service, mach_task_self(), 0, &connection) != 0) {
		printf("gl_test: no GPU service in this kernel to put the picture on the panel with\n");
		return 0;
	}
	for (int i = 0; i < seconds * 5; i++) {
		struct show show = { (uint64_t)(uintptr_t)shown, (uint32_t)width, (uint32_t)height, (uint32_t)width * 4, 0 };
		kern_return_t result = IOConnectCallStructMethod(connection, 0x7f, &show, sizeof(show), NULL, NULL);

		if (i == 0) {
			printf("gl_test: the picture is on the panel (%#x), for %d seconds\n", result, seconds);
		}
		usleep(200000);
	}
	return 0;
}
