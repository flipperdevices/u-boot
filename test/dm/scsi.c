// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2015 Google, Inc
 */

#include <dm.h>
#include <part.h>
#include <scsi.h>
#include <dm/test.h>
#include <test/test.h>
#include <test/ut.h>

/* Test that sandbox SCSI works correctly */
static int dm_test_scsi_base(struct unit_test_state *uts)
{
	const struct disk_partition *info;
	const struct disk_part *part;
	struct udevice *dev;

	ut_assertok(scsi_scan(false));

	/*
	 * We expect some sort of partition on the disk image, created by
	 * test_ut_dm_init()
	 */
	ut_assertok(uclass_first_device_err(UCLASS_PARTITION, &dev));

	part = dev_get_uclass_plat(dev);
	ut_asserteq(1, part->partnum);

	info = &part->gpt_part_info;
	ut_asserteq_str("sda1", info->name);
	ut_asserteq_str("0x83", info->type);
	ut_asserteq(0x83 /* linux */, info->sys_ind);

	return 0;
}
DM_TEST(dm_test_scsi_base, UTF_SCAN_PDATA | UTF_SCAN_FDT);

/* Test looking up a SCSI block device by target and LUN */
static int dm_test_scsi_get_blk_by_lun(struct unit_test_state *uts)
{
	struct blk_desc *desc;
	struct udevice *dev;

	ut_assertok(scsi_scan(false));
	ut_assertok(uclass_first_device_err(UCLASS_SCSI, &dev));

	/* The sandbox emulator answers on target 0, LUN 0 only */
	ut_assertok(scsi_get_blk_by_lun(dev, 0, 0, &desc));
	ut_asserteq(0, desc->target);
	ut_asserteq(0, desc->lun);
	ut_asserteq_ptr(dev, dev_get_parent(desc->bdev));

	ut_asserteq(-ENODEV, scsi_get_blk_by_lun(dev, 0, 1, &desc));
	ut_asserteq(-ENODEV, scsi_get_blk_by_lun(dev, 1, 0, &desc));

	return 0;
}
DM_TEST(dm_test_scsi_get_blk_by_lun, UTF_SCAN_PDATA | UTF_SCAN_FDT);
