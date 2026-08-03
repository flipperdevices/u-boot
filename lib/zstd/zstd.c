// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2021 Google LLC
 */

#define LOG_CATEGORY	LOGC_BOOT

#include <abuf.h>
#include <log.h>
#include <malloc.h>
#include <asm/unaligned.h>
#include <linux/errno.h>
#include <linux/zstd.h>

/**
 * zstd_frame_starts_at() - does a zstd frame header begin here?
 *
 * @src:	buffer to inspect
 * @size:	bytes available at @src
 * Return:	true if @src begins with a zstd or skippable frame magic
 */
static bool zstd_frame_starts_at(const void *src, size_t size)
{
	u32 magic;

	if (size < sizeof(magic))
		return false;

	magic = get_unaligned_le32(src);

	return magic == ZSTD_MAGICNUMBER ||
	       (magic & ZSTD_MAGIC_SKIPPABLE_MASK) == ZSTD_MAGIC_SKIPPABLE_START;
}

int zstd_decompress(struct abuf *in, struct abuf *out)
{
	const u8 *in_pos = abuf_data(in);
	size_t in_left = abuf_size(in);
	u8 *out_pos = abuf_data(out);
	size_t out_left = abuf_size(out);
	size_t wsize, total = 0;
	unsigned int frames = 0;
	void *workspace;
	zstd_dctx *ctx;
	int ret;

	wsize = zstd_dctx_workspace_bound();
	workspace = malloc(wsize);
	if (!workspace) {
		debug("%s: cannot allocate workspace of size %zu\n", __func__,
			wsize);
		return -ENOMEM;
	}

	ctx = zstd_init_dctx(workspace, wsize);
	if (!ctx) {
		log_err("%s: zstd_init_dctx() failed\n", __func__);
		ret = -EPERM;
		goto do_free;
	}

	/*
	 * The payload may be several frames concatenated - pzstd and multiple
	 * appended writes both produce those, and zstd_decompress_dctx() only
	 * ever decodes one. Keep decoding until the output fills up or the
	 * input stops looking like a frame; a non-frame tail is the junk the
	 * size probe below has always been here to tolerate.
	 */
	while (out_left && zstd_frame_starts_at(in_pos, in_left)) {
		size_t csize, dsize;

		/*
		 * Find out how large the frame actually is, there may be junk at
		 * the end of the frame that zstd_decompress_dctx() can't handle.
		 */
		csize = zstd_find_frame_compressed_size(in_pos, in_left);
		if (zstd_is_error(csize)) {
			log_err("%s: failed to detect compressed size: %d\n",
				__func__, zstd_get_error_code(csize));
			ret = -EINVAL;
			goto do_free;
		}

		dsize = zstd_decompress_dctx(ctx, out_pos, out_left, in_pos,
					     csize);
		if (zstd_is_error(dsize)) {
			log_err("%s: failed to decompress: %d\n", __func__,
				zstd_get_error_code(dsize));
			ret = -EINVAL;
			goto do_free;
		}

		in_pos += csize;
		in_left -= csize;
		out_pos += dsize;
		out_left -= dsize;
		total += dsize;
		frames++;
	}

	if (!frames) {
		log_err("%s: no zstd frame found\n", __func__);
		ret = -EINVAL;
		goto do_free;
	}

	ret = total;
do_free:
	free(workspace);
	return ret;
}
