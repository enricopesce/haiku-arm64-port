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

extern "C" {
#include <libfdt.h>
}

#include "arm64_fdt_parser.h"
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


static bool sIgnoredGICv3FdtNode;
static bool sIgnoredArmV8TimerFdtNode;


static void
handle_gicv3_node(const void* fdt, int node)
{
	if (gKernelArgs.arch_args.interrupt_controller.kind[0] != 0)
		return;

	int parent = fdt_parent_offset(fdt, node);
	if (parent < 0) {
		sIgnoredGICv3FdtNode = true;
		return;
	}

	int regLength;
	const void* reg = fdt_getprop(fdt, node, "reg", &regLength);
	int addressCellsLength;
	const void* addressCells = fdt_getprop(fdt, parent, "#address-cells",
		&addressCellsLength);
	int sizeCellsLength;
	const void* sizeCells = fdt_getprop(fdt, parent, "#size-cells",
		&sizeCellsLength);

	arm64_fdt_range distributor;
	arm64_fdt_range redistributors;
	if (!arm64_fdt_parse_gicv3(reg, regLength, addressCells,
			addressCellsLength, sizeCells, sizeCellsLength, distributor,
			redistributors)) {
		sIgnoredGICv3FdtNode = true;
		return;
	}

	intc_info& intc = gKernelArgs.arch_args.interrupt_controller;
	strcpy(intc.kind, INTC_KIND_GICV3);
	intc.regs1.start = distributor.start;
	intc.regs1.size = distributor.size;
	intc.regs2.start = redistributors.start;
	intc.regs2.size = redistributors.size;
}


static void
handle_armv8_timer_node(const void* fdt, int node)
{
	if (gKernelArgs.arch_args.timer_irq != 0)
		return;

	int interruptsLength;
	const void* interrupts = fdt_getprop(fdt, node, "interrupts",
		&interruptsLength);
	uint32 irq;
	if (!arm64_fdt_parse_virtual_timer(interrupts, interruptsLength, irq)) {
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
