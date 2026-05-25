// SPDX-License-Identifier: GPL-2.0+
/*
 * Bootmethod for the Boot Loader Specification (type #2 entry files)
 *
 * Reuses the pxelinux parser/boot path: each on-disk entry is read into a
 * struct pxe_label via parse_label_keys() and booted via label_boot().
 *
 * Spec: https://uapi-group.org/specifications/specs/boot_loader_specification/
 *
 * Every *.conf file in <prefix>/loader/entries/ is surfaced as its own
 * bootflow via the bootstd iterator's sub-entry axis. The bootmeth is
 * called repeatedly with seq = 0, 1, 2, ... for the same (partition,
 * bootdev) tuple and returns -ENOENT once the entry list is exhausted.
 */

#define LOG_CATEGORY UCLASS_BOOTSTD

#include <bootdev.h>
#include <bootflow.h>
#include <bootmeth.h>
#include <bootstd.h>
#include <command.h>
#include <dm.h>
#include <extlinux.h>
#include <fs.h>
#include <malloc.h>
#include <mapmem.h>
#include <pxe_utils.h>
#include <sort.h>
#include <alist.h>
#include <linux/sizes.h>

#define BLS_DIR		"loader/entries"
#define BLS_SUFFIX	".conf"

/**
 * struct bls_priv - per-bootmeth-instance state
 *
 * Caches a single flat list of all *.conf entries discovered across every
 * bootstd prefix on a given (bootdev, partition) tuple so that successive
 * read_bootflow() calls answer from RAM instead of re-walking the
 * filesystem. Sorted ascending by full path; read_bootflow() indexes from
 * the end so the seq-th entry the iterator surfaces is the seq-th newest.
 *
 * Invalidated on seq == 0 (new scan) or when the (bootdev, partition) key
 * changes.
 *
 * @paths:	alist of ``char *`` full paths (strdup'd, owned), sorted
 *		ascending by full path.
 * @cache_blk:	Block device the cache was built against.
 * @cache_part:	Partition number the cache was built against.
 */
struct bls_priv {
	struct alist paths;
	struct udevice *cache_blk;
	int cache_part;
};

static void bls_cache_drop(struct bls_priv *priv)
{
	char **arr = priv->paths.data;
	uint i;

	for (i = 0; i < priv->paths.count; i++)
		free(arr[i]);
	alist_empty(&priv->paths);
	priv->cache_blk = NULL;
	priv->cache_part = 0;
}

static int bls_check(struct udevice *dev, struct bootflow_iter *iter)
{
	int ret;

	/* This only works on block devices */
	ret = bootflow_iter_check_blk(iter);
	if (ret)
		return log_msg_ret("blk", ret);

	return 0;
}

static int bls_getfile(struct pxe_context *ctx, const char *file_path,
		       char *file_addr, enum bootflow_img_t type, ulong *sizep)
{
	struct extlinux_info *info = ctx->userdata;
	ulong addr;
	int ret;

	addr = simple_strtoul(file_addr, NULL, 16);

	/* Allow up to 1GB */
	*sizep = 1 << 30;
	ret = bootmeth_read_file(info->dev, info->bflow, file_path, addr,
				 type, sizep);
	if (ret)
		return log_msg_ret("read", ret);

	return 0;
}

/**
 * bls_load_all() - Enumerate every BLS *.conf across all bootstd prefixes
 *
 * Walks ``<prefix>/loader/entries/`` for each prefix in @prefixes, collects
 * every regular file with a ``.conf`` suffix as a strdup'd full path in
 * @paths, then sorts the merged result ascending by full path (callers
 * index from the end for the spec's descending order).
 *
 * The spec leaves ordering between prefixes unspecified; sorting full
 * paths is a deterministic-and-cheap stand-in.
 *
 * The Boot Loader Specification says entries should be sorted by sort-key
 * (descending), then version (descending), then filename (descending). For
 * the time being only the filename criterion is implemented, which is
 * sufficient for most distros that encode kernel version into the filename.
 *
 * TODO: implement proper spec-compliant ordering. That requires reading
 * each candidate entry, parsing its 'sort-key' and 'version' fields (the
 * latter compared with strverscmp()-style logic), and only falling back to
 * filename order when those tie.
 */
