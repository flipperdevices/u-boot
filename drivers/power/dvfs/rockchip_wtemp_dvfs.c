// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018 Fuzhou Rockchip Electronics Co., Ltd
 *
 * Wide temperature DVFS driver.
 *
 * Parses temperature thresholds from DTS OPP table:
 * - rockchip,low-temp: Low temperature threshold (millidegrees)
 * - rockchip,high-temp: High temperature threshold (millidegrees)
 * - rockchip,max-volt: Maximum voltage (uV)
 *
 * If these properties are not present in the DTS, the driver will not
 * perform any DVFS adjustments - it will only provide status information.
 *
 * Policy (when enabled):
 * - Below low-temp: increase voltage for stability, lower freq if over max-volt
 * - Above high-temp: throttle to lowest OPP
 */
#include <config.h>
#include <dm.h>
#include <clk.h>
#include <dvfs.h>
#include <thermal.h>
#include <linux/delay.h>
#include <linux/list.h>

#include <asm/arch-rockchip/clock.h>
#include <power/regulator.h>

#define FDT_PATH_CPUS			"/cpus"
#define FDT_PATH_THERMAL_ZONES		"/thermal-zones"

#define OPP_TABLE_MAX		20
#define RATE_LOWER_LEVEL_N	2
#define DIFF_VOLTAGE_UV		50000
#define TEMP_STRING_LEN		12

static LIST_HEAD(pm_e_head);

enum pm_id {
	PM_CPU,
};

struct opp_table {
	u64 hz;
	u32 uv;
};

struct lmt_param {
	int low_temp;		/* milli degree */
	int high_temp;		/* milli degree */
	int tz_temp;		/* milli degree, from thermal zone */
	int max_volt;		/* uV */

	bool ltemp_limit;	/* low_temp threshold enabled */
	bool htemp_limit;	/* high_temp threshold enabled */
	bool tztemp_limit;	/* thermal zone threshold enabled */
};

struct pm_element {
	int id;
	const char *name;
	int volt_diff;
	u32 opp_nr;
	struct opp_table opp[OPP_TABLE_MAX];
	struct lmt_param lmt;
	struct clk clk;
	struct list_head node;
};

struct wtemp_dvfs_priv {
	struct udevice *thermal;
	struct pm_element *cpu;
};

static struct pm_element pm_cpu = {
	.id		= PM_CPU,
	.name		= "cpu",
	.volt_diff	= DIFF_VOLTAGE_UV,
	.lmt = {
		.low_temp	= 0,
		.high_temp	= 0,
		.tz_temp	= 0,
		.max_volt	= 0,
		.ltemp_limit	= false,
		.htemp_limit	= false,
		.tztemp_limit	= false,
	},
};

static void temp2string(int temp, char *data, int len)
{
	int decimal_point;
	int integer;

	integer = abs(temp) / 1000;
	decimal_point = abs(temp) % 1000;
	snprintf(data, len, "%s%d.%d",
		 temp < 0 ? "-" : "", integer, decimal_point);
}

static ulong wtemp_get_lowlevel_rate(ulong rate, u32 level,
				     struct pm_element *e)
{
	struct opp_table *opp;
	int i, count, idx = 0;

	opp = e->opp;
	count = e->opp_nr;

	for (i = 0; i < count; i++) {
		if (opp[i].hz >= rate) {
			idx = (i <= level) ? 0 : i - level;
			break;
		}
	}

	return opp[idx].hz;
}

static ulong __wtemp_clk_get_rate(struct pm_element *e)
{
	return clk_get_rate(&e->clk);
}

static ulong __wtemp_clk_set_rate(struct pm_element *e, ulong rate)
{
	return clk_set_rate(&e->clk, rate);
}

/*
 * Find current OPP index based on frequency
 * Returns -1 if not found (frequency between OPPs)
 */
static int wtemp_find_current_opp(struct pm_element *e)
{
	ulong rate = __wtemp_clk_get_rate(e);
	int i;

	for (i = 0; i < e->opp_nr; i++) {
		if (e->opp[i].hz == rate)
			return i;
	}
	/* Find closest lower OPP */
	for (i = e->opp_nr - 1; i >= 0; i--) {
		if (e->opp[i].hz <= rate)
			return i;
	}
	return 0;
}

