/*
 * Copyright 2026 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include "arm64_fdt_parser.h"

#include <limits.h>


static const uint64_t kGICv3DistributorSize = 0x10000;
static const uint64_t kGICv3RedistributorFrameSize = 0x20000;
static const uint32_t kGICInterruptTypePPI = 1;
static const uint32_t kGICInterruptBasePPI = 16;
static const size_t kArmV8VirtualTimerInterrupt = 2;


static uint32_t
read_fdt_u32(const uint8_t* data)
{
	return (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16
		| (uint32_t)data[2] << 8 | data[3];
}


static bool
parse_cell_count(const void* property, int length, uint32_t defaultValue,
	uint32_t& value)
{
	if (property == NULL) {
		if (length >= 0)
			return false;
		value = defaultValue;
		return true;
	}
	if (length != (int)sizeof(uint32_t))
		return false;

	value = read_fdt_u32((const uint8_t*)property);
	return value == 1 || value == 2;
}


static bool
parse_range(const uint8_t* property, size_t entryCells, uint32_t addressCells,
	uint32_t sizeCells, size_t index, arm64_fdt_range& range)
{
	property += index * entryCells * sizeof(uint32_t);

	range.start = read_fdt_u32(property);
	property += sizeof(uint32_t);
	if (addressCells == 2) {
		range.start = range.start << 32 | read_fdt_u32(property);
		property += sizeof(uint32_t);
	}

	range.size = read_fdt_u32(property);
	if (sizeCells == 2) {
		property += sizeof(uint32_t);
		range.size = range.size << 32 | read_fdt_u32(property);
	}

	return range.start != 0 && range.size != 0
		&& range.start <= UINT64_MAX - range.size;
}


bool
arm64_fdt_parse_gicv3(const void* reg, int regLength,
	const void* addressCellsProperty, int addressCellsLength,
	const void* sizeCellsProperty, int sizeCellsLength,
	arm64_fdt_range& distributor, arm64_fdt_range& redistributors)
{
	uint32_t addressCells;
	uint32_t sizeCells;
	if (!parse_cell_count(addressCellsProperty, addressCellsLength, 2,
			addressCells)
		|| !parse_cell_count(sizeCellsProperty, sizeCellsLength, 1,
			sizeCells)) {
		return false;
	}

	if (reg == NULL || regLength < 0)
		return false;

	const size_t entryCells = addressCells + sizeCells;
	const size_t entrySize = entryCells * sizeof(uint32_t);
	if ((size_t)regLength < 2 * entrySize
		|| (size_t)regLength % entrySize != 0) {
		return false;
	}

	const uint8_t* property = (const uint8_t*)reg;
	if (!parse_range(property, entryCells, addressCells, sizeCells, 0,
			distributor)
		|| !parse_range(property, entryCells, addressCells, sizeCells, 1,
			redistributors)) {
		return false;
	}

	return distributor.size >= kGICv3DistributorSize
		&& redistributors.size >= kGICv3RedistributorFrameSize;
}


bool
arm64_fdt_parse_virtual_timer(const void* interrupts, int interruptsLength,
	uint32_t& irq)
{
	const size_t tupleSize = 3 * sizeof(uint32_t);
	if (interrupts == NULL || interruptsLength < 0
		|| (size_t)interruptsLength < (kArmV8VirtualTimerInterrupt + 1)
			* tupleSize
		|| (size_t)interruptsLength % tupleSize != 0) {
		return false;
	}

	const uint8_t* property = (const uint8_t*)interrupts
		+ kArmV8VirtualTimerInterrupt * tupleSize;
	uint32_t type = read_fdt_u32(property);
	uint32_t number = read_fdt_u32(property + sizeof(uint32_t));
	if (type != kGICInterruptTypePPI || number >= 16)
		return false;

	irq = kGICInterruptBasePPI + number;
	return true;
}
