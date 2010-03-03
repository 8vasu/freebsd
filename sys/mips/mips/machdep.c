    /*	$OpenBSD: machdep.c,v 1.33 1998/09/15 10:58:54 pefo Exp $	*/
/* tracked to 1.38 */
/*
 * Copyright (c) 1988 University of Utah.
 * Copyright (c) 1992, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * the Systems Programming Group of the University of Utah Computer
 * Science Department, The Mach Operating System project at
 * Carnegie-Mellon University and Ralph Campbell.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	from: @(#)machdep.c	8.3 (Berkeley) 1/12/94
 *	Id: machdep.c,v 1.33 1998/09/15 10:58:54 pefo Exp
 *	JNPR: machdep.c,v 1.11.2.3 2007/08/29 12:24:49
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_cputype.h"
#include "opt_ddb.h"
#include "opt_md.h"
#include "opt_msgbuf.h"

#include <sys/param.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/bus.h>
#include <sys/conf.h>
#include <sys/cpu.h>
#include <sys/kernel.h>
#include <sys/linker.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/msgbuf.h>
#include <sys/reboot.h>
#include <sys/sched.h>
#include <sys/sysctl.h>
#include <sys/sysproto.h>
#include <sys/vmmeter.h>

#include <vm/vm.h>
#include <vm/vm_kern.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_pager.h>
#include <vm/vm_extern.h>
#include <sys/socket.h>

#include <sys/user.h>
#include <sys/interrupt.h>
#include <sys/cons.h>
#include <sys/syslog.h>
#include <machine/asm.h>
#include <machine/bootinfo.h>
#include <machine/cache.h>
#include <machine/clock.h>
#include <machine/cpu.h>
#include <machine/cpuregs.h>
#include <machine/elf.h>
#include <machine/hwfunc.h>
#include <machine/intr_machdep.h>
#include <machine/md_var.h>
#ifdef DDB
#include <sys/kdb.h>
#include <ddb/ddb.h>
#endif

#include <sys/random.h>
#include <net/if.h>

#define	BOOTINFO_DEBUG	0

char machine[] = "mips";
SYSCTL_STRING(_hw, HW_MACHINE, machine, CTLFLAG_RD, machine, 0, "Machine class");

static char cpu_model[30];
SYSCTL_STRING(_hw, HW_MODEL, model, CTLFLAG_RD, cpu_model, 0, "Machine model");

int cold = 1;
long realmem = 0;
long Maxmem = 0;
int cpu_clock = MIPS_DEFAULT_HZ;
SYSCTL_INT(_hw, OID_AUTO, clockrate, CTLFLAG_RD, 
    &cpu_clock, 0, "CPU instruction clock rate");
int clocks_running = 0;

vm_offset_t kstack0;

/*
 * Each entry in the pcpu_space[] array is laid out in the following manner:
 * struct pcpu for cpu 'n'	pcpu_space[n]
 * boot stack for cpu 'n'	pcpu_space[n] + PAGE_SIZE * 2 - CALLFRAME_SIZ
 *
 * Note that the boot stack grows downwards and we assume that we never
 * use enough stack space to trample over the 'struct pcpu' that is at
 * the beginning of the array.
 *
 * The array is aligned on a (PAGE_SIZE * 2) boundary so that the 'struct pcpu'
 * is always in the even page frame of the wired TLB entry on SMP kernels.
 *
 * The array is in the .data section so that the stack does not get zeroed out
 * when the .bss section is zeroed.
 */
char pcpu_space[MAXCPU][PAGE_SIZE * 2] \
	__aligned(PAGE_SIZE * 2) __section(".data");

struct pcpu *pcpup = (struct pcpu *)pcpu_space;

vm_offset_t phys_avail[PHYS_AVAIL_ENTRIES + 2];
vm_offset_t physmem_desc[PHYS_AVAIL_ENTRIES + 2];

#ifdef UNIMPLEMENTED
struct platform platform;
#endif

