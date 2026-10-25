/* Detect unnecessary limiter activation on normal full-range media PCM. */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/hk_audio.h>
int main(int argc, char **argv)
{
    assert(argc == 2);
    unsigned gain = (unsigned)strtoul(argv[1], NULL, 10);
    uint64_t limited = 0, limited_after_start = 0;
    int16_t pcm[1920];
    hk_guard_configure(true, gain);
    hk_guard_attenuation(144);
    for (unsigned block = 0; block < 150; ++block) {
        for (unsigned i = 0; i < 960; ++i) {
            unsigned f = block * 960 + i;
            pcm[2*i] = (int16_t)lrint(29490 * sin(6.283185307179586 * 997 * f / 48000));
            pcm[2*i+1] = (int16_t)lrint(20000 * sin(6.283185307179586 * 440 * f / 48000));
        }
        hk_guard_process(pcm, 960);
        struct hk_guard_report r;
        hk_guard_take_report(&r);
        limited += r.limited_frames;
        if (block >= 10) limited_after_start += r.limited_frames;
    }
    printf("gain %.1f dB: %llu/144000 frames peak-limited\n", gain / 8.0, (unsigned long long)limited);
    fflush(stdout);
    if (gain == 0) assert(limited_after_start == 0);
    else assert(limited > 72000);
    return 0;
}
