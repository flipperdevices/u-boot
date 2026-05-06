/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Configuration for the Flipper One (RK3576)
 *
 * Copyright (c) 2026 Flipper FZCO
 */

#ifndef __CONFIG_FLIPPER_ONE_RK3576_H
#define __CONFIG_FLIPPER_ONE_RK3576_H

#define ROCKCHIP_DEVICE_SETTINGS \
	"stdin=serial,flipper-one-btns\0" \
	"stdout=serial,vidconsole\0" \
	"stderr=serial,vidconsole\0"

#define BOOT_TARGETS "mmc0 scsi"

#include "rk3576_common.h"

#endif /* __CONFIG_FLIPPER_ONE_RK3576_H */
