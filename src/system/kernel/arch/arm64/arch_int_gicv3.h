/*
 * Copyright 2026 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef ARCH_ARM64_GICV3_H
#define ARCH_ARM64_GICV3_H

#include <SupportDefs.h>

#include "soc.h"


class GICv3InterruptController : public InterruptController {
public:
	GICv3InterruptController(phys_addr_t distributor, phys_addr_t redistributors,
		size_t redistributorSize);

	void EnableInterrupt(int32 irq);
	void DisableInterrupt(int32 irq);
	status_t SetInterruptAffinity(int32 irq, int32 cpu);
	void HandleInterrupt();
	void SendMulticastIci(CPUSet& cpuSet);
	void SendBroadcastIci();

private:
	volatile uint32* _RedistributorForCpu(int32 cpu) const;
	void _PerCpuInit();
	void _EnableInterrupt(int32 irq);
	void _DisableInterrupt(int32 irq);
	void _SendIci(int32 cpu);

	volatile uint32* fGicdRegs;
	volatile uint8* fGicrRegs;
	size_t fGicrSize;
	uint32 fMaxIrq;
};

#endif