static void wtemp_print_opp_status(struct pm_element *e)
{
	ulong rate = __wtemp_clk_get_rate(e);
	int current_opp = wtemp_find_current_opp(e);
	int i;

	printf("\nDVFS: %s OPP table (%d entries):\n", e->name, e->opp_nr);
	printf("  #   Frequency (MHz)   Voltage (mV)   Status\n");
	printf("  --  ---------------   ------------   ------\n");

	for (i = 0; i < e->opp_nr; i++) {
		printf("  %2d  %10llu       %7d       %s\n",
		       i, e->opp[i].hz / 1000000, e->opp[i].uv / 1000,
		       (i == current_opp) ? "<-- ACTIVE" : "");
	}
	printf("\nCurrent rate: %lu Hz (%lu MHz)\n", rate, rate / 1000000);
	printf("Max OPP: #%d @ %llu MHz\n", e->opp_nr - 1, e->opp[e->opp_nr - 1].hz / 1000000);
}

/*
 * Policy: Lower frequency for low temperature stability
 */
static void wtemp_dvfs_low_temp_adjust(struct udevice *dev, struct pm_element *e)
{
	struct wtemp_dvfs_priv *priv = dev_get_priv(dev);
	ulong org_rate, tgt_rate, rb_rate;

	org_rate = __wtemp_clk_get_rate(e);

	/* For low temp, lower frequency to be safe */
	tgt_rate = wtemp_get_lowlevel_rate(org_rate, RATE_LOWER_LEVEL_N, priv->cpu);
	__wtemp_clk_set_rate(e, tgt_rate);

	rb_rate = __wtemp_clk_get_rate(e);
	if (tgt_rate != rb_rate)
		printf("DVFS WARN: %s: target rate=%ld, readback rate=%ld\n",
		       e->name, tgt_rate, rb_rate);

	printf("DVFS: %s(low temp): %ld -> %ld Hz\n",
	       e->name, org_rate, rb_rate);
}

/*
 * Policy: Throttle for high temperature
 *
 * Just set opp table[0] rate, i.e. the lowest performance.
 */
static void wtemp_dvfs_high_temp_adjust(struct udevice *dev, struct pm_element *e)
{
	ulong org_rate, tgt_rate, rb_rate;

	org_rate = __wtemp_clk_get_rate(e);

	tgt_rate = e->opp[0].hz;
	__wtemp_clk_set_rate(e, tgt_rate);
	rb_rate = __wtemp_clk_get_rate(e);
	if (tgt_rate != rb_rate) {
		printf("DVFS WARN: %s: target rate=%ld, readback rate=%ld\n",
		       e->name, tgt_rate, rb_rate);
		return;
	}

	printf("DVFS: %s(high temp): %ld -> %ld Hz\n",
	       e->name, org_rate, rb_rate);
}

static int __wtemp_dvfs_apply(struct udevice *dev, struct pm_element *e, int temp)
{
	int applied = 0;

	/* Low temperature: increase voltage for stability */
	if (e->lmt.ltemp_limit && temp <= e->lmt.low_temp) {
		wtemp_dvfs_low_temp_adjust(dev, e);
		applied = 1;
	}

	/* High temperature: throttle to lowest OPP */
	if (e->lmt.tztemp_limit && temp >= e->lmt.tz_temp) {
		wtemp_dvfs_high_temp_adjust(dev, e);
		applied = 1;
	} else if (e->lmt.htemp_limit && temp >= e->lmt.high_temp) {
		wtemp_dvfs_high_temp_adjust(dev, e);
		applied = 1;
	}

	return applied;
}

