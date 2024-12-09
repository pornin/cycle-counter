/*
 * This module activates the in-CPU cycle counter and enables direct access
 * from unprivileged (userland) code, for benchmark purposes. It can be used
 * on ARMv8 (aarch64) and some RISC-V (riscv64) systems.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/smp.h>

#if defined(__aarch64__)

static void
enable_counter(void *data)
{
	/*
	 * ARMv8: the counter is PMCCNTR_EL0. Its behaviour and access is
	 * controlled by several other registers:
	 *
	 * PMINTENCLR_EL1
	 *    Controls generation of interrupts on cycle counter overflow.
	 *    We do not want such interrupts; to disable them, we write
	 *    1 in bit 31 of the register.
	 *
	 * PMCNTENSET_EL0
	 *    Controls whether the cycle counter is active. We enable the
	 *    cycle counter by writing 1 in bit 31.
	 *
	 * PMUSERENR_EL0
	 *    Controls whether reading from the cycle counter is allowed
	 *    from userland. Either bit 0 or bit 2 needs to be set. Here
	 *    we set both, which is probably overkill, but it works.
	 *
	 * PMCR_EL0
	 *    We need to conserve most of the bits here, but we set the
	 *    some low bits to specific values:
	 *      LC (bit 6)   1 for ensuring a 64-bit counter (not 32-bit)
	 *      DP (bit 5)   0 to leave the cycle counter accessible
	 *      D (bit 3)    0 to increment the counter every cycle
	 *      C (bit 2)    1 to reset the counter to zero
	 *      E (bit 0)    1 to allow enabling through PMCNTENSET_EL0
	 *    Since we read that register first then write it back, we use
	 *    a fence to ensure no compiler or CPU shenanigans with stale
	 *    information.
	 *
	 * PMCCFILTR_EL0
	 *    Controls whether the cycle counter is incremented in various
	 *    security levels ("exception levels"). For our benchmarking
	 *    purposes we only really need incrementation in userland (EL0)
	 *    but here we just enable counting at all levels. This is done
	 *    by setting bit 27 to 1, and all other bits to 0.
	 *
	 * Reference: Arm Architecture Reference Manual -- Armv8, for
	 * Armv8-A architecture profile, DDI 0487F.c (ID072120) (2020),
	 * section D13.4.
	 */
	(void)data;
	printk(KERN_INFO "enable pmccntr_el0 on CPU %d\n", smp_processor_id());
	asm volatile("msr pmintenclr_el1, %0" : : "r" (BIT(31)));
	asm volatile("msr pmcntenset_el0, %0" : : "r" (BIT(31)));
	asm volatile("msr pmuserenr_el0, %0" : : "r" (BIT(0)|BIT(2)|BIT(6)));
	unsigned long x;
	asm volatile("mrs %0, pmcr_el0" : "=r" (x));
	x |= BIT(0) | BIT(2);
	isb();
	asm volatile("msr pmcr_el0, %0" : : "r" (x));
	asm volatile("msr pmccfiltr_el0, %0" : : "r" (BIT(27)));
}

static void
disable_counter(void *data)
{
	(void)data;
	printk(KERN_INFO "disable pmccntr_el0 on CPU %d\n", smp_processor_id());
	asm volatile("msr pmcntenset_el0, %0" : : "r" (0));
	asm volatile("msr pmuserenr_el0, %0" : : "r" (0));
}

#elif defined(__riscv)

#include <asm/sbi.h>

static void
enable_counter(void *data)
{
	/*
	 * On RISC-V, we need to authorize read access to the cycle counter
	 * from userland, and also to enable incrementation of the counter
	 * at each cycle. Authorization is done by setting bit 0 of register
	 * scounteren to 1; here, we set all bits of that register, which has
	 * the side-effect of enabling access to all performance counters:
	 * bit 1 should always bet set (it's for access to the real-time
	 * clock, the fixed-frequency counter read by rdtime); bit 2 is
	 * for the counter of retired instructions, i.e. the total number of
	 * exec instructions; bit 3 to 31 are for other performance counters.
	 *
	 * Setting scounteren is not enough. That bit only allows access
	 * to the cycle counter from userland if the supervisor (i.e.
	 * the kernel) can read it. Access to the counter by the
	 * supervisor must also be enabled, which must be done by the
	 * machine-level code (i.e. the hypervisor). Similarly, the
	 * machine-level code should also start the counter, because it
	 * is usually inhibited by default to save some power (i.e. even
	 * with allowed access, the counter seems "stuck" to a fixed
	 * value). Both operations cannot be done by the supervisor;
	 * thus, the kernel code must request them to the relevant
	 * hypervisor part, which is done through a specific interface
	 * called SBI (Supervisor Binary Interface) (my test board is a
	 * StarFive VisionFive 2 and the hypervisor implementation in it
	 * appears to be OpenSBI).
	 */
	(void)data;
	printk(KERN_INFO "enable_rdcycle on CPU %d\n", smp_processor_id());
	struct sbiret r = sbi_ecall(SBI_EXT_PMU, SBI_EXT_PMU_COUNTER_START,
		0, 1, SBI_PMU_START_FLAG_SET_INIT_VALUE, 0, 0, 0);
	printk(KERN_INFO "CPU %d: sbi_ecall() returned %ld, %ld\n",
		smp_processor_id(), r.error, r.value);
	csr_write(CSR_SCOUNTEREN, GENMASK(31, 0));
}

static void
disable_counter(void *data)
{
	/*
	 * Ideally we should also ask the SBI to stop the counter. Here
	 * we just reset allowed access to only the timer (which is accessed
	 * through rdtime from userland).
	 */
	(void)data;
	printk(KERN_INFO "disable_rdcycle on CPU %d\n", smp_processor_id());
	csr_write(CSR_SCOUNTEREN, 0x2);
}

#else

#error This module is for ARMv8 and RISC-V only.

#endif

static int __init
init(void)
{
	on_each_cpu(enable_counter, NULL, 1);
	return 0;
}

static void __exit
fini(void)
{
	on_each_cpu(disable_counter, NULL, 1);
}

MODULE_DESCRIPTION("Enables user-mode access to in-CPU cycle counter");
MODULE_LICENSE("GPL");
module_init(init);
module_exit(fini);