static int bls_load_all(struct alist *paths, struct blk_desc *desc,
			struct bootflow *bflow,
			const char *const *prefixes)
{
	char dirpath[256];
	int ret;
	int i;

	for (i = 0; prefixes && prefixes[i]; i++) {
		struct fs_dir_stream *dirs;
		struct fs_dirent *dent;

		/* fs_closedir() below resets the global fs_type. */
		ret = bootmeth_setup_fs(bflow, desc);
		if (ret)
			return log_msg_ret("fs", ret);

		snprintf(dirpath, sizeof(dirpath), "%s%s",
			 prefixes[i], BLS_DIR);
		dirs = fs_opendir(dirpath);
		if (!dirs)
			continue;

		while ((dent = fs_readdir(dirs))) {
			size_t len = strlen(dent->name);
			char *full;

			if (dent->type != FS_DT_REG)
				continue;
			if (len <= strlen(BLS_SUFFIX))
				continue;
			if (strcmp(dent->name + len - strlen(BLS_SUFFIX),
				   BLS_SUFFIX))
				continue;

			full = malloc(strlen(dirpath) + 1 + len + 1);
			if (!full) {
				fs_closedir(dirs);
				return -ENOMEM;
			}
			sprintf(full, "%s/%s", dirpath, dent->name);

			if (!alist_add(paths, full)) {
				free(full);
				fs_closedir(dirs);
				return -ENOMEM;
			}
		}
		fs_closedir(dirs);
	}

	if (paths->count)
		qsort(paths->data, paths->count, paths->obj_size,
		      strcmp_compar);

	return 0;
}

/*
 * TODO: BLS entry filenames may carry a boot-counter suffix of the form
 * '+TRIES_LEFT[-TRIES_DONE]' immediately before the .conf extension (see
 * the spec section on "Boot counting"). When that is implemented, this
 * bootmeth should:
 *   - parse and strip the suffix from the displayed entry name,
 *   - skip entries whose TRIES_LEFT has reached zero,
 *   - decrement TRIES_LEFT (renaming the file) on each boot attempt.
 * For now the suffix is left intact in the entry name and ignored.
 */
static int bls_read_bootflow(struct udevice *dev, struct bootflow *bflow,
			     int seq)
{
	struct bls_priv *priv = dev_get_priv(dev);
	struct blk_desc *desc;
	const char *const *prefixes;
	struct udevice *bootstd;
	struct pxe_label *label = NULL;
	struct pxe_menu scratch = {};
	const char *fpath;
	const char *base;
	char **slot;
	char *body;
	int ret;

	ret = uclass_first_device_err(UCLASS_BOOTSTD, &bootstd);
	if (ret)
		return log_msg_ret("std", ret);

	/* We require a partitioned block device */
	if (!bflow->blk || !bflow->part)
		return -ENOENT;

	desc = dev_get_uclass_plat(bflow->blk);
	prefixes = bootstd_get_prefixes(bootstd);

	/*
	 * A fresh scan of this tuple always starts at seq == 0; rebuild then
	 * so callers see current on-disk state. seq > 0 only happens after a
	 * successful seq == 0 (the iterator only bumps cur_subseq on
	 * success). Still, check the block device and partition just in case
	 */
	if (!seq || priv->cache_blk != bflow->blk ||
	    priv->cache_part != bflow->part) {
		bls_cache_drop(priv);
		ret = bls_load_all(&priv->paths, desc, bflow, prefixes);
		if (ret)
			return log_msg_ret("scan", ret);
		priv->cache_blk = bflow->blk;
		priv->cache_part = bflow->part;
	}

	if (seq >= (int)priv->paths.count)
		return -ENOENT;

	/* Sorted ascending by full path; index from the end for newest-first. */
	slot = alist_getw(&priv->paths, priv->paths.count - 1 - seq, char *);
	fpath = *slot;
	base = strrchr(fpath, '/');
	base = base ? base + 1 : fpath;

	/*
	 * bls_load_all() finished with fs_closedir(), which resets the
	 * global fs_type. Re-mount the partition so bootmeth_try_file()'s
	 * internal fs_size() call can find the right filesystem driver.
	 */
	ret = bootmeth_setup_fs(bflow, desc);
	if (ret)
		return log_msg_ret("fs", ret);

	ret = bootmeth_try_file(bflow, desc, NULL, fpath);
	if (ret)
		return log_msg_ret("try", ret);

	ret = bootmeth_alloc_file(bflow, SZ_64K, 1, BFI_EXTLINUX_CFG);
	if (ret)
		return log_msg_ret("read", ret);

	label = label_create();
	if (!label)
		return log_msg_ret("lbl", -ENOMEM);

	/*
	 * BLS files have no 'label NAME' header — derive the label name from
	 * the basename (without the .conf suffix) so messages are useful.
	 */
	label->name = strndup(base, strlen(base) - strlen(BLS_SUFFIX));
	if (!label->name) {
		ret = -ENOMEM;
		goto err;
	}

	body = bflow->buf;
	ret = parse_label_keys(&body, &scratch, label, true);
	if (ret < 0)
		goto err;

	/*
	 * scratch is only used to give parse_label_keys() somewhere safe to
	 * stash menu-level state (e.g. a stray 'menu default' line). BLS
	 * entry files don't contain such lines but defensively free anything
	 * that did get allocated.
	 */
	free(scratch.default_label);

	/*
	 * label->menu was populated either from a BLS 'title' line (the
	 * spec-mandated human-readable name) or from a stray 'menu label'
	 * the parser may have picked up. Fall back to the filename-derived
	 * label name when neither is present.
	 */
	bflow->os_name = strdup(label->menu ? label->menu : label->name);
	if (!bflow->os_name) {
		ret = -ENOMEM;
		goto err;
	}

	bflow->bootmeth_priv = label;

	return 0;

err:
	if (label)
		label_destroy(label);
	return log_msg_ret("bls", ret);
}

