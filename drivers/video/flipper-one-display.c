// SPDX-License-Identifier: GPL-2.0+
/*
 * Video driver for the Flipper One 256x144 grayscale SPI display.
 *
 * The display accepts one SPI write transaction per frame.  Each row
 * consists of 256 bytes of 8-bit grayscale pixel data (left justified)
 * followed by 2 footer bytes, giving a frame size of 258 * 144 = 37152
 * bytes, VIDEO_BPP8 format (8-bit grayscale)
 *
 * SPI wiring: mode 3 (CPOL=1, CPHA=1), up to 24 MHz, no MISO.
 *
 * Copyright (c) 2026 Flipper FZCO
 */

#include <dm.h>
#include <asm/gpio.h>
#include <spi.h>
#include <time.h>
#include <video.h>
#include <dm/device_compat.h>

/* Physical display dimensions */
#define DISPLAY_WIDTH		256
#define DISPLAY_HEIGHT		144

/*
 * Wire frame layout: 256 pixels + 2 footer bytes per row.
 * The footer bytes are ignored by the display.
 */
#define ROW_BYTES		258
#define FRAME_BYTES		(ROW_BYTES * DISPLAY_HEIGHT)

/**
 * struct flipper_one_priv - Driver private data
 * @last_spi_sync_ms: Absolute timestamp (ms) of the most recent completed SPI
 *                    frame transfer, used to enforce a per-driver rate limit.
 *                    The video uclass rate-limiting (VIDEO_SYNC_MS) runs
 *                    *after* calling ops->video_sync, so without this guard
 *                    every vidconsole character write and every cyclic tick
 *                    would trigger a full 37 KiB SPI transfer.
 * @active_gpio: Optional GPIO asserted (active-low: driven low) for the
 *               duration of each SPI frame transfer to signal the display MCU
 *               that a new frame is incoming.
 */
struct flipper_one_priv {
	ulong last_spi_sync_ms;
	struct gpio_desc active_gpio;
};

/**
 * flipper_one_sync() - Push the internal framebuffer to the display
 */
static int flipper_one_sync(struct udevice *vid)
{
	struct flipper_one_priv *priv = dev_get_priv(vid);
	struct video_priv *uc_priv = dev_get_uclass_priv(vid);
	int ret;

	/*
	 * The video uclass calls ops->video_sync() unconditionally — the
	 * VIDEO_SYNC_MS rate-limit check only runs *after* this callback
	 * returns.  Guard the expensive SPI transfer here so it fires at most
	 * once per VIDEO_SYNC_MS milliseconds regardless of how often
	 * vidconsole_putc() or the cyclic mechanism calls video_sync().
	 */
	if (get_timer(priv->last_spi_sync_ms) < CONFIG_VIDEO_SYNC_MS)
		return 0;

	ret = dm_spi_claim_bus(vid);
	if (ret) {
		dev_err(vid, "failed to claim SPI bus: %d\n", ret);
		return ret;
	}

	dm_gpio_set_value(&priv->active_gpio, 1);

	/*
	 * Send the entire frame in one transaction so that chip-select
	 * remains active throughout (SPI_XFER_BEGIN asserts CS before the
	 * first bit; SPI_XFER_END deasserts it after the last bit).
	 */
	ret = dm_spi_xfer(vid, FRAME_BYTES * 8, uc_priv->fb, NULL,
			  SPI_XFER_BEGIN | SPI_XFER_END);
	if (ret)
		dev_err(vid, "failed to send frame: %d\n", ret);

	dm_gpio_set_value(&priv->active_gpio, 0);

	dm_spi_release_bus(vid);

	priv->last_spi_sync_ms = get_timer(0);

	return ret;
}

static int flipper_one_probe(struct udevice *dev)
{
	struct flipper_one_priv *priv = dev_get_priv(dev);
	struct video_priv *uc_priv = dev_get_uclass_priv(dev);
	int ret;

	uc_priv->xsize       = DISPLAY_WIDTH;
	uc_priv->ysize       = DISPLAY_HEIGHT;
	uc_priv->bpix        = VIDEO_BPP8;
	uc_priv->rot         = 0;
	uc_priv->line_length = ROW_BYTES;

	/*
	 * The active-gpios line is asserted (logical 1 = GPIO driven to its
	 * active state, which is physically low for GPIO_ACTIVE_LOW) for the
	 * duration of each SPI frame transfer. The display MCU uses this
	 * signal to detect frame boundaries. The GPIO is optional in
	 * device-tree but required for the display to accept frames.
	 */
	ret = gpio_request_by_name(dev, "active-gpios", 0, &priv->active_gpio,
				   GPIOD_IS_OUT);
	if (ret && ret != -ENOENT) {
		dev_err(dev, "failed to request active-gpios: %d\n", ret);
		return ret;
	}

	return 0;
}

static int flipper_one_bind(struct udevice *dev)
{
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);
	plat->size = FRAME_BYTES;

	return 0;
}

static const struct video_ops flipper_one_ops = {
	.video_sync = flipper_one_sync,
};

static const struct udevice_id flipper_one_ids[] = {
	{ .compatible = "flipper,one-display" },
	{ }
};

U_BOOT_DRIVER(flipper_one_display) = {
	.name		= "flipper_one_display",
	.id		= UCLASS_VIDEO,
	.of_match	= flipper_one_ids,
	.ops		= &flipper_one_ops,
	.bind		= flipper_one_bind,
	.probe		= flipper_one_probe,
	.plat_auto	= sizeof(struct video_uc_plat),
	.priv_auto	= sizeof(struct flipper_one_priv),
};