vm_paddr_t	mips_wired_tlb_physmem_start;
vm_paddr_t	mips_wired_tlb_physmem_end;
u_int		need_wired_tlb_page_pool;

static void cpu_startup(void *);
SYSINIT(cpu, SI_SUB_CPU, SI_ORDER_FIRST, cpu_startup, NULL);

struct kva_md_info kmi;

int cpucfg;			/* Value of processor config register */
int num_tlbentries = 64;	/* Size of the CPU tlb */
int cputype;

extern char MipsException[], MipsExceptionEnd[];

/* TLB miss handler address and end */
extern char MipsTLBMiss[], MipsTLBMissEnd[];

/* Cache error handler */
extern char MipsCache[], MipsCacheEnd[];

extern char edata[], end[];
#ifdef DDB
extern vm_offset_t ksym_start, ksym_end;
#endif

u_int32_t bootdev;
struct bootinfo bootinfo;
/*
 * First kseg0 address available for use. By default it's equal to &end.
 * But in some cases there might be additional data placed right after 
 * _end by loader or ELF trampoline.
 */
vm_offset_t kernel_kseg0_end = (vm_offset_t)&end;

static void
cpu_startup(void *dummy)
{

	if (boothowto & RB_VERBOSE)
		bootverbose++;

	printf("real memory  = %lu (%luK bytes)\n", ptoa(realmem),
	    ptoa(realmem) / 1024);

	/*
	 * Display any holes after the first chunk of extended memory.
	 */
	if (bootverbose) {
		int indx;

		printf("Physical memory chunk(s):\n");
		for (indx = 0; phys_avail[indx + 1] != 0; indx += 2) {
			uintptr_t size1 = phys_avail[indx + 1] - phys_avail[indx];

			printf("0x%08llx - 0x%08llx, %llu bytes (%llu pages)\n",
			    (unsigned long long)phys_avail[indx],
			    (unsigned long long)phys_avail[indx + 1] - 1,
			    (unsigned long long)size1,
			    (unsigned long long)size1 / PAGE_SIZE);
		}
	}

	vm_ksubmap_init(&kmi);

	printf("avail memory = %lu (%luMB)\n", ptoa(cnt.v_free_count),
	    ptoa(cnt.v_free_count) / 1048576);
	cpu_init_interrupts();

	/*
	 * Set up buffers, so they can be used to read disk labels.
	 */
	bufinit();
	vm_pager_bufferinit();
}

/*
 * Shutdown the CPU as much as possible
 */
void
cpu_reset(void)
{

	platform_reset();
}

/*
 * Flush the D-cache for non-DMA I/O so that the I-cache can
 * be made coherent later.
 */
void
cpu_flush_dcache(void *ptr, size_t len)
{
	/* TBD */
}

/* Get current clock frequency for the given cpu id. */
int
cpu_est_clockrate(int cpu_id, uint64_t *rate)
{

	return (ENXIO);
}

/*
 * Shutdown the CPU as much as possible
 */
void
cpu_halt(void)
{
	for (;;)
		;
}

SYSCTL_STRUCT(_machdep, CPU_BOOTINFO, bootinfo, CTLFLAG_RD, &bootinfo,
    bootinfo, "Bootinfo struct: kernel filename, BIOS harddisk geometry, etc");

#ifdef PORT_TO_JMIPS
static int
sysctl_machdep_adjkerntz(SYSCTL_HANDLER_ARGS)
{
}

SYSCTL_PROC(_machdep, CPU_ADJKERNTZ, adjkerntz, CTLTYPE_INT | CTLFLAG_RW,
    &adjkerntz, 0, sysctl_machdep_adjkerntz, "I",
    "Local offset from GMT in seconds");
SYSCTL_INT(_machdep, CPU_DISRTCSET, disable_rtc_set, CTLFLAG_RW,
    &disable_rtc_set, 0, "Disable setting the real time clock to system time");