static int bls_boot(struct udevice *dev, struct bootflow *bflow)
{
	struct cmd_tbl cmdtp = {};	/* dummy */
	struct pxe_context ctx;
	struct extlinux_info info;
	struct pxe_label *label = bflow->bootmeth_priv;
	int ret;

	if (!label)
		return log_msg_ret("lbl", -ENOENT);

	info.dev = dev;
	info.bflow = bflow;

	/*
	 * BLS paths are absolute relative to the filesystem root of the
	 * partition the entry lives on. allow_abs_path=true honours that;
	 * passing NULL as the bootfile keeps the prefix empty so absolute
	 * paths are not rebased.
	 */
	ret = pxe_setup_ctx(&ctx, &cmdtp, bls_getfile, &info, true,
			    NULL, false, false);
	if (ret)
		return log_msg_ret("ctx", -EINVAL);

	ret = label_boot(&ctx, label);
	pxe_destroy_ctx(&ctx);
	if (ret)
		return log_msg_ret("boot", -EINVAL);

	return 0;
}

static int bls_bootmeth_bind(struct udevice *dev)
{
	struct bootmeth_uc_plat *plat = dev_get_uclass_plat(dev);

	plat->desc = IS_ENABLED(CONFIG_BOOTSTD_FULL) ?
		"Boot Loader Specification" : "bls";

	return 0;
}

static int bls_bootmeth_probe(struct udevice *dev)
{
	struct bls_priv *priv = dev_get_priv(dev);

	alist_init_struct(&priv->paths, char *);

	return 0;
}

static int bls_bootmeth_remove(struct udevice *dev)
{
	struct bls_priv *priv = dev_get_priv(dev);

	bls_cache_drop(priv);
	alist_uninit(&priv->paths);

	return 0;
}

static struct bootmeth_ops bls_bootmeth_ops = {
	.check		= bls_check,
	.read_bootflow	= bls_read_bootflow,
	.read_file	= bootmeth_common_read_file,
	.boot		= bls_boot,
};

static const struct udevice_id bls_bootmeth_ids[] = {
	{ .compatible = "u-boot,bls" },
	{ }
};

U_BOOT_DRIVER(bootmeth_2bls) = {
	.name		= "bootmeth_bls",
	.id		= UCLASS_BOOTMETH,
	.of_match	= bls_bootmeth_ids,
	.ops		= &bls_bootmeth_ops,
	.bind		= bls_bootmeth_bind,
	.probe		= bls_bootmeth_probe,
	.remove		= bls_bootmeth_remove,
	.priv_auto	= sizeof(struct bls_priv),
};
