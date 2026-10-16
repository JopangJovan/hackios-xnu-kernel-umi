/*
 * camera_test: the wide camera's frames as a program gets them (cam=5).
 *
 * usage: camera_test raw [FRAMES] [noae] [keepraw] [sensor N]
 *        camera_test cycle [FRAMES]
 *   raw: reads FRAMES frames (default 150) from the kernel's /dev/redwood-camera, with src/camera_isp.c's
 *   exposure (unless noae: the kernel's, boot-args cam_shutter and cam_gain) and white balance working
 *   on them, says what came and how fast, and leaves the last one as a picture for the host (xl
 *   camera: logs/, never the repository): one pixel a cell, NV12, or with keepraw the frame itself.
 *   With sensor N, another sensor first (0 the macro, 1 the wide, 2 the front, 3 the ultra-wide).
 *   cycle: each of the four sensors in turn, FRAMES frames each (default 30), the kernel's exposure:
 *   what came of each, and how long the change from one to the next took.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "camera_isp.h"

static double
ms_since(uint64_t then)
{
	static mach_timebase_info_data_t tb;

	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return (double)(mach_absolute_time() - then) * tb.numer / tb.denom / 1e6;
}

static double
ticks_ms(uint64_t ticks)
{
	static mach_timebase_info_data_t tb;

	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return (double)ticks * tb.numer / tb.denom / 1e6;
}

static const char *const sensor_names[4] = { "macro", "wide", "front", "ultra-wide" };

/* REDWOOD_CAMERA_SENSOR: the kernel changes to that sensor (its usual mode) before the next frames */
static bool
choose_sensor(int device, unsigned n)
{
	struct redwood_camera_command c = { .command = REDWOOD_CAMERA_SENSOR, .a = n, .b = ~0u };

	if (write(device, &c, sizeof(c)) != (ssize_t)sizeof(c)) {
		printf("camera_test: the %s camera cannot be chosen: %s\n", n < 4 ? sensor_names[n] : "?", strerror(errno));
		return false;
	}
	return true;
}

static int
cycle(int frames)
{
	int device = open(REDWOOD_CAMERA_DEVICE, O_RDWR), failures = 0;
	uint8_t *buffer = NULL;
	size_t size = 0;

	if (device < 0) {
		perror("camera_test: " REDWOOD_CAMERA_DEVICE);
		return 1;
	}
	for (unsigned n = 0; n < 4; n++) {
		struct redwood_camera_frame head, *f = &head;
		uint64_t asked = mach_absolute_time(), first = 0, last_seq = 0, first_seq = 0;
		int got = 0;
		ssize_t r;

		if (!choose_sensor(device, n)) {
			failures++;
			continue;
		}
		for (int tries = 0; (r = read(device, &head, sizeof(head))) < 0 && errno == EAGAIN && tries < 3; tries++) {
		}
		if (r != (ssize_t)sizeof(head) || memcmp(head.magic, REDWOOD_CAMERA_MAGIC, 8) != 0 || head.sensor != n) {
			printf("camera_test: the %s camera: no frames (%s, header of sensor %u)\n", sensor_names[n],
			    r < 0 ? strerror(errno) : "short", r == (ssize_t)sizeof(head) ? head.sensor : 99);
			failures++;
			if (r < 0 && errno == EIO) {
				break;
			}
			continue;
		}
		size_t want = sizeof(head) + (size_t)head.stride * head.height;

		if (want > size) {
			free(buffer);
			buffer = malloc(want);
			size = want;
		}
		double changed_ms = ms_since(asked);
		struct isp_stats s;

		for (int i = 0; i < frames; i++) {
			r = read(device, buffer, want);
			if (r != (ssize_t)want) {
				printf("camera_test: the %s camera: a read gave %zd of %zu bytes (%s)\n", sensor_names[n], r, want,
				    r < 0 ? strerror(errno) : "short");
				break;
			}
			f = (struct redwood_camera_frame *)buffer;
			got++;
			first = first ? first : f->time;
			first_seq = first_seq ? first_seq : f->sequence;
			last_seq = f->sequence;
		}
		if (got == 0) {
			failures++;
			continue;
		}
		isp_measure(f, buffer + f->header_bytes, &s);
		printf("camera_test: the %s camera (sensor %u): %ux%u, stride %u, Bayer %u; the first frame %.0f ms after it was asked "
		    "for; %d frames of %llu in %.2f s (%.1f a second); R %.1f G %.1f B %.1f above black\n", sensor_names[n], f->sensor,
		    f->width, f->height, f->stride, f->bayer, changed_ms, got, last_seq - first_seq + 1, ticks_ms(f->time - first) / 1000,
		    got > 1 ? (got - 1) * 1000.0 / ticks_ms(f->time - first) : 0.0, s.mean[0], s.mean[1], s.mean[2]);
	}
	free(buffer);
	close(device);
	return failures == 0 ? 0 : 1;
}

