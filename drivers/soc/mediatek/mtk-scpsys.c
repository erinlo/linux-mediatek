/*
 * Copyright (c) 2015 Pengutronix, Sascha Hauer <kernel@pengutronix.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/clk.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/regulator/consumer.h>
#include <linux/soc/mediatek/infracfg.h>

#include "mtk-scpsys.h"

#define SPM_PWR_STATUS			0x060c
#define SPM_PWR_STATUS_2ND		0x0610

#define PWR_RST_B_BIT			BIT(0)
#define PWR_ISO_BIT			BIT(1)
#define PWR_ON_BIT			BIT(2)
#define PWR_ON_2ND_BIT			BIT(3)
#define PWR_CLK_DIS_BIT			BIT(4)

static int scpsys_domain_is_on(struct scp_domain *scpd)
{
	struct scp *scp = scpd->scp;

	u32 status = readl(scp->base + SPM_PWR_STATUS) & scpd->sta_mask;
	u32 status2 = readl(scp->base + SPM_PWR_STATUS_2ND) & scpd->sta_mask;

	/*
	 * A domain is on when both status bits are set. If only one is set
	 * return an error. This happens while powering up a domain
	 */

	if (status && status2)
		return true;
	if (!status && !status2)
		return false;

	return -EINVAL;
}

static int scpsys_power_on(struct generic_pm_domain *genpd)
{
	struct scp_domain *scpd = container_of(genpd, struct scp_domain, genpd);
	struct scp *scp = scpd->scp;
	unsigned long timeout;
	bool expired;
	void __iomem *ctl_addr = scpd->ctl_addr;
	u32 sram_pdn_ack = scpd->sram_pdn_ack_bits;
	u32 val;
	int ret;
	int i;

	if (scpd->supply) {
		ret = regulator_enable(scpd->supply);
		if (ret)
			return ret;
	}

	for (i = 0; i < MAX_CLKS && scpd->clk[i]; i++) {
		ret = clk_prepare_enable(scpd->clk[i]);
		if (ret) {
			for (--i; i >= 0; i--)
				clk_disable_unprepare(scpd->clk[i]);

			goto err_clk;
		}
	}

	val = readl(ctl_addr);
	val |= PWR_ON_BIT;
	writel(val, ctl_addr);
	val |= PWR_ON_2ND_BIT;
	writel(val, ctl_addr);

	/* wait until PWR_ACK = 1 */
	timeout = jiffies + HZ;
	expired = false;
	while (1) {
		ret = scpsys_domain_is_on(scpd);
		if (ret > 0)
			break;

		if (expired) {
			ret = -ETIMEDOUT;
			goto err_pwr_ack;
		}

		cpu_relax();

		if (time_after(jiffies, timeout))
			expired = true;
	}

	val &= ~PWR_CLK_DIS_BIT;
	writel(val, ctl_addr);

	val &= ~PWR_ISO_BIT;
	writel(val, ctl_addr);

	val |= PWR_RST_B_BIT;
	writel(val, ctl_addr);

	val &= ~scpd->sram_pdn_bits;
	writel(val, ctl_addr);

	/* wait until SRAM_PDN_ACK all 0 */
	timeout = jiffies + HZ;
	expired = false;
	while (sram_pdn_ack && (readl(ctl_addr) & sram_pdn_ack)) {

		if (expired) {
			ret = -ETIMEDOUT;
			goto err_pwr_ack;
		}

		cpu_relax();

		if (time_after(jiffies, timeout))
			expired = true;
	}

	if (scpd->bus_prot_mask) {
		ret = mtk_infracfg_clear_bus_protection(scp->infracfg,
				scpd->bus_prot_mask);
		if (ret)
			goto err_pwr_ack;
	}

	return 0;

err_pwr_ack:
	for (i = MAX_CLKS - 1; i >= 0; i--) {
		if (scpd->clk[i])
			clk_disable_unprepare(scpd->clk[i]);
	}
err_clk:
	if (scpd->supply)
		regulator_disable(scpd->supply);

	dev_err(scp->dev, "Failed to power on domain %s\n", genpd->name);

	return ret;
}

