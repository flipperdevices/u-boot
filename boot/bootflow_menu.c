// SPDX-License-Identifier: GPL-2.0+
/*
 * Provide a menu of available bootflows and related options
 *
 * Copyright 2022 Google LLC
 * Written by Simon Glass <sjg@chromium.org>
 */

#define LOG_CATEGORY UCLASS_BOOTSTD

#include <bootflow.h>
#include <bootmeth.h>
#include <bootstd.h>
#include <cli.h>
#include <dm.h>
#include <expo.h>
#include <malloc.h>
#include <menu.h>
#include <video.h>
#include <video_console.h>
#include <watchdog.h>
#include <linux/delay.h>
#include "bootflow_internal.h"

/**
 * struct menu_priv - information about the menu
 *
 * @num_bootflows: Number of bootflows in the menu
 * @last_bootdev: bootdev of the last bootflow added to the menu, NULL if none
 */
struct menu_priv {
	int num_bootflows;
	struct udevice *last_bootdev;
};

int bootflow_menu_new(struct expo **expp, int width, int height, int char_h)
{
	/*
	 * All positions are expressed as proportional fractions of the
	 * reference design (1366 x 768, the classic WXGA resolution used by
	 * the sandbox display). Use integer scaling so the layout fits any
	 * display resolution.
	 */
#define SX(v)  ((v) * width  / 1366)
#define SY(v)  ((v) * height / 768)
	/*
	 * Use the actual console character height when provided; otherwise
	 * fall back to the SY-scaled reference value so the layout is
	 * self-consistent even without a live console (e.g. in unit tests).
	 */
	if (!char_h)
		char_h = max(SY(16), 1);
	struct scene_obj_menu *menu;
	struct menu_priv *priv;
	struct scene *scn;
	struct expo *exp;
	bool use_font;
	void *logo;
	int ret;

	priv = calloc(1, sizeof(*priv));
	if (!priv)
		return log_msg_ret("prv", -ENOMEM);

	ret = expo_new("bootflows", priv, &exp);
	if (ret)
		return log_msg_ret("exp", ret);

	ret = scene_new(exp, "main", MAIN, &scn);
	if (ret < 0)
		return log_msg_ret("scn", ret);

	ret = scene_box(scn, "box", OBJ_BOX, 2, NULL);
	if (ret < 0)
		return log_msg_ret("bmb", ret);
	ret |= scene_obj_set_bbox(scn, OBJ_BOX, SX(30), SY(90),
				  width - SX(30), height - max(SY(48), 6 * char_h));

	ret = scene_menu(scn, "main", OBJ_MENU, &menu);
	ret |= scene_obj_set_pos(scn, OBJ_MENU, SX(100), SY(100));
	ret |= scene_txt_str(scn, "title", OBJ_MENU_TITLE, STR_MENU_TITLE,
			     "U-Boot - Boot Menu", NULL);
	/*
	 * Ensure the title bbox is at least one character-cell tall so it is
	 * not clipped on small displays
	 */
	ret |= scene_obj_set_bbox(scn, OBJ_MENU_TITLE, 0, SY(32),
				  SCENEOB_DISPLAY_MAX,
				  max(SY(32) + SY(30), char_h));
	ret |= scene_obj_set_halign(scn, OBJ_MENU_TITLE, SCENEOA_CENTRE);

	logo = video_get_u_boot_logo();
	/*
	 * Only add the logo when the reserved right-side column is at least
	 * 64 px wide so that a clipped strip does not bleed over the menu
	 * box on small displays (e.g. 256x144 where the column is only 37 px).
	 */
	if (logo && SX(1366 - 1165) >= 64) {
		ret |= scene_img(scn, "ulogo", OBJ_U_BOOT_LOGO, logo, NULL);
		ret |= scene_obj_set_pos(scn, OBJ_U_BOOT_LOGO,
					 width - SX(1366 - 1165), SY(100));
	} else {
		/*
		 * No logo/preview column is shown, so the right side of the box
		 * is empty. Let the menu justify its columns out to the box's
		 * inner right edge (less the border and a small gap) instead of
		 * bunching them in the reference column band on the left.
		 */
		struct scene_obj_box *box;

		box = scene_obj_find(scn, OBJ_BOX, SCENEOBJT_BOX);
		if (box)
			menu->fill_x1 = box->obj.bbox.x1 - box->width - 4;
	}

	ret |= scene_txt_str(scn, "prompt1a", OBJ_PROMPT1A, STR_PROMPT1A,
	     "Use the \x18 and \x19 keys to select which entry is highlighted.",
	     NULL);
	ret |= scene_txt_str(scn, "prompt1b", OBJ_PROMPT1B, STR_PROMPT1B,
	     "Use the UP and DOWN keys to select which entry is highlighted.",
	     NULL);
	ret |= scene_txt_str(scn, "prompt2", OBJ_PROMPT2, STR_PROMPT2,
	     "Press enter to boot the selected OS, 'e' to edit the commands "
	     "before booting or 'c' for a command-line. ESC to return to "
	     "previous menu", NULL);
	ret |= scene_txt_str(scn, "autoboot", OBJ_AUTOBOOT, STR_AUTOBOOT,
	     "The highlighted entry will be executed automatically in %ds.",
	     NULL);
	/*
	 * Prompt1A/B: up to 2 wrapped lines (navigation hint).
	 * Prompt2: up to 4 wrapped lines (boot instructions).
	 * All positions are anchored from the bottom so they remain visible
	 * on small displays; on large displays the SY() values dominate.
	 */
	ret |= scene_obj_set_bbox(scn, OBJ_PROMPT1A, 0,
				  min(SY(590), height - 6 * char_h),
				  SCENEOB_DISPLAY_MAX,
				  min(SY(590), height - 6 * char_h) + max(SY(30), 2 * char_h));
	ret |= scene_obj_set_bbox(scn, OBJ_PROMPT1B, 0,
				  min(SY(620), height - 6 * char_h),
				  SCENEOB_DISPLAY_MAX,
				  min(SY(620), height - 6 * char_h) + max(SY(30), 2 * char_h));
	ret |= scene_obj_set_bbox(scn, OBJ_PROMPT2, SX(100),
				  min(SY(650), height - 4 * char_h),
				  width - SX(100), height);
	ret |= scene_obj_set_bbox(scn, OBJ_AUTOBOOT, 0, height,
				  SCENEOB_DISPLAY_MAX, height + SY(30));
	ret |= scene_obj_set_halign(scn, OBJ_PROMPT1A, SCENEOA_CENTRE);
	ret |= scene_obj_set_halign(scn, OBJ_PROMPT1B, SCENEOA_CENTRE);
	ret |= scene_obj_set_halign(scn, OBJ_PROMPT2, SCENEOA_CENTRE);
	ret |= scene_obj_set_valign(scn, OBJ_PROMPT2, SCENEOA_CENTRE);
	ret |= scene_obj_set_halign(scn, OBJ_AUTOBOOT, SCENEOA_CENTRE);

	use_font = IS_ENABLED(CONFIG_CONSOLE_TRUETYPE);
	scene_obj_set_hide(scn, OBJ_PROMPT1A, use_font);
	scene_obj_set_hide(scn, OBJ_PROMPT1B, !use_font);
	scene_obj_set_hide(scn, OBJ_AUTOBOOT, use_font);

	ret |= scene_txt_str(scn, "cur_item", OBJ_POINTER, STR_POINTER, ">",
			     NULL);
	ret |= scene_menu_set_pointer(scn, OBJ_MENU, OBJ_POINTER);
	if (ret < 0)
		return log_msg_ret("new", -EINVAL);

	exp->show_highlight = true;

	*expp = exp;

#undef SX
#undef SY
	return 0;
}

