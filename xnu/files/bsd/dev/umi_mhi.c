/* QCA6390's BHI/BHIE firmware transfer, with UMI's bounded DMA aperture.
 * Register layout was checked against the running UMI Android MHI controller.
 * This is a transport component: it does not turn on rails, initialize PCIe,
 * select an ath11k chip or expose a network interface by itself.
 */
#include <sys/umi_mhi.h>
#include <sys/errno.h>
#include <libkern/libkern.h>

#define SEGMENT_SIZE (512u * 1024u)
#define IMAGE_LIMIT (16u * 1024u * 1024u)
#define DMA_START 0xa0000000ull
#define DMA_END   0xb0000000ull
#define POLLS 20000u

static void
dma_write_barrier(void)
{
#if defined(__aarch64__)
	__asm__ volatile("dmb oshst" ::: "memory");
#else
	/* Host software model; the kernel target always uses the outer-shareable barrier. */
	__atomic_thread_fence(__ATOMIC_RELEASE);
#endif
}

static bool
span(uint32_t base, uint32_t length, uint32_t limit)
{
	return !(base & 3) && base <= limit && length <= limit - base;
}

static uint32_t
read_reg(const struct umi_mhi *host, uint32_t offset)
{
	return host->io->read32(host->io->context, offset);
}

static void
write_reg(struct umi_mhi *host, uint32_t offset, uint32_t value)
{
	host->io->write32(host->io->context, offset, value);
}

static void
write_address(struct umi_mhi *host, uint32_t offset, uint64_t address)
{
	/* No doorbell is rung until both halves and the size have been written. */
	write_reg(host, offset + 4, (uint32_t)(address >> 32));
	write_reg(host, offset, (uint32_t)address);
}

static void
report(struct umi_mhi *host, const char *stage, uint32_t value)
{
	if (host->io->report) host->io->report(host->io->context, stage, value);
}

int
umi_mhi_probe(struct umi_mhi *host, const struct umi_mhi_io *io)
{
	if (!host || !io || !io->read32 || !io->write32 || !io->allocate ||
	    !io->sync_for_device || !io->delay_us || io->bar_size < 4096) return EINVAL;
	if (host->io) return EBUSY;
	uint32_t version = io->read32(io->context, 8);
	uint32_t length = io->read32(io->context, 0);
	if (length < 0xa8 || length > io->bar_size) return ENODEV;
	uint32_t bhi = io->read32(io->context, 0x28);
	uint32_t bhie = io->read32(io->context, 0x2c);
	uint32_t channels = io->read32(io->context, 0x18);
	uint32_t events = io->read32(io->context, 0x20);
	/* Check every aperture before a caller can issue any firmware transfer. */
	if (bhi < length || bhie < length || channels < length || events < length ||
	    !span(bhi, 0x34, io->bar_size) || !span(bhie, 0x7c, io->bar_size) ||
	    !span(channels, 128 * 8, io->bar_size) || !span(events, 3 * 8, io->bar_size)) return EINVAL;
	if (io->read32(io->context, bhi + 4) != 1) return ENODEV;
	/* Real UMI PBL leaves MHIVER at the reserved-register sentinel. Its
	 * complete BHI layout is readable before the mission-mode version appears. */
	bool cold_pbl = version == 0xdeadbeef && length == 0x100 &&
	    bhi == 0x100 && bhie == 0x224 && channels == 0x300 && events == 0x700 &&
	    io->read32(io->context, bhi + 0x28) == 0;
	if (version != 0x01000000 && !cold_pbl) return ENODEV;
	host->io = io;
	host->bhi = bhi;
	host->bhie = bhie;
	host->channel_db = channels;
	host->event_db = events;
	host->sequence = 1;
	report(host, "MHI version", version);
	return 0;
}

int
umi_mhi_execution_environment(const struct umi_mhi *host, uint32_t *environment)
{
	if (!host || !host->io || !environment) return EINVAL;
	uint32_t ee = read_reg(host, host->bhi + 0x28);
	if (ee > 7) return EIO;
	*environment = ee;
	return 0;
}

static int
allocate(struct umi_mhi *host, size_t size, struct umi_mhi_dma *dma)
{
	int error = host->io->allocate(host->io->context, size, dma);
	if (error) return error;
	if (!dma->bytes || ((uintptr_t)dma->bytes & 7) || dma->size < size || (dma->address & 4095) ||
	    dma->address < DMA_START || dma->address >= DMA_END ||
	    dma->size > DMA_END - dma->address) return EINVAL;
	return 0;
}

