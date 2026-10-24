/* Software endpoint: no device file, MMIO mapping or root operation. */
#define _POSIX_C_SOURCE 200809L
#include <sys/umi_mhi.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEG (512u * 1024u)
#define MAX_DMA 16
struct model {
	uint32_t regs[1024];
	unsigned writes, polls, allocations, syncs, event_doorbells;
	unsigned command_starts;
	unsigned ready_after, fail_allocation;
	uint64_t next;
	struct umi_mhi_dma memory[MAX_DMA];
	bool bhi_timeout, ready_timeout, m0_timeout, amss_timeout;
	bool stale, bad_dma, fail, sbl_complete, m0_requested;
};

static uint64_t address(const uint32_t *words, unsigned word)
{
	return words[word] | (uint64_t)words[word + 1] << 32;
}

static uint32_t rd(void *p, uint32_t off)
{
	struct model *m = p;
	assert(!(off & 3) && off < sizeof(m->regs));
	return m->regs[off / 4];
}

static void *dma_at(struct model *m, uint64_t iova)
{
	for (unsigned i = 0; i < m->allocations; i++)
		if (iova >= m->memory[i].address &&
		    iova < m->memory[i].address + m->memory[i].size)
			return (unsigned char *)m->memory[i].bytes +
			    (size_t)(iova - m->memory[i].address);
	assert(!"unknown DMA context address");
	return NULL;
}

static void check_contexts(struct model *m)
{
	assert(m->event_doorbells == 7 && m->syncs == m->allocations);
	assert(rd(m, 0x10) == 0x00030880);
	const uint32_t *events = dma_at(m, address(m->regs, 0x60 / 4));
	const unsigned elements[] = {32, 256, 32}, vectors[] = {1, 2, 0};
	for (unsigned i = 0; i < 3; i++) {
		const uint32_t *ctx = events + i * 11;
		uint64_t base = address(ctx, 3), length = elements[i] * 16;
		assert(ctx[0] == (i == 2 ? 1u << 16 : 0));
		assert(ctx[1] == 1 && ctx[2] == vectors[i]);
		assert(dma_at(m, base));
		assert(address(ctx, 5) == length && address(ctx, 7) == base);
		assert(address(ctx, 9) == base + length - 16);
		assert(address(m->regs, (0x700 + i * 8) / 4) == base + length - 16);
	}
	const uint32_t *cmd = dma_at(m, address(m->regs, 0x68 / 4));
	assert(address(cmd, 5) == 128 * 16);
	assert(address(cmd, 3) == address(cmd, 7));
	assert(address(cmd, 7) == address(cmd, 9));
	assert(dma_at(m, address(cmd, 3)));
	assert(address(m->regs, 0x80 / 4) == 0xa0000000);
	assert(address(m->regs, 0x88 / 4) == 0xb0000000);
	assert(address(m->regs, 0x98 / 4) == 0xa0000000);
	assert(address(m->regs, 0xa0 / 4) == 0xb0000000);
}

