/* UMI QCA6390 Bluetooth transport survey. Addresses/pins are from UMI's DT
 * and Xiaomi's GCC/GENI drivers. No UART FIFO, reset, regulator or clock is
 * written by stage 1. Stage 2 temporarily votes UART clocks on. */
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/hk_bluetooth.h>
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern bool umi_wifi_is_board(void);
extern void hk_tl(const char *, ...) __printflike(1, 2);
extern void loader_return(void);
extern int umi_rpmh_bluetooth_power(void);
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
/* Set the documented QUPv3 fractional root to 7.3728 MHz. GPLL0_OUT_EVEN
 * is parent selector 6; 384/15625 gives an exact UART sampling clock. */
static bool bt_uart_clock(vm_offset_t gcc)
{
    bt_write(gcc,0x17738,384);
    bt_write(gcc,0x1773c,~(15625u-384u)&0xffff);
    bt_write(gcc,0x17740,~15625u&0xffff);
    bt_write(gcc,0x17734,0x2601); /* dual-edge M/N, GPLL0-even, /1 */
    bt_write(gcc,0x17730,1);
    for(unsigned i=0;i<10000;i++) {
        if(!(bt_read(gcc,0x17730)&1)) return true;
        delay(1);
    }
    return false;
}
static void bt_gpio(vm_offset_t gpio,unsigned pin,unsigned cfg,bool high)
{
    bt_write(gpio,pin*0x1000+4,high?2:0);
    bt_write(gpio,pin*0x1000,cfg);
}
/* Power the radio, issue HCI Reset and Read Local Version, then leave reset
 * asserted. Firmware download is a separate step after this ROM handshake. */
