/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef _UFS_H
#define _UFS_H

struct udevice;

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

/**
 * ufs_get_boot_lun() - find the logical unit the device boots from
 *
 * The bBootLunEn attribute selects boot LU A or boot LU B, and the device
 * presents whichever logical unit carries that bBootLunID through its boot
 * well known logical unit. Resolve the same mapping, so that the caller can
 * read the active boot LU as an ordinary logical unit.
 *
 * @scsi_dev: SCSI device of the UFS host controller
 *
 * Return: LUN of the active boot LU, -ENOENT if booting is disabled or no
 * enabled logical unit carries the selected bBootLunID, -EIO if the device
 * rejects a query, other -ve on error. A failed read of one unit descriptor
 * does not stop the search, but is returned if no unit matches.
 */
int ufs_get_boot_lun(struct udevice *scsi_dev);

#define UFS_RPMB_CID_SIZE	16
#define UFS_RPMB_NUM_REGIONS	4

int ufs_rpmb_route_frames(struct udevice *scsi_dev, unsigned int region,
			  void *req, unsigned long reqlen, void *rsp,
			  unsigned long rsplen);

int ufs_rpmb_get_region_info(struct udevice *scsi_dev, unsigned int region,
			     u8 *size_mult, u8 *rel_wr, u8 *cid);

#endif