static void wr(void *p, uint32_t off, uint32_t value)
{
	struct model *m = p;
	assert(!(off & 3) && off < sizeof(m->regs));
	m->writes++;
	m->regs[off / 4] = value;
	if (off == 0x118 && !m->bhi_timeout) {
		m->regs[0x12c / 4] = (m->fail ? 0xc0000000 : 0x80000000) | value;
		m->sbl_complete = true;
		if (!m->ready_after && !m->ready_timeout) {
			m->regs[0x128 / 4] = 1;
			m->regs[0x48 / 4] = 0x101;
		}
	}
	if (off >= 0x700 && off <= 0x710 && !(off & 7))
		m->event_doorbells |= 1u << ((off - 0x700) / 8);
	if (off == 0x38 && ((value >> 8) & 0xff) == 2) {
		check_contexts(m); /* Event doorbells must precede the M0 request. */
		m->m0_requested = true;
		if (!m->m0_timeout) m->regs[0x48 / 4] = 0x201;
	}
	if (off == 0x260 && !m->amss_timeout) {
		m->regs[0x268 / 4] = 0x80000000 | (m->stale ? value - 1 : value);
		m->regs[0x128 / 4] = 2;
	}
	if (off == 0x70) {
		uint64_t command_wp = address(m->regs, 0x70 / 4);
		uint64_t command_iova = command_wp - 16;
		uint32_t *command = dma_at(m, command_iova);
		unsigned type = (command[3] >> 16) & 0xff;
		assert(type == 1 || type == 18);
		if (type == 18) {
			unsigned channel = command[3] >> 24;
			uint32_t *channel_contexts = dma_at(m, address(m->regs, 0x58 / 4));
			uint32_t *channel_context = channel_contexts + channel * 11;
			assert(channel == (m->command_starts ? 21u : 20u));
			assert(address(channel_context, 9) == address(channel_context, 3));
			channel_context[0] = (channel_context[0] & ~0xffu) | 2u;
			m->command_starts++;
		}
		uint32_t *event_contexts = dma_at(m, address(m->regs, 0x60 / 4));
		uint32_t *event_context = event_contexts;
		uint64_t event_base = address(event_context, 3);
		uint64_t event_rp = address(event_context, 7);
		uint32_t *event = dma_at(m, event_rp);
		event[0] = (uint32_t)command_iova;
		event[1] = (uint32_t)(command_iova >> 32);
		event[2] = 1u << 24; /* command completion success */
		event[3] = 0x21u << 16;
		uint64_t event_next = event_rp + 16;
		if (event_next == event_base + 32 * 16) event_next = event_base;
		event_context[7] = (uint32_t)event_next;
		event_context[8] = (uint32_t)(event_next >> 32);
	}
}

static int alloc_dma(void *p, size_t size, struct umi_mhi_dma *dma)
{
	struct model *m = p;
	if (m->fail_allocation == m->allocations + 1) return ENOMEM;
	assert(m->allocations < MAX_DMA);
	dma->size = (size + 4095) & ~(size_t)4095;
	assert(posix_memalign(&dma->bytes, 4096, dma->size) == 0);
	memset(dma->bytes, 0xa5, dma->size);
	dma->address = m->bad_dma ? 0xb0000000 : m->next;
	m->next += dma->size;
	m->memory[m->allocations++] = *dma;
	return 0;
}

static void sync_dma(void *p, const struct umi_mhi_dma *dma)
{
	struct model *m = p;
	assert(dma->bytes);
	m->syncs++;
}

static void delay_us(void *p, unsigned us)
{
	struct model *m = p;
	assert(us == 1000);
	m->polls++;
	if (m->sbl_complete && !m->ready_timeout && !m->m0_requested &&
	    m->polls >= m->ready_after) {
		m->regs[0x128 / 4] = 1;
		m->regs[0x48 / 4] = 0x101;
		m->regs[0x38 / 4] &= ~2u;
	}
}

static void setup(struct model *m, struct umi_mhi *h, struct umi_mhi_io *io)
{
	memset(m, 0, sizeof(*m));
	memset(h, 0, sizeof(*h));
	m->next = 0xa0000000;
	m->regs[0] = 0x100;
	m->regs[8 / 4] = 0x01000000;
	m->regs[0x10 / 4] = 0x880;
	m->regs[0x18 / 4] = 0x300;
	m->regs[0x20 / 4] = 0x700;
	m->regs[0x28 / 4] = 0x100;
	m->regs[0x2c / 4] = 0x224;
	m->regs[0x104 / 4] = 1;
	*io = (struct umi_mhi_io){ .context=m, .bar_size=4096, .read32=rd, .write32=wr,
	    .allocate=alloc_dma, .sync_for_device=sync_dma, .delay_us=delay_us };
}

static void cleanup(struct model *m)
{
	/* The simulated endpoint has stopped before releasing DMA. */
	for (unsigned i = 0; i < m->allocations; i++) free(m->memory[i].bytes);
}

static void boot_sbl(struct model *m, struct umi_mhi *h, struct umi_mhi_io *io,
    const void *image, size_t size)
{
	assert(umi_mhi_probe(h, io) == 0 && m->writes == 0);
	assert(umi_mhi_probe(h, io) == EBUSY);
	assert(umi_mhi_download_sbl(h, image, size) == 0);
	assert(rd(m, 0x110) == SEG && !memcmp(h->sbl.bytes, image, SEG));
}

