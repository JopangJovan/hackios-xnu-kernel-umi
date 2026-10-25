/* Exercise the real register conversion, including attenuation that previously wrapped. */
#include <assert.h>
#include <stdint.h>
#include "../../xnu/files/bsd/dev/umi_cs35l41_audio.h"
static void check(uint32_t attenuation)
{
    uint32_t reg = umi_cs35l41_volume_register(attenuation);
    int32_t code = (reg >> 3) & 0x7ff;
    if (code & 0x400) code -= 2048;
    uint32_t expected = attenuation > 816 ? 816 : attenuation;
    assert(code == -(int32_t)expected);
    assert((reg & 7) == 4); /* Stock four millisecond soft ramp. */
    assert((reg & ~0x3fffu) == 0);
}
int main(void)
{
    for (uint32_t i = 0; i <= 4096; ++i) check(i);
    check(UINT32_MAX);
    assert(umi_cs35l41_volume_register(240) == 0x3884); /* -30 dB */
    assert(umi_cs35l41_volume_register(144) == 0x3b84); /* -18 dB */
    return 0;
}
