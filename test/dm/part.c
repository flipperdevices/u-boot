// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2020 Sean Anderson <sean.anderson@seco.com>
 */

#include <dm.h>
#include <env.h>
#include <malloc.h>
#include <memalign.h>
#include <mmc.h>
#include <part.h>
#include <part_efi.h>
#include <dm/test.h>
#include <test/ut.h>

static int do_test(struct unit_test_state *uts, int expected,
		   const char *part_str, bool whole)
{
	struct blk_desc *mmc_dev_desc;
	struct disk_partition part_info;

	int ret = part_get_info_by_dev_and_name_or_num("mmc", part_str,
						       &mmc_dev_desc,
						       &part_info, whole);

	ut_assertf(expected == ret, "test(%d, \"%s\", %d) == %d", expected,
		   part_str, whole, ret);
	return 0;
}

static int dm_test_part(struct unit_test_state *uts)
{
	char *oldbootdevice;
	char str_disk_guid[UUID_STR_LEN + 1];
	int ret;
	struct blk_desc *mmc_dev_desc;
	struct disk_partition parts[2] = {
		{
			.start = 48, /* GPT data takes up the first 34 blocks or so */
			.size = 1,
			.name = "test1",
		},
		{
			.start = 49,
			.size = 1,
			.name = "test2",
		},
	};

	ut_asserteq(2, blk_get_device_by_str("mmc", "2", &mmc_dev_desc));
	if (CONFIG_IS_ENABLED(RANDOM_UUID)) {
		gen_rand_uuid_str(parts[0].uuid, UUID_STR_FORMAT_STD);
		gen_rand_uuid_str(parts[1].uuid, UUID_STR_FORMAT_STD);
		gen_rand_uuid_str(str_disk_guid, UUID_STR_FORMAT_STD);
	}
	ut_assertok(gpt_restore(mmc_dev_desc, str_disk_guid, parts,
				ARRAY_SIZE(parts)));

	oldbootdevice = env_get("bootdevice");

#define test(expected, part_str, whole) \
	ut_assertok(do_test(uts, expected, part_str, whole))

	env_set("bootdevice", NULL);
	test(-ENODEV, NULL, true);
	test(-ENODEV, "", true);
	env_set("bootdevice", "0");
	test(0, NULL, true);
	test(0, "", true);
	env_set("bootdevice", "2");
	test(1, NULL, false);
	test(1, "", false);
	test(1, "-", false);
	env_set("bootdevice", "");
	test(-EPROTONOSUPPORT, "0", false);
	test(0, "0", true);
	test(0, ":0", true);
	test(0, ".0", true);
	test(0, ".0:0", true);
	test(-EINVAL, "#test1", true);
	test(1, "2", false);
	test(1, "2", true);
	test(-ENOENT, "2:0", false);
	test(0, "2:0", true);
	test(1, "2:1", false);
	test(2, "2:2", false);
	test(1, "2.0", false);
	test(0, "2.0:0", true);
	test(1, "2.0:1", false);
	test(2, "2.0:2", false);
	test(-EINVAL, "2#bogus", false);
	test(1, "2#test1", false);
	test(2, "2#test2", false);
	ret = 0;

	env_set("bootdevice", oldbootdevice);
	return ret;
}
DM_TEST(dm_test_part, UTF_SCAN_PDATA | UTF_SCAN_FDT);

static int dm_test_part_bootable(struct unit_test_state *uts)
{
	struct blk_desc *desc;
	struct udevice *dev;

	ut_assertok(uclass_get_device_by_name(UCLASS_BLK, "mmc1.blk", &dev));
	desc = dev_get_uclass_plat(dev);
	ut_asserteq(1, part_get_bootable(desc));

	return 0;
}
DM_TEST(dm_test_part_bootable, UTF_SCAN_FDT);

static int do_get_info_test(struct unit_test_state *uts,
			    struct blk_desc *dev_desc, int part, int part_type,
			    struct disk_partition const *reference)
{
	struct disk_partition p;
	int ret;

	memset(&p, 0, sizeof(p));

	ret = part_get_info_by_type(dev_desc, part, part_type, &p);
	printf("part_get_info_by_type(%d, 0x%x) = %d\n", part, part_type, ret);
	if (ut_assertok(ret)) {
		return 0;
	}

	ut_asserteq(reference->start, p.start);
	ut_asserteq(reference->size, p.size);
	ut_asserteq(reference->sys_ind, p.sys_ind);

