#
# (C) Copyright 2023 Rockchip Electronics Co., Ltd
#
# SPDX-License-Identifier:     GPL-2.0+
#

obj-y += rk3576.o
obj-y += clk_rk3576.o
obj-y += syscon_rk3576.o

# BootROM "boost" stub: a freestanding bit of AArch64 code that pokes a fix-up
# value into SRAM so the ROM can boot from SD/eMMC. mkimage prepends it to the
# init image, so it must exist in the build dir.
quiet_cmd_rk3576boost = BOOST   $@
      cmd_rk3576boost = $(CC) -nostdlib -ffreestanding -Os -c -o $@.o $< && \
			$(OBJCOPY) -O binary -j .text $@.o $@
$(obj)/rk3576-boost.bin: $(src)/rk3576-boost.c FORCE
	$(call if_changed,rk3576boost)
always-y += rk3576-boost.bin