SYSCTL_INT(_machdep, CPU_WALLCLOCK, wall_cmos_clock, CTLFLAG_RW,
    &wall_cmos_clock, 0, "Wall CMOS clock assumed");
#endif	/* PORT_TO_JMIPS */

/*
 * Initialize per cpu data structures, include curthread.
 */
void
mips_pcpu0_init()
{
	/* Initialize pcpu info of cpu-zero */
	pcpu_init(PCPU_ADDR(0), 0, sizeof(struct pcpu));
	PCPU_SET(curthread, &thread0);
}

/*
 * Initialize mips and configure to run kernel
 */
void
mips_proc0_init(void)
{
#ifdef SMP
	if (platform_processor_id() != 0)
		panic("BSP must be processor number 0");
#endif
	proc_linkup0(&proc0, &thread0);

	KASSERT((kstack0 & PAGE_MASK) == 0,
		("kstack0 is not aligned on a page boundary: 0x%0lx",
		(long)kstack0));
	thread0.td_kstack = kstack0;
	thread0.td_kstack_pages = KSTACK_PAGES;
	thread0.td_md.md_realstack = roundup2(thread0.td_kstack, PAGE_SIZE * 2);
	/* 
	 * Do not use cpu_thread_alloc to initialize these fields 
	 * thread0 is the only thread that has kstack located in KSEG0 
	 * while cpu_thread_alloc handles kstack allocated in KSEG2.
	 */
	thread0.td_pcb = (struct pcb *)(thread0.td_md.md_realstack +
	    (thread0.td_kstack_pages - 1) * PAGE_SIZE) - 1;
	thread0.td_frame = &thread0.td_pcb->pcb_regs;

	/* Steal memory for the dynamic per-cpu area. */
	dpcpu_init((void *)pmap_steal_memory(DPCPU_SIZE), 0);

	PCPU_SET(curpcb, thread0.td_pcb);
	/*
	 * There is no need to initialize md_upte array for thread0 as it's
	 * located in .bss section and should be explicitly zeroed during 
	 * kernel initialization.
	 */
}

void
cpu_initclocks(void)
{
	platform_initclocks();
}

struct msgbuf *msgbufp=0;

/*
 * Initialize the hardware exception vectors, and the jump table used to
 * call locore cache and TLB management functions, based on the kind
 * of CPU the kernel is running on.
 */
void
mips_vector_init(void)
{
	/*
	 * Copy down exception vector code.
	 */
	if (MipsTLBMissEnd - MipsTLBMiss > 0x80)
		panic("startup: UTLB code too large");

	if (MipsCacheEnd - MipsCache > 0x80)
		panic("startup: Cache error code too large");

	bcopy(MipsTLBMiss, (void *)TLB_MISS_EXC_VEC,
	      MipsTLBMissEnd - MipsTLBMiss);

#ifdef TARGET_OCTEON
/* Fake, but sufficient, for the 32-bit with 64-bit hardware addresses  */
	bcopy(MipsTLBMiss, (void *)XTLB_MISS_EXC_VEC,
	      MipsTLBMissEnd - MipsTLBMiss);
#endif

	bcopy(MipsException, (void *)GEN_EXC_VEC,
	      MipsExceptionEnd - MipsException);

	bcopy(MipsCache, (void *)CACHE_ERR_EXC_VEC,
	      MipsCacheEnd - MipsCache);

	/*
	 * Clear out the I and D caches.
	 */
	mips_icache_sync_all();
	mips_dcache_wbinv_all();

	/* 
	 * Mask all interrupts. Each interrupt will be enabled
	 * when handler is installed for it
	 */
	set_intr_mask(ALL_INT_MASK);
	enableintr();

	/* Clear BEV in SR so we start handling our own exceptions */
	mips_cp0_status_write(mips_cp0_status_read() & ~SR_BOOT_EXC_VEC);

}

/*
 * Fix kernel_kseg0_end address in case trampoline placed debug sympols 
 * data there
 */
