/* UMI vendor actuator umi_ak7377_s5khmx: slave 0x18, DAC register0,
 * byte address/word data, dataShift6; control2=0 plus 1ms init delay.
 * Limit the prototype to DAC150..750 (vendor initial150); this is not an
 * assertion of calibrated infinity/macro endpoints. ABI positions are scaled
 * to that conservative window. Each synchronous capture owns VAF separately. */
#define UFOCUS_MIN 150u
#define UFOCUS_MAX 750u
#define UFOCUS_STEP 24u
static bool ufocus_owned;
static uint32_t ufocus_position = HAKIOS_CAMERA_NO_FOCUS;

static bool
ufocus_read_position(uint32_t *position)
{
    uint8_t bytes[2];
    if (!ufocus_bus_read(0, bytes, 2)) { hakios_tl("UMI focus: position register read failed\n"); return false; }
    uint32_t packed = (uint32_t)bytes[0] << 8 | bytes[1];
    if (packed & 63) {
        uint8_t hi=0,lo=0;
        bool one=ufocus_bus_read(0,&hi,1),two=ufocus_bus_read(1,&lo,1);
        hakios_tl("UMI focus: position packed=%#x unexpected low bits; single bytes=%#x/%#x ACK=%u/%u\n",packed,hi,lo,one,two);
        return false;
    }
    *position = packed >> 6;
    return true;
}

static bool
ufocus_walk(uint32_t target)
{
    if (target < UFOCUS_MIN || target > UFOCUS_MAX || ufocus_position == HAKIOS_CAMERA_NO_FOCUS) return false;
    for (unsigned step = 0; step < 26 && ufocus_position != target; step++) {
        uint32_t now = ufocus_position;
        uint32_t next = now < target ? (target-now > UFOCUS_STEP ? now+UFOCUS_STEP : target) :
            (now-target > UFOCUS_STEP ? now-UFOCUS_STEP : target);
        if (!ufocus_bus_write(0, (uint16_t)(next << 6), 2)) return false;
        ufocus_position = next;
        ums(3);
    }
    uint32_t readback;
    return ufocus_position == target && ufocus_read_position(&readback) && readback == target;
}

static void
ufocus_down(bool panic)
{
    if (!ufocus_owned) return;
    if (!panic && !ufocus_walk(UFOCUS_MIN)) hakios_tl("UMI focus: park/readback failed; requesting standby\n");
    if (!ufocus_bus_write(2, 0x40, 1)) hakios_tl("UMI focus: standby did not acknowledge; VAF cleanup follows\n");
    ufocus_owned = false;
    ufocus_position = HAKIOS_CAMERA_NO_FOCUS;
}

static int
ufocus_up(uint32_t logical)
{
    if (logical > 1023) return EINVAL;
    if (ufocus_owned) return EBUSY;
    uint8_t mode;
    if (!ufocus_bus_read(2, &mode, 1)) { hakios_tl("UMI focus: control register2 read failed\n"); return EIO; }
    hakios_tl("UMI focus: control register2=%#x\n",mode);
    /* Hardware powers on with mode0x20 and position bytes00/ff. Those
     * pre-initialization bytes are not a valid DAC report. Match the vendor
     * initialization first; require position readback only after programming. */
    if (mode != 0 && mode != 0x20 && mode != 0x40) { hakios_tl("UMI focus: unexpected mode=%#x\n",mode); return EBUSY; }
    /* Match the vendor init order: control2=0, 1ms delay, initial DAC150.
     * Hardware refused the extra standby-position preload. Mark ownership
     * before the first write so a partial init still requests standby. */
    ufocus_owned = true;
    if (!ufocus_bus_write(2, 0, 1)) { hakios_tl("UMI focus: activate failed\n"); goto failed; }
    ums(1);
    if (!ufocus_bus_write(0, UFOCUS_MIN << 6, 2)) { hakios_tl("UMI focus: initial position failed\n"); goto failed; }
    ufocus_position = UFOCUS_MIN;
    uint32_t target = UFOCUS_MIN + logical * (UFOCUS_MAX-UFOCUS_MIN) / 1023;
    if (!ufocus_walk(target)) { hakios_tl("UMI focus: walk/readback target=%u failed\n",target); goto failed; }
    ums(10);
    hakios_tl("UMI focus: AK7377 mode=%#x DAC=%u logical=%u readback OK\n", mode, target, logical);
    return 0;
failed:
    ufocus_down(true);
    return EIO;
}

/* An open iOS session retains VAF and its confirmed lens position. A stable
 * target is only checked, never reinitialized or parked between frames. */
static int
ufocus_apply(uint32_t logical)
{
    if (logical > 1023) return EINVAL;
    if (!ufocus_owned) return ufocus_up(logical);
    uint32_t target=UFOCUS_MIN+logical*(UFOCUS_MAX-UFOCUS_MIN)/1023;
    bool moved=target!=ufocus_position;
    if (!ufocus_walk(target)) {
        ufocus_down(true);
        return EIO;
    }
    if (moved) {
        ums(10);
        hakios_tl("UMI focus: retained session DAC=%u logical=%u readback OK\n",target,logical);
    }
    return 0;
}
