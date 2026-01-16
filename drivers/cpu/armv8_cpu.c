// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2024 9elements GmbH
 */
#include <cpu.h>
#include <clk.h>
#include <dm.h>
#include <irq.h>
#include <acpi/acpigen.h>
#include <asm/armv8/cpu.h>
#include <asm/io.h>
#include <asm/system.h>
#include <dm/acpi.h>
#include <linux/bitops.h>
#include <linux/printk.h>
#include <linux/sizes.h>

/* MIDR part numbers */
#define MIDR_PARTNUM_CORTEX_A53		0xD03
#define MIDR_PARTNUM_CORTEX_A72		0xD08
#define MIDR_PARTNUM_CORTEX_A55		0xD05
#define MIDR_PARTNUM_CORTEX_A76		0xD0B

struct armv8_cpu_priv {
    struct clk clk;
    bool has_clk;
};

static const char *armv8_cpu_get_name(void)
{
    u32 partnum = (read_midr() & MIDR_PARTNUM_MASK) >> MIDR_PARTNUM_SHIFT;

    switch (partnum) {
    case MIDR_PARTNUM_CORTEX_A53:
		return "Cortex-A53";
    case MIDR_PARTNUM_CORTEX_A72:
		return "Cortex-A72";
    case MIDR_PARTNUM_CORTEX_A55:
		return "Cortex-A55";
    case MIDR_PARTNUM_CORTEX_A76:
		return "Cortex-A76";
    default:
		return "Unknown";
    }
}

static int armv8_cpu_get_desc(const struct udevice *dev, char *buf, int size)
{
    u32 partnum = (read_midr() & MIDR_PARTNUM_MASK) >> MIDR_PARTNUM_SHIFT;

    snprintf(buf, size, "ARM %s (MIDR %04x)", armv8_cpu_get_name(), partnum);

    return 0;
}

static int armv8_cpu_get_info(const struct udevice *dev, struct cpu_info *info)
{
    struct armv8_cpu_priv *priv = dev_get_priv(dev);

    info->cpu_freq = 0;
    info->features = BIT(CPU_FEAT_L1_CACHE) | BIT(CPU_FEAT_MMU);

    if (priv && priv->has_clk)
		info->cpu_freq = clk_get_rate(&priv->clk);

    return 0;
}

static int armv8_cpu_get_count(const struct udevice *dev)
{
    return uclass_id_count(UCLASS_CPU);
}

static int armv8_cpu_get_vendor(const struct udevice *dev, char *buf, int size)
{
    snprintf(buf, size, "ARM");
    return 0;
}

static int armv8_cpu_is_current(struct udevice *dev)
{
    u64 mpidr = read_mpidr();
    u64 cpu_reg;

    /* Get reg property (MPIDR value for this CPU) */
    cpu_reg = dev_read_u64_default(dev, "reg", 0);
    if (!cpu_reg)
		cpu_reg = dev_read_u32_default(dev, "reg", 0);

    /* Compare affinity fields */
    return (mpidr & 0xFFFFFF) == (cpu_reg & 0xFFFFFF);
}

static int armv8_cpu_probe(struct udevice *dev)
{
    struct armv8_cpu_priv *priv = dev_get_priv(dev);
    int ret;

    priv->has_clk = false;

    ret = clk_get_by_index(dev, 0, &priv->clk);
    if (!ret)
		priv->has_clk = true;

    return 0;
}

#ifdef CONFIG_ACPIGEN
int armv8_cpu_fill_ssdt(const struct udevice *dev, struct acpi_ctx *ctx)
{
	uint core_id = dev_seq(dev);

	acpigen_write_processor_device(ctx, core_id);

	return 0;
}

int armv8_cpu_fill_madt(const struct udevice *dev, struct acpi_ctx *ctx)
{
	struct acpi_madt_gicc *gicc;
	struct cpu_plat *cpu_plat;
	struct udevice *gic;
	u64 gicc_gicv = 0;
	u64 gicc_gich = 0;
	u64 gicc_gicr_base = 0;
	u64 gicc_phys_base = 0;
	u32 gicc_perf_gsiv = 0;
	u64 gicc_mpidr;
	u32 gicc_vgic_maint_irq = 0;
	int addr_index;
	fdt_addr_t addr;
	int ret;
	struct irq req_irq;

	cpu_plat = dev_get_parent_plat(dev);
	if (!cpu_plat)
		return 0;

	ret = irq_get_interrupt_parent(dev, &gic);
	if (ret) {
		log_err("%s: Failed to find interrupt parent for %s\n",
			__func__, dev->name);
		return -ENODEV;
	}

	addr_index = 1;

	if (device_is_compatible(gic, "arm,gic-v3")) {
		addr = dev_read_addr_index(gic, addr_index++);
		if (addr != FDT_ADDR_T_NONE)
			gicc_gicr_base = addr;
	}

	addr = dev_read_addr_index(gic, addr_index++);
	if (addr != FDT_ADDR_T_NONE)
		gicc_phys_base = addr;

	addr = dev_read_addr_index(gic, addr_index++);
	if (addr != FDT_ADDR_T_NONE)
		gicc_gich = addr;

	addr = dev_read_addr_index(gic, addr_index++);
	if (addr != FDT_ADDR_T_NONE)
		gicc_gicv = addr;

	ret = irq_get_by_index(gic, 0, &req_irq);
	if (!ret)
		gicc_vgic_maint_irq = req_irq.id;

	gicc_mpidr = dev_read_u64_default(dev, "reg", 0);
	if (!gicc_mpidr)
		gicc_mpidr = dev_read_u32_default(dev, "reg", 0);

	/*
	 * gicc_vgic_maint_irq and gicc_gicv are the same for every CPU
	 */
	gicc = ctx->current;
	acpi_write_madt_gicc(gicc,
			     dev_seq(dev),
			     gicc_perf_gsiv, /* FIXME: needs a PMU driver */
			     gicc_phys_base,
			     gicc_gicv,
			     gicc_gich,
			     gicc_vgic_maint_irq,
			     gicc_gicr_base,
			     gicc_mpidr,
			     0); /* FIXME: Not defined in DT */

	acpi_inc(ctx, gicc->length);

	return 0;
}

struct acpi_ops armv8_cpu_acpi_ops = {
	.fill_ssdt	= armv8_cpu_fill_ssdt,
	.fill_madt	= armv8_cpu_fill_madt,
};
#endif

static const struct cpu_ops armv8_cpu_ops = {
    .get_count	= armv8_cpu_get_count,
    .get_desc	= armv8_cpu_get_desc,
    .get_info	= armv8_cpu_get_info,
    .get_vendor	= armv8_cpu_get_vendor,
    .is_current	= armv8_cpu_is_current,
};

static const struct udevice_id armv8_cpu_ids[] = {
    { .compatible = "arm,cortex-a53" },
    { .compatible = "arm,cortex-a55" },
    { .compatible = "arm,cortex-a72" },
    { .compatible = "arm,cortex-a73" },
    { .compatible = "arm,cortex-a75" },
    { .compatible = "arm,cortex-a76" },
    { .compatible = "arm,armv8" },
    {}
};

U_BOOT_DRIVER(armv8_cpu) = {
    .name		= "armv8-cpu",
    .id		= UCLASS_CPU,
    .of_match	= armv8_cpu_ids,
    .ops		= &armv8_cpu_ops,
    .probe		= armv8_cpu_probe,
    .priv_auto	= sizeof(struct armv8_cpu_priv),
    .flags		= DM_FLAG_PRE_RELOC,
    ACPI_OPS_PTR(&armv8_cpu_acpi_ops)
};
