/* Strict, bounded RWCTAB02 loader for the verified UMI S5K3T2 module.
 * Validate the complete container before applying init/res1; never stream on. */
static int
ufront_mode_prepare(void)
{
	uint64_t size = 0;
	const char *name = "camera-s5k3t2.tab";
	const uint8_t *file = hakios_wifi_file(name, &size);
	if (!file || size < 40 || size > 65536 || memcmp(file, "RWCTAB02", 8) ||
	    utab_u32(file+8) != 0x20 || utab_u32(file+12) != 2 || utab_u32(file+16) != 2 ||
	    utab_u32(file+20) != 0 || utab_u32(file+24) != 0x3142 ||
	    utab_u32(file+32) != 0x3210 || utab_u32(file+36) != 3) return EINVAL;
	uint64_t offset = 40 + 3*48;
	if (offset > size) return EINVAL;
	const uint8_t *mode = file + 40 + 1*48;
	if (utab_u32(mode) != 2592 || utab_u32(mode+4) != 1940 ||
	    utab_u32(mode+16) != 4 || utab_u32(mode+20) != 0 || utab_u32(mode+44) != 0x2b) return EINVAL;
	for (unsigned p = 0; p < 2; ++p) {
		if (size-offset < 12) return EINVAL;
		uint64_t bytes = 12 + 12ull*utab_u32(file+offset+8);
		if (bytes > size-offset) return EINVAL;
		offset += bytes;
	}
	if (size-offset < 4) return EINVAL;
	unsigned tables = utab_u32(file+offset); offset += 4;
	if (!tables || tables > 32) return EINVAL;
	const uint8_t *selected[2] = { NULL, NULL };
	unsigned counts[2] = { 0, 0 };
	for (unsigned t = 0; t < tables; ++t) {
		if (size-offset < 12) return EINVAL;
		const uint8_t *table = file+offset;
		unsigned count = utab_u32(table+8);
		uint64_t bytes = 12 + 12ull*count;
		if (bytes > size-offset) return EINVAL;
		int slot = !memcmp(table, "init\0\0\0\0", 8) ? 0 :
		    !memcmp(table, "res1\0\0\0\0", 8) ? 1 : -1;
		if (slot >= 0) {
			if (selected[slot] || !count || count > 4096) return EINVAL;
			selected[slot] = table+12; counts[slot] = count;
			for (unsigned i = 0; i < count; ++i) {
				const uint8_t *e = table+12+12*i;
				if (e[4] != 2 || e[5] != 2 || e[6] || e[7] || utab_u32(e+8) > 100000 ||
				    (e[0] == 0 && e[1] == 1 && (e[2] || e[3]))) return EINVAL;
			}
		}
		offset += bytes;
	}
	if (offset != size || !selected[0] || !selected[1]) return EINVAL;
	hakios_tl("UMI front: initialize S5K3T2 mode1 without stream-on (%u+%u writes)\n", counts[0], counts[1]);
	for (unsigned t = 0; t < 2; ++t)
		for (unsigned i = 0; i < counts[t]; ++i) {
			const uint8_t *e = selected[t]+12*i;
			uint16_t reg = (uint16_t)(e[0] | e[1] << 8), value = (uint16_t)(e[2] | e[3] << 8);
			if (!usensor_write(reg, value)) {
				hakios_tl("UMI camera: table %u entry %u register %#x NACK\n", t, i, reg);
				return EIO;
			}
			unsigned us = utab_u32(e+8);
			if (us >= 1000) ums(us/1000);
			if (us%1000) delay(us%1000);
		}
	/* Restore the indirect-write page before verifying direct output registers. */
	if (!usensor_write(0x6028, 0x4000)) return EIO;
	static const unsigned regs[5] = { 0x034c, 0x034e, 0x0342, 0x0340, 0x0100 };
	const unsigned expected[5] = { 2592, 1940, utab_u32(mode+32), utab_u32(mode+36), 0 };
	for (unsigned i = 0; i < 5; ++i) {
		uint16_t value = 0;
		if (!usensor_read(regs[i], &value)) return EIO;
		hakios_tl("UMI camera: mode register %#x=%#x expected %#x\n", regs[i], value, expected[i]);
		if (value != expected[i]) return EIO;
	}
	return 0;
}
