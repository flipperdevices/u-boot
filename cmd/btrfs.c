// SPDX-License-Identifier: GPL-2.0+
/*
 * 2017 by Marek Behún <kabel@kernel.org>
 */

#include <command.h>
#include <btrfs.h>
#include <fs.h>

int do_btrsubvol(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	if (argc != 3)
		return CMD_RET_USAGE;

	if (fs_set_blk_dev(argv[1], argv[2], FS_TYPE_BTRFS))
		return 1;

	btrfs_list_subvols();
	return 0;
}

static int do_btr_ls(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	return do_ls(cmdtp, flag, argc, argv, FS_TYPE_BTRFS);
}

static int do_btr_load(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	return do_load(cmdtp, flag, argc, argv, FS_TYPE_BTRFS);
}

U_BOOT_CMD(btrsubvol, 3, 1, do_btrsubvol,
	"list subvolumes of a BTRFS filesystem",
	"<interface> <dev[:part]>\n"
	"     - List subvolumes of a BTRFS filesystem."
);

U_BOOT_CMD(
	btrls, 4, 1, do_btr_ls,
	"list files in a directory (default /) on a BTRFS filesystem",
	"<interface> [<dev[:part]>] [directory]\n"
	"    - list files from 'dev' on 'interface' in 'directory'"
);

U_BOOT_CMD(
	btrload, 7, 0, do_btr_load,
	"load binary file from a BTRFS filesystem",
	"<interface> [<dev[:part]>] <addr> <filename> [bytes [pos]]\n"
	"    - load file 'filename' from 'dev' on 'interface' to 'addr'\n"
	"      'pos' is the file offset to start from; if 'bytes' is 0 or\n"
	"      omitted, the whole file is read. Sets env fileaddr/filesize."
);
