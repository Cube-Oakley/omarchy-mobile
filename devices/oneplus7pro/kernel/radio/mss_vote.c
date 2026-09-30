// SPDX-License-Identifier: GPL-2.0-only
/*
 * Set the application processor's active-only RPMh vote on the modem rail
 * (cmd-db "mss.lvl") at runtime, and keep it across system suspend.
 *
 * Until rpmhpd's sync_state runs, rpmhpd clamps every domain to its top
 * corner, so after the modem's proxy vote is dropped at handover the AP still
 * votes MSS at the top corner for the whole boot; stock drops its vote. Held
 * there, QLink cannot restart after the modem sleeps and the modem asserts.
 * Writing /sys/module/mss_vote/parameters/level sends one synchronous
 * active-only vote of that corner index (0..9) through the rpmhpd device;
 * unloading restores the top corner, which is what rpmhpd last sent.
 *
 * rpmhpd sends its clamped vote again whenever genpd powers the MSS domain on
 * or off: at a modem stop or start, and on every system resume, when genpd
 * powers the domain on in resume_noirq and off again once the transition
 * completes. So the module puts a device of its own in the MSS domain and,
 * after each of those, sends the chosen corner again (a few microseconds of
 * the top corner rather than all of the next boot).
 *
 * docs/cellular-sim-20260926.md, docs/sleep-20260927.md
 */
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/slab.h>
#include <soc/qcom/cmd-db.h>
#include <soc/qcom/rpmh.h>
#include <soc/qcom/tcs.h>

#define MSS_TOP_CORNER 9

static struct platform_device *rpmhpd_pdev;
static u32 mss_addr;
static int level = -1;
static unsigned int reasserted;
static struct device *holder;	/* named after us, the modem's power domains */
static struct device *mss_dev;	/* our member of the MSS domain */

static int mss_send(u32 corner)
{
	struct tcs_cmd cmd = { .addr = mss_addr, .data = corner };

	return rpmh_write(&rpmhpd_pdev->dev, RPMH_ACTIVE_ONLY_STATE, &cmd, 1);
}

static int level_set(const char *val, const struct kernel_param *kp)
{
	int corner, ret;

	ret = kstrtoint(val, 0, &corner);
	if (ret)
		return ret;
	if (corner < 0 || corner > MSS_TOP_CORNER)
		return -EINVAL;
	if (!rpmhpd_pdev)
		return -ENODEV;

	ret = mss_send(corner);
	pr_info("mss_vote: mss.lvl (0x%x) active corner %d: %d\n", mss_addr, corner, ret);
	if (!ret)
		WRITE_ONCE(level, corner);
	return ret;
}

static const struct kernel_param_ops level_ops = {
	.set = level_set,
	.get = param_get_int,
};
module_param_cb(level, &level_ops, &level, 0600);
MODULE_PARM_DESC(level, "AP active-only corner index for mss.lvl (write to send)");
module_param(reasserted, uint, 0400);
MODULE_PARM_DESC(reasserted, "Times the corner was sent again after rpmhpd re-voted");

/* Runs under the MSS genpd lock, right after rpmhpd's own vote. */
static int mss_power_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	int corner = READ_ONCE(level);
	int ret;

	if (action != GENPD_NOTIFY_ON && action != GENPD_NOTIFY_OFF)
		return NOTIFY_DONE;
	if (corner < 0 || corner == MSS_TOP_CORNER)
		return NOTIFY_DONE;

	ret = mss_send(corner);
	reasserted++;
	pr_info_ratelimited("mss_vote: domain %s, corner %d again: %d\n",
			    action == GENPD_NOTIFY_ON ? "on" : "off", corner, ret);
	return NOTIFY_OK;
}

static struct notifier_block mss_nb = {
	.notifier_call = mss_power_notify,
};

static void holder_release(struct device *dev)
{
	kfree(dev);
}

static void mss_domain_leave(void)
{
	if (mss_dev) {
		dev_pm_genpd_remove_notifier(mss_dev);
		dev_pm_domain_detach(mss_dev, false);
		mss_dev = NULL;
	}
	if (holder) {
		of_node_put(holder->of_node);
		put_device(holder);
		holder = NULL;
	}
}

static int mss_domain_join(void)
{
	struct device_node *modem;
	struct device *dev;
	int ret;

	modem = of_find_compatible_node(NULL, NULL, "qcom,sm8150-mpss-pas");
	if (!modem)
		return -ENODEV;

	holder = kzalloc(sizeof(*holder), GFP_KERNEL);
	if (!holder) {
		of_node_put(modem);
		return -ENOMEM;
	}
	device_initialize(holder);
	holder->release = holder_release;
	holder->of_node = modem;
	ret = dev_set_name(holder, "mss_vote");
	if (ret)
		goto fail;

	/* A virtual genpd device, genpd:1:mss_vote; never powers the domain on. */
	dev = dev_pm_domain_attach_by_name(holder, "mss");
	if (IS_ERR_OR_NULL(dev)) {
		ret = dev ? PTR_ERR(dev) : -ENODEV;
		goto fail;
	}
	mss_dev = dev;

	ret = dev_pm_genpd_add_notifier(mss_dev, &mss_nb);
	if (ret)
		goto fail;
	return 0;

fail:
	mss_domain_leave();
	return ret;
}

static int __init mss_vote_init(void)
{
	struct device_node *np;
	int ret;

	if (!of_machine_is_compatible("oneplus,guacamole"))
		return -ENODEV;

	mss_addr = cmd_db_read_addr("mss.lvl");
	if (!mss_addr)
		return -ENODEV;

	np = of_find_compatible_node(NULL, NULL, "qcom,sm8150-rpmhpd");
	if (!np)
		return -ENODEV;
	rpmhpd_pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!rpmhpd_pdev)
		return -ENODEV;
	if (!rpmhpd_pdev->dev.parent || !dev_get_drvdata(rpmhpd_pdev->dev.parent)) {
		put_device(&rpmhpd_pdev->dev);
		rpmhpd_pdev = NULL;
		return -EPROBE_DEFER;
	}

	ret = mss_domain_join();
	if (ret) {
		pr_err("mss_vote: cannot follow the MSS domain: %d\n", ret);
		put_device(&rpmhpd_pdev->dev);
		rpmhpd_pdev = NULL;
		return ret;
	}

	pr_info("mss_vote: ready, mss.lvl at 0x%x, following %s\n", mss_addr, dev_name(mss_dev));
	return 0;
}

static void __exit mss_vote_exit(void)
{
	mss_domain_leave();
	if (level >= 0 && level != MSS_TOP_CORNER)
		pr_info("mss_vote: restore top corner: %d\n", mss_send(MSS_TOP_CORNER));
	put_device(&rpmhpd_pdev->dev);
}

module_init(mss_vote_init);
module_exit(mss_vote_exit);
MODULE_DESCRIPTION("Runtime AP vote on the SM8150 modem rail, kept across suspend");
MODULE_LICENSE("GPL");