int main(void)
{
	struct model m;
	struct umi_mhi h;
	struct umi_mhi_io io;
	size_t size = 2 * SEG + 17;
	unsigned char *image = malloc(size);
	assert(image);
	for (size_t i = 0; i < size; i++) image[i] = (unsigned char)(i * 13);

	setup(&m, &h, &io);
	boot_sbl(&m, &h, &io, image, size);
	unsigned writes = m.writes;
	assert(umi_mhi_download_amss(&h, image, size) == EBUSY && m.writes == writes);
	assert(umi_mhi_prepare_ready(&h) == 0 && h.contexts_ready);
	assert(umi_mhi_prepare_ready(&h) == EALREADY);
	assert(umi_mhi_download_amss(&h, image, size) == 0 && h.amss_sent);
	assert(umi_mhi_wait_mission(&h) == 0);
	uint64_t *v = h.vectors.bytes;
	assert(rd(&m, 0x258) == 48);
	assert(v[0] == h.image.address && v[1] == SEG);
	assert(v[2] == h.image.address + SEG && v[3] == SEG);
	assert(v[4] == h.image.address + 2 * SEG && v[5] == 17);
	assert(!memcmp(h.image.bytes, image, size));
	assert(umi_mhi_start_ipcr(&h) == 0 && h.ipcr_started);
	assert(m.command_starts == 2);
	assert(umi_mhi_start_ipcr(&h) == EALREADY);
	uint32_t *channels = dma_at(&m, address(m.regs, 0x58 / 4));
	uint32_t *tx = channels + 20 * 11, *rx = channels + 21 * 11;
	assert(tx[0] == 0x202 && tx[1] == 1 && tx[2] == 1);
	assert(rx[0] == 0x202 && rx[1] == 2 && rx[2] == 1);
	assert(address(rx, 5) == 32 * 16);
	assert(address(rx, 7) == h.ipcr_rx_ring.address);
	assert(address(rx, 9) == h.ipcr_rx_ring.address + 31 * 16);
	uint32_t *rx_tres = h.ipcr_rx_ring.bytes;
	assert(rx_tres[2] == 8192 && rx_tres[3] == ((2u << 16) | (1u << 9)));
	assert(rx_tres[0] == (uint32_t)h.ipcr_rx_buffers.address);
	/* A QMI BDF chunk exceeds 4 KiB; every slot must have its full 8 KiB
	 * backing, while oversized sends must fail before publishing a TRE. */
	assert(h.ipcr_rx_buffers.size == 31u * 8192u);
	assert(h.ipcr_tx_buffers.size == 32u * 8192u);
	writes = m.writes;
	assert(umi_mhi_send_qrtr_packet(&h, image, 8193, 1, UINT16_MAX) == EINVAL);
	assert(m.writes == writes && !h.ipcr_tx_pending && !h.poisoned);
	unsigned received = UINT32_MAX;
	assert(umi_mhi_poll_ipcr(&h, 1, &received) == 0 && received == 0);
	writes = m.writes;
	assert(umi_mhi_download_amss(&h, image, size) == EALREADY && m.writes == writes);
	cleanup(&m);
	puts("PASS: complete SBL/READY/M0/BHIE boot, packed contexts, ring ordering, full image");

	setup(&m, &h, &io);
	m.ready_after = 7;
	boot_sbl(&m, &h, &io, image, size);
	m.regs[0x38 / 4] = 2; /* RESET clears asynchronously, along with READY/EE. */
	assert(umi_mhi_prepare_ready(&h) == 0 && m.polls == 7);
	cleanup(&m);
	puts("PASS: delayed SBL execution environment, READY and RESET transitions");

	setup(&m, &h, &io);
	m.bad_dma = true;
	assert(umi_mhi_probe(&h, &io) == 0);
	assert(umi_mhi_download_sbl(&h, image, size) == EINVAL && m.writes == 0);
	cleanup(&m);
	puts("PASS: out-of-aperture DMA refused before register writes");

	for (unsigned stage = 0; stage < 4; stage++) {
		setup(&m, &h, &io);
		assert(umi_mhi_probe(&h, &io) == 0);
		m.bhi_timeout = stage == 0;
		m.ready_timeout = stage == 1;
		m.m0_timeout = stage == 2;
		m.amss_timeout = stage == 3;
		int result = umi_mhi_download_sbl(&h, image, size);
		if (!result) result = umi_mhi_prepare_ready(&h);
		if (!result) result = umi_mhi_download_amss(&h, image, size);
		assert(result == ETIMEDOUT && h.poisoned && h.sbl.bytes && m.polls == 20000);
		writes = m.writes;
		assert(umi_mhi_download_sbl(&h, image, size) == EIO && m.writes == writes);
		assert(umi_mhi_prepare_ready(&h) == EIO && m.writes == writes);
		cleanup(&m);
	}
	puts("PASS: bounded BHI/READY/M0/BHIE timeouts, retained DMA, retries blocked");

	setup(&m, &h, &io);
	boot_sbl(&m, &h, &io, image, size);
	assert(umi_mhi_prepare_ready(&h) == 0);
	m.stale = true;
	assert(umi_mhi_download_amss(&h, image, size) == ETIMEDOUT && h.poisoned);
	cleanup(&m);
	puts("PASS: stale BHIE completion sequence rejected");

	setup(&m, &h, &io);
	boot_sbl(&m, &h, &io, image, size);
	m.regs[0x48 / 4] = 4;
	writes = m.writes;
	assert(umi_mhi_prepare_ready(&h) == EIO && h.poisoned && m.writes == writes);
	cleanup(&m);
	setup(&m, &h, &io);
	boot_sbl(&m, &h, &io, image, size);
	m.regs[0x38 / 4] = UINT32_MAX;
	writes = m.writes;
	assert(umi_mhi_prepare_ready(&h) == EIO && h.poisoned && m.writes == writes);
	cleanup(&m);
	puts("PASS: SYSERR and inaccessible controller stop before context publication");

	setup(&m, &h, &io);
	boot_sbl(&m, &h, &io, image, size);
	m.fail_allocation = 4;
	writes = m.writes;
	assert(umi_mhi_prepare_ready(&h) == ENOMEM && m.writes == writes);
	assert(umi_mhi_prepare_ready(&h) == EALREADY && m.writes == writes);
	cleanup(&m);
	puts("PASS: partial allocation failure leaves unpublished DMA retained");

	setup(&m, &h, &io);
	m.regs[8 / 4] = UINT32_MAX;
	assert(umi_mhi_probe(&h, &io) == ENODEV && m.writes == 0);
	setup(&m, &h, &io);
	m.regs[0x2c / 4] = 4092;
	assert(umi_mhi_probe(&h, &io) == EINVAL && m.writes == 0);
	setup(&m, &h, &io);
	assert(umi_mhi_probe(&h, &io) == 0);
	m.regs[0x128 / 4] = 2;
	assert(umi_mhi_download_sbl(&h, image, size) == EALREADY && m.writes == 0);
	puts("PASS: inaccessible BAR, invalid offsets and already running firmware refused");
	setup(&m, &h, &io);
	m.regs[8 / 4] = 0xdeadbeef;
	assert(umi_mhi_probe(&h, &io) == 0 && m.writes == 0);
	setup(&m, &h, &io);
	m.regs[8 / 4] = 0xdeadbeef;
	m.regs[0x128 / 4] = 2;
	assert(umi_mhi_probe(&h, &io) == ENODEV && m.writes == 0);
	setup(&m, &h, &io);
	m.regs[8 / 4] = 0xdeadbeef;
	m.regs[0x28 / 4] = 0x180;
	assert(umi_mhi_probe(&h, &io) == ENODEV && m.writes == 0);
	puts("PASS: captured cold-PBL signature accepted only with exact layout and PBL EE");
	setup(&m, &h, &io);
	boot_sbl(&m, &h, &io, image, size);
	assert(umi_mhi_prepare_ready(&h) == 0);
	assert(umi_mhi_wait_mission(&h) == EBUSY);
	assert(umi_mhi_download_amss(&h, image, size) == 0);
	m.regs[0x128 / 4] = 1;
	assert(umi_mhi_wait_mission(&h) == ETIMEDOUT && h.poisoned && m.polls == 20000);
	cleanup(&m);
	puts("PASS: mission mode requires completed BHIE and has a bounded transition wait");
	free(image);
	return 0;
}
