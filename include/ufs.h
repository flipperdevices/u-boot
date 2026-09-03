/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef _UFS_H
#define _UFS_H

struct udevice;

/* Well known logical unit ids, as they appear in the LUN field of a UPIU */
enum {
	UFS_UPIU_REPORT_LUNS_WLUN	= 0x81,
	UFS_UPIU_BOOT_WLUN		= 0xB0,
	UFS_UPIU_RPMB_WLUN		= 0xC4,
	UFS_UPIU_UFS_DEVICE_WLUN	= 0xD0,
};

/**
 * ufs_probe() - initialize all devices in the UFS uclass
 *
 * Return: 0 if Ok, -ve on error
 */
int ufs_probe(void);

/**
 * ufs_probe_dev() - initialize a particular device in the UFS uclass
 *
 * @index: index in the uclass sequence
 *
 * Return: 0 if successfully probed, -ve on error
 */
int ufs_probe_dev(int index);

#endif
