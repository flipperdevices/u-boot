// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2025 Alexey Charkov <alchark@gmail.com>
 */

#include <dm.h>
#include <spl.h>
#include <spl_load.h>
#include <scsi.h>
#include <errno.h>
#include <image.h>
#include <ufs.h>
#include <linux/compiler.h>
#include <log.h>

static ulong spl_ufs_load_read(struct spl_load_info *load, ulong off, ulong size, void *buf)
{
	struct blk_desc *bd = load->priv;
	lbaint_t sector = off >> bd->log2blksz;
	lbaint_t count = size >> bd->log2blksz;

	return blk_dread(bd, sector, count, buf) << bd->log2blksz;
}

/**
 * spl_ufs_load_image_raw_os() - Load the Falcon mode OS payload
 * @spl_image:	Image description to fill in
 * @bootdev:	Boot device being loaded from
 * @bd:		Logical unit holding the payload
 * @sector:	Sector of that logical unit at which the payload starts
 *
 * Only Falcon mode through TF-A is supported for now: the payload is a FIT
 * with TF-A as its firmware, which hands over to a Linux kernel loadable.
 *
 * Return:	0 on success, -EMEDIUMTYPE if an image was loaded fine but is
 *		not an OS payload, such as U-Boot's own FIT, -EOPNOTSUPP if it
 *		is meant for a Falcon mode flow which is not supported, or the
 *		error from loading it
 */
static int spl_ufs_load_image_raw_os(struct spl_image_info *spl_image,
				     struct spl_boot_device *bootdev,
				     struct blk_desc *bd, ulong sector)
{
	struct spl_load_info load;
	int err;

	spl_load_init(&load, spl_ufs_load_read, bd, bd->blksz);
	err = spl_load(spl_image, bootdev, &load, 0, sector << bd->log2blksz);
	if (err)
		return err;

	switch (spl_image->os) {
	case IH_OS_ARM_TRUSTED_FIRMWARE:
		/*
		 * A regular U-Boot FIT carries TF-A as its firmware, too, so a
		 * FIT with TF-A is only accepted if it hands over to Linux.
		 */
		if (CONFIG_IS_ENABLED(LOAD_FIT) &&
		    spl_fit_images_find(spl_image_fdt_addr(spl_image),
					IH_OS_LINUX) >= 0)
			return 0;
		break;
	case IH_OS_LINUX:
	case IH_OS_TEE:
		log_debug("Falcon mode through %s is not supported\n",
			  genimg_get_os_name(spl_image->os));
		return -EOPNOTSUPP;
	}

	puts("Expected OS image is not found\n");
	return -EMEDIUMTYPE;
}

static int spl_ufs_get_boot_lu_blk(struct blk_desc **bdp)
{
	struct udevice *ufs, *scsi;
	int lun, ret;

	ret = uclass_first_device_err(UCLASS_UFS, &ufs);
	if (ret)
		return ret;

	ret = device_find_first_child_by_uclass(ufs, UCLASS_SCSI, &scsi);
	if (ret)
		return ret;

	lun = ufs_get_boot_lun(scsi);
	if (lun < 0)
		return lun;

	/* A UFS controller only ever has a single target */
	return scsi_get_blk_by_lun(scsi, 0, lun, bdp);
}

/**
 * spl_ufs_get_uboot_blk() - Find the logical unit holding U-Boot
 * @bdp:	Returns the block device of that logical unit
 *
 * The logical unit is either the active boot LU or the one at a fixed SCSI
 * device number, as the SPL_UFS_RAW_U_BOOT_USE_* choice selects.
 *
 * Return:	0 on success, or a negative error number
 */
static int spl_ufs_get_uboot_blk(struct blk_desc **bdp)
{
	int devnum, err;

	if (IS_ENABLED(CONFIG_SPL_UFS_RAW_U_BOOT_USE_BOOT_LUN)) {
		err = spl_ufs_get_boot_lu_blk(bdp);
		if (err)
			printf("%s: UFS active boot LU not found (error=%d)\n",
			       __func__, err);
		return err;
	}

	devnum = IF_ENABLED_INT(CONFIG_SPL_UFS_RAW_U_BOOT_USE_DEVNUM,
				CONFIG_SPL_UFS_RAW_U_BOOT_DEVNUM);
	*bdp = blk_get_devnum_by_uclass_id(UCLASS_SCSI, devnum);
	return *bdp ? 0 : -ENODEV;
}

static int spl_ufs_load_image(struct spl_image_info *spl_image,
			      struct spl_boot_device *bootdev)
{
	unsigned long sector = CONFIG_SPL_UFS_RAW_U_BOOT_SECTOR;
	struct spl_image_info pristine = *spl_image;
	struct blk_desc *bd = NULL;
	struct spl_load_info load;
	int err;

	/* try to recognize storage devices immediately */
	scsi_scan(false);

	if (spl_falcon_boot()) {
		ulong os_sector = config_opt_enabled(CONFIG_SPL_OS_BOOT,
						     CONFIG_SPL_UFS_RAW_OS_SECTOR,
						     CONFIG_SPL_UFS_RAW_U_BOOT_SECTOR);
		int os_devnum = config_opt_enabled(CONFIG_SPL_OS_BOOT,
						   CONFIG_SPL_UFS_RAW_OS_DEVNUM,
						   -1);
		struct blk_desc *os_bd;
		int ret;

		os_bd = blk_get_devnum_by_uclass_id(UCLASS_SCSI, os_devnum);
		if (os_bd) {
			err = spl_ufs_load_image_raw_os(spl_image, bootdev,
							os_bd, os_sector);
			if (!err)
				return 0;
		} else {
			puts("spl_ufs_load_image: UFS OS device not found\n");
			err = -ENODEV;
		}

		puts("spl_ufs_load_image: Failed to load falcon payload\n");
		log_debug("(error=%d)\n", err);
		if (CONFIG_IS_ENABLED(OS_BOOT_SECURE))
			return err;

		puts("Fallback to U-Boot\n");
		ret = spl_ufs_get_uboot_blk(&bd);
		if (ret)
			return ret;

		/*
		 * With the OS sharing U-Boot's location, -EMEDIUMTYPE means
		 * we've already just loaded the U-Boot image, so no need to
		 * re-read it again. Compare the block devices rather than how
		 * they were found, as that is what makes it the same location.
		 */
		if (os_bd == bd && os_sector == sector)
			return err == -EMEDIUMTYPE ? 0 : err;

		/* Don't let the failed attempt leak into the U-Boot one */
		*spl_image = pristine;
	}

	if (!bd) {
		err = spl_ufs_get_uboot_blk(&bd);
		if (err)
			return err;
	}

	spl_load_init(&load, spl_ufs_load_read, bd, bd->blksz);
	err = spl_load(spl_image, bootdev, &load, 0, sector << bd->log2blksz);
	if (err) {
		puts("spl_ufs_load_image: ufs block read error\n");
		log_debug("(error=%d)\n", err);
		return err;
	}

	return 0;
}

SPL_LOAD_IMAGE_METHOD("UFS", 0, BOOT_DEVICE_UFS, spl_ufs_load_image);