static int scpsys_power_off(struct generic_pm_domain *genpd)
{
	struct scp_domain *scpd = container_of(genpd, struct scp_domain, genpd);
	struct scp *scp = scpd->scp;
	unsigned long timeout;
	bool expired;
	void __iomem *ctl_addr = scpd->ctl_addr;
	u32 pdn_ack = scpd->sram_pdn_ack_bits;
	u32 val;
	int ret;
	int i;

	if (scpd->bus_prot_mask) {
		ret = mtk_infracfg_set_bus_protection(scp->infracfg,
				scpd->bus_prot_mask);
		if (ret)
			goto out;
	}

	val = readl(ctl_addr);
	val |= scpd->sram_pdn_bits;
	writel(val, ctl_addr);

	/* wait until SRAM_PDN_ACK all 1 */
	timeout = jiffies + HZ;
	expired = false;
	while (pdn_ack && (readl(ctl_addr) & pdn_ack) != pdn_ack) {
		if (expired) {
			ret = -ETIMEDOUT;
			goto out;
		}

		cpu_relax();

		if (time_after(jiffies, timeout))
			expired = true;
	}

	val |= PWR_ISO_BIT;
	writel(val, ctl_addr);

	val &= ~PWR_RST_B_BIT;
	writel(val, ctl_addr);

	val |= PWR_CLK_DIS_BIT;
	writel(val, ctl_addr);

	val &= ~PWR_ON_BIT;
	writel(val, ctl_addr);

	val &= ~PWR_ON_2ND_BIT;
	writel(val, ctl_addr);

	/* wait until PWR_ACK = 0 */
	timeout = jiffies + HZ;
	expired = false;
	while (1) {
		ret = scpsys_domain_is_on(scpd);
		if (ret == 0)
			break;

		if (expired) {
			ret = -ETIMEDOUT;
			goto out;
		}

		cpu_relax();

		if (time_after(jiffies, timeout))
			expired = true;
	}

	for (i = 0; i < MAX_CLKS && scpd->clk[i]; i++)
		clk_disable_unprepare(scpd->clk[i]);

	if (scpd->supply)
		regulator_disable(scpd->supply);

	return 0;

out:
	dev_err(scp->dev, "Failed to power off domain %s\n", genpd->name);

	return ret;
}

static bool scpsys_active_wakeup(struct device *dev)
{
	struct generic_pm_domain *genpd;
	struct scp_domain *scpd;

	genpd = pd_to_genpd(dev->pm_domain);
	scpd = container_of(genpd, struct scp_domain, genpd);

	return scpd->active_wakeup;
}

static void init_clks(struct platform_device *pdev, struct clk *clk[CLK_MAX])
{
	enum clk_id clk_ids[] = {
		CLK_MM,
		CLK_MFG,
		CLK_VENC,
		CLK_VENC_LT
	};

	static const char * const clk_names[] = {
		"mm",
		"mfg",
		"venc",
		"venc_lt",
	};

	int i;

	for (i = 0; i < ARRAY_SIZE(clk_ids); i++)
		clk[clk_ids[i]] = devm_clk_get(&pdev->dev, clk_names[i]);
}