static int __wtemp_common_ofdata_to_platdata(ofnode node, struct pm_element *e)
{
	ofnode opp_node, opp_entry;
	u32 phandle, uv[3];
	uint64_t hz;
	int ret, val;

	/* Get clock from CPU node - this properly handles SCMI clocks */
	ret = clk_get_by_index_nodev(node, 0, &e->clk);
	if (ret) {
		printf("DVFS: %s: clk_get_by_index_nodev failed, ret=%d\n", e->name, ret);
		return ret;
	}
	debug("DVFS: %s: got clock id=%lu from %s\n",
	       e->name, e->clk.id, e->clk.dev ? e->clk.dev->name : "NULL");

	/* Get opp-table and parse rockchip properties */
	if (!ofnode_read_u32(node, "operating-points-v2", &phandle)) {
		opp_node = ofnode_get_by_phandle(phandle);

		/* Parse rockchip-specific temperature/voltage limits */
		val = ofnode_read_s32_default(opp_node, "rockchip,low-temp", -ENODATA);
		if (val != -ENODATA) {
			e->lmt.low_temp = val;
			e->lmt.ltemp_limit = true;
		}

		val = ofnode_read_s32_default(opp_node, "rockchip,high-temp", -ENODATA);
		if (val != -ENODATA) {
			e->lmt.high_temp = val;
			e->lmt.htemp_limit = true;
		}

		val = ofnode_read_s32_default(opp_node, "rockchip,max-volt", -ENODATA);
		if (val != -ENODATA) {
			e->lmt.max_volt = val;
		}

		/* Parse OPP entries */
		ofnode_for_each_subnode(opp_entry, opp_node) {
			if (e->opp_nr >= OPP_TABLE_MAX) {
				printf("DVFS: over max(%d) opp table items\n",
				       OPP_TABLE_MAX);
				break;
			}
			ofnode_read_u64(opp_entry, "opp-hz", &hz);
			/*
			 * Handle upstream format: opp-microvolt = <min target max>;
			 * We read the first value which is the target voltage.
			 */
			if (ofnode_read_u32_array(opp_entry, "opp-microvolt", uv, 3) == 0) {
				e->opp[e->opp_nr].uv = uv[0];
			} else if (ofnode_read_u32_array(opp_entry, "opp-microvolt", uv, 1) == 0) {
				e->opp[e->opp_nr].uv = uv[0];
			} else {
				debug("DVFS: %s: Can't read opp-microvolt for %s\n",
				      e->name, ofnode_get_name(opp_entry));
				continue;
			}
			e->opp[e->opp_nr].hz = hz;
			e->opp_nr++;
			debug("DVFS: %s: opp[%d]: hz=%lld, uv=%d, %s\n",
			      e->name, e->opp_nr - 1,
			      hz, e->opp[e->opp_nr - 1].uv, ofnode_get_name(opp_entry));
		}
	}

	if (!e->opp_nr) {
		printf("DVFS: %s: Can't find opp table\n", e->name);
		return -EINVAL;
	}

	/* If max_volt not set, use highest OPP voltage */
	if (!e->lmt.max_volt && e->opp_nr > 0)
		e->lmt.max_volt = e->opp[e->opp_nr - 1].uv;

	return 0;
}

static int wtemp_dvfs_apply(struct udevice *dev)
{
	struct wtemp_dvfs_priv *priv = dev_get_priv(dev);
	struct list_head *node;
	struct pm_element *e;
	char s_temp[TEMP_STRING_LEN];
	int temp, ret;
	bool any_limit = false;

	/* Check if any limits are configured */
	list_for_each(node, &pm_e_head) {
		e = list_entry(node, struct pm_element, node);
		if (e->lmt.ltemp_limit || e->lmt.htemp_limit || e->lmt.tztemp_limit) {
			any_limit = true;
			break;
		}
	}

	if (!any_limit) {
		debug("DVFS: No temperature limits configured in DTS, skipping\n");
		return 0;
	}

	ret = thermal_get_temp(priv->thermal, &temp);
	if (ret) {
		printf("DVFS: Get temperature failed, ret=%d\n", ret);
		return ret;
	}

	temp2string(temp, s_temp, TEMP_STRING_LEN);
	printf("DVFS: Temperature %s'C\n", s_temp);

	/* Apply dvfs policy for all pm element */
	list_for_each(node, &pm_e_head) {
		e = list_entry(node, struct pm_element, node);
		__wtemp_dvfs_apply(dev, e, temp);
	}

	return 0;
}

static int wtemp_dvfs_repeat_apply(struct udevice *dev)
{
	/* Repeat just calls apply again */
	return wtemp_dvfs_apply(dev);
}

/*
 * Parse upstream thermal zone to get high temp threshold.
 * Look for passive trip point in bigcore-thermal or littlecore-thermal.
 * Sets tztemp_limit flag if found.
 */
