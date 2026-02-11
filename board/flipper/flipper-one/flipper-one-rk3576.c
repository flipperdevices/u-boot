// SPDX-License-Identifier: GPL-2.0+
// SPDX-FileCopyrightText: 2026 Flipper FZCO

#include <adc.h>
#include <env.h>
#include <fdtdec.h>
#include <fdt_support.h>
#include <log.h>
#include <spl.h>
#include <asm/gpio.h>
#include <dm/ofnode.h>
#include <linux/errno.h>
#include <linux/types.h>

#define HW_ID_CHANNEL		2

/*
 * The M.2 slot is sampled wherever this board hands a devicetree onward: SPL on
 * the Falcon path, and ft_board_setup() when U-Boot proper boots the kernel
 * itself. With neither, there is no tree to patch and none of this is built.
 */
#if defined(CONFIG_XPL_BUILD) || defined(CONFIG_OF_BOARD_SETUP)

#define M2_CONFIG_COUNT		4

#define M2_PCIE_PATH		"/soc/pcie@22000000"
#define M2_SATA_PATH		"/soc/sata@2a240000"

/* What the module in the M.2 slot wants off the shared combo PHY, if anything */
enum m2_iface {
	M2_IFACE_NONE,
	M2_IFACE_PCIE,
	M2_IFACE_SATA,
};

struct m2_strap {
	const char *name;
	enum m2_iface iface;
};

/*
 * M.2 Socket 2 (key B) declares what a module is over four straps: CONFIG_0
 * (pin 21), CONFIG_1 (pin 69), CONFIG_2 (pin 75) and CONFIG_3 (pin 1), sampled
 * here as a nibble with CONFIG_0 in bit 0. The host biases all four high, so a
 * module reads back as the set of pins it grounds and an empty slot reads 0xf.
 *
 * All sixteen combinations are assigned, so the slot never has to be guessed
 * at. Only the interface matters here: pcie@22000000 and sata@2a240000 share
 * combphy0, so at most one of the two is ever worth bringing up, and a module
 * that speaks neither leaves both switched off. Interface assignments are from
 * the M.2 pinout table in congatec AN43.
 */
static const struct m2_strap m2_straps[] = {
	{ "SATA SSD",			M2_IFACE_SATA },	/* 0x0 */
	{ "WWAN, SSIC cfg 0",		M2_IFACE_NONE },	/* 0x1 */
	{ "PCIe SSD",			M2_IFACE_PCIE },	/* 0x2 */
	{ "WWAN, SSIC cfg 1",		M2_IFACE_NONE },	/* 0x3 */
	{ "WWAN, PCIe cfg 0",		M2_IFACE_PCIE },	/* 0x4 */
	{ "WWAN, SSIC cfg 2",		M2_IFACE_NONE },	/* 0x5 */
	{ "WWAN, PCIe cfg 1",		M2_IFACE_PCIE },	/* 0x6 */
	{ "WWAN, SSIC cfg 3",		M2_IFACE_NONE },	/* 0x7 */
	{ "WWAN, PCIe + USB3 cfg 0",	M2_IFACE_PCIE },	/* 0x8 */
	{ "WWAN, PCIe cfg 2",		M2_IFACE_PCIE },	/* 0x9 */
	{ "WWAN, PCIe + USB3 cfg 1",	M2_IFACE_PCIE },	/* 0xa */
	{ "WWAN, PCIe cfg 3",		M2_IFACE_PCIE },	/* 0xb */
	{ "WWAN, PCIe + USB3 cfg 2",	M2_IFACE_PCIE },	/* 0xc */
	{ "WWAN, PCIe + USB3, vendor",	M2_IFACE_PCIE },	/* 0xd */
	{ "WWAN, PCIe + USB3 cfg 3",	M2_IFACE_PCIE },	/* 0xe */
	{ "empty",			M2_IFACE_NONE },	/* 0xf */
};

