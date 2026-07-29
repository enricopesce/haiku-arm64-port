/*
 * Copyright 2019-2021 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *   Alexander von Gluck IV <kallisti5@unixzen.com>
 */

#include <arch_cpu_defs.h>
#include <arch_dtb.h>
#include <arch_smp.h>
#include <boot/platform.h>
#include <boot/stage2.h>

#include <limits.h>

extern "C" {
#include <libfdt.h>
}

#include "dtb.h"


void arm64_handle_fdt_psci_node(const void *fdt, int node);
void arm64_handle_fdt_cpu_node(const void *fdt, int node);


/* TODO: Code taken from ARM port just for building purposes */

/* The potential interrupt controoller would be present in the dts as:
 * compatible = "arm,gic-v3";
 */
const struct supported_interrupt_controllers {
	const char*	dtb_compat;
	const char*	kind;
} kSupportedInterruptControllers[] = {
	{ "arm,cortex-a9-gic", INTC_KIND_GICV1 },
	{ "arm,cortex-a15-gic", INTC_KIND_GICV2 },
	{ "arm,gic-400", INTC_KIND_GICV2 },
	{ "ti,omap3-intc", INTC_KIND_OMAP3 },
	{ "marvell,pxa-intc", INTC_KIND_PXA },
};


enum {
	kGICv3DistributorSize = 0x10000,
	kGICv3RedistributorFrameSize = 0x20000,
	kGICInterruptTypePPI = 1,
	kGICInterruptBasePPI = 16,
	kArmV8VirtualTimerInterrupt = 2
};

static bool sIgnoredGICv3FdtNode;
static bool sIgnoredArmV8TimerFdtNode;


static bool
get_fdt_cells(const void* fdt, int node, const char* name, uint32 defaultValue,
	uint32& value)
{
	int length;
	const uint32* property = (const uint32*)fdt_getprop(fdt, node, name, &length);
	if (property == NULL) {
		value = defaultValue;
		return true;
	}
	if (length != (int)sizeof(uint32))
		return false;

	value = fdt32_to_cpu(*property);
	return value == 1 || value == 2;
}


static bool
get_fdt_reg(const void* fdt, int node, size_t index, addr_range& range)
{
	int parent = fdt_parent_offset(fdt, node);
	if (parent < 0)
		return false;

	uint32 addressCells;
	uint32 sizeCells;
	if (!get_fdt_cells(fdt, parent, "#address-cells", 2, addressCells)
		|| !get_fdt_cells(fdt, parent, "#size-cells", 1, sizeCells)) {
		return false;
	}

	int length;
	const uint32* property = (const uint32*)fdt_getprop(fdt, node, "reg", &length);
	if (property == NULL || length < 0)
		return false;

	size_t entryCells = addressCells + sizeCells;
	size_t entrySize = entryCells * sizeof(uint32);
	if (index == SIZE_MAX || index > SIZE_MAX / entrySize
		|| (index + 1) * entrySize > (size_t)length) {
		return false;
	}

	property += index * entryCells;
	range.start = fdt32_to_cpu(*property++);
	if (addressCells == 2)
		range.start = (range.start << 32) | fdt32_to_cpu(*property++);
	range.size = fdt32_to_cpu(*property++);
	if (sizeCells == 2)
		range.size = (range.size << 32) | fdt32_to_cpu(*property);
	if (range.start == 0 || range.size == 0 || range.start > UINT64_MAX - range.size)
		return false;

	return true;
}


static bool
get_armv8_virtual_timer_ppi(const void* fdt, int node, uint32& irq)
{
	int length;
	const uint32* property = (const uint32*)fdt_getprop(fdt, node, "interrupts", &length);
	if (property == NULL || length != 4 * 3 * (int)sizeof(uint32))
		return false;

	property += kArmV8VirtualTimerInterrupt * 3;
	uint32 type = fdt32_to_cpu(property[0]);
	uint32 number = fdt32_to_cpu(property[1]);
	if (type != kGICInterruptTypePPI || number >= 16)
		return false;

	irq = kGICInterruptBasePPI + number;
	return true;
}