struct scp *init_scp(struct platform_device *pdev,
			const struct scp_domain_data *scp_domain_data, int num)
{
	struct genpd_onecell_data *pd_data;
	struct resource *res;
	int i, j;
	struct scp *scp;
	struct clk *clk[CLK_MAX];

	scp = devm_kzalloc(&pdev->dev, sizeof(*scp), GFP_KERNEL);
	if (!scp)
		return ERR_PTR(-ENOMEM);

	scp->dev = &pdev->dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	scp->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(scp->base))
		return ERR_CAST(scp->base);

	scp->infracfg = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
			"infracfg");
	if (IS_ERR(scp->infracfg)) {
		dev_err(&pdev->dev, "Cannot find infracfg controller: %ld\n",
				PTR_ERR(scp->infracfg));
		return ERR_CAST(scp->infracfg);
	}

	scp->domains = devm_kzalloc(&pdev->dev,
				sizeof(*scp->domains) * num, GFP_KERNEL);
	if (!scp->domains)
		return ERR_PTR(-ENOMEM);

	pd_data = &scp->pd_data;

	pd_data->domains = devm_kzalloc(&pdev->dev,
			sizeof(*pd_data->domains) * num, GFP_KERNEL);
	if (!pd_data->domains)
		return ERR_PTR(-ENOMEM);

	for (i = 0; i < num; i++) {
		struct scp_domain *scpd = &scp->domains[i];
		const struct scp_domain_data *data = &scp_domain_data[i];

		scpd->supply = devm_regulator_get_optional(&pdev->dev, data->name);
		if (IS_ERR(scpd->supply)) {
			if (PTR_ERR(scpd->supply) == -ENODEV)
				scpd->supply = NULL;
			else
				return ERR_CAST(scpd->supply);
		}
	}

	pd_data->num_domains = num;

	init_clks(pdev, clk);

	for (i = 0; i < num; i++) {
		struct scp_domain *scpd = &scp->domains[i];
		struct generic_pm_domain *genpd = &scpd->genpd;
		const struct scp_domain_data *data = &scp_domain_data[i];

		for (j = 0; j < MAX_CLKS && data->clk_id[j]; j++) {
			struct clk *c = clk[data->clk_id[j]];

			if (IS_ERR(c)) {
				dev_err(&pdev->dev, "%s: clk unavailable\n",
					data->name);
				return ERR_CAST(c);
			}

			scpd->clk[j] = c;
		}

		pd_data->domains[i] = genpd;
		scpd->scp = scp;

		scpd->sta_mask = data->sta_mask;
		scpd->ctl_addr = scp->base + data->ctl_offs;
		scpd->sram_pdn_bits = data->sram_pdn_bits;
		scpd->sram_pdn_ack_bits = data->sram_pdn_ack_bits;
		scpd->bus_prot_mask = data->bus_prot_mask;
		scpd->active_wakeup = data->active_wakeup;

		genpd->name = data->name;
		genpd->power_off = scpsys_power_off;
		genpd->power_on = scpsys_power_on;
		genpd->dev_ops.active_wakeup = scpsys_active_wakeup;
	}

	return scp;
}

void mtk_register_power_domains(struct platform_device *pdev,
				struct scp *scp, int num)
{
	struct genpd_onecell_data *pd_data;
	int i, ret;

	for (i = 0; i < num; i++) {
		struct scp_domain *scpd = &scp->domains[i];
		struct generic_pm_domain *genpd = &scpd->genpd;

		/*
		 * Initially turn on all domains to make the domains usable
		 * with !CONFIG_PM and to get the hardware in sync with the
		 * software.  The unused domains will be switched off during
		 * late_init time.
		 */
		genpd->power_on(genpd);

		pm_genpd_init(genpd, NULL, false);
	}

	/*
	 * We are not allowed to fail here since there is no way to unregister
	 * a power domain. Once registered above we have to keep the domains
	 * valid.
	 */

	pd_data = &scp->pd_data;

	ret = of_genpd_add_provider_onecell(pdev->dev.of_node, pd_data);
	if (ret)
		dev_err(&pdev->dev, "Failed to add OF provider: %d\n", ret);
}

/*
 * MT2701 power domain support
 */

#include <dt-bindings/power/mt2701-power.h>

