// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 Rockchip Electronics Co., Ltd
 */

#define LOG_CATEGORY LOGC_ARCH

#include <dm.h>
#include <misc.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <asm/armv8/mmu.h>
#include <asm/arch-rockchip/bootrom.h>
#include <asm/arch-rockchip/hardware.h>

#define SYS_GRF_BASE		0x2600A000
#define SYS_GRF_SOC_CON2	0x0008
#define  SYS_GRF_PWM2_CH0_IN_PHASE_A_SEL	BIT(12)
#define SYS_GRF_SOC_CON7	0x001c
#define SYS_GRF_SOC_CON11	0x002c
#define  SYS_GRF_USB0_SLV_TIMEOUT_ENA		BIT(15)
#define SYS_GRF_SOC_CON12	0x0030
#define  SYS_GRF_BUS_APB_SLV_TIMEOUT_ENA	BIT(15)
#define  SYS_GRF_BUS_AHB_SLV_TIMEOUT_ENA	BIT(14)
#define  SYS_GRF_VO0_APB_SLV_TIMEOUT_ENA	BIT(13)
#define  SYS_GRF_UFS_APB_SLV_TIMEOUT_ENA	BIT(12)
#define  SYS_GRF_UFS_AXI_SLV_TIMEOUT_ENA	BIT(11)
#define  SYS_GRF_GMAC_AHB_SLV_TIMEOUT_ENA	BIT(10)
#define  SYS_GRF_GMAC_APB_SLV_TIMEOUT_ENA	BIT(9)
#define  SYS_GRF_DSMC_SLV_TIMEOUT_ENA		BIT(8)
#define  SYS_GRF_USB1_SLV_TIMEOUT_ENA		BIT(7)
#define  SYS_GRF_SATA1_SLV_TIMEOUT_ENA		BIT(6)
#define  SYS_GRF_SATA0_SLV_TIMEOUT_ENA		BIT(5)
#define  SYS_GRF_PCIE1_SLV_TIMEOUT_ENA		BIT(4)
#define  SYS_GRF_PCIE1_DBI_TIMEOUT_ENA		BIT(3)
#define  SYS_GRF_PCIE0_SLV_TIMEOUT_ENA		BIT(2)
#define  SYS_GRF_PCIE0_DBI_TIMEOUT_ENA		BIT(1)
#define  SYS_GRF_NVM_SLV_TIMEOUT_ENA		BIT(0)

#define GPIO0_IOC_BASE		0x26040000
#define GPIO0B_PULL_L		0x0024
#define GPIO0B_IE_L		0x002C

#define SYS_SGRF_BASE		0x26004000
#define SYS_SGRF_SOC_CON14	0x0058
#define SYS_SGRF_SOC_CON15	0x005C
#define SYS_SGRF_SOC_CON20	0x0070

#define FW_PMU1SGRF_BASE	0x26003000
#define PMU1SGRF_SLV_LOOKUP0	0x80

#define FW_SYS_SGRF_BASE	0x26005000
#define SGRF_DOMAIN_CON1	0x4
#define SGRF_DOMAIN_CON2	0x8
#define SGRF_DOMAIN_CON3	0xc
#define SGRF_DOMAIN_CON4	0x10
#define SGRF_DOMAIN_CON5	0x14

#define USB_GRF_BASE		0x2601E000
#define USB3OTG0_CON1		0x0030
#define  USB3OTG0_HOST_NUM_U3_PORT_MASK GENMASK(15, 12)
#define  USB3OTG0_HOST_NUM_U3_PORT(n)	(((n) << 12) & USB3OTG0_HOST_NUM_U3_PORT_MASK)
#define  USB3OTG0_HOST_NUM_U2_PORT_MASK	GENMASK(11, 8)
#define  USB3OTG0_HOST_NUM_U2_PORT(n)	(((n) << 8) & USB3OTG0_HOST_NUM_U2_PORT_MASK)
#define  USB3OTG0_PIPE_CLK_SEL		BIT(7)
#define  USB3OTG0_MEM_GATE_EN		BIT(6)
/* Bits 5:4 are for xHCI legacy SMI support (set then clear to signal write) */
#define  USB3OTG0_HOST_LEGACY_SMI_BAR		BIT(5)
#define  USB3OTG0_HOST_LEGACY_SMI_PCI_CMD	BIT(4)
#define  USB3OTG0_PHYSTATUS_CON_MASK	GENMASK(3, 2)
#define  USB3OTG0_PHYSTATUS_FROM_PHY	(0x0 << 2)	/* 0b00 = use phystatus from PHY */
#define  USB3OTG0_PHYSTATUS_SET_0	(0x2 << 2)	/* 0b10 = force phystatus to 0 */
#define  USB3OTG0_PME_EN		BIT(1)
/*
 * Datasheet: "host_u3_port_disable" - should disable U3 port.
 * Original Rockchip code sets HOST_NUM_U3_PORT=0 instead; unclear if this bit works.
 */
