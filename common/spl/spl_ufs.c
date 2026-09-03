// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2025 Alexey Charkov <alchark@gmail.com>
 */

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

static int spl_ufs_load_image_raw_os(struct spl_image_info *spl_image,
				     struct spl_boot_device *bootdev,
				     struct spl_load_info *load)
{
	ulong sector = config_opt_enabled(CONFIG_SPL_OS_BOOT,
					  CONFIG_SPL_UFS_RAW_OS_SECTOR,
					  CONFIG_SPL_UFS_RAW_U_BOOT_SECTOR);
	struct blk_desc *bd = load->priv;
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
	int devnum = config_opt_enabled(CONFIG_SPL_UFS_RAW_U_BOOT_USE_DEVNUM,
					CONFIG_SPL_UFS_RAW_U_BOOT_DEVNUM, -1);
	struct spl_load_info load;
	struct blk_desc *bd;
	int err;

	/* try to recognize storage devices immediately */
	scsi_scan(false);
	if (CONFIG_IS_ENABLED(UFS_RAW_U_BOOT_USE_BOOT_WLUN)) {
		/* A UFS controller only ever has a single target */
		if (scsi_get_blk_by_lun(0, UFS_UPIU_BOOT_WLUN, &bd)) {
			puts("spl_ufs_load_image: UFS boot LU not found\n");
			return -ENODEV;
		}
	} else {
		bd = blk_get_devnum_by_uclass_id(UCLASS_SCSI, devnum);
		if (!bd)
			return -ENODEV;
	}

	spl_load_init(&load, spl_ufs_load_read, bd, bd->blksz);
	if (spl_falcon_boot()) {
		int os_devnum = config_opt_enabled(CONFIG_SPL_OS_BOOT,
						   CONFIG_SPL_UFS_RAW_OS_DEVNUM,
						   -1);
		struct spl_load_info *os_load = &load;
		struct spl_load_info os_load_info;
		struct blk_desc *os_bd;

		/*
		 * U-Boot's load info can be reused only when both images are
		 * addressed by SCSI device number and those numbers match.
		 * Anything else needs a device, and a load info, of its own.
		 */
		if (!CONFIG_IS_ENABLED(UFS_RAW_U_BOOT_USE_DEVNUM) ||
		    os_devnum != devnum) {
			os_bd = blk_get_devnum_by_uclass_id(UCLASS_SCSI, os_devnum);
			if (!os_bd) {
				puts("spl_ufs_load_image: UFS OS device not found\n");
				if (CONFIG_IS_ENABLED(OS_BOOT_SECURE))
					return -ENODEV;
				goto fallback;
			}
			spl_load_init(&os_load_info, spl_ufs_load_read, os_bd,
				      os_bd->blksz);
			os_load = &os_load_info;
		}

		err = spl_ufs_load_image_raw_os(spl_image, bootdev, os_load);
		if (!err)
			return 0;

		puts("spl_ufs_load_image: Failed to load falcon payload\n");
		log_debug("(error=%d)\n", err);
		if (CONFIG_IS_ENABLED(OS_BOOT_SECURE))
			return err;

fallback:
		puts("Fallback to U-Boot\n");
	}

	err = spl_load(spl_image, bootdev, &load, 0, sector << bd->log2blksz);
	if (err) {
		puts("spl_ufs_load_image: ufs block read error\n");
		log_debug("(error=%d)\n", err);
		return err;
	}

	return 0;
}

SPL_LOAD_IMAGE_METHOD("UFS", 0, BOOT_DEVICE_UFS, spl_ufs_load_image);
