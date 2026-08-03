// SPDX-License-Identifier: GPL-2.0+
// SPDX-FileCopyrightText: 2026 Flipper FZCO

#include <adc.h>
#include <env.h>
#include <fdtdec.h>
#include <fdt_support.h>
#include <log.h>
#include <linux/errno.h>
#include <linux/types.h>

#define HW_ID_CHANNEL		2

struct board_model {
	unsigned int adc_low;
	unsigned int adc_high;
	const char *fdtfile;
};

static const struct board_model board_models[] = {
	{ 0, 208, "rockchip/rk3576-flipper-one-rev-f0b0c1.dtb" },
	{ 209, 616, "rockchip/rk3576-flipper-one-rev-f0b1c2.dtb" },
};

/*
 * Called once per FIT configuration node while SPL picks the control DTB, so
 * cache the verdict instead of resampling the channel for every candidate.
 */
static const struct board_model *get_board_model(void)
{
	static const struct board_model *model;
	static bool detected;
	unsigned int val;
	int i, ret;

	if (detected)
		return model;

	ret = adc_channel_single_shot("adc@2ae00000", HW_ID_CHANNEL, &val);
	if (ret) {
		log_debug("HW ID read failed: %d\n", ret);
		return NULL;
	}

	detected = true;

	for (i = 0; i < ARRAY_SIZE(board_models); i++) {
		unsigned int min = board_models[i].adc_low;
		unsigned int max = board_models[i].adc_high;

		if (min <= val && val <= max) {
			model = &board_models[i];
			log_debug("HW ID %u: %s\n", val, model->fdtfile);
			return model;
		}
	}

	log_debug("HW ID %u: unknown board model\n", val);

	return NULL;
}

int rk_board_late_init(void)
{
	const struct board_model *model = get_board_model();

	if (model)
		env_set("fdtfile", model->fdtfile);

	return 0;
}

int board_fit_config_name_match(const char *name)
{
	const struct board_model *model = get_board_model();

	if (model && !strcmp(name, model->fdtfile))
		return 0;

	return -EINVAL;
}

#ifdef CONFIG_OF_BOARD_SETUP
int ft_board_setup(void *blob, struct bd_info *bd)
{
	return 0;
}
#endif