static int
wait_transfer(struct umi_mhi *host, uint32_t status_offset, uint32_t sequence, bool check_sequence)
{
	uint32_t status = 0;
	for (unsigned i = 0; i < POLLS; i++) {
		status = read_reg(host, status_offset);
		if (status == UINT32_MAX || (status >> 30) == 3) break;
		if ((status >> 30) == 2 && (!check_sequence || (status & 0x3fffffff) == sequence)) {
			report(host, "firmware transfer complete", status);
			return 0;
		}
		host->io->delay_us(host->io->context, 1000);
	}
	host->poisoned = true;
	report(host, "firmware transfer failed; DMA retained until endpoint reset", status);
	return status == UINT32_MAX || (status >> 30) == 3 ? EIO : ETIMEDOUT;
}

/* MHI context fields are 4-byte aligned, including the 64-bit ring pointers.
 * Explicit words avoid unaligned 64-bit accesses through an uncached mapping.
 */
static void
context_address(uint32_t *context, unsigned word, uint64_t value)
{
	context[word] = (uint32_t)value;
	context[word + 1] = (uint32_t)(value >> 32);
}

int
umi_mhi_prepare_ready(struct umi_mhi *host)
{
	if (!host || !host->io) return EINVAL;
	if (host->poisoned) return EIO;
	if (host->channels.bytes || host->contexts_ready) return EALREADY;
	if (!host->sbl_sent) return EBUSY;
	/* BHI completion can precede both the SBL EE and READY/reset transition. */
	uint32_t status = 0, control = 0, ee = 0;
	bool ready = false;
	int error;
	for (unsigned i = 0; i < POLLS; i++) {
		status = read_reg(host, 0x48);
		control = read_reg(host, 0x38);
		error = umi_mhi_execution_environment(host, &ee);
		if (error || status == UINT32_MAX || control == UINT32_MAX || (status & 4)) {
			host->poisoned = true;
			return EIO;
		}
		if (ee == 1 && (status & 1) && !(control & 2) &&
		    ((status >> 8) & 0xff) == 1) {
			ready = true;
			break;
		}
		host->io->delay_us(host->io->context, 1000);
	}
	if (!ready) {
		host->poisoned = true;
		report(host, "READY timeout; SBL DMA retained until endpoint reset", status);
		return ETIMEDOUT;
	}
	uint32_t config = read_reg(host, 0x10);
	if (config == UINT32_MAX) { host->poisoned = true; return EIO; }
	struct umi_mhi_dma *tables[] = { &host->channels, &host->events, &host->commands,
	    &host->command_ring, &host->event_ring[0], &host->event_ring[1], &host->event_ring[2] };
	const size_t sizes[] = { 128 * 44, 3 * 44, 44, 128 * 16, 32 * 16, 256 * 16, 32 * 16 };
	for (unsigned i = 0; i < sizeof(tables) / sizeof(tables[0]); i++) {
		error = allocate(host, sizes[i], tables[i]);
		if (error) return error;
		memset(tables[i]->bytes, 0, tables[i]->size);
	}
	const unsigned elements[] = { 32, 256, 32 }, vectors[] = { 1, 2, 0 };
	for (unsigned i = 0; i < 3; i++) {
		uint32_t *context = (uint32_t *)host->events.bytes + i * 11;
		uint64_t base = host->event_ring[i].address;
		uint64_t length = elements[i] * 16;
		context[0] = i == 2 ? 1u << 16 : 0; /* stock interrupt moderation */
		context[1] = 1;                   /* valid software event ring */
		context[2] = vectors[i];
		context_address(context, 3, base);
		context_address(context, 5, length);
		context_address(context, 7, base);
		context_address(context, 9, base + length - 16);
	}
	uint32_t *command = host->commands.bytes;
	context_address(command, 3, host->command_ring.address);
	context_address(command, 5, 128 * 16);
	context_address(command, 7, host->command_ring.address);
	context_address(command, 9, host->command_ring.address);
	for (unsigned i = 0; i < sizeof(tables) / sizeof(tables[0]); i++)
		host->io->sync_for_device(host->io->context, tables[i]);
	write_address(host, 0x58, host->channels.address);
	write_address(host, 0x60, host->events.address);
	write_address(host, 0x68, host->commands.address);
	write_address(host, 0x80, DMA_START);
	write_address(host, 0x88, DMA_END);
	write_address(host, 0x98, DMA_START);
	write_address(host, 0xa0, DMA_END);
	write_reg(host, 0x10, (config & 0xffff) | (3u << 16));
	dma_write_barrier();
	/* Publish free event entries while READY, before requesting M0. */
	for (unsigned e = 0; e < 3; e++)
		write_address(host, host->event_db + e * 8,
		    host->event_ring[e].address + elements[e] * 16 - 16);
	write_reg(host, 0x38, (control & ~0xff00u) | 0x200); /* request M0 */
	for (unsigned i = 0; i < POLLS; i++) {
		status = read_reg(host, 0x48);
		if (status == UINT32_MAX || (status & 4)) break;
		if (((status >> 8) & 0xff) == 2) {
			host->contexts_ready = true;
			report(host, "MHI contexts installed; M0", status);
			return 0;
		}
		host->io->delay_us(host->io->context, 1000);
	}
	host->poisoned = true;
	report(host, "M0 failed; DMA retained until endpoint reset", status);
	return status == UINT32_MAX || (status & 4) ? EIO : ETIMEDOUT;
}

