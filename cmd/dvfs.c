// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018 Fuzhou Rockchip Electronics Co., Ltd
 */

#include <command.h>
#include <dm.h>
#include <dvfs.h>
#include <string.h>

static int do_dvfs(struct cmd_tbl *cmdtp, int flag,
		   int argc, char *const argv[])
{
	struct udevice *dev;
	int ret;

	ret = uclass_get_device(UCLASS_DVFS, 0, &dev);
	if (ret) {
		if (ret != -ENODEV)
			printf("DVFS: Get dvfs device failed, ret=%d\n", ret);
		return ret;
	}

	if (argc == 1)
		return dvfs_apply(dev);
	else if (!strcmp(argv[1], "repeat"))
		return dvfs_repeat_apply(dev);
	else if (!strcmp(argv[1], "status"))
		return dvfs_status(dev);
	else if (!strcmp(argv[1], "max"))
		return dvfs_set_max(dev);
	else
		return CMD_RET_USAGE;

	return 0;
}

U_BOOT_CMD(
	dvfs, 2, 1, do_dvfs,
	"Start DVFS policy",
	"dvfs        - apply dvfs policy once (adjust for temperature)\n"
	"dvfs repeat - repeat apply dvfs policy until achieve target temperature\n"
	"dvfs status - show current OPP table and active OPP\n"
	"dvfs max    - set CPU to maximum OPP (ignore temperature)"
);
