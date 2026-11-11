/* Included in the UMI build of hakios.cell.c to reuse its ARP, DHCP and
 * Ethernet framing. No MHI addresses, DMA buffers or callbacks survive
 * inside ifnet. The mission worker is the only queue consumer. */
#include <sys/errno.h>
#include <sys/umi_sdx55_net.h>

#define SDX55_NET_MTU (HAKIOS_NET_FRAME_MAX - 14u)
#define SDX55_NET_QUEUE 16u
static struct {
	uint16_t size;
	uint8_t bytes[SDX55_NET_MTU];
} sdx55_tx_queue[SDX55_NET_QUEUE];
static unsigned sdx55_head, sdx55_count;

static const struct hakios_net_link sdx55_link = {
	.name = "SDX55 PCIe",
	.unit = CELL_UNIT,
	.address = { 0x02, 0x68, 0x61, 0x6b, 0x69, 0x01 },
	.transmit = cell_transmit,
};

static size_t
sdx55_ipv4_size(const uint8_t *p, size_t length)
{
	if (!p || length < 20 || p[0] >> 4 != 4) return 0;
	size_t header = (p[0] & 0xf) * 4u;
	size_t total = (size_t)p[2] << 8 | p[3];
	if (header < 20 || total < header || total > length || total > SDX55_NET_MTU) return 0;
	return total;
}

/* cell_transmit holds cell_lock and has already checked session.up. */
static bool
sdx55_enqueue_locked(const uint8_t *p, size_t length)
{
	size_t total = sdx55_ipv4_size(p, length);
	if (!total || total > session.mtu || get32(p + 12) != session.address ||
	    sdx55_count == SDX55_NET_QUEUE) return false;
	unsigned tail = (sdx55_head + sdx55_count) % SDX55_NET_QUEUE;
	memcpy(sdx55_tx_queue[tail].bytes, p, total);
	sdx55_tx_queue[tail].size = (uint16_t)total;
	sdx55_count++;
	return true;
}

size_t
umi_sdx55_net_dequeue(void *out, size_t capacity)
{
	size_t size = 0;
	lck_mtx_lock(&cell_lock);
	if (sdx55_attached && session.up && sdx55_count && out &&
	    capacity >= sdx55_tx_queue[sdx55_head].size) {
		size = sdx55_tx_queue[sdx55_head].size;
		memcpy(out, sdx55_tx_queue[sdx55_head].bytes, size);
		sdx55_head = (sdx55_head + 1) % SDX55_NET_QUEUE;
		sdx55_count--;
	}
	lck_mtx_unlock(&cell_lock);
	return size;
}

bool
umi_sdx55_net_receive(const void *packet, size_t length)
{
	if (!length || sdx55_ipv4_size(packet, length) != length) return false;
	lck_mtx_lock(&cell_lock);
	bool accept = sdx55_attached && session.up;
	lck_mtx_unlock(&cell_lock);
	/* up, down and receive are serialized by the mission worker. */
	if (accept) cell_receive(CELL_MUX, packet, length);
	return accept;
}

int
umi_sdx55_net_up(uint32_t address, uint32_t mask, uint32_t gateway,
    uint32_t dns1, uint32_t dns2, uint32_t mtu)
{
	unsigned prefix = address >> 24;
	if (!prefix || prefix == 127 || prefix >= 224 || (mtu && mtu < 68)) return EINVAL;
	lck_mtx_lock(&cell_attach_lock);
	if (cell_attached || (!sdx55_attached && !hakios_net_attach(&sdx55_link))) {
		lck_mtx_unlock(&cell_attach_lock);
		return EBUSY;
	}
	lck_mtx_lock(&cell_lock);
	if (sdx55_attached && session.up) {
		lck_mtx_unlock(&cell_lock);
		lck_mtx_unlock(&cell_attach_lock);
		return EALREADY;
	}
	sdx55_attached = true;
	sdx55_head = sdx55_count = 0;
	session.modem_mask = mask;
	session.modem_gateway = gateway;
	session.address = address;
	/* WDS is point-to-point. Present an on-link router to the DHCP client;
	 * keep .0 or .255 bearer addresses usable as ordinary host addresses. */
	session.mask = 0xffffff00u;
	while (!(address & ~session.mask) || (address | session.mask) == UINT32_MAX)
		session.mask <<= 1;
	session.router = (address & session.mask) | 1u;
	if (session.router == address) session.router++;
	session.dns1 = dns1;
	session.dns2 = dns2;
	session.mtu = mtu && mtu < SDX55_NET_MTU ? mtu : SDX55_NET_MTU;
	session.up = true;
	lck_mtx_unlock(&cell_lock);
	hakios_net_link_state_unit(CELL_UNIT, true);
	lck_mtx_unlock(&cell_attach_lock);
	return 0;
}

void
umi_sdx55_net_down(void)
{
	lck_mtx_lock(&cell_attach_lock);
	lck_mtx_lock(&cell_lock);
	bool was_up = sdx55_attached && session.up;
	if (sdx55_attached) {
		bzero(&session, sizeof(session));
		sdx55_head = sdx55_count = 0;
	}
	lck_mtx_unlock(&cell_lock);
	if (was_up) hakios_net_link_state_unit(CELL_UNIT, false);
	lck_mtx_unlock(&cell_attach_lock);
}
