// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright Contributors to the U-Boot project.

/*
 * Early initialization code to be executed before RAM training on RK3576.
 * At the moment this only contains a BootROM fixup for booting from an SD card
 *
 * Anything in this file is to be compiled into a freestanding raw executable
 * binary and prepended to the RKNS image by mkimage during build
 */

#include <stdint.h>

#define SYS_SRAM_BASE	0x3ff80000
#define OFFSET		0x03b0

int _start(void)
{
	uint32_t *sram = (void*)(SYS_SRAM_BASE + OFFSET);

	/* set unknown value in sram to fix boot from sdmmc */
	*(sram) = 0x3ffff800;

	return 0;
}
