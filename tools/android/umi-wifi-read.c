/* Read QCA6390's direct MHI register aperture under Android.
 * Build: aarch64-linux-gnu-gcc -O2 -Wall -Wextra -Werror -static -o umi-wifi-read umi-wifi-read.c
 * Run as root with PCI runtime PM held on by umi-wifi-read.sh.
 * No PCI configuration, BAR window, doorbell or MMIO register is written.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define DEVICE "/sys/bus/pci/devices/0000:01:00.0/"
#define MAP_SIZE 4096u

struct reg { const char *name; uint32_t offset; };

static uint32_t
read32(const volatile uint8_t *bar, uint32_t offset)
{
	uint32_t value = *(const volatile uint32_t *)(bar + offset);
	__asm__ volatile("dmb oshld" ::: "memory");
	return value;
}

static void
dump(const volatile uint8_t *bar, uint32_t base, const struct reg *regs, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		printf("%-20s %#06x = %#010x\n", regs[i].name, base + regs[i].offset,
		    read32(bar, base + regs[i].offset));
	}
}

static int
has_value(const char *path, const char *wanted)
{
	char value[32] = {0};
	int fd = open(path, O_RDONLY);
	if (fd < 0) return 0;
	ssize_t n = read(fd, value, sizeof(value) - 1);
	close(fd);
	return n > 0 && strcmp(value, wanted) == 0;
}

int
main(void)
{
	static const struct reg mhi[] = {
		{"MHIREGLEN", 0}, {"MHIVER", 8}, {"MHICFG", 0x10},
		{"CHDBOFF", 0x18}, {"ERDBOFF", 0x20}, {"BHIOFF", 0x28},
		{"BHIEOFF", 0x2c}, {"MHICTRL", 0x38}, {"MHISTATUS", 0x48},
		{"CCABAP_LO", 0x58}, {"CCABAP_HI", 0x5c},
		{"ECABAP_LO", 0x60}, {"ECABAP_HI", 0x64},
		{"CRCBAP_LO", 0x68}, {"CRCBAP_HI", 0x6c},
		{"CTRLBASE_LO", 0x80}, {"CTRLBASE_HI", 0x84},
		{"CTRLLIMIT_LO", 0x88}, {"CTRLLIMIT_HI", 0x8c},
		{"DATABASE_LO", 0x98}, {"DATABASE_HI", 0x9c},
		{"DATALIMIT_LO", 0xa0}, {"DATALIMIT_HI", 0xa4},
	};
	static const struct reg bhi[] = {
		{"BHI_VERSION_MINOR", 0}, {"BHI_VERSION_MAJOR", 4},
		{"BHI_EXECENV", 0x28}, {"BHI_STATUS", 0x2c}, {"BHI_ERRCODE", 0x30},
	};
	static const struct reg bhie[] = {
		{"BHIE_TXVECSTATUS", 0x44}, {"BHIE_RXVECSTATUS", 0x78},
	};
	if (!has_value(DEVICE "power/control", "on\n") ||
	    !has_value(DEVICE "power/runtime_status", "active\n")) {
		fprintf(stderr, "PCI runtime PM must be held on and active; refusing MMIO\n");
		return 1;
	}
	uint32_t id = 0;
	int fd = open(DEVICE "config", O_RDONLY);
	ssize_t n = fd < 0 ? -1 : pread(fd, &id, sizeof(id), 0);
	if (fd >= 0) close(fd);
	if (n != sizeof(id) || id != 0x110117cb) {
		fprintf(stderr, "Expected QCA6390 17cb:1101, read %#x; refusing MMIO\n", id);
		return 1;
	}
	fd = open(DEVICE "resource0", O_RDONLY | O_SYNC);
	if (fd < 0) { perror("open resource0"); return 1; }
	void *mapping = mmap(NULL, MAP_SIZE, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (mapping == MAP_FAILED) { perror("mmap resource0"); return 1; }
	const volatile uint8_t *bar = mapping;
	uint32_t version = read32(bar, 8);
	if (version == 0 || version == UINT32_MAX) {
		fprintf(stderr, "Invalid MHI version %#x; aborting capture\n", version);
		munmap(mapping, MAP_SIZE);
		return 1;
	}
	dump(bar, 0, mhi, sizeof(mhi) / sizeof(mhi[0]));
	uint32_t bhi_base = read32(bar, 0x28), bhie_base = read32(bar, 0x2c);
	if (bhi_base >= 0x100 && !(bhi_base & 3) && bhi_base <= MAP_SIZE - 0x34)
		dump(bar, bhi_base, bhi, sizeof(bhi) / sizeof(bhi[0]));
	else fprintf(stderr, "BHI offset %#x outside mapped aperture; skipped\n", bhi_base);
	if (bhie_base >= 0x100 && !(bhie_base & 3) && bhie_base <= MAP_SIZE - 0x7c)
		dump(bar, bhie_base, bhie, sizeof(bhie) / sizeof(bhie[0]));
	else fprintf(stderr, "BHIE offset %#x outside mapped aperture; skipped\n", bhie_base);
	munmap(mapping, MAP_SIZE);
	return 0;
}