#define  USB3OTG0_HOST_U3_PORT_DISABLE	BIT(0)

#define TOP_CRU_BASE		0x27200000
#define TOP_CRU_SOFTRST_CON47	0x0abc
#define  SOFTRST47_ARESETN_USB3OTG0	BIT(5)

enum {
	BROM_BOOTSOURCE_FSPI0 = 3,
	BROM_BOOTSOURCE_FSPI1_M1 = 6,
};

const char * const boot_devices[BROM_LAST_BOOTSOURCE + 1] = {
	[BROM_BOOTSOURCE_EMMC] = "/soc/mmc@2a330000",
	[BROM_BOOTSOURCE_FSPI0] = "/soc/spi@2a340000/flash@0",
	[BROM_BOOTSOURCE_FSPI1_M1] = "/soc/spi@2a300000/flash@0",
	[BROM_BOOTSOURCE_SD] = "/soc/mmc@2a310000",
	[BROM_BOOTSOURCE_UFS] = "/soc/ufshc@2a2d0000",
};

static struct mm_region rk3576_mem_map[] = {
	{
		/* I/O area */
		.virt = 0x20000000UL,
		.phys = 0x20000000UL,
		.size = 0xb080000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* PMU_SRAM, CBUF, SYSTEM_SRAM */
		.virt = 0x3fe70000UL,
		.phys = 0x3fe70000UL,
		.size = 0x190000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* MSCH_DDR_PORT */
		.virt = 0x40000000UL,
		.phys = 0x40000000UL,
		.size = 0x400000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
			 PTE_BLOCK_INNER_SHARE
	}, {
		/* PCIe 0+1 */
		.virt = 0x900000000UL,
		.phys = 0x900000000UL,
		.size = 0x100800000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* List terminator */
		0,
	}
};

struct mm_region *mem_map = rk3576_mem_map;

void board_debug_uart_init(void)
{
}

u32 read_brom_bootsource_id(void)
{
	u32 bootsource_id = readl(BROM_BOOTSOURCE_ID_ADDR);

	/* Re-map the raw value read from reg to a redefined or existing
	 * BROM_BOOTSOURCE enum value to avoid having to create a larger
	 * boot_devices table.
	 */
	if (bootsource_id == 0x23)
		return BROM_BOOTSOURCE_FSPI1_M1;
	else if (bootsource_id == 0x81)
		return BROM_BOOTSOURCE_USB;
	else if (bootsource_id > BROM_LAST_BOOTSOURCE)
		log_debug("Unknown bootsource %x\n", bootsource_id);

	return bootsource_id;
}

#define HP_TIMER_BASE			CONFIG_ROCKCHIP_STIMER_BASE
#define HP_CTRL_REG			0x04
#define TIMER_EN			BIT(0)
#define HP_LOAD_COUNT0_REG		0x14
#define HP_LOAD_COUNT1_REG		0x18

void rockchip_stimer_init(void)
{
	u32 reg;

	if (!IS_ENABLED(CONFIG_XPL_BUILD))
		return;

	reg = readl(HP_TIMER_BASE + HP_CTRL_REG);
	if (reg & TIMER_EN)
		return;

	asm volatile("msr cntfrq_el0, %0" : : "r" (CONFIG_COUNTER_FREQUENCY));
	writel(0xffffffff, HP_TIMER_BASE + HP_LOAD_COUNT0_REG);
	writel(0xffffffff, HP_TIMER_BASE + HP_LOAD_COUNT1_REG);
	writel((TIMER_EN << 16) | TIMER_EN, HP_TIMER_BASE + HP_CTRL_REG);
}