	return 0;
}

static int dm_test_part_get_info_by_type(struct unit_test_state *uts)
{
	char str_disk_guid[UUID_STR_LEN + 1];
	struct blk_desc *mmc_dev_desc;
	struct disk_partition gpt_parts[] = {
		{
			.start = 48, /* GPT data takes up the first 34 blocks or so */
			.size = 1,
			.name = "test1",
			.sys_ind = 0,
		},
		{
			.start = 49,
			.size = 1,
			.name = "test2",
			.sys_ind = 0,
		},
	};
	struct disk_partition mbr_parts[] = {
		{
			.start = 1,
			.size = 33,
			.name = "gpt",
			.sys_ind = EFI_PMBR_OSTYPE_EFI_GPT,
		},
		{
			.start = 48,
			.size = 1,
			.name = "test1",
			.sys_ind = 0x83,
		},
	};

	ut_asserteq(2, blk_get_device_by_str("mmc", "2", &mmc_dev_desc));
	if (CONFIG_IS_ENABLED(RANDOM_UUID)) {
		gen_rand_uuid_str(gpt_parts[0].uuid, UUID_STR_FORMAT_STD);
		gen_rand_uuid_str(gpt_parts[1].uuid, UUID_STR_FORMAT_STD);
		gen_rand_uuid_str(str_disk_guid, UUID_STR_FORMAT_STD);
	}
	ut_assertok(gpt_restore(mmc_dev_desc, str_disk_guid, gpt_parts,
				ARRAY_SIZE(gpt_parts)));

	ut_assertok(write_mbr_partitions(mmc_dev_desc, mbr_parts,
					 ARRAY_SIZE(mbr_parts), 0));

#define get_info_test(_part, _part_type, _reference) \
	ut_assertok(do_get_info_test(uts, mmc_dev_desc, _part, _part_type, \
				     _reference))

	for (int i = 0; i < ARRAY_SIZE(gpt_parts); i++) {
		get_info_test(i + 1, PART_TYPE_UNKNOWN, &gpt_parts[i]);
	}

	for (int i = 0; i < ARRAY_SIZE(mbr_parts); i++) {
		get_info_test(i + 1, PART_TYPE_DOS, &mbr_parts[i]);
	}

	for (int i = 0; i < ARRAY_SIZE(gpt_parts); i++) {
		get_info_test(i + 1, PART_TYPE_EFI, &gpt_parts[i]);
	}

	return 0;
}
DM_TEST(dm_test_part_get_info_by_type, UTF_SCAN_PDATA | UTF_SCAN_FDT);

static int dm_test_part_get_info_by_uuid(struct unit_test_state *uts)
{
	struct disk_partition parts[] = {
		{
			.start = 48,
			.size = 1,
			.name = "test1",
			.uuid = "c5bce7a2-03f0-4d03-9048-01ff23b9d527",
		},
		{
			.start = 49,
			.size = 1,
			.name = "test2",
			.uuid = "9df346e8-2c53-4cd8-b9ac-3af83f9a9b74",
		},
	};
	char disk_guid[UUID_STR_LEN + 1] =
		"8d60b397-1bb6-4d33-80ee-b1587d24c2f8";
	struct blk_desc *mmc_dev_desc;
	struct disk_partition info;
	int part, i;

	ut_asserteq(2, blk_get_device_by_str("mmc", "2", &mmc_dev_desc));

	if (CONFIG_IS_ENABLED(RANDOM_UUID)) {
		for (i = 0; i < ARRAY_SIZE(parts); i++)
			gen_rand_uuid_str(parts[i].uuid, UUID_STR_FORMAT_STD);

		gen_rand_uuid_str(disk_guid, UUID_STR_FORMAT_STD);
	}

	ut_assertok(gpt_restore(mmc_dev_desc, disk_guid, parts,
				ARRAY_SIZE(parts)));

	for (i = 0; i < ARRAY_SIZE(parts); i++) {
		part = part_get_info_by_uuid(mmc_dev_desc, parts[i].uuid,
					     &info);

		ut_asserteq(i + 1, part);
		ut_asserteq_str(parts[i].name, info.name);
		ut_asserteq(parts[i].start, info.start);
		ut_asserteq(parts[i].size, info.size);
	}

	part = part_get_info_by_uuid(mmc_dev_desc,
				     "00000000-0000-0000-0000-000000000000",
				     &info);
	ut_assert(part < 0);

	return 0;
}
DM_TEST(dm_test_part_get_info_by_uuid, UTF_SCAN_PDATA | UTF_SCAN_FDT);