static void
handle_gicv3_node(const void* fdt, int node)
{
	if (gKernelArgs.arch_args.interrupt_controller.kind[0] != 0)
		return;

	addr_range distributor;
	addr_range redistributors;
	if (!get_fdt_reg(fdt, node, 0, distributor)
		|| !get_fdt_reg(fdt, node, 1, redistributors)
		|| distributor.size < kGICv3DistributorSize
		|| redistributors.size < kGICv3RedistributorFrameSize) {
		sIgnoredGICv3FdtNode = true;
		return;
	}

	intc_info& intc = gKernelArgs.arch_args.interrupt_controller;
	strcpy(intc.kind, INTC_KIND_GICV3);
	intc.regs1 = distributor;
	intc.regs2 = redistributors;
}


static void
handle_armv8_timer_node(const void* fdt, int node)
{
	if (gKernelArgs.arch_args.timer_irq != 0)
		return;

	uint32 irq;
	if (!get_armv8_virtual_timer_ppi(fdt, node, irq)) {
		sIgnoredArmV8TimerFdtNode = true;
		return;
	}

	gKernelArgs.arch_args.timer_irq = irq;
}


void
arch_handle_fdt(const void* fdt, int node)
{
	const char* deviceType = (const char*)fdt_getprop(fdt, node,
		"device_type", NULL);

	if (deviceType != NULL) {
		if (strcmp(deviceType, "cpu") == 0) {
			arm64_handle_fdt_cpu_node(fdt, node);
		}
	}

	int compatibleLen;
	const char* compatible = (const char*)fdt_getprop(fdt, node,
		"compatible", &compatibleLen);

	if (compatible == NULL)
		return;

	intc_info &interrupt_controller = gKernelArgs.arch_args.interrupt_controller;
	if (dtb_has_fdt_string(compatible, compatibleLen, "arm,gic-v3"))
		handle_gicv3_node(fdt, node);

	if (interrupt_controller.kind[0] == 0) {
		for (uint32 i = 0; i < B_COUNT_OF(kSupportedInterruptControllers); i++) {
			if (dtb_has_fdt_string(compatible, compatibleLen,
				kSupportedInterruptControllers[i].dtb_compat)) {

				memcpy(interrupt_controller.kind, kSupportedInterruptControllers[i].kind,
					sizeof(interrupt_controller.kind));

				dtb_get_reg(fdt, node, 0, interrupt_controller.regs1);
				dtb_get_reg(fdt, node, 1, interrupt_controller.regs2);
			}
		}
	}

	if (dtb_has_fdt_string(compatible, compatibleLen, "arm,armv8-timer"))
		handle_armv8_timer_node(fdt, node);

	if (strcmp(compatible, "arm,psci-1.0") == 0)
		arm64_handle_fdt_psci_node(fdt, node);
}


void
arch_dtb_set_kernel_args(void)
{
	intc_info &interrupt_controller = gKernelArgs.arch_args.interrupt_controller;
	dprintf("Chosen interrupt controller:\n");
	if (interrupt_controller.kind[0] == 0) {
		dprintf("kind: None!\n");
	} else {
		dprintf("  kind: %s\n", interrupt_controller.kind);
		dprintf("  regs: %#" B_PRIx64 ", %#" B_PRIx64 "\n",
			interrupt_controller.regs1.start,
			interrupt_controller.regs1.size);
		dprintf("        %#" B_PRIx64 ", %#" B_PRIx64 "\n",
			interrupt_controller.regs2.start,
			interrupt_controller.regs2.size);
		if (strcmp(interrupt_controller.kind, INTC_KIND_GICV3) == 0) {
			dprintf("discovered GICv3 from FDT: gicd=%#" B_PRIx64 " (%#" B_PRIx64
				" bytes), gicr=%#" B_PRIx64 " (%#" B_PRIx64 " bytes)\n",
				interrupt_controller.regs1.start, interrupt_controller.regs1.size,
				interrupt_controller.regs2.start, interrupt_controller.regs2.size);
		}
	}
	if (gKernelArgs.arch_args.timer_irq != 0) {
		dprintf("selected ARMv8 virtual timer PPI from FDT: irq=%lu\n",
			(uint64)gKernelArgs.arch_args.timer_irq);
	}
	if (sIgnoredGICv3FdtNode)
		dprintf("ignoring malformed GICv3 FDT node\n");
	if (sIgnoredArmV8TimerFdtNode)
		dprintf("ignoring malformed ARMv8 timer FDT node\n");
}
