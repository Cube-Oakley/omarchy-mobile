// SPDX-License-Identifier: GPL-2.0-only
/* Development diagnostic: own the bus clocks/power before reading retained
 * display state. No panel commands, reset, plane programming or IRQ handling.
 * Load manually in a timed RAM probe; do not use as a display driver.
 */
#include <linux/clk.h>
#include <linux/interconnect.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

struct op13_probe {
	void __iomem *regs;
	struct clk_bulk_data *clocks;
	int num_clocks;
	struct icc_path *bus;
};

static void power_put(void *data)
{
	pm_runtime_put_sync(data);
}

static void clocks_put(void *data)
{
	struct op13_probe *p = data;
	clk_bulk_disable_unprepare(p->num_clocks, p->clocks);
}

static void bus_put(void *data)
{
	icc_set_bw(data, 0, 0);
}

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct op13_probe *p = dev_get_drvdata(dev);
	static const struct { const char *name; u32 base; } pipes[] = {
		{ "vig0", 0x5000 }, { "vig1", 0x7000 },
		{ "dma0", 0x25000 }, { "dma1", 0x27000 },
	};
	int i, len = 0;

	/* All offsets are relative to the stock MDSS base, not upstream MDP. */
	dev_info(dev, "reading powered MDSS version\n");
	len += sysfs_emit_at(buf, len, "version=%08x\n", readl(p->regs));
	for (i = 0; i < ARRAY_SIZE(pipes); i++) {
		void __iomem *base = p->regs + pipes[i].base;
		dev_info(dev, "reading %s\n", pipes[i].name);
		len += sysfs_emit_at(buf, len,
			"%s size=%08x addr=%08x stride=%08x format=%08x op=%08x\n",
			pipes[i].name, readl(base), readl(base + 0x14),
			readl(base + 0x24), readl(base + 0x30), readl(base + 0x38));
	}
	for (i = 0; i < 4; i++) {
		void __iomem *base = p->regs + 0x6a400 + i * 0x1000;
		dev_info(dev, "reading SPR%d\n", i);
		len += sysfs_emit_at(buf, len, "spr%d version=%08x config=%08x mixers=%08x\n",
			i, readl(base), readl(base + 4), readl(base + 8));
	}
	return len;
}
static DEVICE_ATTR_RO(state);
static struct attribute *op13_attrs[] = { &dev_attr_state.attr, NULL };
static const struct attribute_group op13_group = { .attrs = op13_attrs };

static int op13_display_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct op13_probe *p;
	struct resource *res;
	int ret;

	if (!of_machine_is_compatible("oneplus,cph2655-omarchy-probe"))
		return -ENODEV;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || res->start != 0x0ae00000 || resource_size(res) != 0x93800)
		return -EINVAL;
	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(p->regs))
		return PTR_ERR(p->regs);
	p->num_clocks = devm_clk_bulk_get_all(dev, &p->clocks);
	if (p->num_clocks < 0)
		return dev_err_probe(dev, p->num_clocks, "clocks unavailable\n");
	if (p->num_clocks != 3)
		return -EINVAL;
	p->bus = devm_of_icc_get(dev, "cpu-cfg");
	if (IS_ERR(p->bus))
		return dev_err_probe(dev, PTR_ERR(p->bus), "register bus unavailable\n");
	if (!p->bus)
		return -ENODEV;
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return dev_err_probe(dev, ret, "display power unavailable\n");
	ret = devm_add_action_or_reset(dev, power_put, dev);
	if (ret)
		return ret;
	ret = icc_set_bw(p->bus, 0, 57000); /* upstream SM8750 register bus vote */
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, bus_put, p->bus);
	if (ret)
		return ret;
	ret = clk_bulk_prepare_enable(p->num_clocks, p->clocks);
	if (ret)
		return dev_err_probe(dev, ret, "display clocks unavailable\n");
	ret = devm_add_action_or_reset(dev, clocks_put, p);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, p);
	dev_info(dev, "clock/power references held; read state manually\n");
	return devm_device_add_group(dev, &op13_group);
}

static const struct of_device_id op13_match[] = {
	{ .compatible = "omarchy,oneplus13-display-probe" }, {}
};
MODULE_DEVICE_TABLE(of, op13_match);
static struct platform_driver op13_driver = {
	.probe = op13_display_probe,
	.driver = { .name = "oneplus13-display-probe", .of_match_table = op13_match,
		.suppress_bind_attrs = true },
};
module_platform_driver(op13_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("OnePlus 13 powered display state diagnostic");
