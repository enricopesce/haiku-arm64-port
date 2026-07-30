/*
 * Copyright 2026 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <interrupts.h>
#include <kernel.h>
#include <smp.h>
#include <vm/vm.h>

#include <arch/cpu.h>
#include <KernelExport.h>
#include <util/AutoLock.h>

#include "arch_int_gicv3.h"
#include "gicv2_regs.h"


#define ICI_IRQ 0
#define PRIVATE_IRQ_BASE 32
#define GICD_SIZE 0x10000
#define GICR_FRAME_SIZE 0x20000
#define GICR_TYPER 0x0008
#define GICR_TYPER_LAST (1u << 4)
#define GICR_WAKER 0x0014
#define GICR_SGI_BASE 0x10000
#define GICR_IGROUPR0 0x0080
#define GICR_ISENABLER0 0x0100
#define GICR_ICENABLER0 0x0180
#define GICR_IPRIORITYR 0x0400
#define GICR_CTLR_RWP (1u << 3)
#define GICD_CTLR_RWP (1u << 31)
#define GICV3_SPECIAL_INTERRUPT_BASE 1020


static spinlock sGicv3DistributorLock = B_SPINLOCK_INITIALIZER;


static inline void
gicv3_write_barrier()
{
	asm volatile("dsb ishst" ::: "memory");
}


static inline void
gicv3_wait_for_distributor(volatile uint32* gicd)
{
	gicv3_write_barrier();
	while ((gicd[GICD_REG_CTLR] & GICD_CTLR_RWP) != 0)
		;
}


static inline uint32
mpidr_affinity(uint64 mpidr)
{
	return ((mpidr >> 32) & 0xff) << 24 | ((mpidr >> 16) & 0xff) << 16
		| ((mpidr >> 8) & 0xff) << 8 | (mpidr & 0xff);
}


static inline uint64
mpidr_router_affinity(uint64 mpidr)
{
	return ((mpidr >> 32) & 0xff) << 32 | (mpidr & 0xffffff);
}


GICv3InterruptController::GICv3InterruptController(phys_addr_t distributor,
	phys_addr_t redistributors, size_t redistributorSize)
	:
	fGicdRegs(NULL),
	fGicrRegs(NULL),
	fCpuGicr(NULL),
	fGicrSize(redistributorSize),
	fMaxIrq(0),
	fPrivateEnabled(0)
{
	reserve_io_interrupt_vectors(1020, 0, INTERRUPT_TYPE_IRQ);

	fCpuGicr = new(std::nothrow) volatile uint32*[smp_get_num_cpus()];
	if (fCpuGicr == NULL)
		panic("not able to allocate GICv3 redistributor table");
	for (int32 cpu = 0; cpu < smp_get_num_cpus(); cpu++)
		fCpuGicr[cpu] = NULL;

	area_id area = vm_map_physical_memory(B_SYSTEM_TEAM, "intc-gicv3-gicd",
		(void**)&fGicdRegs, B_ANY_KERNEL_ADDRESS, GICD_SIZE,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, distributor, false);
	if (area < 0)
		panic("not able to map GICv3 distributor");

	area = vm_map_physical_memory(B_SYSTEM_TEAM, "intc-gicv3-gicr",
		(void**)&fGicrRegs, B_ANY_KERNEL_ADDRESS, fGicrSize,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, redistributors, false);
	if (area < 0)
		panic("not able to map GICv3 redistributors");

	fMaxIrq = ((fGicdRegs[GICD_REG_TYPER] & 0x1f) + 1) * 32;
	if (fMaxIrq > 1020)
		fMaxIrq = 1020;

	fGicdRegs[GICD_REG_CTLR] = 0;
	gicv3_wait_for_distributor(fGicdRegs);
	// Enable affinity routing before accessing the GICv3 register layout.
	fGicdRegs[GICD_REG_CTLR] = 1u << 4;
	gicv3_wait_for_distributor(fGicdRegs);
	for (uint32 irq = PRIVATE_IRQ_BASE; irq < fMaxIrq; irq += 32) {
		fGicdRegs[GICD_REG_ICENABLER + irq / 32] = 0xffffffff;
		fGicdRegs[GICD_REG_IGROUP + irq / 32] = 0xffffffff;
	}
	gicv3_wait_for_distributor(fGicdRegs);
	// Enable non-secure Group 1 interrupts and affinity routing.
	fGicdRegs[GICD_REG_CTLR] = (1u << 4) | (1u << 1);
	gicv3_wait_for_distributor(fGicdRegs);

	// Pre-touch each implemented redistributor from the boot CPU. CPU affinity
	// metadata is not final for every AP yet, so do not populate fCpuGicr here.
	uint32 redistributorCount = 0;
	for (size_t offset = 0; offset + GICR_FRAME_SIZE <= fGicrSize;
		offset += GICR_FRAME_SIZE) {
		volatile uint32* gicr = (volatile uint32*)(fGicrRegs + offset);
		uint64 typer = *(volatile uint64*)((volatile uint8*)gicr + GICR_TYPER);
		_RedistributorInit(gicr);
		redistributorCount++;
		if ((typer & GICR_TYPER_LAST) != 0)
			break;
	}
	if (redistributorCount < (uint32)smp_get_num_cpus())
		panic("not enough GICv3 redistributors");

	call_all_cpus_sync([](void* arg, int) {
		((GICv3InterruptController*)arg)->InitPerCpu();
	}, this);
	EnableInterrupt(ICI_IRQ);
}


volatile uint32*
GICv3InterruptController::_RedistributorForCpu(int32 cpu)
{
	if (cpu < 0 || cpu >= smp_get_num_cpus())
		return NULL;
	if (fCpuGicr[cpu] != NULL)
		return fCpuGicr[cpu];

	uint64 mpidr = cpu == smp_get_current_cpu()
		? READ_SPECIALREG(MPIDR_EL1) : gCPU[cpu].arch.mpidr;
	uint32 wanted = mpidr_affinity(mpidr);
	for (size_t offset = 0; offset + GICR_FRAME_SIZE <= fGicrSize;
		offset += GICR_FRAME_SIZE) {
		volatile uint32* regs = (volatile uint32*)(fGicrRegs + offset);
		uint64 typer = *(volatile uint64*)((volatile uint8*)regs + GICR_TYPER);
		if ((uint32)(typer >> 32) == wanted) {
			fCpuGicr[cpu] = regs;
			return regs;
		}
	}
	return NULL;
}


void
GICv3InterruptController::_RedistributorInit(volatile uint32* gicr)
{
	gicr[GICR_WAKER / 4] &= ~(1u << 1);
	while ((gicr[GICR_WAKER / 4] & (1u << 2)) != 0)
		;

	volatile uint32* sgi = (volatile uint32*)((volatile uint8*)gicr + GICR_SGI_BASE);
	sgi[GICR_IGROUPR0 / 4] = 0xffffffff;
	sgi[GICR_ICENABLER0 / 4] = 0xffffffff;
	for (uint32 i = 0; i < 8; i++)
		sgi[GICR_IPRIORITYR / 4 + i] = 0x80808080;
	gicv3_write_barrier();
	while ((gicr[0] & GICR_CTLR_RWP) != 0)
		;
}


void
GICv3InterruptController::InitPerCpu()
{
	// APs are still in the early boot rendez-vous and have not yet run the
	// global TLB invalidation in main(). Discard their cached invalid
	// translations before they use the newly mapped redistributor pages.
	arch_cpu_global_tlb_invalidate();

	volatile uint32* gicr = _RedistributorForCpu(smp_get_current_cpu());
	if (gicr == NULL)
		panic("no GICv3 redistributor for current CPU");
	// A redistributor can remain, or return, asleep until its PE is running.
	// Perform the architecturally per-PE wake and SGI/PPI setup here even
	// though the boot CPU pre-touched these pages before the rendez-vous.
	_RedistributorInit(gicr);
	volatile uint32* sgi = (volatile uint32*)((volatile uint8*)gicr + GICR_SGI_BASE);
	sgi[GICR_ISENABLER0 / 4] = (uint32)atomic_get(&fPrivateEnabled);
	gicv3_write_barrier();

	uint64 sre = READ_SPECIALREG(ICC_SRE_EL1);
	WRITE_SPECIALREG(ICC_SRE_EL1, sre | ICC_SRE_EL1_SRE);
	asm volatile("isb" ::: "memory");
	uint64 control = READ_SPECIALREG(ICC_CTLR_EL1);
	WRITE_SPECIALREG(ICC_CTLR_EL1, control & ~ICC_CTLR_EL1_EOIMODE);
	WRITE_SPECIALREG(ICC_PMR_EL1, ICC_PMR_EL1_PRIO_MASK);
	WRITE_SPECIALREG(ICC_IGRPEN1_EL1, 1);
	asm volatile("isb" ::: "memory");
}


void
GICv3InterruptController::EnableInterrupt(int32 irq)
{
	if (irq < PRIVATE_IRQ_BASE) {
		if (irq >= 0)
			atomic_or(&fPrivateEnabled, 1u << irq);
		call_all_cpus_sync([](void* arg, int) {
			((GICv3InterruptController*)InterruptController::Get())
				->_EnableInterrupt((int32)(addr_t)arg);
		}, (void*)(addr_t)irq);
	} else {
		_EnableInterrupt(irq);
	}
}


void
GICv3InterruptController::_EnableInterrupt(int32 irq)
{
	if (irq < 0 || (uint32)irq >= fMaxIrq)
		return;

	if (irq < PRIVATE_IRQ_BASE) {
		volatile uint32* gicr = _RedistributorForCpu(smp_get_current_cpu());
		if (gicr == NULL)
			return;
		volatile uint32* sgi = (volatile uint32*)((volatile uint8*)gicr + GICR_SGI_BASE);
		sgi[GICR_ISENABLER0 / 4] = 1u << irq;
	} else {
		InterruptsSpinLocker locker(sGicv3DistributorLock);
		uint32 configuration = fGicdRegs[GICD_REG_ICFGR + irq / 16];
		configuration &= ~(3u << (2 * (irq % 16)));
		fGicdRegs[GICD_REG_ICFGR + irq / 16] = configuration;
		volatile uint64* router = (volatile uint64*)((volatile uint8*)fGicdRegs
			+ 0x6000 + 8 * irq);
		*router = mpidr_router_affinity(gCPU[0].arch.mpidr);
		fGicdRegs[GICD_REG_IPRIORITYR + irq / 4] = 0x80808080;
		fGicdRegs[GICD_REG_ISENABLER + irq / 32] = 1u << (irq % 32);
		gicv3_wait_for_distributor(fGicdRegs);
	}
	gicv3_write_barrier();
}


void
GICv3InterruptController::DisableInterrupt(int32 irq)
{
	if (irq >= 0 && irq < PRIVATE_IRQ_BASE)
		atomic_and(&fPrivateEnabled, ~(1u << irq));
	_DisableInterrupt(irq);
	if (irq < PRIVATE_IRQ_BASE) {
		call_all_cpus_sync([](void* arg, int) {
			((GICv3InterruptController*)InterruptController::Get())
				->_DisableInterrupt((int32)(addr_t)arg);
		}, (void*)(addr_t)irq);
	}
}


void
GICv3InterruptController::_DisableInterrupt(int32 irq)
{
	if (irq < 0 || (uint32)irq >= fMaxIrq)
		return;
	if (irq < PRIVATE_IRQ_BASE) {
		volatile uint32* gicr = _RedistributorForCpu(smp_get_current_cpu());
		if (gicr != NULL) {
			volatile uint32* sgi = (volatile uint32*)((volatile uint8*)gicr + GICR_SGI_BASE);
			sgi[GICR_ICENABLER0 / 4] = 1u << irq;
		}
	} else
		fGicdRegs[GICD_REG_ICENABLER + irq / 32] = 1u << (irq % 32);
	gicv3_write_barrier();
}


status_t
GICv3InterruptController::SetInterruptAffinity(int32 irq, int32 cpu)
{
	if (irq < PRIVATE_IRQ_BASE || (uint32)irq >= fMaxIrq || cpu < 0
		|| cpu >= smp_get_num_cpus())
		return B_BAD_VALUE;

	InterruptsSpinLocker locker(sGicv3DistributorLock);
	volatile uint64* router = (volatile uint64*)((volatile uint8*)fGicdRegs + 0x6000 + 8 * irq);
	*router = mpidr_router_affinity(gCPU[cpu].arch.mpidr);
	gicv3_write_barrier();
	return B_OK;
}


void
GICv3InterruptController::HandleInterrupt()
{
	uint32 iar = READ_SPECIALREG(ICC_IAR1_EL1);
	uint32 irq = iar & 0x3ff;
	if (irq >= GICV3_SPECIAL_INTERRUPT_BASE)
		return;
	if (irq == ICI_IRQ)
		smp_intercpu_interrupt_handler(smp_get_current_cpu());
	else
		io_interrupt_handler(irq, true);
	WRITE_SPECIALREG(ICC_EOIR1_EL1, iar);
	asm volatile("dsb sy" ::: "memory");
}


void
GICv3InterruptController::_SendIci(int32 cpu)
{
	uint64 mpidr = gCPU[cpu].arch.mpidr;
	uint64 sgi = ((mpidr >> 32) & 0xff) << 48 | ((mpidr >> 16) & 0xff) << 32
		| ((mpidr >> 8) & 0xff) << 16 | (1ull << (mpidr & 0xf));
	WRITE_SPECIALREG(ICC_SGI1R_EL1, sgi);
	asm volatile("isb" ::: "memory");
}


void
GICv3InterruptController::SendMulticastIci(CPUSet& cpuSet)
{
	for (int32 cpu = 0; cpu < smp_get_num_cpus(); cpu++) {
		if (cpuSet.GetBit(cpu))
			_SendIci(cpu);
	}
}


void
GICv3InterruptController::SendBroadcastIci()
{
	WRITE_SPECIALREG(ICC_SGI1R_EL1, ICC_SGI1R_EL1_IRM);
	asm volatile("isb" ::: "memory");
}
