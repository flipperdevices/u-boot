// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip RK3576 Power Domain Driver for U-Boot
 *
 * Copyright (c) 2024
 */

#include <dm.h>
#include <power-domain-uclass.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/iopoll.h>
#include <dt-bindings/power/rockchip,rk3576-power.h>

/* PMU register offsets */
#define RK3576_PMU_PWR_CON		0x210
#define RK3576_PMU_PWR_STATUS		0x230
#define RK3576_PMU_CHAIN_STATUS		0x248
#define RK3576_PMU_MEM_STATUS		0x250
#define RK3576_PMU_MEM_PWR		0x300
#define RK3576_PMU_BIU_IDLE_REQ		0x110
#define RK3576_PMU_BIU_IDLE_ACK		0x120
#define RK3576_PMU_BIU_IDLE_STATUS	0x128
#define RK3576_PMU_REPAIR_STATUS	0x570
#define RK3576_PMU_CLK_UNGATE		0x140

#define DOMAIN_TIMEOUT_US		10000

struct rk3576_pd_info {
    const char *name;
    u32 pwr_offset;
    u32 pwr_mask;
    u32 pwr_w_mask;
    u32 status_mask;
    u32 req_offset;
    u32 req_mask;
    u32 req_w_mask;
    u32 idle_mask;
    u32 repair_status_mask;
    u32 clk_ungate_mask;
};

#define DOMAIN(_name, p_off, pwr, sts, r_sts, r_off, req, idle, g_mask) \
{								\
    .name = _name,						\
    .pwr_offset = p_off,					\
    .pwr_mask = pwr,					\
    .pwr_w_mask = (pwr) << 16,				\
    .status_mask = sts,					\
    .req_offset = r_off,					\
    .req_mask = req,					\
    .req_w_mask = (req) << 16,				\
    .idle_mask = idle,					\
    .repair_status_mask = r_sts,				\
    .clk_ungate_mask = g_mask,				\
}

static const struct rk3576_pd_info rk3576_pd_info[] = {
    [RK3576_PD_NPU]     = DOMAIN("npu",    0x0, BIT(0),  BIT(0), 0,       0x0, 0,       0,       0),
    [RK3576_PD_NVM]     = DOMAIN("nvm",    0x0, BIT(6),  0,      BIT(6),  0x4, BIT(2),  BIT(18), BIT(2)),
    [RK3576_PD_SDGMAC]  = DOMAIN("sdgmac", 0x0, BIT(7),  0,      BIT(7),  0x4, BIT(1),  BIT(17), 0x6),
    [RK3576_PD_AUDIO]   = DOMAIN("audio",  0x0, BIT(8),  0,      BIT(8),  0x4, BIT(0),  BIT(16), BIT(0)),
    [RK3576_PD_PHP]     = DOMAIN("php",    0x0, BIT(9),  0,      BIT(9),  0x0, BIT(15), BIT(15), BIT(15)),
    [RK3576_PD_SUBPHP]  = DOMAIN("subphp", 0x0, BIT(10), 0,      BIT(10), 0x0, 0,       0,       0),
    [RK3576_PD_VOP]     = DOMAIN("vop",    0x0, BIT(11), 0,      BIT(11), 0x0, 0x6000,  0x6000,  0x6000),
    [RK3576_PD_VO1]     = DOMAIN("vo1",    0x0, BIT(14), 0,      BIT(14), 0x0, BIT(12), BIT(12), 0x7000),
    [RK3576_PD_VO0]     = DOMAIN("vo0",    0x0, BIT(15), 0,      BIT(15), 0x0, BIT(11), BIT(11), 0x6800),
    [RK3576_PD_USB]     = DOMAIN("usb",    0x4, BIT(0),  0,      BIT(16), 0x0, BIT(10), BIT(10), 0x6400),
    [RK3576_PD_VI]      = DOMAIN("vi",     0x4, BIT(1),  0,      BIT(17), 0x0, BIT(9),  BIT(9),  BIT(9)),
    [RK3576_PD_VEPU0]   = DOMAIN("vepu0",  0x4, BIT(2),  0,      BIT(18), 0x0, BIT(7),  BIT(7),  0x280),
    [RK3576_PD_VEPU1]   = DOMAIN("vepu1",  0x4, BIT(3),  0,      BIT(19), 0x0, BIT(8),  BIT(8),  BIT(8)),
    [RK3576_PD_VDEC]    = DOMAIN("vdec",   0x4, BIT(4),  0,      BIT(20), 0x0, BIT(6),  BIT(6),  BIT(6)),
    [RK3576_PD_VPU]     = DOMAIN("vpu",    0x4, BIT(5),  0,      BIT(21), 0x0, BIT(5),  BIT(5),  BIT(5)),
    [RK3576_PD_NPUTOP]  = DOMAIN("nputop", 0x4, BIT(6),  0,      BIT(22), 0x0, 0x18,    0x18,    0x18),
    [RK3576_PD_NPU0]    = DOMAIN("npu0",   0x4, BIT(7),  0,      BIT(23), 0x0, BIT(1),  BIT(1),  0x1a),
    [RK3576_PD_NPU1]    = DOMAIN("npu1",   0x4, BIT(8),  0,      BIT(24), 0x0, BIT(2),  BIT(2),  0x1c),
    [RK3576_PD_GPU]     = DOMAIN("gpu",    0x4, BIT(9),  0,      BIT(25), 0x0, BIT(0),  BIT(0),  BIT(0)),
};

