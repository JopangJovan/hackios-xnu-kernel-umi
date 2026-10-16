/*
 * What the phone's ISP would do with the wide camera's RAW10 frames, done by the processor instead
 * (src/camera_isp.c): the levels of a frame, the exposure for the next ones (AE), the white balance
 * (AWB), and a picture: each 2x2 Bayer cell one pixel of colour, scaled to the size asked for, the
 * gains, a gamma, as 8-bit YCbCr 4:2:0 (NV12, full range: CoreVideo's 420f) or BGRA.
 */
#ifndef CAMERA_ISP_H
#define CAMERA_ISP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "redwood_camera.h"

struct isp_stats {
	double          mean[3];        /* R, G, B above black, in 10-bit steps */
	double          bright;         /* the level of green that 1 % of the cells are above */
	uint32_t        hist[64];       /* green above black, 16 steps a bin */
	uint32_t        cells;
};

struct isp {
	float           wb[3];          /* the white balance's gains, green 1 */
	float           digital;        /* more gain, for what AE cannot reach with the sensor's */
	uint8_t         gamma[4096];    /* linear (959 above black is 3836) to 8 bits, sRGB's curve */
	uint32_t        asked_lines, asked_gain, asked_frame_lines;     /* what AE asked for last */
	bool            asked;
	uint32_t        max_frame_lines;        /* the longest frame AE may ask for (0: twice the shortest) */
};

void isp_init(struct isp *isp);
/* the levels, from every 4th cell of every 4th pair of lines */
void isp_measure(const struct redwood_camera_frame *f, const uint8_t *raw, struct isp_stats *s);
/* the exposure for the frames to come: true, and the command filled in, when it should change */
bool isp_ae(struct isp *isp, const struct redwood_camera_frame *f, const struct isp_stats *s, struct redwood_camera_command *c);
/* the white balance: grey world, a little of the way at a time */
void isp_awb(struct isp *isp, const struct isp_stats *s);
/*
 * A picture of the frame, width x height, of the sensor's middle part with the picture's proportions
 * (rows from first_row to end_row only, for doing it in pieces). NV12: y and uv planes, full range (420f) or video
 * range (420v); BGRA: y only.
 */
void isp_render_nv12(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, uint8_t *y, size_t y_stride,
    uint8_t *uv, size_t uv_stride, uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row, bool video_range);
void isp_render_bgra(const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw, uint8_t *out, size_t stride,
    uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row);

#endif