int arch_cpu_init(void)
{
	u32 val;

	if (!IS_ENABLED(CONFIG_SPL_BUILD))
		return 0;

	/* Allow pmu sram access for non-secure masters */
	writel(0xffff3fff, FW_PMU1SGRF_BASE + PMU1SGRF_SLV_LOOKUP0);

	/* Set the emmc to access ddr memory */
	val = readl(FW_SYS_SGRF_BASE + SGRF_DOMAIN_CON2);
	writel(val | 0x7, FW_SYS_SGRF_BASE + SGRF_DOMAIN_CON2);

	/* Set the sdmmc0 to access ddr memory */
	val = readl(FW_SYS_SGRF_BASE + SGRF_DOMAIN_CON5);
	writel(val | 0x700, FW_SYS_SGRF_BASE + SGRF_DOMAIN_CON5);

	/* Set the UFS to access ddr memory */
	val = readl(FW_SYS_SGRF_BASE + SGRF_DOMAIN_CON3);
	writel(val | 0x70000, FW_SYS_SGRF_BASE + SGRF_DOMAIN_CON3);

	/* Set the fspi0 and fspi1 to access ddr memory */
	val = readl(FW_SYS_SGRF_BASE + SGRF_DOMAIN_CON4);
	writel(val | 0x7700, FW_SYS_SGRF_BASE + SGRF_DOMAIN_CON4);

	/* Set the decom to access ddr memory */
	val = readl(FW_SYS_SGRF_BASE + SGRF_DOMAIN_CON1);
	writel(val | 0x700, FW_SYS_SGRF_BASE + SGRF_DOMAIN_CON1);

	/*
	 * Set the GPIO0B0~B3 pull up and input enable.
	 * Keep consistent with other IO.
	 */
	writel(0x00ff00ff, GPIO0_IOC_BASE + GPIO0B_PULL_L);
	writel(0x000f000f, GPIO0_IOC_BASE + GPIO0B_IE_L);

	/*
	 * Set SYS_GRF_SOC_CON2[12](input of pwm2_ch0) as 0,
	 * keep consistent with other pwm.
	 */
	writel(RK_CLRBITS(SYS_GRF_PWM2_CH0_IN_PHASE_A_SEL),
	       SYS_GRF_BASE + SYS_GRF_SOC_CON2);

	/* Enable noc slave response timeout */
	writel(RK_SETBITS(SYS_GRF_USB0_SLV_TIMEOUT_ENA),
	       SYS_GRF_BASE + SYS_GRF_SOC_CON11);
	/* Enable most slave timeouts, except PCIe and NVM */
	writel(RK_CLRSETBITS(SYS_GRF_PCIE1_SLV_TIMEOUT_ENA |
			     SYS_GRF_PCIE1_DBI_TIMEOUT_ENA |
			     SYS_GRF_PCIE0_SLV_TIMEOUT_ENA |
			     SYS_GRF_PCIE0_DBI_TIMEOUT_ENA |
			     SYS_GRF_NVM_SLV_TIMEOUT_ENA,
			     SYS_GRF_BUS_APB_SLV_TIMEOUT_ENA |
			     SYS_GRF_BUS_AHB_SLV_TIMEOUT_ENA |
			     SYS_GRF_VO0_APB_SLV_TIMEOUT_ENA |
			     SYS_GRF_UFS_APB_SLV_TIMEOUT_ENA |
			     SYS_GRF_UFS_AXI_SLV_TIMEOUT_ENA |
			     SYS_GRF_GMAC_AHB_SLV_TIMEOUT_ENA |
			     SYS_GRF_GMAC_APB_SLV_TIMEOUT_ENA |
			     SYS_GRF_DSMC_SLV_TIMEOUT_ENA |
			     SYS_GRF_USB1_SLV_TIMEOUT_ENA |
			     SYS_GRF_SATA1_SLV_TIMEOUT_ENA |
			     SYS_GRF_SATA0_SLV_TIMEOUT_ENA),
	       SYS_GRF_BASE + SYS_GRF_SOC_CON12);

	/*
	 * Enable cci channels for below module AXI R/W
	 * Module: GMAC0/1, MMU0/1(PCIe, SATA, USB3)
	 */
	writel(0xffffff00, SYS_SGRF_BASE + SYS_SGRF_SOC_CON20);

	if (read_brom_bootsource_id() == BROM_BOOTSOURCE_USB) {
		/* Assert USB3OTG0 reset */
		writel(RK_SETBITS(SOFTRST47_ARESETN_USB3OTG0),
		       TOP_CRU_BASE + TOP_CRU_SOFTRST_CON47);
		udelay(1000);
		/* De-assert USB3OTG0 reset */
		writel(RK_CLRBITS(SOFTRST47_ARESETN_USB3OTG0),
		       TOP_CRU_BASE + TOP_CRU_SOFTRST_CON47);
		udelay(1000);
		/* Force phystatus to 0 */
		writel(RK_CLRSETBITS(USB3OTG0_PHYSTATUS_CON_MASK,
				     USB3OTG0_PHYSTATUS_SET_0),
		       USB_GRF_BASE + USB3OTG0_CON1);
	} else {
		/*
		 * Configure USB3OTG0 for U2-only mode (U3 disabled via port count).
		 * USBDP PHY driver will reconfigure when USB3 is needed.
		 */
		/* Configure: 0 U3 ports, 1 U2 port, pipe_clk_sel, force phystatus=0 */
		writel(RK_CLRSETBITS(USB3OTG0_HOST_NUM_U3_PORT_MASK |
					 USB3OTG0_HOST_NUM_U2_PORT_MASK |
					 USB3OTG0_PIPE_CLK_SEL |
				     USB3OTG0_MEM_GATE_EN |
				    //  USB3OTG0_HOST_LEGACY_SMI_BAR |
				    //  USB3OTG0_HOST_LEGACY_SMI_PCI_CMD |
				     USB3OTG0_PHYSTATUS_CON_MASK |
				     USB3OTG0_PME_EN,
				     USB3OTG0_HOST_NUM_U3_PORT(0) |
				     USB3OTG0_HOST_NUM_U2_PORT(1) |
				     USB3OTG0_PIPE_CLK_SEL |
				     USB3OTG0_PHYSTATUS_SET_0 |
					 USB3OTG0_HOST_U3_PORT_DISABLE),
		       USB_GRF_BASE + USB3OTG0_CON1);
	}

	return 0;
}