int bootflow_menu_add(struct expo *exp, struct bootflow *bflow, int seq,
		      struct scene **scnp)
{
	struct menu_priv *priv = exp->priv;
	char str[2], *label, *key;
	struct udevice *media;
	struct scene *scn;
	const char *name;
	uint preview_id;
	uint scene_id;
	bool add_gap;
	int ret;

	ret = expo_first_scene_id(exp);
	if (ret < 0)
		return log_msg_ret("scn", ret);
	scene_id = ret;
	scn = expo_lookup_scene_id(exp, scene_id);

	*str = seq < 10 ? '0' + seq : 'A' + seq - 10;
	str[1] = '\0';
	key = strdup(str);
	if (!key)
		return log_msg_ret("key", -ENOMEM);

	media = dev_get_parent(bflow->dev);
	if (device_get_uclass_id(media) == UCLASS_MASS_STORAGE)
		name = "usb";
	else
		name = media->name;
	label = strdup(name);

	if (!label) {
		free(key);
		return log_msg_ret("nam", -ENOMEM);
	}

	add_gap = priv->last_bootdev != bflow->dev;

	/* disable this gap for now, since it looks a little ugly */
	add_gap = false;
	priv->last_bootdev = bflow->dev;

	ret = expo_str(exp, "prompt", STR_POINTER, ">");
	ret |= scene_txt_str(scn, "label", ITEM_LABEL + seq,
			      STR_LABEL + seq, label, NULL);
	ret |= scene_txt_str(scn, "desc", ITEM_DESC + seq, STR_DESC + seq,
			    bflow->os_name ? bflow->os_name :
			    bflow->name, NULL);
	ret |= scene_txt_str(scn, "key", ITEM_KEY + seq, STR_KEY + seq, key,
			      NULL);
	preview_id = 0;
	if (bflow->logo) {
		preview_id = ITEM_PREVIEW + seq;
		ret |= scene_img(scn, "preview", preview_id,
				     bflow->logo, NULL);
	}
	ret |= scene_menuitem(scn, OBJ_MENU, "item", ITEM + seq,
				  ITEM_KEY + seq, ITEM_LABEL + seq,
				  ITEM_DESC + seq, preview_id,
				  add_gap ? SCENEMIF_GAP_BEFORE : 0,
				  NULL);