static int parse_thermal_zone_high_temp(struct pm_element *cpu)
{
	ofnode tz_root, tz_node, trips_node, trip;
	const char *tz_name, *trip_type;
	int temp;

	tz_root = ofnode_path(FDT_PATH_THERMAL_ZONES);
	if (!ofnode_valid(tz_root))
		return -ENOENT;

	ofnode_for_each_subnode(tz_node, tz_root) {
		tz_name = ofnode_get_name(tz_node);
		if (!tz_name)
			continue;

		/* Look for CPU thermal zones */
		if (!strstr(tz_name, "bigcore") && !strstr(tz_name, "littlecore"))
			continue;

		trips_node = ofnode_find_subnode(tz_node, "trips");
		if (!ofnode_valid(trips_node))
			continue;

		/* Look for passive trip point */
		ofnode_for_each_subnode(trip, trips_node) {
			trip_type = ofnode_read_string(trip, "type");
			if (trip_type && !strcmp(trip_type, "passive")) {
				temp = ofnode_read_s32_default(trip, "temperature", -ENODATA);
				if (temp != -ENODATA) {
					cpu->lmt.tz_temp = temp;
					cpu->lmt.tztemp_limit = true;
					debug("DVFS: tz_temp=%d from %s\n", temp, tz_name);
					return 0;
				}
			}
		}

		/* Fallback: look for alert trip */
		ofnode_for_each_subnode(trip, trips_node) {
			const char *name = ofnode_get_name(trip);
			if (name && strstr(name, "alert")) {
				temp = ofnode_read_s32_default(trip, "temperature", -ENODATA);
				if (temp != -ENODATA) {
					cpu->lmt.tz_temp = temp;
					cpu->lmt.tztemp_limit = true;
					debug("DVFS: tz_temp=%d from %s alert\n", temp, tz_name);
					return 0;
				}
			}
		}
	}

	return -ENOENT;
}

static int wtemp_dvfs_ofdata_to_platdata(struct udevice *dev)
{
	struct wtemp_dvfs_priv *priv = dev_get_priv(dev);
	ofnode cpus, cpu;
	const char *name;
	int ret;

	INIT_LIST_HEAD(&pm_e_head);

	/* Parse cpu node */
	priv->cpu = &pm_cpu;
	cpus = ofnode_path(FDT_PATH_CPUS);
	if (!ofnode_valid(cpus)) {
		printf("DVFS: Can't find %s\n", FDT_PATH_CPUS);
		return -ENOENT;
	}

	ofnode_for_each_subnode(cpu, cpus) {
		name = ofnode_get_property(cpu, "device_type", NULL);
		if (!name)
			continue;
		if (!strcmp(name, "cpu")) {
			ret = __wtemp_common_ofdata_to_platdata(cpu, priv->cpu);
			if (ret)
				return ret;
			break;
		}
	}

	list_add_tail(&priv->cpu->node, &pm_e_head);

	/* Try to get high_temp from thermal zones if not in OPP table */
	if (!priv->cpu->lmt.htemp_limit)
		parse_thermal_zone_high_temp(priv->cpu);

	/* Log configuration status */
	if (priv->cpu->lmt.ltemp_limit || priv->cpu->lmt.htemp_limit ||
	    priv->cpu->lmt.tztemp_limit) {
		printf("DVFS: cpu: low=%d%s, high=%d%s, tz=%d%s, max_volt=%d, opp_nr=%d\n",
		       priv->cpu->lmt.low_temp / 1000,
		       priv->cpu->lmt.ltemp_limit ? "°C" : "(off)",
		       priv->cpu->lmt.high_temp / 1000,
		       priv->cpu->lmt.htemp_limit ? "°C" : "(off)",
		       priv->cpu->lmt.tz_temp / 1000,
		       priv->cpu->lmt.tztemp_limit ? "°C" : "(off)",
		       priv->cpu->lmt.max_volt / 1000,
		       priv->cpu->opp_nr);
	} else {
		printf("DVFS: cpu: No temperature thresholds in DTS, DVFS disabled (opp_nr=%d)\n",
		       priv->cpu->opp_nr);
	}

	return 0;
}

