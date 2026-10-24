/* Software model for UMI's PCIe bootstrap; never accesses a real device. */
#include <sys/umi_pcie.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GCC 0x00100000u
#define GPIO 0x0f900000u
#define PARF 0x01c00000u
#define PHY 0x01c06000u
#define DBI 0x60000000u
#define ATU 0x60001000u
#define CONF 0x60100000u
#define BAR 0x60300000u
#define BIT(n) (1u << (n))
struct region { uint32_t base, size; uint32_t *words; };
struct model {
	struct region regions[8];
	unsigned writes, phy_writes, delay, conf_reads, bar_reads;
	bool powered, gdsc, clocks, perst_released, link, cfg_enabled, bar_enabled;
	bool fail_power, fail_gdsc, fail_clock, fail_phy, fail_link, fail_atu, wrong_id, wrong_mhi;
};
static uint32_t *reg(struct model *m, uint32_t addr)
{
	assert(!(addr & 3));
	for (unsigned i = 0; i < 8; i++)
		if (addr >= m->regions[i].base && addr - m->regions[i].base < m->regions[i].size)
			return m->regions[i].words + (addr - m->regions[i].base) / 4;
	assert(!"access outside UMI register map");
	return NULL;
}
static uint32_t rd(void *context, uint32_t addr)
{
	struct model *m = context;
	if ((addr >= PARF && addr < PARF + 0x3000) ||
	    (addr >= PHY && addr < PHY + 0x1000) ||
	    (addr >= DBI && addr < DBI + 0x2000)) assert(m->powered && m->gdsc && m->clocks);
	if (addr >= CONF && addr < CONF + 0x1000) {
		assert(m->link && m->cfg_enabled);
		m->conf_reads++;
	}
	if (addr >= BAR && addr < BAR + 0x1000) {
		assert(m->bar_enabled && (*reg(m, CONF + 4) & 7) == 2);
		m->bar_reads++;
	}
	return *reg(m, addr);
}
static void wr(void *context, uint32_t addr, uint32_t value)
{
	struct model *m = context;
	m->writes++;
	if ((addr >= PARF && addr < PARF + 0x3000) ||
	    (addr >= PHY && addr < PHY + 0x1000) ||
	    (addr >= DBI && addr < DBI + 0x2000)) assert(m->powered && m->gdsc && m->clocks);
	if (addr >= CONF && addr < CONF + 0x1000) assert(m->link && m->cfg_enabled);
	if (addr == CONF + 4 || addr == DBI + 4) assert(!(value & 4)); /* Never DMA. */
	if (addr == GPIO + 79 * 0x1000 + 4) m->perst_released = (value & 2) != 0;
	if (addr >= PHY && addr < PHY + 0x1000) {
		assert(!m->perst_released);
		m->phy_writes++;
	}
	*reg(m, addr) = value;
	if (addr == GCC + 0x6b004 && !(value & 1) && !m->fail_gdsc) {
		assert(m->powered && !m->perst_released);
		*reg(m, addr) |= BIT(31);
		m->gdsc = true;
	}
	if (addr == GCC + 0x6b038 || addr == GCC + 0x6f014) *reg(m, addr) &= ~1u;
	if (addr == GCC + 0x52008 && (value & 0x2f) == 0x2f) {
		m->clocks = !m->fail_clock;
		const uint32_t gates[] = {0x6b010,0x6b014,0x6b01c,0x6b024,0x6b028};
		if (!m->fail_clock)
			for (unsigned i = 0; i < 5; i++) *reg(m, GCC + gates[i]) &= ~BIT(31);
		if (value & BIT(4)) {
			assert(m->phy_writes == 99);
			if (!m->fail_phy) *reg(m, PHY + 0x814) = 0;
		}
	}
	if (addr == PARF + 0x1b0 && (value & BIT(8))) {
		assert(m->perst_released && m->phy_writes == 99);
		if (!m->fail_link) {
			*reg(m, DBI + 0xf28) |= BIT(10);
			*reg(m, DBI + 0x80) = BIT(29) | 0x00120000;
			m->link = true;
		}
	}
	if (addr == ATU + 4 && (value & BIT(31))) {
		if (m->fail_atu) { *reg(m, addr) = 0; return; }
		assert(*reg(m, ATU) == 4 && *reg(m, ATU + 8) == CONF);
		assert(*reg(m, ATU + 16) == CONF + 0xfff && *reg(m, ATU + 20) == 0x01000000);
		m->cfg_enabled = true;
	}
	if (addr == ATU + 0x204 && (value & BIT(31))) {
		assert((*reg(m, CONF + 4) & 7) == 0);
		assert(*reg(m, ATU + 0x200) == 0 && *reg(m, ATU + 0x208) == BAR);
		assert(*reg(m, ATU + 0x210) == BAR + 0xfffff && *reg(m, ATU + 0x214) == BAR);
		m->bar_enabled = true;
	}
}
static int power_on(void *context)
{
	struct model *m = context;
	assert(!m->perst_released);
	if (m->fail_power) return EIO;
	m->powered = true;
	return 0;
}
static void delay_us(void *context, unsigned us)
{
	((struct model *)context)->delay += us;
}
static void setup(struct model *m, struct umi_pcie *p, struct umi_pcie_io *io)
{
	memset(m, 0, sizeof(*m));
	memset(p, 0, sizeof(*p));
	m->regions[0] = (struct region){GCC, 0xa0000, NULL};
	m->regions[1] = (struct region){GPIO, 0x80000, NULL};
	m->regions[2] = (struct region){PARF, 0x3000, NULL};
	m->regions[3] = (struct region){PHY, 0x1000, NULL};
	m->regions[4] = (struct region){DBI, 0x1000, NULL};
	m->regions[5] = (struct region){ATU, 0x1000, NULL};
	m->regions[6] = (struct region){CONF, 0x1000, NULL};
	m->regions[7] = (struct region){BAR, 0x1000, NULL};
	for (unsigned i = 0; i < 8; i++) {
		m->regions[i].words = calloc(1, m->regions[i].size);
		assert(m->regions[i].words);
	}
	*reg(m, GCC + 0x6b004) = 1;
	const uint32_t gates[] = {0x6b010,0x6b014,0x6b01c,0x6b024,0x6b028};
	for (unsigned i = 0; i < 5; i++) *reg(m, GCC + gates[i]) = BIT(31);
	*reg(m, PHY + 0x814) = BIT(6);
	*reg(m, CONF) = 0x110117cb;
	*reg(m, CONF + 0x10) = 4; /* 64-bit BAR */
	*reg(m, BAR + 8) = 0x01000000;
	*io = (struct umi_pcie_io){.context=m,.read32=rd,.write32=wr,
	    .power_on=power_on,.delay_us=delay_us};
}
static void cleanup(struct model *m)
{
	for (unsigned i = 0; i < 8; i++) free(m->regions[i].words);
}
int main(void)
{
	struct model m;
	struct umi_pcie p;
	struct umi_pcie_io io;
	setup(&m, &p, &io);
	assert(umi_pcie_enumerate(&p, &io) == 0 && p.enumerated && !p.failed);
	assert(m.conf_reads && m.bar_reads == 1 && m.phy_writes == 99);
	assert((*reg(&m, CONF + 4) & 7) == 2 && (*reg(&m, DBI + 4) & 7) == 2);
	assert((*reg(&m, CONF + 0x10) & ~15u) == BAR && *reg(&m, CONF + 0x14) == 0);
	unsigned writes = m.writes;
	assert(umi_pcie_enumerate(&p, &io) == EBUSY && m.writes == writes);
	umi_pcie_quiesce(&p);
	assert(!p.enumerated && !m.perst_released && !(*reg(&m, GPIO + 20 * 0x1000 + 4) & 2));
	cleanup(&m);
	puts("PASS: power/reset/PHY/link/config/BAR ordering, no DMA, endpoint quiesce");

	for (unsigned fault = 0; fault < 8; fault++) {
		setup(&m, &p, &io);
		switch (fault) {
		case 0: m.fail_power = true; break;
		case 1: m.fail_gdsc = true; break;
		case 2: m.fail_clock = true; break;
		case 3: m.fail_phy = true; break;
		case 4: m.fail_link = true; break;
		case 5: m.fail_atu = true; break;
		case 6: *reg(&m, CONF) = 0x12345678; break;
		case 7: *reg(&m, BAR + 8) = 0xffffffff; break;
		}
		assert(umi_pcie_enumerate(&p, &io) != 0 && p.failed && !p.enumerated);
		assert(!m.perst_released && !(*reg(&m, GPIO + 20 * 0x1000 + 4) & 2));
		assert(m.delay < 2000000);
		if (fault < 3) assert(!m.phy_writes);
		if (fault < 6) assert(!m.conf_reads);
		if (fault < 7) assert(!m.bar_reads);
		cleanup(&m);
	}
	puts("PASS: power/GDSC/clock/PHY/link/iATU/identity/MHI failures stop and assert reset");
	setup(&m, &p, &io);
	*reg(&m, BAR) = 0x100;
	*reg(&m, BAR + 8) = 0xdeadbeef;
	*reg(&m, BAR + 0x18) = 0x300;
	*reg(&m, BAR + 0x20) = 0x700;
	*reg(&m, BAR + 0x28) = 0x100;
	*reg(&m, BAR + 0x2c) = 0x224;
	*reg(&m, BAR + 0x104) = 1;
	assert(umi_pcie_enumerate(&p, &io) == 0 && p.enumerated);
	umi_pcie_quiesce(&p);
	cleanup(&m);
	puts("PASS: real UMI cold-PBL signature enumerates without MHIVER initialized");
	return 0;
}
