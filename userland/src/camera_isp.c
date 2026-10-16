/*
 * The wide camera's frames made into pictures by the processor (see camera_isp.h). A first version:
 * one pixel per 2x2 cell (no interpolation), the nearest cell for each pixel of the picture, grey
 * world white balance, no colour matrix, no lens shading, no noise reduction.
 */
#include <math.h>
#include <string.h>
#include "camera_isp.h"

#define WHITE           959             /* 1023 less the black level of 64 */
#define LUT_WHITE       3836            /* WHITE in the gamma table's steps */

void
isp_init(struct isp *isp)
{
	memset(isp, 0, sizeof(*isp));
	isp->wb[0] = isp->wb[1] = isp->wb[2] = 1;
	isp->digital = 1;
	for (int i = 0; i < 4096; i++) {
		double v = i >= LUT_WHITE ? 1 : (double)i / LUT_WHITE;

		v = v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1 / 2.4) - 0.055;
		isp->gamma[i] = (uint8_t)(v * 255 + 0.5);
	}
}

/* where red and blue are in a cell (0 top left, 1 top right, 2 bottom left, 3 bottom right) */
static void
cell_places(uint32_t bayer, int *r, int *b)
{
	static const int red[4] = { 0, 1, 2, 3 }, blue[4] = { 3, 2, 1, 0 };

	*r = red[bayer & 3];
	*b = blue[bayer & 3];
}

/* the four values of the cell at cx of the pair of lines from `top' (above black, 0 and up) */
static inline void
cell(const uint8_t *top, size_t stride, uint32_t cx, int v[4])
{
	const uint8_t *g0 = top + (size_t)(cx >> 1) * 5, *g1 = g0 + stride;
	unsigned o = (cx & 1) * 2;

	v[0] = (g0[o] << 2 | (g0[4] >> (o * 2) & 3)) - 64;
	v[1] = (g0[o + 1] << 2 | (g0[4] >> (o * 2 + 2) & 3)) - 64;
	v[2] = (g1[o] << 2 | (g1[4] >> (o * 2) & 3)) - 64;
	v[3] = (g1[o + 1] << 2 | (g1[4] >> (o * 2 + 2) & 3)) - 64;
	for (int i = 0; i < 4; i++) {
		v[i] = v[i] < 0 ? 0 : v[i];
	}
}

void
isp_measure(const struct redwood_camera_frame *f, const uint8_t *raw, struct isp_stats *s)
{
	uint32_t cells_w = f->width / 2, cells_h = f->height / 2;
	double sum[3] = { 0, 0, 0 };
	int r, b;

	memset(s, 0, sizeof(*s));
	cell_places(f->bayer, &r, &b);
	for (uint32_t cy = 0; cy < cells_h; cy += 4) {
		const uint8_t *top = raw + (size_t)cy * 2 * f->stride;

		for (uint32_t cx = 0; cx < cells_w; cx += 4) {
			int v[4], g = 0;

			cell(top, f->stride, cx, v);
			for (int i = 0; i < 4; i++) {
				g += i != r && i != b ? v[i] : 0;
			}
			g /= 2;
			sum[0] += v[r];
			sum[1] += g;
			sum[2] += v[b];
			s->hist[g >> 4 > 63 ? 63 : g >> 4]++;
			s->cells++;
		}
	}
	for (int c = 0; c < 3; c++) {
		s->mean[c] = s->cells ? sum[c] / s->cells : 0;
	}
	for (int i = 63, above = 0; i >= 0; i--) {
		above += s->hist[i];
		if (above * 100 >= (int)s->cells) {
			s->bright = i * 16 + 8;
			break;
		}
	}
}

/*
 * Green's mean towards 15 % of white, unless that would put 1 % of the picture into white. The
 * exposure, longest first: the frame's lines at the shortest frame, then gain to 16x, then frames up
 * to max_frame_lines, then gain to the most; beyond that, digital gain (up to 4x).
 */