static void bt_hci_probe(vm_offset_t gcc,vm_offset_t gpio,vm_offset_t uart)
{
    uint32_t root[5]={bt_read(gcc,0x17730),bt_read(gcc,0x17734),bt_read(gcc,0x17738),bt_read(gcc,0x1773c),bt_read(gcc,0x17740)};
    uint32_t pin21=bt_read(gpio,21*0x1000),io21=bt_read(gpio,21*0x1000+4),pin124=bt_read(gpio,124*0x1000),io124=bt_read(gpio,124*0x1000+4);
    uint32_t pins[4]; for(unsigned p=0;p<4;p++) pins[p]=bt_read(gpio,(16+p)*0x1000);
    static const uint32_t uregs[]={0x48,0x4c,0x22c,0x258,0x25c,0x260,0x264,0x268,0x26c,0x270,0x280,0x284,0x288,0x28c,0x294,0x2a4,0x2a8,0x80c,0x810,0x814};
    uint32_t uvals[sizeof(uregs)/sizeof(uregs[0])];
    for(unsigned p=0;p<sizeof(uvals)/sizeof(uvals[0]);p++) uvals[p]=bt_read(uart,uregs[p]);
    uint32_t vote=bt_read(gcc,0x52008),mask=(1u<<6)|(1u<<7)|(1u<<16);
    uint32_t hwcg=bt_read(gcc,0x17008)&2u, added=mask&~vote;
    bt_write(gcc,0x17008,bt_read(gcc,0x17008)&~2u);
    bt_write(gcc,0x52008,vote|mask);
    unsigned i;
    for(i=0;i<10000;i++) if(!((bt_read(gcc,0x17004)|bt_read(gcc,0x17008)|bt_read(gcc,0x1772c))&0x80000000)) break; else delay(1);
    int error= i==10000 ? ETIMEDOUT : 0;
    if(!error && !bt_uart_clock(gcc)) error=ETIMEDOUT;
    if(!error) error=umi_rpmh_bluetooth_power();
    if(error) { hk_tl("UMI BT: power/clock setup failed errno=%d\n",error); goto cleanup; }
    /* Reset low, sideband awake, then release reset as the Android driver does. */
    bt_gpio(gpio,21,0x241, false); /* GPIO, 4 mA, pull-down, output */
    bt_gpio(gpio,124,0x240, true); /* GPIO, 4 mA, no pull, output */
    delay(56000); /* stock bt_power holds reset low for about 56 ms */
    bt_gpio(gpio,21,0x241, true);
    delay(56000);
    /* UART pins 16..19 use QUP6 alternate function 1. */
    for(unsigned pin=16;pin<=19;pin++) {
        uint32_t cfg=pin==17 ? 5u : (pin==18 ? 7u : 4u);
        bt_write(gpio,pin*0x1000,cfg);
    }
    bt_write(uart,0x48,0x41); bt_write(uart,0x4c,0x41);
    bt_write(uart,0x258,0); bt_write(uart,0x22c,0);
    bt_write(uart,0x25c,2); bt_write(uart,0x280,0);
    bt_write(uart,0x260,0xf); bt_write(uart,0x264,0);
    bt_write(uart,0x284,0x4380e); bt_write(uart,0x288,0xc3e0e);
    bt_write(uart,0x268,8); bt_write(uart,0x28c,8); bt_write(uart,0x26c,0);
    bt_write(uart,0x294,160); bt_write(uart,0x2a4,0); bt_write(uart,0x2a8,0);
    bt_write(uart,0x80c,1); bt_write(uart,0x810,0); bt_write(uart,0x814,6);
    bt_write(uart,0x618,0xffffffff); bt_write(uart,0x648,0xffffffff);
    bt_write(uart,0x630,(1u<<27)|0x80); /* START_READ, RFR_OPEN */
    const uint8_t cmd[]={0x01,0x03,0x0c,0x00};
    bt_write(uart,0x270,sizeof(cmd)); bt_write(uart,0x600,1u<<27);
    for(unsigned b=0;b<sizeof(cmd);b++) bt_write(uart,0x700,cmd[b]);
    uint8_t data[32]; unsigned n=0;
    for(unsigned us=0;us<300000 && n<7;us++) {
        uint32_t st=bt_read(uart,0x804),words=st&0x1ffffff;
        while(words--) { unsigned bytes=4; if(!words&&(st>>31)) bytes=(st>>28)&7; uint32_t w=bt_read(uart,0x780);
            for(unsigned b=0;b<bytes;b++) if(n<sizeof(data)) data[n++]=(w>>(b*8))&0xff; }
        if(n>=7) break; delay(1);
    }
    hk_tl("UMI BT: HCI reset response bytes=%u MIRQ=%#x SIRQ=%#x RX=%#x GPIO16=%#x GPIO19=%#x:",n,bt_read(uart,0x610),bt_read(uart,0x640),bt_read(uart,0x804),bt_read(gpio,16*0x1000+4),bt_read(gpio,19*0x1000+4));
    for(unsigned b=0;b<n;b++) hk_tl(" %02x",data[b]);
    hk_tl("\n");
    if(n>=7 && data[0]==4 && data[1]==0x0e && data[3]==1 && data[4]==3 && data[5]==0x0c && data[6]==0) {
        hk_tl("UMI BT: controller answered HCI Reset; requesting local version\n");
        const uint8_t ver[]={0x01,0x01,0x10,0x00};
        bt_write(uart,0x618,0xffffffff); bt_write(uart,0x270,sizeof(ver)); bt_write(uart,0x600,1u<<27);
        for(unsigned b=0;b<sizeof(ver);b++) bt_write(uart,0x700,ver[b]);
        n=0; for(unsigned us=0;us<300000 && n<15;us++) {
            uint32_t st=bt_read(uart,0x804),words=st&0x1ffffff;
            while(words--) { unsigned bytes=4; if(!words&&(st>>31)) bytes=(st>>28)&7; uint32_t w=bt_read(uart,0x780);
                for(unsigned b=0;b<bytes;b++) if(n<sizeof(data)) data[n++]=(w>>(b*8))&0xff; }
            if(n>=15) break; delay(1);
        }
        hk_tl("UMI BT: HCI version response bytes=%u:",n); for(unsigned b=0;b<n;b++) hk_tl(" %02x",data[b]); hk_tl("\n");
    }
    bt_write(uart,0x604,4); bt_write(uart,0x634,4);
    for(i=0;i<10000 && (bt_read(uart,0x40)&0x1001);i++) delay(1);
    if(bt_read(uart,0x40)&0x1001) { bt_write(uart,0x604,2); bt_write(uart,0x634,2); }
    bt_gpio(gpio,21,0x241,false); /* leave the controller held in reset */
cleanup:
    bt_write(gpio,21*0x1000+4,io21&2); bt_write(gpio,21*0x1000,pin21);
    bt_write(gpio,124*0x1000+4,io124&2); bt_write(gpio,124*0x1000,pin124);
    for(unsigned p=0;p<4;p++) bt_write(gpio,(16+p)*0x1000,pins[p]);
    bt_write(gcc,0x17738,root[2]); bt_write(gcc,0x1773c,root[3]); bt_write(gcc,0x17740,root[4]);
    bt_write(gcc,0x17734,root[1]); bt_write(gcc,0x17730,root[0]|1);
    for(i=0;i<10000 && (bt_read(gcc,0x17730)&1);i++) delay(1);
    if(added) bt_write(gcc,0x52008,bt_read(gcc,0x52008)&~added);
    bt_write(gcc,0x17008,(bt_read(gcc,0x17008)&~2u)|hwcg);
}
void umi_bluetooth_init(void)
{
    uint32_t probe = 0;
    if (!PE_parse_boot_argn("umi_bt_probe", &probe, sizeof(probe)) || probe < 1 || probe > 4) return;
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
        if (uart && probe == 4) bt_hci_probe(gcc,gpio,uart);
    }
    if (probe >= 2) {
        bt_write(gcc,0x17008,(bt_read(gcc,0x17008) & ~2u) | saved_slave_hwcg);
        if (added_votes) bt_write(gcc,0x52008,bt_read(gcc,0x52008) & ~added_votes);
    }
    hk_tl("UMI BT: diagnostic finished\n");
    loader_return();
}