static int
raw(int frames, bool ae, bool keep_raw, int sensor)
{
	struct redwood_camera_frame head, *f;
	struct isp isp;
	struct isp_stats s;
	uint64_t started = mach_absolute_time(), previous = 0, first_seq = 0;
	size_t bytes;
	int device = open(REDWOOD_CAMERA_DEVICE, O_RDWR), got = 0, failures = 0;
	ssize_t n;

	if (device < 0) {
		perror("camera_test: " REDWOOD_CAMERA_DEVICE);
		return 1;
	}
	if (sensor >= 0 && !choose_sensor(device, (unsigned)sensor)) {
		return 1;
	}
	for (int tries = 0; (n = read(device, &head, sizeof(head))) < 0 && errno == EAGAIN && tries < 3; tries++) {
	}
	if (n != (ssize_t)sizeof(head) || memcmp(head.magic, REDWOOD_CAMERA_MAGIC, 8) != 0) {
		perror("camera_test: the first frame's header");
		return 1;
	}
	printf("camera_test: the first frame after %.0f ms: %ux%u, stride %u, Bayer %u, black %u, %u lines of %u ns a frame "
	    "(%.2f frames/s at the shortest), exposure %u lines, gain %u (%u is 1x)\n", ms_since(started), head.width, head.height,
	    head.stride, head.bayer, head.black, head.frame_lines_min, head.line_ns, 1e9 / ((double)head.frame_lines_min * head.line_ns),
	    head.exposure_lines, head.gain, head.gain_unity);
	bytes = sizeof(head) + (size_t)head.stride * head.height;
	f = malloc(bytes);
	isp_init(&isp);
	while (got < frames && failures < 5) {
		struct redwood_camera_command c;
		uint64_t t0 = mach_absolute_time();

		n = read(device, f, bytes);
		if (n != (ssize_t)bytes) {
			printf("camera_test: a read gave %zd of %zu bytes (%s)\n", n, bytes, n < 0 ? strerror(errno) : "short");
			failures++;
			continue;
		}
		double read_ms = ms_since(t0);
		const uint8_t *raw = (const uint8_t *)f + f->header_bytes;

		got++;
		first_seq = first_seq ? first_seq : f->sequence;
		isp_measure(f, raw, &s);
		isp_awb(&isp, &s);
		if (ae && isp_ae(&isp, f, &s, &c) && write(device, &c, sizeof(c)) != (ssize_t)sizeof(c)) {
			perror("camera_test: the exposure");
		}
		if (got <= 3 || got % 15 == 0) {
			printf("camera_test: frame %llu (%d read): %.1f ms after the last, read in %.1f ms; exposure %.2f ms (%u lines), gain "
			    "%.2fx, digital %.2fx, frame %.1f ms; R %.1f G %.1f B %.1f above black, 1 %% above %.0f; wb %.2f %.2f\n",
			    f->sequence, got, previous ? ticks_ms(f->time - previous) : 0.0, read_ms,
			    f->exposure_lines * (double)f->line_ns / 1e6, f->exposure_lines, (double)f->gain / f->gain_unity, isp.digital,
			    f->frame_lines * (double)f->line_ns / 1e6, s.mean[0], s.mean[1], s.mean[2], s.bright, isp.wb[0], isp.wb[2]);
		}
		previous = f->time;
	}
	if (got == 0) {
		return 1;
	}
	printf("camera_test: %d frames of %llu (%llu skipped) in %.1f s\n", got, f->sequence - first_seq + 1,
	    f->sequence - first_seq + 1 - got, ms_since(started) / 1000);

	if (keep_raw) {
		struct redwood_camera_command *k = (struct redwood_camera_command *)((uint8_t *)f + f->header_bytes) - 1;
		uint32_t frame_bytes = f->stride * f->height, w = f->width, h = f->height, stride = f->stride;

		/* (the command goes over the end of the header, just before the frame's bytes) */
		memset(k, 0, sizeof(*k));
		k->command = REDWOOD_CAMERA_KEEP;
		k->bytes = frame_bytes;
		k->a = w;
		k->b = h;
		k->c = stride;
		k->d = 1;
		if (write(device, k, sizeof(*k) + frame_bytes) != (ssize_t)(sizeof(*k) + frame_bytes)) {
			perror("camera_test: the frame for the host");
		} else {
			printf("camera_test: the frame (RAW10) is the host's (xl camera)\n");
		}
		close(device);
		return 0;
	}
	/* the last one, as a picture for the host: one pixel per cell, NV12 */
	uint32_t w = f->width / 2, h = f->height / 2;
	size_t picture = (size_t)w * h * 3 / 2;
	struct redwood_camera_command *keep = malloc(sizeof(*keep) + picture);
	uint8_t *y = (uint8_t *)(keep + 1);
	uint64_t t0 = mach_absolute_time();

	isp_render_nv12(&isp, f, (const uint8_t *)f + f->header_bytes, y, w, y + (size_t)w * h, w, w, h, 0, h, false);
	printf("camera_test: the picture (%ux%u NV12) made in %.1f ms\n", w, h, ms_since(t0));
	memset(keep, 0, sizeof(*keep));
	keep->command = REDWOOD_CAMERA_KEEP;
	keep->bytes = (uint32_t)picture;
	keep->a = w;
	keep->b = h;
	keep->c = w;
	keep->d = 2;
	if (write(device, keep, sizeof(*keep) + picture) != (ssize_t)(sizeof(*keep) + picture)) {
		perror("camera_test: the picture for the host");
	} else {
		printf("camera_test: the picture is the host's (xl camera)\n");
	}
	close(device);
	return 0;
}

int
main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc >= 2 && strcmp(argv[1], "raw") == 0) {
		bool ae = true, keep_raw = false;
		int sensor = -1;

		for (int i = 3; i < argc; i++) {
			ae = ae && strcmp(argv[i], "noae") != 0;
			keep_raw = keep_raw || strcmp(argv[i], "keepraw") == 0;
			if (strcmp(argv[i], "sensor") == 0 && i + 1 < argc) {
				sensor = atoi(argv[++i]);
			}
		}
		return raw(argc >= 3 ? atoi(argv[2]) : 150, ae, keep_raw, sensor);
	}
	if (argc >= 2 && strcmp(argv[1], "cycle") == 0) {
		return cycle(argc >= 3 ? atoi(argv[2]) : 30);
	}
	fprintf(stderr, "usage: camera_test raw [FRAMES] [noae] [keepraw] [sensor N]\n       camera_test cycle [FRAMES]\n");
	return 2;
}