static int
check_download(struct umi_mhi *host, const void *image, size_t size, uint32_t wanted_ee)
{
	if (!host || !host->io || !image || !size || size > IMAGE_LIMIT) return EINVAL;
	if (host->poisoned) return EIO;
	uint32_t ee;
	int error = umi_mhi_execution_environment(host, &ee);
	if (error) return error;
	if (ee == 2 || ee == 4) return EALREADY;
	return ee == wanted_ee ? 0 : EBUSY;
}

int
umi_mhi_download_sbl(struct umi_mhi *host, const void *image, size_t size)
{
	int error = check_download(host, image, size, 0); /* PBL */
	if (error) return error;
	if (host->sbl.bytes || host->sbl_sent) return EALREADY;
	size_t length = size < SEGMENT_SIZE ? size : SEGMENT_SIZE;
	error = allocate(host, length, &host->sbl);
	if (error) return error;
	memcpy(host->sbl.bytes, image, length);
	host->io->sync_for_device(host->io->context, &host->sbl);
	uint32_t sequence = host->sequence++;
	write_reg(host, host->bhi + 0x20, 0); /* polling, vector zero */
	write_reg(host, host->bhi + 0x2c, 0); /* discard previous completion */
	write_address(host, host->bhi + 8, host->sbl.address);
	write_reg(host, host->bhi + 0x10, (uint32_t)length);
	dma_write_barrier();
	write_reg(host, host->bhi + 0x18, sequence);
	error = wait_transfer(host, host->bhi + 0x2c, sequence, false);
	if (!error) host->sbl_sent = true;
	return error;
}

int
umi_mhi_download_amss(struct umi_mhi *host, const void *image, size_t size)
{
	int error = check_download(host, image, size, 1); /* SBL */
	if (error) return error;
	if (!host->sbl_sent || !host->contexts_ready) return EBUSY;
	/* SBL firmware download needs the host's READY/context/M0 setup first. */
	if (((read_reg(host, 0x48) >> 8) & 0xff) != 2) return EBUSY;
	if (host->image.bytes || host->vectors.bytes || host->amss_sent) return EALREADY;
	/* The full image is segmented, not just the bytes after the initial SBL. */
	uint32_t count = (uint32_t)((size + SEGMENT_SIZE - 1) / SEGMENT_SIZE);
	error = allocate(host, size, &host->image);
	if (error) return error;
	error = allocate(host, count * 16u, &host->vectors);
	if (error) return error;
	memcpy(host->image.bytes, image, size);
	uint64_t *vectors = host->vectors.bytes;
	for (uint32_t i = 0; i < count; i++) {
		size_t offset = (size_t)i * SEGMENT_SIZE;
		size_t length = size - offset;
		if (length > SEGMENT_SIZE) length = SEGMENT_SIZE;
		/* ARM64 UMI is little endian, as are the MHI vector entries. */
		vectors[2 * i] = host->image.address + offset;
		vectors[2 * i + 1] = length;
	}
	host->io->sync_for_device(host->io->context, &host->image);
	host->io->sync_for_device(host->io->context, &host->vectors);
	uint32_t sequence = host->sequence++;
	write_address(host, host->bhie + 0x2c, host->vectors.address);
	write_reg(host, host->bhie + 0x34, count * 16u);
	dma_write_barrier();
	write_reg(host, host->bhie + 0x3c, sequence);
	error = wait_transfer(host, host->bhie + 0x44, sequence, true);
	if (!error) host->amss_sent = true;
	return error;
}