bool
isp_ae(struct isp *isp, const struct redwood_camera_frame *f, const struct isp_stats *s, struct redwood_camera_command *c)
{
	double now = (double)f->exposure_lines * f->gain / f->gain_unity * isp->digital, ratio, want;
	double target = 0.15 * WHITE, mean = s->mean[1] * isp->digital > 0.5 ? s->mean[1] * isp->digital : 0.5;
	double bright = s->bright * isp->digital;
	uint32_t fast = f->frame_lines_min - f->margin, longest = isp->max_frame_lines ? isp->max_frame_lines : 2 * f->frame_lines_min;
	uint32_t lines, gain, frame_lines = 0;
	double digital = 1;

	if (isp->asked && (f->exposure_lines != isp->asked_lines || f->gain != isp->asked_gain)) {
		return false;                   /* (the last change not in the frames yet) */
	}
	/* (the measured levels are the sensor's: digital gain is applied to the picture only) */
	ratio = target / mean;
	if (bright * ratio > 0.9 * WHITE && bright > 0) {
		ratio = 0.9 * WHITE / bright > target / mean * 0.5 ? 0.9 * WHITE / bright : target / mean * 0.5;
	}
	ratio = ratio < 0.25 ? 0.25 : ratio > 4 ? 4 : ratio;
	if (ratio > 0.94 && ratio < 1.06) {
		return false;
	}
	want = now * pow(ratio, 0.7);
	if (want <= fast) {
		lines = want < 4 ? 4 : (uint32_t)want;
		gain = f->gain_unity;
	} else if (want <= (double)fast * 16) {
		lines = fast;
		gain = (uint32_t)(want / fast * f->gain_unity);
	} else {
		lines = (uint32_t)(want / 16);
		lines = lines > longest - f->margin ? longest - f->margin : lines < fast ? fast : lines;
		frame_lines = lines + f->margin;
		double g = want / lines;

		if (g > (double)f->gain_max / f->gain_unity) {
			digital = g / ((double)f->gain_max / f->gain_unity);
			digital = digital > 4 ? 4 : digital;
			g = (double)f->gain_max / f->gain_unity;
		}
		gain = (uint32_t)(g * f->gain_unity);
	}
	isp->digital = (float)digital;
	isp->asked_lines = lines;
	isp->asked_gain = gain;
	isp->asked_frame_lines = frame_lines;
	isp->asked = true;
	memset(c, 0, sizeof(*c));
	c->command = REDWOOD_CAMERA_EXPOSURE;
	c->a = lines;
	c->b = gain;
	c->c = frame_lines;
	return true;
}

void
isp_awb(struct isp *isp, const struct isp_stats *s)
{
	if (s->mean[0] < 2 || s->mean[2] < 2 || s->mean[1] < 2) {
		return;
	}
	float r = (float)(s->mean[1] / s->mean[0]), b = (float)(s->mean[1] / s->mean[2]);

	r = r < 0.5f ? 0.5f : r > 4 ? 4 : r;
	b = b < 0.5f ? 0.5f : b > 4 ? 4 : b;
	isp->wb[0] = isp->wb[0] * 0.8f + r * 0.2f;
	isp->wb[2] = isp->wb[2] * 0.8f + b * 0.2f;
}

/* the part of the sensor's cells the picture shows: its middle, in the picture's proportions */
static void
crop(const struct redwood_camera_frame *f, uint32_t width, uint32_t height, uint32_t *x0, uint32_t *y0, uint32_t *cw, uint32_t *ch)
{
	uint32_t w = f->width / 2, h = f->height / 2;

	if ((uint64_t)width * h > (uint64_t)height * w) {
		*cw = w;
		*ch = (uint32_t)((uint64_t)w * height / width);
	} else {
		*ch = h;
		*cw = (uint32_t)((uint64_t)h * width / height);
	}
	*x0 = (w - *cw) / 2;
	*y0 = (h - *ch) / 2;
}

