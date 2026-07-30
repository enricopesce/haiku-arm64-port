/*
 * Copyright 2026 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include "arm64_fdt_parser.h"

#include <stdio.h>
#include <string.h>


static int sFailures;


static void
put_u32(uint8_t* data, uint32_t value)
{
	data[0] = value >> 24;
	data[1] = value >> 16;
	data[2] = value >> 8;
	data[3] = value;
}


static void
expect(bool condition, const char* name)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", name);
		sFailures++;
	}
}


static void
make_qemu_reg(uint8_t* reg)
{
	const uint32_t cells[] = {
		0, 0x08000000, 0, 0x00010000,
		0, 0x080a0000, 0, 0x00f60000
	};
	for (size_t i = 0; i < sizeof(cells) / sizeof(cells[0]); i++)
		put_u32(reg + i * sizeof(uint32_t), cells[i]);
}


static void
make_qemu_timer(uint8_t* interrupts)
{
	const uint32_t cells[] = {
		1, 13, 4, 1, 14, 4, 1, 11, 4, 1, 10, 4
	};
	for (size_t i = 0; i < sizeof(cells) / sizeof(cells[0]); i++)
		put_u32(interrupts + i * sizeof(uint32_t), cells[i]);
}


int
main()
{
	uint8_t addressCells[4];
	uint8_t sizeCells[4];
	put_u32(addressCells, 2);
	put_u32(sizeCells, 2);

	uint8_t reg[8 * sizeof(uint32_t)];
	make_qemu_reg(reg);
	arm64_fdt_range distributor = {};
	arm64_fdt_range redistributors = {};
	expect(arm64_fdt_parse_gicv3(reg, sizeof(reg), addressCells,
			sizeof(addressCells), sizeCells, sizeof(sizeCells), distributor,
			redistributors),
		"valid QEMU GICv3");
	expect(distributor.start == 0x08000000
			&& distributor.size == 0x10000,
		"QEMU GICD range");
	expect(redistributors.start == 0x080a0000
			&& redistributors.size == 0xf60000,
		"QEMU GICR range");

	expect(!arm64_fdt_parse_gicv3(NULL, -1, addressCells,
			sizeof(addressCells), sizeCells, sizeof(sizeCells), distributor,
			redistributors),
		"missing reg");
	expect(!arm64_fdt_parse_gicv3(reg, sizeof(reg) - sizeof(uint32_t),
			addressCells, sizeof(addressCells), sizeCells, sizeof(sizeCells),
			distributor, redistributors),
		"truncated reg entry");
	expect(!arm64_fdt_parse_gicv3(reg, sizeof(reg) - 1, addressCells,
			sizeof(addressCells), sizeCells, sizeof(sizeCells), distributor,
			redistributors),
		"reg length not multiple of entry");

	uint8_t malformedReg[sizeof(reg)];
	memcpy(malformedReg, reg, sizeof(reg));
	put_u32(malformedReg + 3 * sizeof(uint32_t), 0xffff);
	expect(!arm64_fdt_parse_gicv3(malformedReg, sizeof(malformedReg),
			addressCells, sizeof(addressCells), sizeCells, sizeof(sizeCells),
			distributor, redistributors),
		"undersized GICD");
	memcpy(malformedReg, reg, sizeof(reg));
	put_u32(malformedReg + 7 * sizeof(uint32_t), 0x1ffff);
	expect(!arm64_fdt_parse_gicv3(malformedReg, sizeof(malformedReg),
			addressCells, sizeof(addressCells), sizeCells, sizeof(sizeCells),
			distributor, redistributors),
		"undersized GICR");
	memcpy(malformedReg, reg, sizeof(reg));
	put_u32(malformedReg, 0xffffffff);
	put_u32(malformedReg + sizeof(uint32_t), 0xffff0000);
	put_u32(malformedReg + 2 * sizeof(uint32_t), 0);
	put_u32(malformedReg + 3 * sizeof(uint32_t), 0x10001);
	expect(!arm64_fdt_parse_gicv3(malformedReg, sizeof(malformedReg),
			addressCells, sizeof(addressCells), sizeCells, sizeof(sizeCells),
			distributor, redistributors),
		"range overflow");

	uint8_t badCells[8] = {};
	put_u32(badCells, 3);
	expect(!arm64_fdt_parse_gicv3(reg, sizeof(reg), badCells, 4,
			sizeCells, sizeof(sizeCells), distributor, redistributors),
		"unsupported address cells");
	put_u32(badCells, 2);
	expect(!arm64_fdt_parse_gicv3(reg, sizeof(reg), badCells, 8,
			sizeCells, sizeof(sizeCells), distributor, redistributors),
		"malformed address cells length");
	put_u32(badCells, 0);
	expect(!arm64_fdt_parse_gicv3(reg, sizeof(reg), addressCells,
			sizeof(addressCells), badCells, 4, distributor, redistributors),
		"unsupported size cells");
	expect(!arm64_fdt_parse_gicv3(reg, sizeof(reg), addressCells,
			sizeof(addressCells), badCells, 8, distributor, redistributors),
		"malformed size cells length");

	uint8_t interrupts[12 * sizeof(uint32_t)];
	make_qemu_timer(interrupts);
	uint32_t irq = 0;
	expect(arm64_fdt_parse_virtual_timer(interrupts, sizeof(interrupts), irq)
			&& irq == 27,
		"valid QEMU virtual timer PPI");
	expect(!arm64_fdt_parse_virtual_timer(NULL, -1, irq),
		"missing timer interrupts");
	expect(!arm64_fdt_parse_virtual_timer(interrupts,
			8 * sizeof(uint32_t), irq),
		"short timer interrupts");
	put_u32(interrupts + 6 * sizeof(uint32_t), 0);
	expect(!arm64_fdt_parse_virtual_timer(interrupts, sizeof(interrupts), irq),
		"timer interrupt is not PPI");
	make_qemu_timer(interrupts);
	put_u32(interrupts + 7 * sizeof(uint32_t), 16);
	expect(!arm64_fdt_parse_virtual_timer(interrupts, sizeof(interrupts), irq),
		"timer PPI out of range");

	if (sFailures != 0)
		return 1;
	puts("ARM64 FDT parser tests passed");
	return 0;
}
