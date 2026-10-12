/*
 * Minimal GICv3 support for generic (non-Apple) ARMv8 boards.
 *
 * Open-source XNU has no interrupt controller driver: on Apple hardware that
 * is a kext (AppleInterruptController), and the timer arrives as FIQ. Here
 * everything is a Group 1 IRQ, which is what an EL1 guest gets from a stock
 * hypervisor or from QEMU's virt machine:
 *
 *   - the CPU interface is driven through the ICC_*_EL1 system registers
 *   - the EL1 virtual timer PPI is routed to the kernel's rtclock
 *   - any other interrupt that fires is masked and counted, until a real
 *     driver registers a handler for it
 *
 * Registers come from the device tree node /arm-io/gic:
 *   reg = <distributor offset, size, redistributor offset, size>
 */
#include <mach/mach_types.h>
#include <kern/debug.h>
#include <arm/cpu_data_internal.h>
#include <arm/machine_routines.h>
#include <arm/rtclock.h>
#include <arm64/proc_reg.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <pexpert/arm64/board_config.h>

#if defined(GENERIC_GIC_V3_IRQ)

#define GICD_CTLR               0x0000
#define GICD_CTLR_ENABLE_G1NS   (1u << 1)
#define GICD_CTLR_ARE_NS        (1u << 4)
#define GICD_CTLR_RWP           (1u << 31)
#define GICD_ICENABLER(n)       (0x0180 + 4 * (n))

#define GICR_FRAME_SIZE         0x20000     /* RD_base + SGI_base per CPU */
#define GICR_TYPER              0x0008
#define GICR_TYPER_LAST         (1u << 4)
#define GICR_WAKER              0x0014
#define GICR_WAKER_PROCSLEEP    (1u << 1)
#define GICR_WAKER_CHILDASLEEP  (1u << 2)
#define GICR_SGI_BASE           0x10000
#define GICR_IGROUPR0           (GICR_SGI_BASE + 0x0080)
#define GICR_ISENABLER0         (GICR_SGI_BASE + 0x0100)
#define GICR_ICENABLER0         (GICR_SGI_BASE + 0x0180)
#define GICR_IPRIORITYR(n)      (GICR_SGI_BASE + 0x0400 + (n))
#define GICR_IGRPMODR0          (GICR_SGI_BASE + 0x0d00)

#define GIC_INTID_VTIMER        27          /* EL1 virtual timer PPI */
#define GIC_INTID_SPURIOUS_MIN  1020
#define GIC_PRIORITY_DEFAULT    0xa0

static vm_offset_t gicd_base, gicr_base;
static vm_size_t   gicr_size;
static vm_offset_t gicr_cpu_base;           /* boot CPU's redistributor frame */

static uint64_t gic_active_iar;             /* acknowledged, not yet EOI'd */
uint64_t        gic_unhandled_count;
uint32_t        gic_last_unhandled;

void gic_v3_irq_cpu_init(void);
void gic_v3_irq_quiesce(void);

static inline uint32_t
gic_read32(vm_offset_t addr)
{
	return *(volatile uint32_t *)addr;
}

static inline void
gic_write32(vm_offset_t addr, uint32_t value)
{
	*(volatile uint32_t *)addr = value;
}

static void
gic_mask(uint32_t intid)
{
	if (intid < 32) {
		gic_write32(gicr_cpu_base + GICR_ICENABLER0, 1u << intid);
	} else {
		gic_write32(gicd_base + GICD_ICENABLER(intid / 32), 1u << (intid % 32));
	}
}

static void
gic_irq_handler(__unused void *target, __unused void *refCon, __unused void *nub, __unused int source)
{
	for (;;) {
		uint64_t iar = __builtin_arm_rsr64("ICC_IAR1_EL1");
		uint32_t intid = (uint32_t)(iar & 0xffffff);

		if (intid >= GIC_INTID_SPURIOUS_MIN) {
			break;
		}
		gic_active_iar = iar;

		if (intid == GIC_INTID_VTIMER) {
			/* same sequence as the FIQ path in sleh_fiq() */
			getCpuDatap()->cpu_decrementer = -1; /* Large */
			rtclock_intr(TRUE);
		} else {
			/* nobody owns this interrupt yet: keep it from storming */
			gic_mask(intid);
			gic_unhandled_count++;
			gic_last_unhandled = intid;
		}

		__builtin_arm_wsr64("ICC_EOIR1_EL1", iar);
		__builtin_arm_isb(ISB_SY);
		gic_active_iar = 0;
	}
}

/*
 * Leave the controller in a state another environment can take over from:
 * finish any interrupt that was acknowledged but not completed. Used when
 * control goes back to the boot loader (see loader_return.c).
 */
void
gic_v3_irq_quiesce(void)
{
	if (gic_active_iar != 0) {
		__builtin_arm_wsr64("ICC_EOIR1_EL1", gic_active_iar);
		__builtin_arm_isb(ISB_SY);
		gic_active_iar = 0;
	}
}