#define RK3576_OTP_CPU_CODE_OFFSET		0x02
#define RK3576_OTP_SPECIFICATION_OFFSET		0x08

int checkboard(void)
{
	u8 cpu_code[2], specification;
	struct udevice *dev;
	char suffix[2];
	int ret;

	if (!IS_ENABLED(CONFIG_ROCKCHIP_OTP) || !CONFIG_IS_ENABLED(MISC))
		return 0;

	ret = uclass_get_device_by_driver(UCLASS_MISC,
					  DM_DRIVER_GET(rockchip_otp), &dev);
	if (ret) {
		log_debug("Could not find otp device, ret=%d\n", ret);
		return 0;
	}

	/* cpu-code: SoC model, e.g. 0x35 0x76 */
	ret = misc_read(dev, RK3576_OTP_CPU_CODE_OFFSET, cpu_code, 2);
	if (ret < 0) {
		log_debug("Could not read cpu-code, ret=%d\n", ret);
		return 0;
	}

	/* specification: SoC variant, e.g. 0xA for RK3576J */
	ret = misc_read(dev, RK3576_OTP_SPECIFICATION_OFFSET, &specification, 1);
	if (ret < 0) {
		log_debug("Could not read specification, ret=%d\n", ret);
		return 0;
	}
	specification &= 0x1f;

	/* for RK3576J i.e. '@' + 0xA = 'J' */
	suffix[0] = specification > 1 ? '@' + specification : '\0';
	suffix[1] = '\0';

	printf("SoC:   RK%02x%02x%s\n", cpu_code[0], cpu_code[1], suffix);

	return 0;
}
