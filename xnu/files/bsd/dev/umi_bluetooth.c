/* UMI QCA6390 Bluetooth transport survey. Addresses/pins are from UMI's DT
 * and Xiaomi's GCC/GENI drivers. No UART FIFO, reset, regulator or clock is
 * written by stage 1. Stage 2 temporarily votes UART clocks on. */
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/hk_bluetooth.h>
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern bool umi_wifi_is_board(void);
extern void hk_tl(const char *, ...) __printflike(1, 2);
extern void loader_return(void);
static uint32_t bt_read(vm_offset_t base, uint32_t offset)
{
    uint32_t value = *(volatile uint32_t *)(base + offset);
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}
static void bt_write(vm_offset_t base, uint32_t offset, uint32_t value)
{
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile uint32_t *)(base + offset) = value;
    __asm__ volatile("dsb osh" ::: "memory");
}
/* Internal loopback exercises FIFO framing without driving the controller. */
static void bt_loopback(vm_offset_t uart)
{
    if ((bt_read(uart,0x40) & 0x1001) || bt_read(uart,0x800) || bt_read(uart,0x804)) {
        hk_tl("UMI BT: loopback skipped: sequencer/FIFO busy\n"); return;
    }
    static const uint32_t offsets[] = {0x48,0x4c,0x22c,0x258,0x25c,0x260,0x264,
        0x268,0x26c,0x270,0x280,0x284,0x288,0x28c,0x294,0x2a4,0x2a8,0x80c,0x810,0x814};
    uint32_t saved[sizeof(offsets)/sizeof(offsets[0])];
    for (unsigned i=0;i<sizeof(saved)/sizeof(saved[0]);i++) saved[i]=bt_read(uart,offsets[i]);
    bt_write(uart,0x48,0x41); bt_write(uart,0x4c,0x41);
    bt_write(uart,0x22c,1); bt_write(uart,0x258,0);
    bt_write(uart,0x25c,2); bt_write(uart,0x280,0);
    bt_write(uart,0x260,0xf); bt_write(uart,0x264,0);
    bt_write(uart,0x284,0x4380e); bt_write(uart,0x288,0xc3e0e);
    bt_write(uart,0x268,8); bt_write(uart,0x28c,8); bt_write(uart,0x26c,0);
    bt_write(uart,0x294,160); bt_write(uart,0x2a4,0); bt_write(uart,0x2a8,0);
    bt_write(uart,0x80c,1); bt_write(uart,0x810,0); bt_write(uart,0x814,6);
    bt_write(uart,0x618,0xffffffff); bt_write(uart,0x648,0xffffffff);
    bt_write(uart,0x630,(1u<<27)|0x80);
    static const uint8_t pattern[]={0x01,0x03,0x0c,0x00,0x55,0xaa,0x7e,0xff};
    bt_write(uart,0x270,sizeof(pattern)); bt_write(uart,0x600,1u<<27);
    for (unsigned i=0;i<sizeof(pattern);i++) bt_write(uart,0x700,pattern[i]);
    uint8_t received[sizeof(pattern)]; unsigned count=0, elapsed;
    for(elapsed=0;elapsed<20000 && count<sizeof(pattern);elapsed++) {
        uint32_t status=bt_read(uart,0x804), words=status&0x1ffffff;
        while(words--) {
            unsigned bytes=4;
            if (!words && (status&(1u<<31))) bytes=(status>>28)&7;
            uint32_t value=bt_read(uart,0x780);
            for(unsigned b=0;b<bytes;b++) {
                if(count<sizeof(received)) received[count++]=(value>>(8*b))&0xff;
            }
        }
        delay(1);
    }
    hk_tl("UMI BT: loopback %s: %u/%u bytes in %u us, IRQ=%#x/%#x\n",
        count==sizeof(pattern) && !memcmp(pattern,received,sizeof(pattern)) ? "PASS":"FAIL",
        count,(unsigned)sizeof(pattern),elapsed,bt_read(uart,0x610),bt_read(uart,0x640));
    for (unsigned i=0;i<count;i++) hk_tl("UMI BT: loopback byte%u=%#x expected=%#x\n",i,received[i],pattern[i]);
    bt_write(uart,0x604,4); bt_write(uart,0x634,4);
    unsigned waited;
    for(waited=0;waited<10000 && (bt_read(uart,0x40)&0x1001);waited++) delay(1);
    if(bt_read(uart,0x40)&0x1001) {
        bt_write(uart,0x604,2); bt_write(uart,0x634,2);
        for(waited=0;waited<10000 && (bt_read(uart,0x40)&0x1001);waited++) delay(1);
    }
    hk_tl("UMI BT: loopback cleanup status=%#x\n",bt_read(uart,0x40));
    if (!(bt_read(uart,0x40)&0x1001)) {
        for(unsigned i=0;i<sizeof(saved)/sizeof(saved[0]);i++) bt_write(uart,offsets[i],saved[i]);
        bt_write(uart,0x618,0xffffffff); bt_write(uart,0x648,0xffffffff);
    }
}
void umi_bluetooth_init(void)
{
    uint32_t probe = 0;
    if (!PE_parse_boot_argn("umi_bt_probe", &probe, sizeof(probe)) || probe < 1 || probe > 3) return;
    if (!umi_wifi_is_board()) { hk_tl("UMI BT: board identity mismatch\n"); return; }
    vm_offset_t gcc = ml_io_map(0x00100000, 0x80000);
    vm_offset_t gpio = ml_io_map(0x0f900000, 0x80000);
    if (!gcc || !gpio) { hk_tl("UMI BT: GCC/TLMM mapping failed\n"); loader_return(); return; }
    static const uint32_t clocks[] = {0x52008, 0x17004, 0x17008, 0x1772c, 0x17730, 0x17734, 0x17738, 0x1773c, 0x17740};
    for (unsigned i = 0; i < sizeof(clocks)/sizeof(clocks[0]); ++i)
        hk_tl("UMI BT: GCC %#x=%#x\n", clocks[i], bt_read(gcc, clocks[i]));
    static const unsigned pins[] = {16,17,18,19,21,124};
    for (unsigned i = 0; i < sizeof(pins)/sizeof(pins[0]); ++i)
        hk_tl("UMI BT: GPIO%u cfg=%#x io=%#x\n", pins[i], bt_read(gpio,pins[i]*0x1000), bt_read(gpio,pins[i]*0x1000+4));
    uint32_t added_votes = 0, saved_slave_hwcg = 0;
    if (probe >= 2) {
        uint32_t vote = bt_read(gcc, 0x52008), mask = (1u << 6) | (1u << 7) | (1u << 16);
        added_votes = mask & ~vote;
        saved_slave_hwcg = bt_read(gcc,0x17008) & 2u;
        bt_write(gcc,0x17008,bt_read(gcc,0x17008) & ~2u);
        bt_write(gcc, 0x52008, vote | mask);
        unsigned waited;
        for (waited = 0; waited < 10000; ++waited) {
            if (!((bt_read(gcc,0x17004) | bt_read(gcc,0x17008) | bt_read(gcc,0x1772c)) & 0x80000000)) break;
            delay(1);
        }
        hk_tl("UMI BT: temporary UART clock vote: %u us, CBCR=%#x/%#x/%#x\n", waited,
              bt_read(gcc,0x17004),bt_read(gcc,0x17008),bt_read(gcc,0x1772c));
    }
    /* Avoid unclocked GENI reads, which can stall the interconnect. */
    if ((bt_read(gcc,0x17004) | bt_read(gcc,0x17008) | bt_read(gcc,0x1772c)) & 0x80000000) {
        hk_tl("UMI BT: UART clocks halted; UART MMIO skipped\n");
    } else {
        vm_offset_t uart = ml_io_map(0x00998000, 0x4000);
        static const uint32_t regs[] = {0x40,0x48,0x4c,0x64,0x68,0x6c,0x7c,0x258,0x610,0x640,0x800,0x804,0xe24,0xe28};
        if (uart) for (unsigned i=0; i<sizeof(regs)/sizeof(regs[0]); ++i)
            hk_tl("UMI BT: GENI %#x=%#x\n",regs[i],bt_read(uart,regs[i]));
        if (uart && probe == 3) bt_loopback(uart);
    }
    if (probe >= 2) {
        bt_write(gcc,0x17008,(bt_read(gcc,0x17008) & ~2u) | saved_slave_hwcg);
        if (added_votes) bt_write(gcc,0x52008,bt_read(gcc,0x52008) & ~added_votes);
    }
    hk_tl("UMI BT: survey finished; no controller command sent\n");
    loader_return();
}
