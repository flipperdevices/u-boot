// SPDX-License-Identifier: GPL-2.0+
/*
 * Flipper One MCU buttons keyboard driver
 * Copyright (C) 2026 Flipper FZCO
 */

#include <dm.h>
#include <i2c.h>
#include <input.h>
#include <keyboard.h>
#include <log.h>
#include <linux/bitmap.h>
#include <linux/byteorder/little_endian.h>
#include <linux/input.h>

/*
 * MCU register addresses are 16-bit, sent big-endian over I2C (per i2c_set_chip_offset_len(2)).
 * Register values are 16-bit little-endian.
 */
#define FOMCU_REG_INTSTS_INPUT		0x0100
#define FOMCU_INTSTS_INPUT_BTN		BIT(0)

#define FOMCU_REG_INPUT_BTNS		0x0200

/* Button bits in FOMCU_REG_INPUT_BTNS */
#define FO_BTN_VIEW			BIT(0)
#define FO_BTN_ESCAPE			BIT(1)
#define FO_BTN_POWER			BIT(2)
#define FO_BTN_EDIT			BIT(3)
#define FO_BTN_RUN			BIT(4)
#define FO_BTN_APPSELECT		BIT(5)
#define FO_BTN_BACK			BIT(6)
#define FO_BTN_DOWN			BIT(7)
#define FO_BTN_RIGHT			BIT(8)
#define FO_BTN_CENTER			BIT(9)
#define FO_BTN_LEFT			BIT(10)
#define FO_BTN_UP			BIT(11)
#define FO_BTN_PTT			BIT(12)

/* Mask of all bit positions that have a non-RESERVED keycode mapping */
#define FO_BTN_MAPPED_MASK		(FO_BTN_APPSELECT | FO_BTN_BACK | \
					 FO_BTN_DOWN | FO_BTN_RIGHT | \
					 FO_BTN_CENTER | FO_BTN_LEFT | FO_BTN_UP)

/*
 * Keycode table indexed by button bit position in FOMCU_REG_INPUT_BTNS.
 * KEY_RESERVED (0) means the button is not mapped.
 *
 * The input layer converts KEY_UP/DOWN/LEFT/RIGHT to ANSI escape sequences
 * which cli_ch_process folds back to control characters recognised by
 * bootmenu_conv_key().  KEY_ENTER -> '\r' -> BKEY_SELECT,
 * KEY_ESC -> '\e' -> BKEY_QUIT.
 */
static const int fo_btn_keycodes[] = {
	[0]  = KEY_RESERVED,	/* VIEW */
	[1]  = KEY_RESERVED,	/* ESCAPE */
	[2]  = KEY_RESERVED,	/* POWER */
	[3]  = KEY_RESERVED,	/* EDIT */
	[4]  = KEY_RESERVED,	/* RUN */
	[5]  = KEY_TAB,		/* APPSELECT */
	[6]  = KEY_ESC,		/* BACK */
	[7]  = KEY_DOWN,	/* DOWN */
	[8]  = KEY_RIGHT,	/* RIGHT */
	[9]  = KEY_ENTER,	/* CENTER */
	[10] = KEY_LEFT,	/* LEFT */
	[11] = KEY_UP,		/* UP */
	[12] = KEY_RESERVED,	/* PTT */
};

struct fo_keyb_priv {
	unsigned long last_btns;
};

static int fo_read_reg(struct udevice *dev, u32 reg, u16 *val)
{
	__le16 buf;
	int ret;

	ret = dm_i2c_read(dev, reg, (u8 *)&buf, sizeof(buf));
	if (ret)
		return ret;

	*val = le16_to_cpu(buf);
	return 0;
}

/*
 * Called by the input layer when its fifo is empty.  Reads the interrupt
 * status (clearing it in the process) and, if a button event is signalled,
 * refreshes last_btns from the hardware.  Then passes the full currently-held
 * keycode set to input_send_keycodes(), which handles repeat timing and
 * ASCII/ANSI conversion.
 */
static int fo_read_keys(struct input_config *input)
{
	struct udevice *dev = input->dev;
	struct fo_keyb_priv *priv = dev_get_priv(dev);
	int keycodes[ARRAY_SIZE(fo_btn_keycodes)];
	unsigned long i;
	int n = 0;
	u16 reg;

	/*
	 * Read interrupt status to detect changes.  The read also clears the
	 * pending bits, preventing spurious events when the next OS sets up
	 * its own interrupt handler.
	 */
	if (!fo_read_reg(dev, FOMCU_REG_INTSTS_INPUT, &reg) &&
	    (reg & FOMCU_INTSTS_INPUT_BTN)) {
		if (!fo_read_reg(dev, FOMCU_REG_INPUT_BTNS, &reg))
			priv->last_btns = reg & FO_BTN_MAPPED_MASK;
	}

	for_each_set_bit(i, &priv->last_btns, ARRAY_SIZE(fo_btn_keycodes))
		keycodes[n++] = fo_btn_keycodes[i];

	input_send_keycodes(input, keycodes, n);
	return 1;
}

static int fo_keyb_start(struct udevice *dev)
{
	struct fo_keyb_priv *priv = dev_get_priv(dev);
	u16 reg;
	int ret;

	/*
	 * Drain any interrupt status accumulated before the driver started so
	 * that the first real poll starts from a clean baseline.
	 */
	fo_read_reg(dev, FOMCU_REG_INTSTS_INPUT, &reg);

	/*
	 * Snapshot the current button state.  Without this, buttons already
	 * held at startup would look like fresh presses on the first poll.
	 */
	ret = fo_read_reg(dev, FOMCU_REG_INPUT_BTNS, &reg);
	if (!ret)
		priv->last_btns = reg & FO_BTN_MAPPED_MASK;

	return 0;
}

static int fo_keyb_probe(struct udevice *dev)
{
	struct keyboard_priv *uc_priv = dev_get_uclass_priv(dev);
	struct stdio_dev *sdev = &uc_priv->sdev;
	struct input_config *input = &uc_priv->input;
	int ret;

	/* MCU register addresses are 16-bit wide */
	ret = i2c_set_chip_offset_len(dev, 2);
	if (ret)
		return ret;

	input_add_tables(input, false);
	input_set_delays(input, 250, 50);
	input->dev = dev;
	input->read_keys = fo_read_keys;

	strcpy(sdev->name, "flipper-one-btns");
	return input_stdio_register(sdev);
}

static const struct keyboard_ops fo_keyb_ops = {
	.start	= fo_keyb_start,
};

static const struct udevice_id fo_keyb_ids[] = {
	{ .compatible = "flipper,one-mcu" },
	{ }
};

U_BOOT_DRIVER(flipper_one_keyb) = {
	.name		= "flipper_one_btns",
	.id		= UCLASS_KEYBOARD,
	.of_match	= fo_keyb_ids,
	.probe		= fo_keyb_probe,
	.ops		= &fo_keyb_ops,
	.priv_auto	= sizeof(struct fo_keyb_priv),
};