/* one line of the picture as 8-bit R, G, B */
static void
render_line(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, uint32_t width, uint32_t height,
    uint32_t row, uint8_t *rgb)
{
	uint32_t x0, y0, cw, ch, k[3];
	int r, b;

	crop(f, width, height, &x0, &y0, &cw, &ch);
	cell_places(f->bayer, &r, &b);
	for (int c = 0; c < 3; c++) {
		k[c] = (uint32_t)(isp->wb[c] * isp->digital * 4 * 256);
	}
	const uint8_t *top = raw + (size_t)(y0 + (uint64_t)row * ch / height) * 2 * f->stride;
	uint64_t step = ((uint64_t)cw << 16) / width, at = (uint64_t)x0 << 16;

	for (uint32_t x = 0; x < width; x++, at += step) {
		int v[4], g = 0;
		uint32_t i;

		cell(top, f->stride, (uint32_t)(at >> 16), v);
		for (int j = 0; j < 4; j++) {
			g += j != r && j != b ? v[j] : 0;
		}
		i = (uint32_t)v[r] * k[0] >> 8;
		rgb[3 * x] = isp->gamma[i > 4095 ? 4095 : i];
		i = (uint32_t)g * k[1] >> 9;
		rgb[3 * x + 1] = isp->gamma[i > 4095 ? 4095 : i];
		i = (uint32_t)v[b] * k[2] >> 8;
		rgb[3 * x + 2] = isp->gamma[i > 4095 ? 4095 : i];
	}
}

void
isp_render_nv12(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, uint8_t *y, size_t y_stride,
    uint8_t *uv, size_t uv_stride, uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row)
{
	uint8_t line[2][3 * 8192];

	if (width > 8192) {
		return;
	}
	for (uint32_t row = first_row & ~1u; row < end_row && row + 1 < height + 1; row += 2) {
		render_line(isp, f, raw, width, height, row, line[0]);
		render_line(isp, f, raw, width, height, row + 1 < height ? row + 1 : row, line[1]);
		for (int l = 0; l < 2 && row + l < height; l++) {
			uint8_t *out = y + (size_t)(row + l) * y_stride;
			const uint8_t *p = line[l];

			for (uint32_t x = 0; x < width; x++, p += 3) {
				out[x] = (uint8_t)((77 * p[0] + 150 * p[1] + 29 * p[2]) >> 8);
			}
		}
		uint8_t *o = uv + (size_t)(row / 2) * uv_stride;

		for (uint32_t x = 0; x + 1 < width + 1; x += 2) {
			uint32_t x1 = x + 1 < width ? x + 1 : x;
			int R = line[0][3 * x] + line[0][3 * x1] + line[1][3 * x] + line[1][3 * x1];
			int G = line[0][3 * x + 1] + line[0][3 * x1 + 1] + line[1][3 * x + 1] + line[1][3 * x1 + 1];
			int B = line[0][3 * x + 2] + line[0][3 * x1 + 2] + line[1][3 * x + 2] + line[1][3 * x1 + 2];
			int cb = ((-43 * R - 85 * G + 128 * B) >> 10) + 128, cr = ((128 * R - 107 * G - 21 * B) >> 10) + 128;

			o[x] = (uint8_t)(cb < 0 ? 0 : cb > 255 ? 255 : cb);
			o[x + 1] = (uint8_t)(cr < 0 ? 0 : cr > 255 ? 255 : cr);
		}
	}
}

void
isp_render_bgra(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, uint8_t *out, size_t stride,
    uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row)
{
	uint8_t line[3 * 8192];

	if (width > 8192) {
		return;
	}
	for (uint32_t row = first_row; row < end_row && row < height; row++) {
		uint8_t *o = out + (size_t)row * stride;

		render_line(isp, f, raw, width, height, row, line);
		for (uint32_t x = 0; x < width; x++) {
			o[4 * x] = line[3 * x + 2];
			o[4 * x + 1] = line[3 * x + 1];
			o[4 * x + 2] = line[3 * x];
			o[4 * x + 3] = 255;
		}
	}
}
