// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright Contributors to the U-Boot project.

/*
 * Early initialization code to be executed before RAM training on RK3576
 *
 * Anything in this file is to be compiled into a freestanding raw executable
 * binary and prepended to the RKNS image by mkimage during build
 */

#include <stdint.h>

#define SYS_SRAM_BASE	0x3ff80000
#define OFFSET		0x03b0

int _start(void)
{
	uint32_t *sram = (void *)(SYS_SRAM_BASE + OFFSET);
	uint64_t sctlr;

	/* set unknown value in sram to fix boot from sdmmc */
	*(sram) = 0x3ffff800;

	/*
	 * Enable the EL3 instruction cache before returning to the BootROM.
	 *
	 * The ROM runs with MMU and caches off. After a Maskrom USB download
	 * to DRAM (command 0x472) it verifies the *entire* image with a
	 * bit-serial CRC-16 executed from slow, non-cacheable on-chip ROM.
	 * With the I-cache off, every byte costs ~101 uncached ROM instruction
	 * fetches, so image verification can take noticeable time (~124 KB/s).
	 * Turning on the EL3 I-cache lets the tiny CRC loop run from L1I,
	 * measured ~2 MB/s (~16x speedup). The bit persists into the ROM's
	 * 0x472/CRC path. Harmless for the SD/storage path (small 2nd stage).
	 */
	asm volatile("mrs %0, sctlr_el3" : "=r"(sctlr));
	sctlr |= (1UL << 12);
	asm volatile("msr sctlr_el3, %0; isb" :: "r"(sctlr) : "memory");

	return 0;
}