void
mips_postboot_fixup(void)
{
#ifdef DDB
	Elf_Size *trampoline_data = (Elf_Size*)kernel_kseg0_end;
	Elf_Size symtabsize = 0;

	if (trampoline_data[0] == SYMTAB_MAGIC) {
		symtabsize = trampoline_data[1];
		kernel_kseg0_end += 2 * sizeof(Elf_Size);
		/* start of .symtab */
		ksym_start = kernel_kseg0_end;
		kernel_kseg0_end += symtabsize;
		/* end of .strtab */
		ksym_end = kernel_kseg0_end;
	}
#endif
}

/*
 * Many SoCs have a means to reset the core itself.  Others do not, or
 * the method is unknown to us.  For those cases, we jump to the mips
 * reset vector and hope for the best.  This works well in practice.
 */
void
mips_generic_reset()
{
	((void(*)(void))(intptr_t)MIPS_VEC_RESET)();
}

#ifdef SMP
void
mips_pcpu_tlb_init(struct pcpu *pcpu)
{
	vm_paddr_t pa;
	struct tlb tlb;
	int lobits;

	/*
	 * Map the pcpu structure at the virtual address 'pcpup'.
	 * We use a wired tlb index to do this one-time mapping.
	 */
	memset(&tlb, 0, sizeof(tlb));
	pa = vtophys(pcpu);
	lobits = PTE_RW | PTE_V | PTE_G | PTE_CACHE;
	tlb.tlb_hi = (vm_offset_t)pcpup;
	tlb.tlb_lo0 = mips_paddr_to_tlbpfn(pa) | lobits;
	tlb.tlb_lo1 = mips_paddr_to_tlbpfn(pa + PAGE_SIZE) | lobits;
	Mips_TLBWriteIndexed(PCPU_TLB_ENTRY, &tlb);
}
#endif

/*
 * Initialise a struct pcpu.
 */
void
cpu_pcpu_init(struct pcpu *pcpu, int cpuid, size_t size)
{

	pcpu->pc_next_asid = 1;
	pcpu->pc_asid_generation = 1;
#ifdef SMP
	if ((vm_offset_t)pcpup >= VM_MIN_KERNEL_ADDRESS)
		mips_pcpu_tlb_init(pcpu);
#endif
}

int
fill_dbregs(struct thread *td, struct dbreg *dbregs)
{

	/* No debug registers on mips */
	return (ENOSYS);
}

int
set_dbregs(struct thread *td, struct dbreg *dbregs)
{

	/* No debug registers on mips */
	return (ENOSYS);
}

void
spinlock_enter(void)
{
	struct thread *td;

	td = curthread;
	if (td->td_md.md_spinlock_count == 0)
		td->td_md.md_saved_intr = disableintr();
	td->td_md.md_spinlock_count++;
	critical_enter();
}

void
spinlock_exit(void)
{
	struct thread *td;

	td = curthread;
	critical_exit();
	td->td_md.md_spinlock_count--;
	if (td->td_md.md_spinlock_count == 0)
		restoreintr(td->td_md.md_saved_intr);
}

u_int32_t
get_cyclecount(void)
{
	u_int32_t count;

	mfc0_macro(count, 9);
	return (count);
}

/*
 * call platform specific code to halt (until next interrupt) for the idle loop
 */
void
cpu_idle(int busy)
{
	if (mips_cp0_status_read() & SR_INT_ENAB)
		__asm __volatile ("wait");
	else
		panic("ints disabled in idleproc!");
}

void
dumpsys(struct dumperinfo *di __unused)
{

	printf("Kernel dumps not implemented on this architecture\n");
}

int
cpu_idle_wakeup(int cpu)
{

	return (0);
}

int
is_physical_memory(vm_offset_t addr)
{
	int i;

	for (i = 0; physmem_desc[i + 1] != 0; i += 2) {
		if (addr >= physmem_desc[i] && addr < physmem_desc[i + 1])
			return (1);
	}

	return (0);
}