	if (ret < 0)
		return log_msg_ret("itm", -EINVAL);
	priv->num_bootflows++;
	*scnp = scn;

	return 0;
}

int bootflow_menu_add_all(struct expo *exp)
{
	struct bootflow *bflow;
	struct scene *scn;
	int ret, i;

	for (ret = bootflow_first_glob(&bflow), i = 0; !ret && i < 36;
	     ret = bootflow_next_glob(&bflow), i++) {
		struct bootmeth_uc_plat *ucp;

		if (bflow->state != BOOTFLOWST_READY)
			continue;

		/* No media to show for BOOTMETHF_GLOBAL bootmeths */
		ucp = dev_get_uclass_plat(bflow->method);
		if (ucp->flags & BOOTMETHF_GLOBAL)
			continue;

		ret = bootflow_menu_add(exp, bflow, i, &scn);
		if (ret)
			return log_msg_ret("bao", ret);
	}

	return 0;
}

int bootflow_menu_setup(struct bootstd_priv *std, bool text_mode,
			struct expo **expp)
{
	struct video_priv *vid_priv;
	struct udevice *dev;
	struct expo *exp;
	int ret;

	/* For now we only support a video console */
	ret = uclass_first_device_err(UCLASS_VIDEO, &dev);
	if (ret)
		return log_msg_ret("vid", ret);
	vid_priv = dev_get_uclass_priv(dev);

	{
		struct udevice *cons;
		struct vidconsole_priv *vc_priv = NULL;
		int char_h = 0;

		if (!device_find_first_child_by_uclass(dev, UCLASS_VIDEO_CONSOLE,
						       &cons))
			vc_priv = dev_get_uclass_priv(cons);
		if (vc_priv)
			char_h = vc_priv->y_charsize;

		ret = bootflow_menu_new(&exp, vid_priv->xsize, vid_priv->ysize,
					char_h);
	}
	if (ret)
		return log_msg_ret("bmn", ret);

	ret = expo_set_display(exp, dev);
	if (ret)
		return log_msg_ret("dis", ret);

	ret = expo_set_scene_id(exp, MAIN);
	if (ret)
		return log_msg_ret("scn", ret);

	if (text_mode)
		expo_set_text_mode(exp, text_mode);

	*expp = exp;

	return 0;
}

int bootflow_menu_start(struct bootstd_priv *std, bool text_mode,
			struct expo **expp)
{
	struct scene *scn;
	struct expo *exp;
	uint scene_id;
	int ret;

	ret = bootflow_menu_setup(std, text_mode, &exp);
	if (ret)
		return log_msg_ret("bmd", ret);

	ret = bootflow_menu_add_all(exp);
	if (ret)
		return log_msg_ret("bma", ret);

	if (ofnode_valid(std->theme)) {
		ret = expo_apply_theme(exp, std->theme);
		if (ret)
			return log_msg_ret("thm", ret);
	}

	ret = expo_calc_dims(exp);
	if (ret)
		return log_msg_ret("bmd", ret);

	ret = expo_first_scene_id(exp);
	if (ret < 0)
		return log_msg_ret("scn", ret);
	scene_id = ret;
	scn = expo_lookup_scene_id(exp, scene_id);

	scene_set_highlight_id(scn, OBJ_MENU);

	ret = scene_arrange(scn);
	if (ret)
		return log_msg_ret("arr", ret);

	*expp = exp;

	return 0;
}

int bootflow_menu_poll(struct expo *exp, int *seqp)
{
	struct bootflow *sel_bflow;
	struct expo_action act;
	struct scene *scn;
	int item, ret;

	sel_bflow = NULL;

	scn = expo_lookup_scene_id(exp, exp->scene_id);

	item = scene_menu_get_cur_item(scn, OBJ_MENU);
	*seqp = item > 0 ? item - ITEM : -1;

	ret = expo_poll(exp, &act);
	if (ret)
		return log_msg_ret("bmp", ret);

	switch (act.type) {
	case EXPOACT_SELECT:
		*seqp = act.select.id - ITEM;
		break;
	case EXPOACT_POINT_ITEM: {
		struct scene *scn = expo_lookup_scene_id(exp, MAIN);

		if (!scn)
			return log_msg_ret("bms", -ENOENT);
		ret = scene_menu_select_item(scn, OBJ_MENU, act.select.id);
		if (ret)
			return log_msg_ret("bmp", ret);
		return -ERESTART;
	}
	case EXPOACT_QUIT:
		return -EPIPE;
	default:
		return -EAGAIN;
	}

	return 0;
}