static vm_offset_t
gic_find_redistributor(void)
{
	uint64_t mpidr = __builtin_arm_rsr64("MPIDR_EL1");
	/* GICR_TYPER[63:32] packs Aff3.Aff2.Aff1.Aff0 */
	uint32_t affinity = (uint32_t)(((mpidr >> 32) & 0xff) << 24 | (mpidr & 0xffffff));

	for (vm_offset_t frame = gicr_base; frame + GICR_FRAME_SIZE <= gicr_base + gicr_size; frame += GICR_FRAME_SIZE) {
		uint64_t typer = *(volatile uint64_t *)(frame + GICR_TYPER);

		if ((uint32_t)(typer >> 32) == affinity) {
			return frame;
		}
		if (typer & GICR_TYPER_LAST) {
			break;
		}
	}
	panic("gic: no redistributor for MPIDR 0x%llx", mpidr);
}

/* Per-CPU bring-up; only the boot CPU is supported for now. */
void
gic_v3_irq_cpu_init(void)
{
	if (gicd_base == 0) {
		DTEntry entry;
		void const *prop;
		unsigned int prop_size;

		if (SecureDTLookupEntry(NULL, "/arm-io/gic", &entry) != kSuccess ||
		    SecureDTGetProperty(entry, "reg", &prop, &prop_size) != kSuccess ||
		    prop_size < 4 * sizeof(uint64_t)) {
			panic("gic: no usable /arm-io/gic node in the device tree");
		}
		uint64_t const *reg = prop;
		vm_offset_t soc_base = pe_arm_get_soc_base_phys();

		gicd_base = ml_io_map(soc_base + reg[0], reg[1]);
		gicr_base = ml_io_map(soc_base + reg[2], reg[3]);
		gicr_size = reg[3];
		if (gicd_base == 0 || gicr_base == 0) {
			panic("gic: cannot map the distributor/redistributor");
		}
	} else if (gicr_cpu_base != 0) {
		return;
	}

	/* System-register CPU interface */
	__builtin_arm_wsr64("ICC_SRE_EL1", __builtin_arm_rsr64("ICC_SRE_EL1") | 1);
	__builtin_arm_isb(ISB_SY);

	gicr_cpu_base = gic_find_redistributor();

	uint32_t waker = gic_read32(gicr_cpu_base + GICR_WAKER);
	if (waker & (GICR_WAKER_PROCSLEEP | GICR_WAKER_CHILDASLEEP)) {
		gic_write32(gicr_cpu_base + GICR_WAKER, waker & ~GICR_WAKER_PROCSLEEP);
		while (gic_read32(gicr_cpu_base + GICR_WAKER) & GICR_WAKER_CHILDASLEEP) {
			/* spin until the redistributor is awake */
		}
	}

	/* Virtual timer PPI: Group 1 (IRQ), default priority, enabled */
	gic_write32(gicr_cpu_base + GICR_IGROUPR0, gic_read32(gicr_cpu_base + GICR_IGROUPR0) | (1u << GIC_INTID_VTIMER));
	gic_write32(gicr_cpu_base + GICR_IGRPMODR0, gic_read32(gicr_cpu_base + GICR_IGRPMODR0) & ~(1u << GIC_INTID_VTIMER));
	*(volatile uint8_t *)(gicr_cpu_base + GICR_IPRIORITYR(GIC_INTID_VTIMER)) = GIC_PRIORITY_DEFAULT;
	gic_write32(gicr_cpu_base + GICR_ISENABLER0, 1u << GIC_INTID_VTIMER);

	/* Distributor: affinity routing, Group 1 enabled (normally already so) */
	uint32_t ctlr = gic_read32(gicd_base + GICD_CTLR);
	if ((ctlr & (GICD_CTLR_ENABLE_G1NS | GICD_CTLR_ARE_NS)) != (GICD_CTLR_ENABLE_G1NS | GICD_CTLR_ARE_NS)) {
		gic_write32(gicd_base + GICD_CTLR, ctlr | GICD_CTLR_ENABLE_G1NS | GICD_CTLR_ARE_NS);
		while (gic_read32(gicd_base + GICD_CTLR) & GICD_CTLR_RWP) {
			/* wait for the write to take effect */
		}
	}

	/* CPU interface: accept all priorities, no preemption grouping, Group 1 on */
	__builtin_arm_wsr64("ICC_PMR_EL1", 0xff);
	__builtin_arm_wsr64("ICC_BPR1_EL1", 0);
	__builtin_arm_wsr64("ICC_IGRPEN1_EL1", 1);
	__builtin_arm_isb(ISB_SY);

	ml_install_interrupt_handler(NULL, 0, NULL, gic_irq_handler, NULL);
	kprintf("gic: v3, distributor %p, redistributor %p, virtual timer on PPI %d\n",
	    (void *)gicd_base, (void *)gicr_cpu_base, GIC_INTID_VTIMER);
}

#endif /* GENERIC_GIC_V3_IRQ */
