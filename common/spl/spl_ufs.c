// SPDX-License-Identifier: GPL-2.0+

#include <spl.h>
#include <spl_load.h>
#include <sata.h>
#include <scsi.h>
#include <errno.h>
#include <image.h>
#include <linux/compiler.h>
#include <log.h>

static ulong h_spl_load_read(struct spl_load_info *load, ulong off,
			     ulong size, void *buf)
{
	struct blk_desc *bd = load->priv;
	lbaint_t sector = off >> bd->log2blksz;
	lbaint_t count = size >> bd->log2blksz;

	return blk_dread(bd, sector, count, buf) << bd->log2blksz;
}

static int spl_ufs_load_image(struct spl_image_info *spl_image,
			      struct spl_boot_device *bootdev)
{
	int err = -ENOSYS;
	struct blk_desc *bd;
	struct spl_load_info load;
	unsigned long sector = CONFIG_SPL_UFS_RAW_U_BOOT_SECTOR;

	/* try to recognize storage devices immediately */
	scsi_scan(false);
	bd = blk_get_devnum_by_uclass_id(UCLASS_SCSI, 0);
	if (!bd)
		return -ENODEV;

	spl_load_init(&load, h_spl_load_read, bd, bd->blksz);
	err = spl_load(spl_image, bootdev, &load, 0, sector << bd->log2blksz);
	if (err) {
		puts("spl_ufs_load_image: ufs block read error\n");
		log_debug("(error=%d)\n", err);
		return err;
	}

	return 0;
}
SPL_LOAD_IMAGE_METHOD("UFS", 0, BOOT_DEVICE_UFS, spl_ufs_load_image);