struct rk3576_pmu_priv {
    void __iomem *base;
};

static bool rk3576_pmu_domain_is_on(struct rk3576_pmu_priv *priv,
		    const struct rk3576_pd_info *info)
{
    u32 val;

    if (info->repair_status_mask) {
	val = readl(priv->base + RK3576_PMU_REPAIR_STATUS);
	return val & info->repair_status_mask;
    }

    if (info->status_mask == 0) {
	/* Check idle status for idle-only domains */
	val = readl(priv->base + RK3576_PMU_BIU_IDLE_STATUS);
	return !(val & info->idle_mask);
    }

    val = readl(priv->base + RK3576_PMU_PWR_STATUS);
    return !(val & info->status_mask);
}

static int rk3576_pmu_set_idle_request(struct rk3576_pmu_priv *priv,
		       const struct rk3576_pd_info *info,
		       bool idle)
{
    u32 val, target;
    int ret;

    if (info->req_mask == 0)
	return 0;

    val = idle ? (info->req_mask | info->req_w_mask) : info->req_w_mask;
    writel(val, priv->base + RK3576_PMU_BIU_IDLE_REQ + info->req_offset);

    /* Wait for ack */
    target = idle ? info->idle_mask : 0;
    ret = readl_poll_timeout(priv->base + RK3576_PMU_BIU_IDLE_ACK,
		 val, (val & info->idle_mask) == target,
		 DOMAIN_TIMEOUT_US);
    if (ret) {
	pr_err("%s: failed to get ack for %s, val=0x%x\n",
	       __func__, info->name, val);
	return ret;
    }

    /* Wait for idle status */
    ret = readl_poll_timeout(priv->base + RK3576_PMU_BIU_IDLE_STATUS,
		 val, (val & info->idle_mask) == target,
		 DOMAIN_TIMEOUT_US);
    if (ret) {
	pr_err("%s: failed to set idle for %s, val=0x%x\n",
	       __func__, info->name, val);
	return ret;
    }

    return 0;
}

static int rk3576_pmu_ungate_clk(struct rk3576_pmu_priv *priv,
		 const struct rk3576_pd_info *info,
		 bool ungate)
{
    u32 val;
    u32 clk_ungate_w_mask;

    if (!info->clk_ungate_mask)
	return 0;

    clk_ungate_w_mask = info->clk_ungate_mask << 16;
    val = ungate ? (info->clk_ungate_mask | clk_ungate_w_mask) : clk_ungate_w_mask;
    writel(val, priv->base + RK3576_PMU_CLK_UNGATE);

    return 0;
}

