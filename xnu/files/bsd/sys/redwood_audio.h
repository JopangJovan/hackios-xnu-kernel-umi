/*
 * /dev/redwood-audio: the sound iOS plays, on its way to whatever can make it heard.
 *
 * iOS expects its output devices from Apple's audio drivers through IOAudio2; neither exists
 * here. Instead the audio server (mediaserverd) is given a plug-in of ours that presents one
 * stereo output device, and the plug-in writes what the server has mixed for that device
 * here. In the kernel the samples go to a sink -- a driver for something that plays them --
 * and, if asked for, into a recording that outlives the kernel (see dev/redwood_audio.c).
 *
 *   write(): whole frames in the format below. Never blocks: the writer is the audio server's
 *            real-time thread, which paces itself (48000 frames a second while anything
 *            plays, nothing at all otherwise). What the sink cannot take is dropped.
 */
#ifndef _SYS_REDWOOD_AUDIO_H_
#define _SYS_REDWOOD_AUDIO_H_

#include <stdint.h>

#define REDWOOD_AUDIO_DEVICE    "/dev/redwood-audio"

/* The one format: 16-bit signed little-endian samples, left then right. */
#define REDWOOD_AUDIO_RATE      48000
#define REDWOOD_AUDIO_CHANNELS  2
#define REDWOOD_AUDIO_FRAME     4       /* bytes */

#ifdef KERNEL
/*
 * For drivers. A sink is offered frames as they are written and says how many it took (the
 * rest are lost); it is called from the writer's thread and must not wait.
 */
typedef uint32_t (*redwood_audio_sink_t)(const int16_t *frames, uint32_t count);

void redwood_audio_set_sink(const char *name, redwood_audio_sink_t sink);
void redwood_audio_init(void);
void redwood_virtio_sound_init(void);   /* boot-arg virtio_sound=1: QEMU's virtio sound device */
void redwood_speaker_init(void);        /* boot-arg speaker_probe=N: a look at the phone's speaker amplifiers */
#endif

#endif /* _SYS_REDWOOD_AUDIO_H_ */
