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

/*
 * Load the Falcon mode OS payload. Returns -ENOENT if an image was loaded fine
 * but is not something to boot the OS from.
 */
static int spl_ufs_load_image_raw_os(struct spl_image_info *spl_image,
				     struct spl_boot_device *bootdev,
				     int devnum, ulong sector)
{
	struct spl_load_info load;
	struct blk_desc *bd;
	int err;

	bd = blk_get_devnum_by_uclass_id(UCLASS_SCSI, devnum);
	if (!bd) {
		puts("spl_ufs_load_image: UFS OS device not found\n");
		return -ENODEV;
	}

	spl_load_init(&load, spl_ufs_load_read, bd, bd->blksz);
	err = spl_load(spl_image, bootdev, &load, 0, sector << bd->log2blksz);
	if (err)
		return err;

	switch (spl_image->os) {
	case IH_OS_LINUX:
		return 0;
	case IH_OS_TEE:
	case IH_OS_ARM_TRUSTED_FIRMWARE:
		/*
		 * Secure firmware goes on to whatever the FIT carries, and a
		 * regular U-Boot FIT carries it just as well, so only take a
		 * FIT which hands over to Linux.
		 */
		if (CONFIG_IS_ENABLED(LOAD_FIT) &&
		    spl_fit_images_find(spl_image_fdt_addr(spl_image),
					IH_OS_LINUX) >= 0)
			return 0;
		break;
	}

	puts("Expected OS image is not found\n");
	return -ENOENT;
}

static int spl_ufs_load_image(struct spl_image_info *spl_image,
			      struct spl_boot_device *bootdev)
{
	unsigned long sector = CONFIG_SPL_UFS_RAW_U_BOOT_SECTOR;
	int devnum = config_opt_enabled(CONFIG_SPL_UFS_RAW_U_BOOT_USE_DEVNUM,
					CONFIG_SPL_UFS_RAW_U_BOOT_DEVNUM, -1);
	struct spl_image_info pristine = *spl_image;
	struct spl_load_info load;
	struct blk_desc *bd;
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

		err = spl_ufs_load_image_raw_os(spl_image, bootdev, os_devnum,
						os_sector);
		if (!err)
			return 0;

		puts("spl_ufs_load_image: Failed to load falcon payload\n");
		log_debug("(error=%d)\n", err);
		if (CONFIG_IS_ENABLED(OS_BOOT_SECURE))
			return err;

		puts("Fallback to U-Boot\n");
		/*
		 * With the OS sharing U-Boot's location, reading it again
		 * would only give the same result: what was just loaded is
		 * the U-Boot image, if it loaded at all. Only a U-Boot location
		 * given by SCSI device number can be known to be the same.
		 */
		if (CONFIG_IS_ENABLED(UFS_RAW_U_BOOT_USE_DEVNUM) &&
		    os_devnum == devnum && os_sector == sector)
			return err == -ENOENT ? 0 : err;

		/* Don't let the failed attempt leak into the U-Boot one */
		*spl_image = pristine;
	}

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
	err = spl_load(spl_image, bootdev, &load, 0, sector << bd->log2blksz);
	if (err) {
		puts("spl_ufs_load_image: ufs block read error\n");
		log_debug("(error=%d)\n", err);
		return err;
	}

	return 0;
}

SPL_LOAD_IMAGE_METHOD("UFS", 0, BOOT_DEVICE_UFS, spl_ufs_load_image);