static int rk3576_pmu_set_power(struct rk3576_pmu_priv *priv,
		const struct rk3576_pd_info *info, bool on)
{
    u32 val;
    bool is_on;
    int ret;

    if (info->pwr_mask == 0)
	return 0;

    /* Write power control - 0 = power on, 1 = power off */
    val = on ? info->pwr_w_mask : (info->pwr_mask | info->pwr_w_mask);
    writel(val, priv->base + RK3576_PMU_PWR_CON + info->pwr_offset);

    /* Wait for power state */
    ret = readl_poll_timeout(priv->base + RK3576_PMU_REPAIR_STATUS,
		 val, !!(val & info->repair_status_mask) == on,
		 DOMAIN_TIMEOUT_US);
    if (ret) {
	is_on = rk3576_pmu_domain_is_on(priv, info);
	pr_err("%s: failed to set %s %s, is_on=%d\n",
	       __func__, info->name, on ? "on" : "off", is_on);
	return ret;
    }

    return 0;
}

static int rk3576_power_domain_on(struct power_domain *pd)
{
    struct rk3576_pmu_priv *priv = dev_get_priv(pd->dev);
    const struct rk3576_pd_info *info;
    int ret;

    if (pd->id >= ARRAY_SIZE(rk3576_pd_info))
	return -EINVAL;

    info = &rk3576_pd_info[pd->id];
    if (!info->name)
	return -EINVAL;

    debug("%s: enabling %s\n", __func__, info->name);

    if (rk3576_pmu_domain_is_on(priv, info))
	return 0;

    rk3576_pmu_ungate_clk(priv, info, true);

    ret = rk3576_pmu_set_power(priv, info, true);
    if (ret)
	goto out;

    ret = rk3576_pmu_set_idle_request(priv, info, false);

out:
    rk3576_pmu_ungate_clk(priv, info, false);
    return ret;
}

static int rk3576_power_domain_off(struct power_domain *pd)
{
    struct rk3576_pmu_priv *priv = dev_get_priv(pd->dev);
    const struct rk3576_pd_info *info;
    int ret;

    if (pd->id >= ARRAY_SIZE(rk3576_pd_info))
	return -EINVAL;

    info = &rk3576_pd_info[pd->id];
    if (!info->name)
	return -EINVAL;

    debug("%s: disabling %s\n", __func__, info->name);

    if (!rk3576_pmu_domain_is_on(priv, info))
	return 0;

    rk3576_pmu_ungate_clk(priv, info, true);

    ret = rk3576_pmu_set_idle_request(priv, info, true);
    if (ret)
	goto out;

    ret = rk3576_pmu_set_power(priv, info, false);

out:
    rk3576_pmu_ungate_clk(priv, info, false);
    return ret;
}

static int rk3576_power_domain_request(struct power_domain *pd)
{
    debug("%s: domain id=%lu\n", __func__, pd->id);
    return 0;
}

static int rk3576_power_domain_rfree(struct power_domain *pd)
{
    debug("%s: domain id=%lu\n", __func__, pd->id);
    return 0;
}

static int rk3576_power_domain_probe(struct udevice *dev)
{
    struct rk3576_pmu_priv *priv = dev_get_priv(dev);

    priv->base = dev_read_addr_ptr(dev->parent);
    if (!priv->base)
	return -EINVAL;

    debug("%s: base=%p\n", __func__, priv->base);
    return 0;
}

static const struct udevice_id rk3576_power_domain_ids[] = {
    { .compatible = "rockchip,rk3576-power-controller" },
    { }
};

struct power_domain_ops rk3576_power_domain_ops = {
    .request = rk3576_power_domain_request,
    .rfree = rk3576_power_domain_rfree,
    .on = rk3576_power_domain_on,
    .off = rk3576_power_domain_off,
};

U_BOOT_DRIVER(rk3576_power_domain) = {
    .name = "rk3576_power_domain",
    .id = UCLASS_POWER_DOMAIN,
    .of_match = rk3576_power_domain_ids,
    .probe = rk3576_power_domain_probe,
    .priv_auto = sizeof(struct rk3576_pmu_priv),
    .ops = &rk3576_power_domain_ops,
};
