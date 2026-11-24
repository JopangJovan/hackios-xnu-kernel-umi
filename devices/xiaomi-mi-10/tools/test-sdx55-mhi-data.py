#!/usr/bin/env python3
"""Run the bounded IP_HW0 client against an in-memory MHI controller."""
from pathlib import Path
import subprocess
import tempfile

bsd = Path(__file__).resolve().parents[1] / 'overlays/xnu/files/bsd'
source = (bsd / 'dev/umi_mhi.c').read_text().replace(
    '#include <libkern/libkern.h>', '#include <stdio.h>\n#include <string.h>')
source = source.replace('#include <sys/systm.h>', '')
model = r'''
#include <assert.h>

static struct umi_mhi h;
static uint32_t channels[111 * 11], events[17 * 11], commands[11], cr[128 * 4];
static uint32_t er[17][128 * 4];
static uint8_t arena[128 * 1024] __attribute__((aligned(4096)));
static int calls, starts, bells, fail_channel, fail_allocation;
static unsigned satellite_opcode, satellite_commands;
static unsigned diag_starts, diag_bells;
static size_t allocated;
static unsigned produced[17];

static int alloc_model(void *unused, size_t n, struct umi_mhi_dma *dma)
{
    if (++calls == fail_allocation) return ENOMEM;
    size_t bytes = (n + 4095) & ~(size_t)4095;
    assert(bytes <= sizeof(arena) - allocated);
    *dma = (struct umi_mhi_dma){ arena + allocated, 0x20100000u + allocated, bytes };
    allocated += bytes;
    return 0;
}

static void event(unsigned ring, uint64_t pointer, uint32_t len, uint32_t control)
{
    unsigned slot = produced[ring]++ % 128;
    uint32_t *entry = er[ring] + slot * 4;
    words_set_address(entry, 0, pointer);
    entry[2] = len;
    entry[3] = control;
    words_set_address(events + ring * 11, 7, h.event_ring[ring].address + produced[ring] % 128 * 16);
}

static uint32_t reg_read(void *unused, uint32_t reg)
{
    return reg == 0x128 ? 2 : 0x200;
}

static const uint8_t frame[] = {0, 1, 0, 4, 0x45, 0, 0, 4};
static uint8_t captured[32768];
static unsigned deliveries, captured_len, tx_bells, tx_mode, fake_tx_slot;
static void deliver(void *unused, const void *data, size_t length)
{
    assert(length <= sizeof(captured));
    memcpy(captured, data, length);
    captured_len = length;
    ++deliveries;
}
static void delay_model(void *unused, unsigned us)
{
    if (!tx_mode || tx_mode == 5) return;
    uint32_t *ctx = channels + 100*11;
    uint64_t base = words_address(ctx, 3), ptr = base + fake_tx_slot*16;
    if (!base || words_address(ctx, 9) != base + ((fake_tx_slot+1)%512)*16) return;
    uint32_t *tre = (uint32_t *)(arena + (ptr - 0x20100000u));
    assert(tre[2] == sizeof(frame) && tre[3] == ((2u<<16)|(1u<<9)));
    uint64_t payload = words_address(tre, 0);
    assert(payload >= 0x20100000 && payload + sizeof(frame) <= 0x20100000u + allocated);
    assert(!memcmp(arena + (payload - 0x20100000u), frame, sizeof(frame)));
    if (tx_mode == 6) {
        event(7, 0, 6u<<24, (100u<<24)|(0x22u<<16)); /* pending DB-mode request */
        tx_mode = 1;
        return;
    }
    uint32_t cca = tx_mode == 4 ? 4 : 2;
    event(7, tx_mode == 2 ? ptr + 16 : ptr,
          (cca<<24) | (tx_mode == 3 ? sizeof(frame)-1 : sizeof(frame)), (100u<<24)|(0x22u<<16));
    fake_tx_slot = (fake_tx_slot+1)%512;
}

static void reg_write(void *unused, uint32_t reg, uint32_t value)
{
    if (reg == 0x70) {
        uint32_t descriptor = cr[(h.command_wp - h.command_ring.address) / 4 + 3];
        int channel = descriptor >> 24;
        if (channel == DIAG_TX_CHANNEL || channel == DIAG_RX_CHANNEL) {
            assert(channel == DIAG_TX_CHANNEL + diag_starts++);
            event(0, h.command_wp, 1u << 24, 0x21u << 16);
            return;
        }
        if (channel == 50) {
            assert((descriptor >> 16 & 0xff) == satellite_opcode);
            assert(channels[50 * 11 + 1] == 0xa && events[4 * 11 + 1] == 0xa);
            ++satellite_commands;
            event(0, h.command_wp, (channel == fail_channel ? 0x11u : 1u) << 24, 0x21u << 16);
            return;
        }
        assert(channel == 100 + starts);
        uint32_t *ctx = channels + channel * 11;
        assert(ctx[0] == 0x301 && ctx[1] == (channel == 100 ? 1 : 2));
        assert(ctx[2] == (channel == 100 ? 7 : 8));
        assert(words_address(ctx, 3) == words_address(ctx, 9)); /* empty at START */
        ++starts;
        event(0, h.command_wp, (channel == fail_channel ? 0x11u : 1u) << 24, 0x21u << 16);
    } else if (reg == 0x300 + 101 * 8) {
        assert(starts == 2);
        ++bells;
    } else if (reg == 0x300 + 100 * 8) {
        assert(starts == 2 && tx_mode != 0); /* no UL until a real submit */
        ++tx_bells;
    } else if (reg == 0x300 + DIAG_RX_CHANNEL * 8) {
        assert(diag_starts == 2);
        ++diag_bells;
    }
}
static void sync_noop(void *unused, const struct umi_mhi_dma *dma) { }
static void report_noop(void *unused, const char *stage, uint32_t value) { }
static struct umi_mhi_io io = {
    .profile = UMI_MHI_PROFILE_SDX55, .bar_size = 4096,
    .dma_start = 0x20000000, .dma_end = 0x20200000,
    .read32 = reg_read, .write32 = reg_write, .allocate = alloc_model,
    .cell_rx = deliver, .diag_rx = deliver, .delay_us = delay_model, .sync_for_device = sync_noop, .report = report_noop
};

static void reset(void)
{
    memset(&h, 0, sizeof(h)); memset(produced, 0, sizeof(produced));
    memset(channels, 0, sizeof(channels)); memset(events, 0, sizeof(events));
    starts = bells = calls = fail_channel = fail_allocation = allocated = 0;
    satellite_opcode = satellite_commands = 0;
    diag_starts = diag_bells = 0;
    deliveries = captured_len = tx_bells = tx_mode = fake_tx_slot = 0;
    h.io = &io; h.bhi = 0x100; h.channel_db = 0x300; h.event_db = 0x700;
    h.ipcr_started = h.contexts_ready = h.amss_sent = true;
    h.channels.bytes = channels; h.events.bytes = events; h.commands.bytes = commands;
    h.command_ring = (struct umi_mhi_dma){cr, 0x20008000, sizeof(cr)};
    h.command_wp = h.command_ring.address;
    for (unsigned i = 0; i < 9; ++i) {
        if (i > 3 && i < 7) continue;
        uint64_t base = 0x20010000u + 4096 * i;
        h.event_ring[i] = (struct umi_mhi_dma){er[i], base, sizeof(er[i])};
        h.event_consumer[i] = base;
        h.event_wp[i] = base + sizeof(er[i]) - 16;
        words_set_address(events + i * 11, 7, base);
        words_set_address(events + i * 11, 9, h.event_wp[i]);
    }
    setup_stock_channel_contexts(&h);
}

int main(void)
{
    reset();
    assert(umi_mhi_sdx55_sat_context(&h, 2, true, 0x20180000, 64) == EINVAL);
    assert(umi_mhi_sdx55_sat_context(&h, 21, false, 0x20180000, 64) == EINVAL);
    assert(umi_mhi_sdx55_sat_context(&h, 50, false, 0x201ffffc, 64) == EINVAL);
    assert(!umi_mhi_sdx55_sat_context(&h, 4, true, 0x20180000, 0x2c));
    assert(!umi_mhi_sdx55_sat_context(&h, 50, false, 0x20181000, 0x2c));
    assert(words_address(events + 4 * 11, 3) == 0x20180000 && words_address(events + 4 * 11, 5) == 0x2c);
    assert(words_address(channels + 50 * 11, 3) == 0x20181000 && words_address(channels + 50 * 11, 5) == 0x2c);
    satellite_opcode = 0x12;
    assert(umi_mhi_sdx55_sat_channel(&h, 21, true) == EINVAL && satellite_commands == 0);
    fail_channel = 50;
    assert(umi_mhi_sdx55_sat_channel(&h, 50, true) == EIO && !h.poisoned);
    fail_channel = 0;
    assert(!umi_mhi_sdx55_sat_channel(&h, 50, true));
    satellite_opcode = 0x10;
    assert(!umi_mhi_sdx55_sat_channel(&h, 50, false) && satellite_commands == 3);
    assert(!umi_mhi_probe_cell_data(&h) && starts == 2 && bells == 1);
    reset();
    assert(!umi_mhi_sdx55_diag_start(&h) && diag_starts == 2 && diag_bells == 1);
    unsigned diag_received;
    uint64_t diag_base = h.diag_rx_ring.address;
    unsigned diag_event_type = (DIAG_RX_CHANNEL << 24) | (0x22u << 16);
    memset(h.diag_rx_buffers.bytes, 0x7e, h.diag_rx_buffers.size);
    /* The firmware can return multiple descriptors with one overflow event. */
    event(DIAG_RX_EVENT, diag_base + 16, (3u << 24) | DIAG_BUFFER_BYTES, diag_event_type);
    assert(!umi_mhi_poll_ipcr(&h, 1, &diag_received) && h.diag_started && !h.poisoned);
    assert(deliveries == 2 && captured_len == DIAG_BUFFER_BYTES && diag_bells == 2);
    event(DIAG_RX_EVENT, diag_base + 32, (4u << 24) | 96, diag_event_type);
    assert(!umi_mhi_poll_ipcr(&h, 1, &diag_received) && h.diag_started);
    assert(deliveries == 3 && captured_len == 96 && diag_bells == 3);
    event(DIAG_RX_EVENT, diag_base + 16, (2u << 24) | 8, diag_event_type);
    assert(!umi_mhi_poll_ipcr(&h, 1, &diag_received) && !h.diag_started && !h.poisoned);
    assert(deliveries == 3);
    reset();
    io.profile = UMI_MHI_PROFILE_QCA6390;
    assert(umi_mhi_probe_cell_data(&h) != 0 && !starts && !calls);
    io.profile = UMI_MHI_PROFILE_SDX55;
    h.ipcr_started = false;
    assert(umi_mhi_probe_cell_data(&h) != 0 && !calls);
    reset(); fail_allocation = 3;
    assert(umi_mhi_probe_cell_data(&h) == ENOMEM && !starts && !bells);
    reset(); fail_channel = 101;
    assert(umi_mhi_probe_cell_data(&h) == EIO && h.poisoned && !bells);
    reset();
    assert(umi_mhi_probe_cell_data(&h) == 0 && starts == 2 && bells == 1);
    assert(umi_mhi_probe_cell_data(&h) == EALREADY);
    uint32_t *rx = channels + 101 * 11;
    uint64_t base = words_address(rx, 3);
    uint32_t *tre = (uint32_t *)(arena + (base - 0x20100000u));
    assert(tre[2] == 32768 && (tre[3] >> 16) == 2);
    assert(words_address(tre, 0) >= 0x20100000 && words_address(tre, 0) + 32768 <= 0x20100000u + allocated);
    assert(words_address(rx, 9) == base + 16 && allocated <= sizeof(arena));
    /* Burst-mode requests must ring only when the channel has queued work. */
    unsigned received;
    event(7, 0, 6u << 24, (100u << 24) | (0x22u << 16));
    event(8, 0, 5u << 24, (101u << 24) | (0x22u << 16));
    assert(umi_mhi_poll_ipcr(&h, 1, &received) == 0 && !received && bells == 2);
    event(8, base, (2u << 24) | 32, (101u << 24) | (0x22u << 16));
    assert(umi_mhi_poll_ipcr(&h, 1, &received) == 0 && !received);
    assert(words_address(rx, 9) == base + 32);
    event(8, base, (2u << 24) | 32, (101u << 24) | (0x22u << 16)); /* stale */
    assert(umi_mhi_poll_ipcr(&h, 1, &received) == EIO && h.poisoned);
    reset();
    assert(umi_mhi_probe_cell_data(&h) == 0);
    event(8, words_address(channels + 101*11, 3), (2u << 24) | 32769u,
        (101u << 24) | (0x22u << 16));
    assert(umi_mhi_poll_ipcr(&h, 1, &received) == EIO && h.poisoned);
    reset();
    assert(umi_mhi_send_cell_data(&h, frame, sizeof(frame), 10) == EBUSY);
    assert(umi_mhi_probe_cell_data(&h) == 0);
    assert(umi_mhi_send_cell_data(&h, NULL, 5, 10) == EINVAL);
    assert(umi_mhi_send_cell_data(&h, frame, 32769, 10) == EMSGSIZE);
    assert(umi_mhi_send_cell_data(&h, frame, sizeof(frame), 0) == EINVAL);
    tx_mode = 6;
    assert(umi_mhi_send_cell_data(&h, frame, sizeof(frame), 10) == 0 && tx_bells == 2);
    assert(umi_mhi_send_cell_data(&h, frame, sizeof(frame), 10) == 0 && tx_bells == 2);
    event(7, 0, 5u<<24, (100u<<24)|(0x22u<<16)); /* empty request arms next TX */
    assert(!umi_mhi_poll_ipcr(&h, 1, &received) && tx_bells == 2);
    assert(umi_mhi_send_cell_data(&h, frame, sizeof(frame), 10) == 0 && tx_bells == 3);
    /* Reuse every slot and cross both TX and event-ring wrap boundaries. */
    for (unsigned i = 0; i < 514; i++)
        assert(!umi_mhi_send_cell_data(&h, frame, sizeof(frame), 10));
    for (unsigned mode = 2; mode <= 5; mode++) {
        reset(); assert(!umi_mhi_probe_cell_data(&h)); tx_mode = mode;
        int error = umi_mhi_send_cell_data(&h, frame, sizeof(frame), 5);
        assert(error == (mode == 5 ? ETIMEDOUT : EIO));
        assert(h.poisoned && umi_mhi_send_cell_data(&h, frame, sizeof(frame), 5) == EIO);
    }
    reset(); assert(!umi_mhi_probe_cell_data(&h));
    base = words_address(channels + 101*11, 3);
    /* Downlink DMA is reused. Only completed transfers are delivered. */
    for (unsigned slot = 0; slot < 9; slot++) {
        unsigned lengths[] = {8, 3, 2, 3, 32768, 1, 1, 0, 8};
        unsigned codes[] =   {2, 3, 4, 2,     3, 3, 2, 2, 2};
        unsigned seen[] =    {1, 1, 1, 2,     2, 2, 2, 2, 3};
        uint32_t *tr = (uint32_t *)(arena + (base-0x20100000u)) + slot*4;
        uint8_t *target = arena + (words_address(tr, 0)-0x20100000u);
        memset(target, 0xcc, lengths[slot]);
        if (slot == 0 || slot == 8) memcpy(target, frame, sizeof(frame));
        if (slot == 1) memcpy(target, frame, 3);
        if (slot == 2) memcpy(target, frame+3, 2);
        if (slot == 3) memcpy(target, frame+5, 3);
        event(8, base+slot*16u, codes[slot]<<24 | lengths[slot], (101u<<24)|(0x22u<<16));
        assert(!umi_mhi_poll_ipcr(&h,1,&received) && !received);
        assert(deliveries == seen[slot] && captured_len == sizeof(frame));
        assert(!memcmp(captured, frame, captured_len));
    }
    puts("PASS: SDX55 satellite contexts, DIAG overflow recovery and IP_HW0 startup, ownership, aggregation and ring wrap");
}
'''
with tempfile.TemporaryDirectory() as directory:
    c = Path(directory) / 'mhi.c'
    exe = Path(directory) / 'mhi'
    c.write_text(source + model)
    subprocess.run(['cc', '-std=gnu11', '-O1', '-fsanitize=address,undefined',
                    '-I', str(bsd), str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