static int m2_read_config(void)
{
	struct gpio_desc gpios[M2_CONFIG_COUNT];
	static int config = -ENODEV;
	static bool sampled;
	ofnode node;
	int ret;

	if (sampled)
		return config;

	node = ofnode_path("/m2-slot");
	if (!ofnode_valid(node))
		return -ENODEV;

	ret = gpio_request_list_by_name_nodev(node, "m2-config-gpios", gpios,
					      M2_CONFIG_COUNT, GPIOD_IS_IN);
	if (ret != M2_CONFIG_COUNT) {
		log_debug("M.2: CONFIG straps unavailable: %d\n", ret);
		if (ret > 0)
			gpio_free_list_nodev(gpios, ret);

		return ret < 0 ? ret : -EINVAL;
	}

	ret = dm_gpio_get_values_as_int(gpios, M2_CONFIG_COUNT);
	gpio_free_list_nodev(gpios, M2_CONFIG_COUNT);
	if (ret < 0) {
		log_debug("M.2: CONFIG strap read failed: %d\n", ret);
		return ret;
	}

	sampled = true;
	config = ret;

	return config;
}

static int m2_set_status(void *blob, const char *path, bool enable)
{
	int offset, ret;

	offset = fdt_path_offset(blob, path);
	if (offset < 0)
		return offset;

	ret = enable ? fdt_status_okay(blob, offset)
		     : fdt_status_disabled(blob, offset);
	if (ret != -FDT_ERR_NOSPACE)
		return ret;

	/*
	 * "okay" grows into "disabled", and SPL is handed the tree exactly as
	 * it was packed into the FIT. Grow it in place, which the Falcon FDT
	 * has room for in its own load region, then look the node up again as
	 * fdt_open_into() moves the blocks and the offset does not survive.
	 */
	ret = fdt_open_into(blob, blob, fdt_totalsize(blob) + 512);
	if (ret)
		return ret;

	offset = fdt_path_offset(blob, path);
	if (offset < 0)
		return offset;

	return enable ? fdt_status_okay(blob, offset)
		      : fdt_status_disabled(blob, offset);
}

static void m2_apply_status(void *blob, const char *path, bool enable)
{
	int ret = m2_set_status(blob, path, enable);

	if (ret)
		printf("M.2: cannot %s %s: %s\n", enable ? "enable" : "disable",
		       path, fdt_strerror(ret));
}

/*
 * Switch the slot's controllers to match what is actually installed, so that
 * Linux does not spend a second training a PCIe link into a slot holding a SATA
 * drive, a USB-only modem, or nothing at all.
 */
static void m2_fixup_fdt(void *blob)
{
	enum m2_iface iface;
	int config;

	config = m2_read_config();
	if (config < 0 || config >= ARRAY_SIZE(m2_straps))
		return;

	iface = m2_straps[config].iface;
	printf("M.2: CONFIG 0x%x: %s\n", config, m2_straps[config].name);

	/* pcie0 arrives enabled and sata0 disabled, so only write the delta */
	if (iface != M2_IFACE_PCIE)
		m2_apply_status(blob, M2_PCIE_PATH, false);
	if (iface == M2_IFACE_SATA)
		m2_apply_status(blob, M2_SATA_PATH, true);
}

#endif /* CONFIG_XPL_BUILD || CONFIG_OF_BOARD_SETUP */

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

#ifdef CONFIG_XPL_BUILD
void spl_perform_board_fixups(struct spl_image_info *spl_image)
{
	/*
	 * In Falcon mode this is the only chance to touch the tree Linux boots
	 * with, as U-Boot proper never runs. It is also ahead of spl_fixup_fdt()
	 * packing the blob, so there is still slack to edit properties in.
	 */
	if (spl_image->fdt_addr)
		m2_fixup_fdt(spl_image->fdt_addr);
}
#endif

#ifdef CONFIG_OF_BOARD_SETUP
int ft_board_setup(void *blob, struct bd_info *bd)
{
	m2_fixup_fdt(blob);

	return 0;
}
#endif
