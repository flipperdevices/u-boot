// SPDX-License-Identifier: GPL-2.0+
/*
 * Handling of common block commands
 *
 * Copyright (c) 2017 Google, Inc
 *
 * (C) Copyright 2000-2011
 * Wolfgang Denk, DENX Software Engineering, wd@denx.de.
 */

#include <blk.h>
#include <command.h>
#include <mapmem.h>
#include <vsprintf.h>

#if CONFIG_IS_ENABLED(IMAGE_SPARSE)
#include <sparse_format.h>
#include <image-sparse.h>

static lbaint_t blk_sparse_write(struct sparse_storage *info, lbaint_t blk,
				 lbaint_t blkcnt, const void *buffer)
{
	struct blk_desc *dev_desc = info->priv;

	return blk_dwrite(dev_desc, blk, blkcnt, buffer);
}

static lbaint_t blk_sparse_reserve(struct sparse_storage *info,
				   lbaint_t blk, lbaint_t blkcnt)
{
	return blkcnt;
}
#endif

int blk_common_cmd(int argc, char *const argv[], enum uclass_id uclass_id,
		   int *cur_devnump)
{
	const char *if_name = blk_get_uclass_name(uclass_id);

	switch (argc) {
	case 0:
	case 1:
		return CMD_RET_USAGE;
	case 2:
		if (strncmp(argv[1], "inf", 3) == 0) {
			blk_list_devices(uclass_id);
			return CMD_RET_SUCCESS;
		} else if (strncmp(argv[1], "dev", 3) == 0) {
			if (blk_print_device_num(uclass_id, *cur_devnump)) {
				printf("\nno %s devices available\n", if_name);
				return CMD_RET_FAILURE;
			}
			return CMD_RET_SUCCESS;
		} else if (strncmp(argv[1], "part", 4) == 0) {
			if (blk_list_part(uclass_id))
				printf("\nno %s partition table available\n",
				       if_name);
			return CMD_RET_SUCCESS;
		}
		return CMD_RET_USAGE;
	case 3:
		if (strncmp(argv[1], "dev", 3) == 0) {
			int dev = (int)dectoul(argv[2], NULL);

			if (!blk_show_device(uclass_id, dev)) {
				*cur_devnump = dev;
				printf("... is now current device\n");
			} else {
				return CMD_RET_FAILURE;
			}
			return CMD_RET_SUCCESS;
		} else if (strncmp(argv[1], "part", 4) == 0) {
			int dev = (int)dectoul(argv[2], NULL);

			if (blk_print_part_devnum(uclass_id, dev)) {
				printf("\n%s device %d not available\n",
				       if_name, dev);
				return CMD_RET_FAILURE;
			}
			return CMD_RET_SUCCESS;
		}
		return CMD_RET_USAGE;

	default: /* at least 4 args */
		if (strcmp(argv[1], "read") == 0) {
			phys_addr_t paddr = hextoul(argv[2], NULL);
			lbaint_t blk = hextoul(argv[3], NULL);
			ulong cnt = hextoul(argv[4], NULL);
			struct blk_desc *desc;
			void *vaddr;
			ulong n;
			int ret;

			printf("\n%s read: device %d block # "LBAFU", count %lu ... ",
			       if_name, *cur_devnump, blk, cnt);

			ret = blk_get_desc(uclass_id, *cur_devnump, &desc);
			if (ret)
				return CMD_RET_FAILURE;
			vaddr = map_sysmem(paddr, desc->blksz * cnt);
			n = blk_dread(desc, blk, cnt, vaddr);
			unmap_sysmem(vaddr);

			printf("%ld blocks read: %s\n", n,
			       n == cnt ? "OK" : "ERROR");
			return n == cnt ? CMD_RET_SUCCESS : CMD_RET_FAILURE;
		} else if (strcmp(argv[1], "write") == 0) {
			phys_addr_t paddr = hextoul(argv[2], NULL);
			lbaint_t blk = hextoul(argv[3], NULL);
			ulong cnt = hextoul(argv[4], NULL);
			struct blk_desc *desc;
			void *vaddr;
			ulong n;
			int ret;

			printf("\n%s write: device %d block # "LBAFU", count %lu ... ",
			       if_name, *cur_devnump, blk, cnt);

			ret = blk_get_desc(uclass_id, *cur_devnump, &desc);
			if (ret)
				return CMD_RET_FAILURE;
			vaddr = map_sysmem(paddr, desc->blksz * cnt);
			n = blk_dwrite(desc, blk, cnt, vaddr);
			unmap_sysmem(vaddr);

			printf("%ld blocks written: %s\n", n,
			       n == cnt ? "OK" : "ERROR");
			return n == cnt ? CMD_RET_SUCCESS : CMD_RET_FAILURE;
		} else if (strcmp(argv[1], "erase") == 0) {
			lbaint_t blk = hextoul(argv[2], NULL);
			ulong cnt = hextoul(argv[3], NULL);
			struct blk_desc *desc;
			ulong n;

			printf("\n%s erase: device %d block # "LBAFU", count %lu ... ",
			       if_name, *cur_devnump, blk, cnt);

			if (blk_get_desc(uclass_id, *cur_devnump, &desc))
				return CMD_RET_FAILURE;

			n = blk_derase(desc, blk, cnt);

			printf("%ld blocks erased: %s\n", n,
			       n == cnt ? "OK" : "ERROR");
			return n == cnt ? CMD_RET_SUCCESS : CMD_RET_FAILURE;
#if CONFIG_IS_ENABLED(IMAGE_SPARSE)
		} else if (strcmp(argv[1], "swrite") == 0) {
			struct sparse_storage sparse;
			struct blk_desc *desc;
			char dest[11];
			void *addr;
			u32 blk;
			int ret;

			if (argc != 4)
				return CMD_RET_USAGE;

			addr = (void *)hextoul(argv[2], NULL);
			blk = hextoul(argv[3], NULL);

			if (!is_sparse_image(addr)) {
				printf("Not a sparse image\n");
				return CMD_RET_FAILURE;
			}

			ret = blk_get_desc(uclass_id, *cur_devnump, &desc);
			if (ret)
				return CMD_RET_FAILURE;

			printf("\n%s sparse write: device %d block # %d ... ",
			       if_name, *cur_devnump, blk);

			sparse.priv = desc;
			sparse.blksz = desc->blksz;
			sparse.start = blk;
			sparse.size = desc->lba - blk;
			sparse.write = blk_sparse_write;
			sparse.reserve = blk_sparse_reserve;
			sparse.mssg = NULL;
			sprintf(dest, "0x" LBAF, sparse.start * sparse.blksz);

			if (write_sparse_image(&sparse, dest, addr, NULL))
				return CMD_RET_FAILURE;
			else
				return CMD_RET_SUCCESS;
#endif
		} else {
			return CMD_RET_USAGE;
		}
	}
}
