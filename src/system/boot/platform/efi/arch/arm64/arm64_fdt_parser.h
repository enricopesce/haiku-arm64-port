/*
 * Copyright 2026 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef ARM64_FDT_PARSER_H
#define ARM64_FDT_PARSER_H

#include <stddef.h>
#include <stdint.h>


struct arm64_fdt_range {
	uint64_t	start;
	uint64_t	size;
};


bool arm64_fdt_parse_gicv3(const void* reg, int regLength,
	const void* addressCells, int addressCellsLength, const void* sizeCells,
	int sizeCellsLength, arm64_fdt_range& distributor,
	arm64_fdt_range& redistributors);

bool arm64_fdt_parse_virtual_timer(const void* interrupts,
	int interruptsLength, uint32_t& irq);

#endif