static const struct scp_domain_data scp_domain_data_mt2701[] __initconst = {
	[MT2701_POWER_DOMAIN_CONN] = {
		.name = "conn",
		.sta_mask = BIT(1),
		.ctl_offs = 0x0280,
		.bus_prot_mask = 0x0104,
		.active_wakeup = true,
	},
	[MT2701_POWER_DOMAIN_DISP] = {
		.name = "disp",
		.sta_mask = BIT(3),
		.ctl_offs = 0x023c,
		.sram_pdn_bits = GENMASK(11, 8),
		.clk_id = {CLK_MM},
		.bus_prot_mask = 0x0002,
		.active_wakeup = true,
	},
	[MT2701_POWER_DOMAIN_MFG] = {
		.name = "mfg",
		.sta_mask = BIT(4),
		.ctl_offs = 0x0214,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.active_wakeup = true,
	},
	[MT2701_POWER_DOMAIN_VDEC] = {
		.name = "vdec",
		.sta_mask = BIT(7),
		.ctl_offs = 0x0210,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.clk_id = {CLK_MM},
		.active_wakeup = true,
	},
	[MT2701_POWER_DOMAIN_ISP] = {
		.name = "isp",
		.sta_mask = BIT(5),
		.ctl_offs = 0x0238,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(13, 12),
		.active_wakeup = true,
	},
	[MT2701_POWER_DOMAIN_BDP] = {
		.name = "bdp",
		.sta_mask = BIT(14),
		.ctl_offs = 0x029c,
		.sram_pdn_bits = GENMASK(11, 8),
		.active_wakeup = true,
	},
	[MT2701_POWER_DOMAIN_ETH] = {
		.name = "eth",
		.sta_mask = BIT(15),
		.ctl_offs = 0x02a0,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.active_wakeup = true,
	},
	[MT2701_POWER_DOMAIN_HIF] = {
		.name = "hif",
		.sta_mask = BIT(16),
		.ctl_offs = 0x02a4,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.active_wakeup = true,
	},
	[MT2701_POWER_DOMAIN_IFR_MSC] = {
		.name = "ifr_msc",
		.sta_mask = BIT(17),
		.ctl_offs = 0x02a8,
		.active_wakeup = true,
	},
};

#define NUM_DOMAINS_MT2701	ARRAY_SIZE(scp_domain_data_mt2701)

static int __init scpsys_probe_mt2701(struct platform_device *pdev)
{
	struct scp *scp;

	scp = init_scp(pdev, scp_domain_data_mt2701, NUM_DOMAINS_MT2701);
	if (IS_ERR(scp))
		return PTR_ERR(scp);

	mtk_register_power_domains(pdev, scp, NUM_DOMAINS_MT2701);

	return 0;
}

/*
 * MT8173 power domain support
 */

#include <dt-bindings/power/mt8173-power.h>

static const struct scp_domain_data scp_domain_data_mt8173[] __initconst = {
	[MT8173_POWER_DOMAIN_VDEC] = {
		.name = "vdec",
		.sta_mask = BIT(7),
		.ctl_offs = 0x0210,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.clk_id = {CLK_MM},
	},
	[MT8173_POWER_DOMAIN_VENC] = {
		.name = "venc",
		.sta_mask = BIT(21),
		.ctl_offs = 0x0230,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_MM, CLK_VENC},
	},
	[MT8173_POWER_DOMAIN_ISP] = {
		.name = "isp",
		.sta_mask = BIT(5),
		.ctl_offs = 0x0238,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(13, 12),
		.clk_id = {CLK_MM},
	},
	[MT8173_POWER_DOMAIN_MM] = {
		.name = "mm",
		.sta_mask = BIT(3),
		.ctl_offs = 0x023c,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(12, 12),
		.clk_id = {CLK_MM},
		.bus_prot_mask = MT8173_TOP_AXI_PROT_EN_MM_M0 |
			MT8173_TOP_AXI_PROT_EN_MM_M1,
	},
	[MT8173_POWER_DOMAIN_VENC_LT] = {
		.name = "venc_lt",
		.sta_mask = BIT(20),
		.ctl_offs = 0x0298,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_MM, CLK_VENC_LT},
	},
	[MT8173_POWER_DOMAIN_AUDIO] = {
		.name = "audio",
		.sta_mask = BIT(24),
		.ctl_offs = 0x029c,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_NONE},
	},
	[MT8173_POWER_DOMAIN_USB] = {
		.name = "usb",
		.sta_mask = BIT(25),
		.ctl_offs = 0x02cc,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.clk_id = {CLK_NONE},
		.active_wakeup = true,
	},
	[MT8173_POWER_DOMAIN_MFG_ASYNC] = {
		.name = "mfg_async",
		.sta_mask = BIT(23),
		.ctl_offs = 0x02c4,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = 0,
		.clk_id = {CLK_MFG},
	},
	[MT8173_POWER_DOMAIN_MFG_2D] = {
		.name = "mfg_2d",
		.sta_mask = BIT(22),
		.ctl_offs = 0x02c0,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(13, 12),
		.clk_id = {CLK_NONE},
	},
	[MT8173_POWER_DOMAIN_MFG] = {
		.name = "mfg",
		.sta_mask = BIT(4),
		.ctl_offs = 0x0214,
		.sram_pdn_bits = GENMASK(13, 8),
		.sram_pdn_ack_bits = GENMASK(21, 16),
		.clk_id = {CLK_NONE},
		.bus_prot_mask = MT8173_TOP_AXI_PROT_EN_MFG_S |
			MT8173_TOP_AXI_PROT_EN_MFG_M0 |
			MT8173_TOP_AXI_PROT_EN_MFG_M1 |
			MT8173_TOP_AXI_PROT_EN_MFG_SNOOP_OUT,
	},
};

