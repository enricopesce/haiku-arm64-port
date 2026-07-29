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
#define GICR_WAKER 0x0014
#define GICR_SGI_BASE 0x10000
#define GICR_IGROUPR0 0x0080
#define GICR_ISENABLER0 0x0100
#define GICR_ICENABLER0 0x0180
#define GICR_IPRIORITYR 0x0400
#define GICV3_SPECIAL_INTERRUPT_BASE 1020


static spinlock sGicv3DistributorLock = B_SPINLOCK_INITIALIZER;


static inline void
gicv3_write_barrier()
{
	asm volatile("dsb ishst" ::: "memory");
}


static inline uint32
mpidr_affinity(uint64 mpidr)
{
	return ((mpidr >> 32) & 0xff) << 24 | ((mpidr >> 16) & 0xff) << 16
		| ((mpidr >> 8) & 0xff) << 8 | (mpidr & 0xff);
}


GICv3InterruptController::GICv3InterruptController(phys_addr_t distributor,
	phys_addr_t redistributors, size_t redistributorSize)
	:
	fGicdRegs(NULL),
	fGicrRegs(NULL),
	fGicrSize(redistributorSize),
	fMaxIrq(0)
{
	reserve_io_interrupt_vectors(1020, 0, INTERRUPT_TYPE_IRQ);

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
	for (uint32 irq = PRIVATE_IRQ_BASE; irq < fMaxIrq; irq += 32) {
		fGicdRegs[GICD_REG_ICENABLER + irq / 32] = 0xffffffff;
		fGicdRegs[GICD_REG_IGROUP + irq / 32] = 0xffffffff;
	}
	gicv3_write_barrier();
	// Enable non-secure Group 1 interrupts and affinity routing.
	fGicdRegs[GICD_REG_CTLR] = (1u << 4) | (1u << 1);
	gicv3_write_barrier();

	call_all_cpus_sync([](void* arg, int) {
		((GICv3InterruptController*)arg)->_PerCpuInit();
	}, this);
	EnableInterrupt(ICI_IRQ);
}


volatile uint32*
GICv3InterruptController::_RedistributorForCpu(int32 cpu) const
{
	if (cpu < 0 || cpu >= smp_get_num_cpus())
		return NULL;

	uint32 wanted = mpidr_affinity(gCPU[cpu].arch.mpidr);
	for (size_t offset = 0; offset + GICR_FRAME_SIZE <= fGicrSize;
		offset += GICR_FRAME_SIZE) {
		volatile uint32* regs = (volatile uint32*)(fGicrRegs + offset);
		uint64 typer = *(volatile uint64*)((volatile uint8*)regs + GICR_TYPER);
		if ((uint32)(typer >> 32) == wanted)
			return regs;
	}
	return NULL;
}


void
GICv3InterruptController::_PerCpuInit()
{
	volatile uint32* gicr = _RedistributorForCpu(smp_get_current_cpu());
	if (gicr == NULL)
		panic("no GICv3 redistributor for current CPU");

	gicr[GICR_WAKER / 4] &= ~(1u << 1);
	while ((gicr[GICR_WAKER / 4] & (1u << 2)) != 0)
		;

	volatile uint32* sgi = (volatile uint32*)((volatile uint8*)gicr + GICR_SGI_BASE);
	sgi[GICR_IGROUPR0 / 4] = 0xffffffff;
	sgi[GICR_ICENABLER0 / 4] = 0xffffffff;
	for (uint32 i = 0; i < 8; i++)
		sgi[GICR_IPRIORITYR / 4 + i] = 0x80808080;
	gicv3_write_barrier();

	uint64 sre = READ_SPECIALREG(ICC_SRE_EL1);
	WRITE_SPECIALREG(ICC_SRE_EL1, sre | ICC_SRE_EL1_SRE);
	WRITE_SPECIALREG(ICC_PMR_EL1, ICC_PMR_EL1_PRIO_MASK);
	WRITE_SPECIALREG(ICC_IGRPEN1_EL1, 1);
	asm volatile("isb" ::: "memory");
}


void
GICv3InterruptController::EnableInterrupt(int32 irq)
{
	if (irq < PRIVATE_IRQ_BASE) {
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
		fGicdRegs[GICD_REG_IPRIORITYR + irq / 4] = 0x80808080;
		fGicdRegs[GICD_REG_ISENABLER + irq / 32] = 1u << (irq % 32);
	}
	gicv3_write_barrier();
}


void
GICv3InterruptController::DisableInterrupt(int32 irq)
{
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
	*router = mpidr_affinity(gCPU[cpu].arch.mpidr);
	gicv3_write_barrier();
	return B_OK;
}


void
GICv3InterruptController::HandleInterrupt()
{
	uint32 irq = READ_SPECIALREG(ICC_IAR1_EL1) & 0x3ff;
	if (irq >= GICV3_SPECIAL_INTERRUPT_BASE)
		return;
	if (irq == ICI_IRQ)
		smp_intercpu_interrupt_handler(smp_get_current_cpu());
	else
		io_interrupt_handler(irq, true);
	WRITE_SPECIALREG(ICC_EOIR1_EL1, irq);
	asm volatile("isb" ::: "memory");
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
