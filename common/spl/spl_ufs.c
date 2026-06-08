// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2025 Alexey Charkov <alchark@gmail.com>
 */

#include <spl.h>
#include <spl_load.h>
#include <scsi.h>
#include <errno.h>
#include <image.h>
#include <linux/compiler.h>
#include <log.h>

static ulong spl_ufs_load_read(struct spl_load_info *load, ulong off, ulong size, void *buf)
{
	struct blk_desc *bd = load->priv;
	lbaint_t sector = off >> bd->log2blksz;
	lbaint_t count = size >> bd->log2blksz;

	return blk_dread(bd, sector, count, buf) << bd->log2blksz;
}

static int spl_ufs_load_image_raw_os(struct spl_image_info *spl_image,
				     struct spl_boot_device *bootdev,
				     struct spl_load_info *load,
				     struct blk_desc *bd)
{
	ulong sector = config_opt_enabled(CONFIG_SPL_OS_BOOT,
					  CONFIG_SPL_UFS_RAW_OS_SECTOR, 0);
	int err;

	err = spl_load(spl_image, bootdev, load, 0,
		       sector << bd->log2blksz);
	if (err)
		return err;

	if (spl_image->os != IH_OS_LINUX && spl_image->os != IH_OS_TEE &&
	    spl_image->os != IH_OS_ARM_TRUSTED_FIRMWARE) {
		puts("Expected OS image is not found\n");
		return -ENOENT;
	}

	return 0;
}

static int spl_ufs_load_image(struct spl_image_info *spl_image,
			      struct spl_boot_device *bootdev)
{
	unsigned long sector = CONFIG_SPL_UFS_RAW_U_BOOT_SECTOR;
	int devnum = CONFIG_SPL_UFS_RAW_U_BOOT_DEVNUM;
	int os_devnum;
	struct spl_load_info load, os_load;
	struct blk_desc *bd, *os_bd;
	int err;

	/* try to recognize storage devices immediately */
	scsi_scan(false);
	bd = blk_get_devnum_by_uclass_id(UCLASS_SCSI, devnum);
	if (!bd)
		return -ENODEV;

	spl_load_init(&load, spl_ufs_load_read, bd, bd->blksz);
	if (IS_ENABLED(CONFIG_SPL_OS_BOOT) && !spl_start_uboot()) {
		os_devnum = config_opt_enabled(CONFIG_SPL_OS_BOOT,
					      CONFIG_SPL_UFS_RAW_OS_DEVNUM,
					      CONFIG_SPL_UFS_RAW_U_BOOT_DEVNUM);
		if (os_devnum == devnum) {
			os_bd = bd;
			os_load = load;
		} else {
			os_bd = blk_get_devnum_by_uclass_id(UCLASS_SCSI, os_devnum);
			if (!os_bd) {
				puts("spl_ufs_load_image: UFS OS device not found\n");
				if (IS_ENABLED(CONFIG_SPL_OS_BOOT_SECURE))
					return -ENODEV;
				goto load_uboot;
			}
			spl_load_init(&os_load, spl_ufs_load_read, os_bd, os_bd->blksz);
		}

		err = spl_ufs_load_image_raw_os(spl_image, bootdev, &os_load, os_bd);
		if (!err)
			return 0;

		puts("spl_ufs_load_image: Failed to load falcon payload\n");
		log_debug("(error=%d)\n", err);
		if (IS_ENABLED(CONFIG_SPL_OS_BOOT_SECURE))
			return err;
	}

load_uboot:
	err = spl_load(spl_image, bootdev, &load, 0, sector << bd->log2blksz);
	if (err) {
		puts("spl_ufs_load_image: ufs block read error\n");
		log_debug("(error=%d)\n", err);
		return err;
	}

	return 0;
}

SPL_LOAD_IMAGE_METHOD("UFS", 0, BOOT_DEVICE_UFS, spl_ufs_load_image);
