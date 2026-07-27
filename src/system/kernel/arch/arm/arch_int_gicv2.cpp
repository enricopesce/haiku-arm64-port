/*
 * Copyright 2021-2022 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <interrupts.h>
#include <interrupt_controller.h>
#include <kernel.h>
#include <vm/vm.h>
#include <smp.h>
#include <KernelExport.h>
#include <util/AutoLock.h>

#include "arch_int_gicv2.h"
#include "gicv2_regs.h"


#define ICI_IRQ 0
#define SHARED_IRQ_BASE 32


static spinlock sGicv2DistributorLock = B_SPINLOCK_INITIALIZER;


static inline void
gicv2_write_barrier()
{
	// Complete normal-memory updates before making an interrupt visible to
	// another CPU or device through the distributor.
	asm volatile("dsb ishst" ::: "memory");
}


GICv2InterruptController::GICv2InterruptController(phys_addr_t gicd_addr, phys_addr_t gicc_addr)
: InterruptController()
{
	reserve_io_interrupt_vectors(1020, 0, INTERRUPT_TYPE_IRQ);

	area_id gicd_area = vm_map_physical_memory(B_SYSTEM_TEAM, "intc-gicv2-gicd",
		(void**)&fGicdRegs, B_ANY_KERNEL_ADDRESS, GICD_REG_SIZE,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		gicd_addr ? gicd_addr : GICD_REG_START, false);
	if (gicd_area < 0) {
		panic("not able to map the memory area for gicd\n");
	}

	area_id gicc_area = vm_map_physical_memory(B_SYSTEM_TEAM, "intc-gicv2-gicc",
		(void**)&fGiccRegs, B_ANY_KERNEL_ADDRESS, GICC_REG_SIZE,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		gicc_addr ? gicc_addr : GICC_REG_START, false);
	if (gicc_area < 0) {
		panic("not able to map the memory area for gicc\n");
	}

	// The ITLinesNumber field gives the number of 32-interrupt blocks minus
	// one. Keep this controller within the architectural GICv2 limit.
	fMaxIrq = (((fGicdRegs[GICD_REG_TYPER] & 0x1f) + 1) * 32);
	if (fMaxIrq > 1020)
		fMaxIrq = 1020;

	// disable GICD
	fGicdRegs[GICD_REG_CTLR] = 0;

	// disable GICC
	fGiccRegs[GICC_REG_CTLR] = 0;

	// Disable every interrupt implemented by this distributor. The old fixed
	// two-register loop silently left SPIs enabled on larger GICs.
	for (uint32 irq = 0; irq < fMaxIrq; irq += 32)
		fGicdRegs[GICD_REG_ICENABLER + irq / 32] = 0xffffffff;
	gicv2_write_barrier();

	// enable GICD
	fGicdRegs[GICD_REG_CTLR] = 0x03;
	gicv2_write_barrier();

	call_all_cpus_sync([](void *arg, int cpu) {
		GICv2InterruptController* self = (GICv2InterruptController *)arg;
		self->_PerCpuInit();
	}, this);

	EnableInterrupt(ICI_IRQ);
}


void GICv2InterruptController::_PerCpuInit()
{
	// set PMR and BPR
	fGiccRegs[GICC_REG_PMR] = 0xff;
	fGiccRegs[GICC_REG_BPR] = 0x07;

	// enable GICC
	fGiccRegs[GICC_REG_CTLR] = 0x01;
	gicv2_write_barrier();
}


void GICv2InterruptController::EnableInterrupt(int32 irq)
{
	if (irq < SHARED_IRQ_BASE) {
		call_all_cpus_sync([](void *arg, int cpu) {
			int32 irq = (int32)(addr_t)arg;
			GICv2InterruptController *self =
				(GICv2InterruptController *)InterruptController::Get();

			self->_EnableInterrupt(irq);
		}, (void*)(addr_t)irq);
	} else {
		_EnableInterrupt(irq);
	}
}


void GICv2InterruptController::_EnableInterrupt(int32 irq)
{
	if (irq < 0 || (uint32)irq >= fMaxIrq)
		return;

	uint32_t ena_reg = GICD_REG_ISENABLER + irq / 32;
	uint32_t ena_val = 1 << (irq % 32);

	uint32_t prio_reg = GICD_REG_IPRIORITYR + irq / 4;
	uint32_t prio_val = fGicdRegs[prio_reg];
	uint32_t prio_shift = (irq % 4) * 8;
	prio_val = (prio_val & ~(0xffu << prio_shift)) | (0x80u << prio_shift);
	fGicdRegs[prio_reg] = prio_val;
	gicv2_write_barrier();
	fGicdRegs[ena_reg] = ena_val;
	gicv2_write_barrier();
}


void GICv2InterruptController::DisableInterrupt(int32 irq)
{
	_DisableInterrupt(irq);
	if (irq < SHARED_IRQ_BASE) {
		call_all_cpus_sync([](void *arg, int cpu) {
			int32 irq = (int32)(addr_t)arg;
			GICv2InterruptController *self =
				(GICv2InterruptController *)InterruptController::Get();

			self->_DisableInterrupt(irq);
		}, (void*)(addr_t)irq);
	}
}


void GICv2InterruptController::_DisableInterrupt(int32 irq)
{
	if (irq < 0 || (uint32)irq >= fMaxIrq)
		return;

	fGicdRegs[GICD_REG_ICENABLER + irq / 32] = 1 << (irq % 32);
	gicv2_write_barrier();
}


status_t
GICv2InterruptController::SetInterruptAffinity(int32 irq, int32 cpu)
{
	// SGIs and PPIs are private to each CPU and cannot be routed here.
	if (irq < SHARED_IRQ_BASE || (uint32)irq >= fMaxIrq || cpu < 0
		|| cpu >= smp_get_num_cpus() || cpu >= 8) {
		return B_BAD_VALUE;
	}

	InterruptsSpinLocker locker(sGicv2DistributorLock);
	uint32 targetRegister = GICD_REG_ITARGETSR + irq / 4;
	uint32 targetShift = (irq % 4) * 8;
	uint32 targets = fGicdRegs[targetRegister];
	targets = (targets & ~(0xffu << targetShift)) | ((1u << cpu) << targetShift);
	fGicdRegs[targetRegister] = targets;
	gicv2_write_barrier();

	return B_OK;
}


bool
GICv2InterruptController::_IsLevelTriggered(uint32 irq) const
{
	// Each GICD_ICFGR word contains 16 two-bit fields. Bit[1] of a field is
	// one for edge-triggered and zero for level-sensitive interrupts.
	uint32 configuration = fGicdRegs[GICD_REG_ICFGR + irq / 16];
	return (configuration & (2u << ((irq % 16) * 2))) == 0;
}


void GICv2InterruptController::HandleInterrupt()
{
	uint32_t iar = fGiccRegs[GICC_REG_IAR];
	uint32_t irqnr = iar & 0x3FF;
	if (irqnr >= GICV2_SPECIAL_INTERRUPT_BASE || irqnr >= fMaxIrq) {
		dprintf("spurious interrupt\n");
		// Special interrupt IDs are not acknowledged interrupts and must not
		// be completed with an EOIR write.
		return;
	}

	if (irqnr == ICI_IRQ) {
		smp_intercpu_interrupt_handler(smp_get_current_cpu());
	} else {
		io_interrupt_handler(irqnr, _IsLevelTriggered(irqnr));
	}

	fGiccRegs[GICC_REG_EOIR] = iar;
}


void GICv2InterruptController::SendMulticastIci(CPUSet& cpuSet)
{
	gicv2_write_barrier();
	fGicdRegs[GICD_REG_SGIR] = (cpuSet.Bits(0) << 16);
	gicv2_write_barrier();
}


void GICv2InterruptController::SendBroadcastIci()
{
	gicv2_write_barrier();
	fGicdRegs[GICD_REG_SGIR] = (0b01 << 24);
	gicv2_write_barrier();
}
