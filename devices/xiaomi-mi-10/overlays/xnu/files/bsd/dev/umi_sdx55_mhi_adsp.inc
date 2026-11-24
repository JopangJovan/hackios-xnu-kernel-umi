/* The stock MHI satellite driver gives the ADSP a view of the SDX55.
 * The ADSP owns event ring 4 and channels 50, 51, 70 and 71.
 */
#define SDX55_SAT_DEVICE 0x03062010u /* PCI 0002:01:00.0, device 0306 */
#define SDX55_SAT_EVENT 4u
static const struct { uint64_t physical; unsigned offset, bytes; bool device; } sdx55_sat_regions[] = {
	{ 0x80b00000, 0, 0x40000, false },
	{ 0x80b40000, 0x40000, 0x10000, false },
	{ 0x0040f00c, 0x50000, 4, true }, /* IPCC SEND */
};

struct sdx55_sat_tre {
	uint64_t pointer;
	uint32_t data, type;
} __attribute__((packed));

struct sdx55_sat_packet {
	uint16_t major, minor, message, sequence, reply, bytes;
	uint32_t device;
	uint64_t reserved;
	struct sdx55_sat_tre tre[8];
} __attribute__((packed));

#define SDX55_SAT_HEADER offsetof(struct sdx55_sat_packet, tre)
_Static_assert(SDX55_SAT_HEADER == 24 && sizeof(struct sdx55_sat_tre) == 16, "MHI satellite wire layout");

static struct {
	bool announced, event_ready;
	uint16_t sequence;
	uint32_t mapped, configured, started;
	unsigned attempts;
	uint64_t retry_after;
} sdx55_sat;

static int
sdx55_sat_send(struct sdx55_sat_packet *packet, unsigned count, uint16_t reply)
{
	if (++sdx55_sat.sequence == UINT16_MAX) sdx55_sat.sequence = 0;
	packet->major = 1;
	packet->minor = 0;
	packet->message = 0xe; /* EVENT */
	packet->sequence = sdx55_sat.sequence;
	packet->reply = reply;
	packet->bytes = count * sizeof(packet->tre[0]);
	packet->device = SDX55_SAT_DEVICE;
	packet->reserved = 0;
	return hakios_adsp_mhi_sat_send(packet, SDX55_SAT_HEADER + packet->bytes);
}

static int
sdx55_sat_map(uint64_t physical, uint32_t bytes, uint64_t *iova)
{
	/* The UMI ADSP asks for two fixed firmware regions and the IPCC SEND
	 * doorbell. Give them their own range outside the IPCR/data pool. */
	for (unsigned i = 0; i < sizeof(sdx55_sat_regions) / sizeof(sdx55_sat_regions[0]); i++) {
		const typeof(sdx55_sat_regions[0]) *region = &sdx55_sat_regions[i];
		if (physical != region->physical || bytes != region->bytes) continue;
		unsigned in_page = physical & 0xfff;
		if (!(sdx55_sat.mapped & BIT(i))) {
			lck_mtx_lock(&hakios_smmu_lock);
			uint64_t *entries = (uint64_t *)(umi_sdx55_dma.tables + SDX55_SAT_TABLE) + region->offset / 4096;
			unsigned pages = (bytes + in_page + 4095) / 4096;
			uint64_t attrs = 3ull | BIT(6) | (2ull << 8) | BIT(10) | BIT(11);
			if (region->device) attrs |= (2ull << 2) | (1ull << 53) | (1ull << 54);
			for (unsigned page = 0; page < pages; page++)
				entries[page] = (physical - in_page + 4096ull * page) | attrs;
			umi_sdx55_clean((vm_offset_t)entries, pages * sizeof(*entries));
			bool synced = umi_sdx55_smmu_sync();
			if (!synced) {
				bzero(entries, pages * sizeof(*entries));
				umi_sdx55_clean((vm_offset_t)entries, pages * sizeof(*entries));
				(void)umi_sdx55_smmu_sync();
			}
			lck_mtx_unlock(&hakios_smmu_lock);
			if (!synced) return EIO;
			sdx55_sat.mapped |= BIT(i);
		}
		*iova = SDX55_SAT_IOVA + region->offset + in_page;
		return 0;
	}
	return EINVAL;
}

static bool
sdx55_sat_mapped(uint64_t iova, uint32_t size)
{
	if (!size || (iova & 3)) return false;
	for (unsigned i = 0; i < sizeof(sdx55_sat_regions) / sizeof(sdx55_sat_regions[0]); i++) {
		if (sdx55_sat_regions[i].device || !(sdx55_sat.mapped & BIT(i))) continue;
		uint64_t begin = SDX55_SAT_IOVA + sdx55_sat_regions[i].offset;
		uint64_t end = begin + sdx55_sat_regions[i].bytes;
		if (iova >= begin && iova < end && size <= end - iova) return true;
	}
	return false;
}