/*
 * Check that the GPT layout adapts to the block size of the device. Neither
 * gpt_fill_header() nor gpt_fill_pte() does any block I/O, so a synthetic
 * descriptor is enough here; partition_entries_offset() reads the device tree
 * '/config' node, which is available without scanning for devices.
 */
static int dm_test_part_gpt_blksz(struct unit_test_state *uts)
{
	static const struct {
		unsigned long blksz;
		lbaint_t lba;
	} cases[] = {
		{   512, 0x2000 },	/* 4MB, the traditional 34-block layout */
		{  1024, 0x1000 },
		{  2048,  0x800 },
		{  4096,  0x400 },	/* 4K native media, e.g. UFS */
		{  8192,  0x200 },
		{ 16384,  0x100 },	/* the array fits in a single block... */
		{ 32768,   0x40 },	/* ...on a device smaller than 34 blocks */
	};

	char str_disk_guid[UUID_STR_LEN + 1] =
		"8d60b397-1bb6-4d33-80ee-b1587d24c2f8";
	struct disk_partition part;
	struct blk_desc desc;
	gpt_header gpt_h;
	gpt_entry *gpt_e;
	int i;

	gpt_e = calloc(GPT_ENTRY_NUMBERS, sizeof(gpt_entry));
	ut_assertnonnull(gpt_e);

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		u64 entry_lba, first_lba, last_lba;
		u32 pte_blks;

		memset(&desc, '\0', sizeof(desc));
		memset(&gpt_h, '\0', sizeof(gpt_h));
		desc.blksz = cases[i].blksz;
		desc.log2blksz = LOG2(desc.blksz);
		desc.lba = cases[i].lba;

		ut_assertok(gpt_fill_header(&desc, &gpt_h, str_disk_guid, 1));

		/*
		 * Size the array with BLOCK_CNT() rather than the
		 * DIV_ROUND_UP() that gpt_pte_blocks() uses, so that this
		 * checks the layout instead of restating the implementation.
		 */
		pte_blks = BLOCK_CNT(le32_to_cpu(gpt_h.num_partition_entries) *
				     le32_to_cpu(gpt_h.sizeof_partition_entry),
				     (&desc));
		entry_lba = le64_to_cpu(gpt_h.partition_entry_lba);
		first_lba = le64_to_cpu(gpt_h.first_usable_lba);
		last_lba = le64_to_cpu(gpt_h.last_usable_lba);

		ut_asserteq_64(1, le64_to_cpu(gpt_h.my_lba));
		ut_asserteq_64(desc.lba - 1, le64_to_cpu(gpt_h.alternate_lba));

		/* the primary array ends where the usable area begins */
		ut_assert(entry_lba >= 2);
		ut_asserteq_64(entry_lba + pte_blks, first_lba);
		ut_assert(first_lba < last_lba);

		/*
		 * write_gpt_table() puts the backup array at
		 * last_usable_lba + 1, so it has to end exactly where the
		 * backup header begins
		 */
		ut_asserteq_64(le64_to_cpu(gpt_h.alternate_lba),
			       last_lba + 1 + pte_blks);

		/* a partition with no start and no size fills the disk */
		memset(&part, '\0', sizeof(part));
		disk_partition_set_uuid(&part, str_disk_guid);
		ut_assertok(gpt_fill_pte(&desc, &gpt_h, gpt_e, &part, 1));
		ut_asserteq_64(first_lba, le64_to_cpu(gpt_e[0].starting_lba));
		ut_asserteq_64(last_lba, le64_to_cpu(gpt_e[0].ending_lba));

		/* a partition overlapping the entry array is rejected */
		part.start = entry_lba;
		part.size = 1;
		ut_asserteq(-ENOSPC,
			    gpt_fill_pte(&desc, &gpt_h, gpt_e, &part, 1));

		/* so is one running past the end of the usable area */
		part.start = first_lba;
		part.size = last_lba - first_lba + 2;
		ut_asserteq(-E2BIG,
			    gpt_fill_pte(&desc, &gpt_h, gpt_e, &part, 1));
	}

	free(gpt_e);

	return 0;
}
DM_TEST(dm_test_part_gpt_blksz, 0);