#define NUM_DOMAINS_MT8173	ARRAY_SIZE(scp_domain_data_mt8173)

static int __init scpsys_probe_mt8173(struct platform_device *pdev)
{
	struct scp *scp;
	struct genpd_onecell_data *pd_data;
	int ret;

	scp = init_scp(pdev, scp_domain_data_mt8173, NUM_DOMAINS_MT8173);
	if (IS_ERR(scp))
		return PTR_ERR(scp);

	mtk_register_power_domains(pdev, scp, NUM_DOMAINS_MT8173);

	pd_data = &scp->pd_data;

	ret = pm_genpd_add_subdomain(pd_data->domains[MT8173_POWER_DOMAIN_MFG_ASYNC],
		pd_data->domains[MT8173_POWER_DOMAIN_MFG_2D]);
	if (ret && IS_ENABLED(CONFIG_PM))
		dev_err(&pdev->dev, "Failed to add subdomain: %d\n", ret);

	ret = pm_genpd_add_subdomain(pd_data->domains[MT8173_POWER_DOMAIN_MFG_2D],
		pd_data->domains[MT8173_POWER_DOMAIN_MFG]);
	if (ret && IS_ENABLED(CONFIG_PM))
		dev_err(&pdev->dev, "Failed to add subdomain: %d\n", ret);

	return 0;
}

/*
 * scpsys driver init
 */

static const struct of_device_id of_scpsys_match_tbl[] = {
	{
		.compatible = "mediatek,mt2701-scpsys",
		.data = scpsys_probe_mt2701,
	}, {
		.compatible = "mediatek,mt8173-scpsys",
		.data = scpsys_probe_mt8173,
	}, {
		/* sentinel */
	}
};

static int __init scpsys_probe(struct platform_device *pdev)
{
	int (*probe)(struct platform_device *);
	const struct of_device_id *of_id;

	of_id = of_match_node(of_scpsys_match_tbl, pdev->dev.of_node);
	if (!of_id || !of_id->data)
		return -EINVAL;

	probe = of_id->data;

	return probe(pdev);
}

static struct platform_driver scpsys_drv = {
	.driver = {
		.name = "mtk-scpsys",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(of_scpsys_match_tbl),
	},
};

static int __init scpsys_drv_init(void)
{
	return platform_driver_probe(&scpsys_drv, scpsys_probe);
}

/*
 * There are some Mediatek drivers which depend on the power domain driver need
 * to probe in earlier initcall levels. So scpsys driver also need to probe
 * earlier.
 *
 * IOMMU(M4U) and SMI drivers for example. SMI is a bridge between IOMMU and
 * multimedia HW. IOMMU depends on SMI, and SMI is a power domain consumer,
 * so the proper probe sequence should be scpsys -> SMI -> IOMMU driver.
 * IOMMU drivers are initialized during subsys_init by default, so we need to
 * move SMI and scpsys drivers to subsys_init or earlier init levels.
 */
subsys_initcall(scpsys_drv_init);