static int
sdx55_sat_command(struct umi_mhi *host, const struct sdx55_sat_tre *tre, uint64_t *address)
{
	unsigned type = (tre->type >> 16) & 0xff, id = tre->type >> 24;
	if (type == 0x65) return sdx55_sat_map(tre->pointer, tre->data, address);
	bool event = (tre->type & 1u) != 0;
	if (type == 0x64 && event && id == SDX55_SAT_EVENT) {
		if (sdx55_sat.started || !sdx55_sat_mapped(tre->pointer, tre->data)) return EINVAL;
		int error = umi_mhi_sdx55_sat_context(host, id, true, tre->pointer, tre->data);
		sdx55_sat.event_ready = !error;
		return error;
	}
	unsigned slot;
	switch (id) {
	case 50: slot = 0; break;
	case 51: slot = 1; break;
	case 70: slot = 2; break;
	case 71: slot = 3; break;
	default: return ENXIO;
	}
	unsigned mask = BIT(slot);
	if (type == 0x64 && !event) {
		if ((sdx55_sat.started & mask) || !sdx55_sat_mapped(tre->pointer, tre->data)) return EINVAL;
		int error = umi_mhi_sdx55_sat_context(host, id, false, tre->pointer, tre->data);
		if (!error) sdx55_sat.configured |= mask;
		return error;
	}
	if (type == 0x12 && sdx55_sat.event_ready && (sdx55_sat.configured & mask) && !(sdx55_sat.started & mask)) {
		int error = umi_mhi_sdx55_sat_channel(host, id, true);
		if (!error) sdx55_sat.started |= mask;
		return error;
	}
	if (type == 0x10 && (sdx55_sat.started & mask)) {
		int error = umi_mhi_sdx55_sat_channel(host, id, false);
		if (!error) sdx55_sat.started &= ~mask;
		return error;
	}
	return ENOTSUP;
}

static void
sdx55_sat_poll(struct umi_mhi *host)
{
	if (!host->ipcr_started || host->poisoned || !hakios_adsp_mhi_sat_open()) return;
	if (!sdx55_sat.announced) {
		if (sdx55_sat.attempts >= 3 || mach_absolute_time() < sdx55_sat.retry_after) return;
		sdx55_sat.attempts++;
		struct sdx55_sat_packet hello = {
			.tre = {
				{BAR, (SDX55_SAT_EVENT << 16) | 1, 0x6e0000},
				{0, 2u << 24, 0x200000}, /* M0 */
				{0, 2u << 24, 0x400000}, /* AMSS */
			},
		};
		int error = sdx55_sat_send(&hello, 3, UINT16_MAX);
		hakios_tl("UMI SDX55: voice MHI satellite hello errno=%d\n", error);
		sdx55_sat.announced = !error;
		clock_interval_to_deadline(1, NSEC_PER_SEC, &sdx55_sat.retry_after);
		return;
	}
	struct sdx55_sat_packet packet;
	unsigned size = hakios_adsp_mhi_sat_receive(&packet, sizeof(packet));
	if (!size) return;
	if (size < SDX55_SAT_HEADER || packet.bytes != size - SDX55_SAT_HEADER ||
	    packet.bytes % sizeof(packet.tre[0]) || packet.major != 1 || packet.minor != 0 ||
	    packet.device != SDX55_SAT_DEVICE) {
		hakios_tl("UMI SDX55: voice MHI satellite invalid frame bytes=%u\n", size);
		return;
	}
	unsigned count = packet.bytes / sizeof(packet.tre[0]);
	if (packet.message != 0xc) {
		hakios_tl("UMI SDX55: voice MHI satellite event type=%#x records=%u\n", packet.message, count);
		return;
	}
	for (unsigned i = 0; i < count; i++) {
		uint64_t address = 0;
		int error = sdx55_sat_command(host, &packet.tre[i], &address);
		hakios_tl("UMI SDX55: voice MHI satellite request=%#x address=%#llx size=%#x mapped=%#llx errno=%d\n",
		    packet.tre[i].type, packet.tre[i].pointer, packet.tre[i].data, address, error);
		packet.tre[i] = (struct sdx55_sat_tre){address, (error ? 0x11u : 1u) << 24, 0x210000};
	}
	int error = sdx55_sat_send(&packet, count, packet.sequence);
	if (error) hakios_tl("UMI SDX55: voice MHI satellite reply errno=%d\n", error);
}