static int wtemp_dvfs_status(struct udevice *dev)
{
	struct wtemp_dvfs_priv *priv = dev_get_priv(dev);
	struct list_head *node;
	struct pm_element *e;
	char s_temp[TEMP_STRING_LEN];
	int temp, ret;

	ret = thermal_get_temp(priv->thermal, &temp);
	if (ret) {
		printf("DVFS: Get temperature failed, ret=%d\n", ret);
		return ret;
	}

	temp2string(temp, s_temp, TEMP_STRING_LEN);
	printf("DVFS: Current temperature: %s°C\n", s_temp);

	list_for_each(node, &pm_e_head) {
		e = list_entry(node, struct pm_element, node);

		printf("DVFS: %s thresholds:\n", e->name);
		if (e->lmt.ltemp_limit)
			printf("  low_temp:  %d°C (enabled)\n", e->lmt.low_temp / 1000);
		else
			printf("  low_temp:  not configured\n");

		if (e->lmt.htemp_limit)
			printf("  high_temp: %d°C (enabled)\n", e->lmt.high_temp / 1000);
		else
			printf("  high_temp: not configured\n");

		if (e->lmt.tztemp_limit)
			printf("  tz_temp:   %d°C (enabled)\n", e->lmt.tz_temp / 1000);
		else
			printf("  tz_temp:   not configured\n");

		if (e->lmt.max_volt)
			printf("  max_volt:  %dmV\n", e->lmt.max_volt / 1000);

		if (!e->lmt.ltemp_limit && !e->lmt.htemp_limit && !e->lmt.tztemp_limit)
			printf("  ** DVFS adjustments DISABLED (no thresholds in DTS)\n");

		wtemp_print_opp_status(e);
	}

	return 0;
}

static int wtemp_dvfs_set_max(struct udevice *dev)
{
	struct list_head *node;
	struct pm_element *e;
	ulong org_rate, max_rate, new_rate;

	list_for_each(node, &pm_e_head) {
		e = list_entry(node, struct pm_element, node);
		if (e->opp_nr == 0)
			continue;

		org_rate = __wtemp_clk_get_rate(e);
		max_rate = e->opp[e->opp_nr - 1].hz;

		printf("DVFS: %s: setting to max OPP #%d @ %lu MHz (voltage %d mV)\n",
		       e->name, e->opp_nr - 1,
		       max_rate / 1000000, e->opp[e->opp_nr - 1].uv / 1000);

		__wtemp_clk_set_rate(e, max_rate);
		new_rate = __wtemp_clk_get_rate(e);

		printf("DVFS: %s: %lu MHz -> %lu MHz\n",
		       e->name, org_rate / 1000000, new_rate / 1000000);

		if (new_rate != max_rate) {
			printf("DVFS WARN: %s: requested %lu Hz but got %lu Hz\n",
			       e->name, max_rate, new_rate);
		}
	}

	return 0;
}

static const struct dm_dvfs_ops wtemp_dvfs_ops = {
	.apply = wtemp_dvfs_apply,
	.repeat_apply = wtemp_dvfs_repeat_apply,
	.status = wtemp_dvfs_status,
	.set_max = wtemp_dvfs_set_max,
};

static int wtemp_dvfs_probe(struct udevice *dev)
{
	struct wtemp_dvfs_priv *priv = dev_get_priv(dev);
	int ret;

	/* Init thermal */
	ret = uclass_get_device(UCLASS_THERMAL, 0, &priv->thermal);
	if (ret) {
		printf("DVFS: Get thermal device failed, ret=%d\n", ret);
		return ret;
	}

	return 0;
}

static const struct udevice_id wtemp_dvfs_match[] = {
	{ .compatible = "operating-points-v2", },
	{},
};

U_BOOT_DRIVER(rockchip_wide_temp_dvfs) = {
	.name		= "rockchip_wide_temp_dvfs",
	.id		= UCLASS_DVFS,
	.ops		= &wtemp_dvfs_ops,
	.of_match	= wtemp_dvfs_match,
	.probe		= wtemp_dvfs_probe,
	.of_to_plat	= wtemp_dvfs_ofdata_to_platdata,
	.priv_auto	= sizeof(struct wtemp_dvfs_priv),
};
