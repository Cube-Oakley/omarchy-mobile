// SPDX-License-Identifier: GPL-2.0-only
/* Experimental CPH2655 retained-panel KMS with DSI panel commands. */
#include <linux/aperture.h>
#include <linux/backlight.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_debugfs.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>
#include "drm_sysfb_helper.h"
#include "oneplus13-retained.h"
#include "aa569-on-120.h"

#define ORIGINAL_FB 0xfc800000U
#define WIDTH 1440
#define HEIGHT 3168
#define PITCH (WIDTH * 4)
#define FRAME_DONE BIT(8)
#define TIMING_SAMPLES 64

/* DSI0 controller registers, DSI 6G layout (+4 from the upstream XML). */
#define DSI0_BASE	0x0ae94000
#define DSI_CTRL	0x004
#define DSI_STATUS0	0x008
#define DSI_CMD_DMA_CTRL 0x03c
#define DSI_DMA_BASE	0x048
#define DSI_DMA_LEN	0x04c
#define DSI_ACK_ERR	0x068
#define DSI_RDBK(i)	(0x06c + 4 * (i))
#define DSI_TRIG_CTRL	0x084
#define DSI_TRIG_DMA	0x090
#define DSI_LANE_STATUS	0x0a8
#define DSI_TIMEOUT	0x0c0
#define DSI_INTR_CTRL	0x110
#define DSI_RDBK_CTRL	0x1d4
#define DSI_CMD_DMA_DONE BIT(0)
#define DSI_DMA_LP	BIT(26)

/* AA569 brightness: DBV 1..4094; above 3515 is the panel's boost range. */
#define BL_NORMAL_MAX	3515
#define BL_DEFAULT	1433

struct op13_timing {
	u64 start, armed, finished, last_te;
};

static bool enable;
module_param(enable, bool, 0444);
MODULE_PARM_DESC(enable, "Enable experimental retained-panel DMA KMS for this RAM boot");
static bool early_flip;
module_param(early_flip, bool, 0444);
MODULE_PARM_DESC(early_flip, "RAM-test flip notification after TE and address latch, before transfer completion");
static bool panel;
module_param(panel, bool, 0444);
MODULE_PARM_DESC(panel, "Panel commands over DSI0: backlight device, sleep while the screen is off");

struct op13_scanout {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	struct drm_display_mode mode;
	void __iomem *regs;
	struct completion done;
	struct drm_framebuffer *held, *failed_fb;
	dma_addr_t blank_dma;
	int irq;
	bool running, faulted;
	atomic64_t te, dones, flips, failures;
	atomic64_t last_te_ns;
	spinlock_t timing_lock;
	u64 timing_count;
	struct op13_timing timing[TIMING_SAMPLES];
	/* Panel commands. hw_lock orders them against frame transfers. */
	struct mutex hw_lock;
	void __iomem *dsi;
	u8 *cmd;
	dma_addr_t cmd_dma;
	bool dsi_ready, dsi_faulted, asleep;
	u16 level;
	atomic64_t dsi_cmds, sleeps, wakes;
	int power_mode;
};

static struct op13_scanout *to_op13(struct drm_device *drm)
{
	return container_of(drm, struct op13_scanout, drm);
}

static irqreturn_t op13_irq(int irq, void *data)
{
	struct op13_scanout *p = data;
	u32 done = readl(p->regs + 0x1014) & FRAME_DONE;
	u32 te = readl(p->regs + 0x36804) & BIT(2);
	if (!done && !te)
		return IRQ_NONE;
	/* Match DPU command-mode semantics: read-pointer/TE supplies vblank;
	 * pingpong completion independently retires the DMA transfer. */
	if (te) {
		atomic64_set(&p->last_te_ns, ktime_get_ns());
		writel(te, p->regs + 0x36808);
		atomic64_inc(&p->te);
		if (READ_ONCE(p->running))
			drm_crtc_handle_vblank(&p->pipe.crtc);
	}
	if (done) {
		writel(done, p->regs + 0x1018);
		atomic64_inc(&p->dones);
		complete(&p->done);
	}
	return IRQ_HANDLED;
}

/* Auto refresh is drained at bind. Only this driver can trigger transfers.
 * Both old and new framebuffers stay referenced through real PP0 completion.
 */
static void op13_complete_event(struct op13_scanout *p, struct drm_pending_vblank_event *event)
{
	unsigned long flags;
	if (!event)
		return;
	spin_lock_irqsave(&p->drm.event_lock, flags);
	drm_crtc_send_vblank_event(&p->pipe.crtc, event);
	spin_unlock_irqrestore(&p->drm.event_lock, flags);
}

static int op13_transfer(struct op13_scanout *p, u32 address,
			 struct drm_pending_vblank_event *event)
{
	struct op13_timing timing = { .start = ktime_get_ns() };
	unsigned long flags;
	u32 line, sequence;
	int ret;
	ret = readl_poll_timeout(p->regs + 0x362b0, line,
		!(line & 0xffff) || (line & 0xffff) >= HEIGHT, 100, 50000);
	if (ret)
		goto failed;
	synchronize_irq(p->irq);
	writel(FRAME_DONE, p->regs + 0x1018);
	reinit_completion(&p->done);
	wmb();
	writel(address, p->regs + 0x5014);
	writel(address, p->regs + 0x7014);
	writel(3, p->regs + 0x16018);
	sequence = drm_crtc_vblank_count(&p->pipe.crtc);
	writel(1, p->regs + 0x1601c);
	timing.armed = ktime_get_ns();
	timing.last_te = atomic64_read(&p->last_te_ns);
	if (early_flip && event) {
		/* Flip notification can overlap userspace rendering with this DMA.
		 * Require both address latch and a processed TE after kickoff; an
		 * idle TE or an unlatched flush must never complete the event.
		 * Keep the old and new GEM objects held through PP completion below.
		 */
		ret = readl_poll_timeout(p->regs + 0x16018, line,
			!(line & 3) && atomic64_read(&p->last_te_ns) > timing.armed &&
			drm_crtc_vblank_count(&p->pipe.crtc) != sequence, 100, 150000);
		if (ret)
			goto failed;
		op13_complete_event(p, event);
		event = NULL;
	}
	ret = wait_for_completion_timeout(&p->done, msecs_to_jiffies(150)) ? 0 : -ETIMEDOUT;
	if (!ret && (readl(p->regs + 0x16018) & 3 ||
	    readl(p->regs + 0x5014) != address || readl(p->regs + 0x7014) != address))
		ret = -EIO;
failed:
	timing.finished = ktime_get_ns();
	spin_lock_irqsave(&p->timing_lock, flags);
	p->timing[p->timing_count++ % TIMING_SAMPLES] = timing;
	spin_unlock_irqrestore(&p->timing_lock, flags);
	if (ret) {
		WRITE_ONCE(p->faulted, true);
		atomic64_inc(&p->failures);
		drm_err(&p->drm, "transfer failed %d, DMA %08x; buffers pinned\n", ret, address);
	} else {
		atomic64_inc(&p->flips);
	}
	op13_complete_event(p, event);
	return ret;
}

/* Command DMA, polled: the MDSS interrupt line is this driver's frame IRQ and
 * its handler knows no DSI sources, so DSI interrupt masks stay disabled.
 * Callers hold hw_lock, so no frame transfer is in flight.
 */
static int op13_dsi_kick(struct op13_scanout *p, int len, bool lp)
{
	u32 st, dma_ctrl;
	int ret;
	if (p->dsi_faulted)
		return -EIO;
	ret = readl_poll_timeout(p->dsi + DSI_STATUS0, st, !(st & 7), 20, 20000);
	if (ret)
		goto fault;
	dma_ctrl = readl(p->dsi + DSI_CMD_DMA_CTRL);
	writel(lp ? dma_ctrl | DSI_DMA_LP : dma_ctrl & ~DSI_DMA_LP, p->dsi + DSI_CMD_DMA_CTRL);
	writel(DSI_CMD_DMA_DONE, p->dsi + DSI_INTR_CTRL);
	wmb();
	writel(lower_32_bits(p->cmd_dma), p->dsi + DSI_DMA_BASE);
	writel(len, p->dsi + DSI_DMA_LEN);
	writel(1, p->dsi + DSI_TRIG_DMA);
	wmb();
	ret = readl_poll_timeout(p->dsi + DSI_INTR_CTRL, st, st & DSI_CMD_DMA_DONE, 20, 200000);
	if (ret)
		goto fault;
	writel(DSI_CMD_DMA_DONE, p->dsi + DSI_INTR_CTRL);
	writel(dma_ctrl, p->dsi + DSI_CMD_DMA_CTRL);
	atomic64_inc(&p->dsi_cmds);
	return 0;
fault:
	/* Keep the buffer; stop issuing commands. Frame transfers continue. */
	p->dsi_faulted = true;
	drm_err(&p->drm, "DSI command timeout: intr %08x status %08x ack %08x timeout %08x lanes %08x\n",
		readl(p->dsi + DSI_INTR_CTRL), readl(p->dsi + DSI_STATUS0),
		readl(p->dsi + DSI_ACK_ERR), readl(p->dsi + DSI_TIMEOUT),
		readl(p->dsi + DSI_LANE_STATUS));
	return ret;
}

/* MSM command layout: packet header bytes 1, 2, 0, then last/long/read flags;
 * the controller appends ECC. Long payloads follow, padded with 0xff. */
static int op13_dsi_write(struct op13_scanout *p, u8 type, const u8 *data, int n, bool lp)
{
	bool is_long = type == 0x39;
	int len = is_long ? 4 + n : 4;
	if (n > 256 || (!is_long && n > 2))
		return -EINVAL;
	memset(p->cmd, 0xff, 4 + 256 + 4);
	if (is_long) {
		p->cmd[0] = n & 0xff;
		p->cmd[1] = n >> 8;
		memcpy(p->cmd + 4, data, n);
	} else {
		p->cmd[0] = n > 0 ? data[0] : 0;
		p->cmd[1] = n > 1 ? data[1] : 0;
	}
	p->cmd[2] = type;
	p->cmd[3] = BIT(7) | (is_long ? BIT(6) : 0);
	wmb();
	return op13_dsi_kick(p, (len + 3) & ~3, lp);
}

/* DCS read of up to two bytes, low power with bus turnaround. Returns the
 * response data type (0x21/0x22 short read responses). */
static int op13_dsi_read(struct op13_scanout *p, u8 dcs, int size, u8 *out)
{
	u8 max[2] = { size, 0 };
	u32 r;
	int ret, i;
	ret = op13_dsi_write(p, 0x37, max, 2, true);
	if (ret)
		return ret;
	writel(1, p->dsi + DSI_RDBK_CTRL);
	wmb();
	writel(0, p->dsi + DSI_RDBK_CTRL);
	wmb();
	memset(p->cmd, 0xff, 8);
	p->cmd[0] = dcs;
	p->cmd[1] = 0;
	p->cmd[2] = 0x06;
	p->cmd[3] = BIT(7) | BIT(5);
	wmb();
	ret = op13_dsi_kick(p, 4, true);
	if (ret)
		return ret;
	r = readl(p->dsi + DSI_RDBK(0));
	for (i = 0; i < size && i < 2; i++)
		out[i] = (r >> (16 - 8 * i)) & 0xff;
	return r >> 24;
}

static int op13_panel_level(struct op13_scanout *p)
{
	u8 dbv[3] = { 0x51, p->level >> 8, p->level & 0xff };
	return op13_dsi_write(p, 0x39, dbv, 3, false);
}

static int op13_panel_power_mode(struct op13_scanout *p)
{
	u8 mode;
	int ret = op13_dsi_read(p, 0x0a, 1, &mode);
	p->power_mode = ret == 0x21 ? mode : -EIO;
	return p->power_mode;
}

/* Stock off sequence: command page 0, display off, sleep in. Rails, reset and
 * the DSI link stay up; TE stops until sleep out. */
static void op13_panel_sleep(struct op13_scanout *p)
{
	static const u8 page0[] = { 0xff, 0x5a, 0xa5, 0x00 };
	u8 dcs;
	if (!p->dsi_ready || p->asleep)
		return;
	if (op13_dsi_write(p, 0x39, page0, sizeof(page0), true))
		return;
	dcs = 0x28;
	if (op13_dsi_write(p, 0x05, &dcs, 1, true))
		return;
	msleep(20);
	dcs = 0x10;
	if (op13_dsi_write(p, 0x05, &dcs, 1, true))
		return;
	msleep(120);
	p->asleep = true;
	atomic64_inc(&p->sleeps);
	op13_panel_power_mode(p);
}

/* The stock 120 Hz on sequence (sleep out, its 120 ms wait, configuration,
 * display on) restores the panel from sleep in without a power cycle. */
static void op13_panel_wake(struct op13_scanout *p)
{
	const u8 *r = aa569_on_120;
	if (!p->dsi_ready || !p->asleep)
		return;
	while (r < aa569_on_120 + sizeof(aa569_on_120)) {
		if (op13_dsi_write(p, r[0], r + 3, r[2], true))
			return;
		if (r[1])
			msleep(r[1]);
		r += 3 + r[2];
	}
	op13_panel_level(p);
	p->asleep = false;
	atomic64_inc(&p->wakes);
	op13_panel_power_mode(p);
}

static int op13_bl_update(struct backlight_device *bd)
{
	struct op13_scanout *p = bl_get_data(bd);
	int ret = 0;
	mutex_lock(&p->hw_lock);
	p->level = clamp(backlight_get_brightness(bd), 1, BL_NORMAL_MAX);
	if (!p->asleep)
		ret = op13_panel_level(p);
	mutex_unlock(&p->hw_lock);
	return ret;
}

static const struct backlight_ops op13_bl_ops = {
	.update_status = op13_bl_update,
};

static int op13_dsi_init(struct op13_scanout *p, struct device *dev)
{
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW, .scale = BACKLIGHT_SCALE_LINEAR,
		.max_brightness = BL_NORMAL_MAX,
	};
	struct backlight_device *bd;
	u8 dbv[2];
	p->dsi = devm_ioremap(dev, DSI0_BASE, 0x400);
	if (!p->dsi)
		return -ENOMEM;
	/* Exact retained profile: DSI 6G v2.9 command mode, four lanes, software
	 * DMA trigger with TE-started MDP frames, no DSI interrupt masks. */
	if (readl(p->dsi) != 0x20090000 || readl(p->dsi + DSI_CTRL) != 0x1f5 ||
	    (readl(p->dsi + DSI_CMD_DMA_CTRL) & ~DSI_DMA_LP) != 0x10000000 ||
	    readl(p->dsi + DSI_TRIG_CTRL) != 0x80000004 ||
	    (readl(p->dsi + DSI_INTR_CTRL) & 0x02020202)) {
		drm_warn(&p->drm, "unexpected DSI state ctrl %08x dma %08x trig %08x intr %08x\n",
			 readl(p->dsi + DSI_CTRL), readl(p->dsi + DSI_CMD_DMA_CTRL),
			 readl(p->dsi + DSI_TRIG_CTRL), readl(p->dsi + DSI_INTR_CTRL));
		return -EINVAL;
	}
	p->cmd = dmam_alloc_coherent(dev, PAGE_SIZE, &p->cmd_dma, GFP_KERNEL);
	if (!p->cmd)
		return -ENOMEM;
	mutex_lock(&p->hw_lock);
	p->dsi_ready = true;
	p->level = BL_DEFAULT;
	if (op13_panel_power_mode(p) < 0 || op13_dsi_read(p, 0x52, 2, dbv) != 0x22) {
		p->dsi_ready = false;
		mutex_unlock(&p->hw_lock);
		drm_warn(&p->drm, "panel did not answer DCS reads; no panel control\n");
		return -EIO;
	}
	p->level = clamp((dbv[0] << 8) | dbv[1], 1, BL_NORMAL_MAX);
	mutex_unlock(&p->hw_lock);
	props.brightness = p->level;
	bd = devm_backlight_device_register(dev, "oneplus13-panel", dev, p, &op13_bl_ops, &props);
	if (IS_ERR(bd))
		return PTR_ERR(bd);
	drm_info(&p->drm, "panel power mode %02x, brightness %u; DSI commands ready\n",
		 p->power_mode, p->level);
	return 0;
}

static void op13_send_event(struct drm_crtc *crtc)
{
	unsigned long flags;
	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	if (crtc->state->event) {
		drm_crtc_send_vblank_event(crtc, crtc->state->event);
		crtc->state->event = NULL;
	}
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
}

static int op13_check(struct drm_simple_display_pipe *pipe,
		     struct drm_plane_state *ps, struct drm_crtc_state *cs)
{
	struct op13_scanout *p = to_op13(pipe->crtc.dev);
	struct drm_framebuffer *fb = ps->fb;
	struct drm_gem_dma_object *obj;
	if (READ_ONCE(p->faulted))
		return -EIO;
	if (!fb)
		return 0;
	if (fb->width != WIDTH || fb->height != HEIGHT || fb->pitches[0] != PITCH ||
	    fb->offsets[0] || fb->modifier != DRM_FORMAT_MOD_LINEAR ||
	    ps->src_x || ps->src_y || ps->src_w != WIDTH << 16 || ps->src_h != HEIGHT << 16)
		return -EINVAL;
	obj = drm_fb_dma_get_gem_obj(fb, 0);
	if (!obj || obj->base.size < PITCH * HEIGHT ||
	    upper_32_bits(obj->dma_addr + PITCH * HEIGHT - 1))
		return -EINVAL;
	return 0;
}

static void op13_update(struct drm_simple_display_pipe *pipe, struct drm_plane_state *old)
{
	struct op13_scanout *p = to_op13(pipe->crtc.dev);
	struct drm_framebuffer *fb = pipe->plane.state->fb;
	struct drm_gem_dma_object *obj;
	struct drm_pending_vblank_event *event;
	unsigned long flags;
	int ret;
	if (!fb || READ_ONCE(p->faulted))
		goto event;
	obj = drm_fb_dma_get_gem_obj(fb, 0);
	/* Detach before notifying: the next atomic commit may swap crtc->state
	 * while this worker is still waiting for the current DMA to finish.
	 */
	spin_lock_irqsave(&p->drm.event_lock, flags);
	event = pipe->crtc.state->event;
	pipe->crtc.state->event = NULL;
	spin_unlock_irqrestore(&p->drm.event_lock, flags);
	drm_framebuffer_get(fb);
	mutex_lock(&p->hw_lock);
	ret = op13_transfer(p, lower_32_bits(obj->dma_addr), event);
	mutex_unlock(&p->hw_lock);
	if (ret) {
		p->failed_fb = fb;
	} else {
		if (p->held)
			drm_framebuffer_put(p->held);
		p->held = fb;
	}
	return;
event:
	/* Release waiters on failure, but retain potentially active DMA objects. */
	op13_send_event(&pipe->crtc);
}

static int op13_enable_vblank(struct drm_simple_display_pipe *pipe) { return 0; }

static void op13_enable(struct drm_simple_display_pipe *pipe,
			struct drm_crtc_state *cs, struct drm_plane_state *ps)
{
	struct op13_scanout *p = to_op13(pipe->crtc.dev);
	mutex_lock(&p->hw_lock);
	op13_panel_wake(p);
	mutex_unlock(&p->hw_lock);
	drm_crtc_vblank_on(&pipe->crtc);
	if (drm_crtc_vblank_get(&pipe->crtc))
		drm_warn(pipe->crtc.dev, "completion counting unavailable\n");
	WRITE_ONCE(p->running, true);
}

static void op13_disable(struct drm_simple_display_pipe *pipe)
{
	struct op13_scanout *p = to_op13(pipe->crtc.dev);
	/* A black frame first: the panel shows its frame memory again on wake,
	 * before the compositor's next frame, so it never flashes old content.
	 * Then display off and sleep in; TE stops until the panel wakes. */
	mutex_lock(&p->hw_lock);
	if (!op13_transfer(p, lower_32_bits(p->blank_dma), NULL)) {
		if (p->held)
			drm_framebuffer_put(p->held);
		if (p->failed_fb)
			drm_framebuffer_put(p->failed_fb);
		p->held = p->failed_fb = NULL;
		op13_panel_sleep(p);
	}
	mutex_unlock(&p->hw_lock);
	op13_send_event(&pipe->crtc);
	WRITE_ONCE(p->running, false);
	drm_crtc_vblank_put(&pipe->crtc);
	drm_crtc_vblank_off(&pipe->crtc);
}

static enum drm_mode_status op13_mode_valid(struct drm_simple_display_pipe *pipe,
					   const struct drm_display_mode *mode)
{
	return drm_mode_equal(mode, &to_op13(pipe->crtc.dev)->mode) ? MODE_OK : MODE_BAD;
}

static const struct drm_simple_display_pipe_funcs op13_pipe_funcs = {
	.mode_valid = op13_mode_valid, .check = op13_check, .update = op13_update,
	.enable = op13_enable, .disable = op13_disable, .enable_vblank = op13_enable_vblank,
};

static int op13_get_modes(struct drm_connector *connector)
{
	struct op13_scanout *p = to_op13(connector->dev);
	struct drm_display_mode *mode = drm_mode_duplicate(connector->dev, &p->mode);
	if (!mode)
		return 0;
	mode->type |= DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);
	return 1;
}
static const struct drm_connector_helper_funcs op13_connector_helpers = {
	.get_modes = op13_get_modes,
};
static const struct drm_connector_funcs op13_connector_funcs = {
	DRM_SYSFB_CONNECTOR_FUNCS,
	.destroy = drm_connector_cleanup,
};
static const struct drm_mode_config_funcs op13_mode_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};
static void op13_commit_tail(struct drm_atomic_commit *state)
{
	struct drm_device *dev = state->dev;
	drm_atomic_helper_commit_modeset_disables(dev, state);
	drm_atomic_helper_commit_modeset_enables(dev, state);
	drm_atomic_helper_commit_planes(dev, state, DRM_PLANE_COMMIT_ACTIVE_ONLY);
	drm_atomic_helper_commit_hw_done(state);
	/* An idle command-mode display has no further frame-done interrupts. */
	drm_atomic_helper_wait_for_flip_done(dev, state);
	drm_atomic_helper_cleanup_planes(dev, state);
}
static const struct drm_mode_config_helper_funcs op13_mode_helpers = {
	.atomic_commit_tail = op13_commit_tail,
};
static const u32 op13_formats[] = { DRM_FORMAT_XRGB8888 };
static const u64 op13_modifiers[] = { DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID };
DEFINE_DRM_GEM_DMA_FOPS(op13_fops);
static const struct drm_driver op13_driver = {
	DRM_GEM_DMA_DRIVER_OPS,
	.name = "oneplus13-scanout", .desc = "OnePlus 13 retained-panel DMA KMS",
	.major = 0, .minor = 1,
	.driver_features = DRIVER_ATOMIC | DRIVER_GEM | DRIVER_MODESET,
	.fops = &op13_fops,
};

static int op13_stats(struct seq_file *m, void *unused)
{
	struct drm_debugfs_entry *entry = m->private;
	struct op13_scanout *p = to_op13(entry->dev);
	struct op13_timing *timing;
	unsigned long flags;
	u64 count, i;
	seq_puts(m, "Continuous TE vblank and independent on-demand DMA completion.\n");
	seq_printf(m, "te %lld\ndones %lld\ncompleted_transfers %lld\nfailures %lld\n",
		atomic64_read(&p->te), atomic64_read(&p->dones),
		atomic64_read(&p->flips), atomic64_read(&p->failures));
	seq_printf(m, "cpu_pixel_copy_bytes 0\nactive_dma %08x/%08x\nfaulted %u\n",
		readl(p->regs + 0x5014), readl(p->regs + 0x7014), READ_ONCE(p->faulted));
	mutex_lock(&p->hw_lock);
	if (p->dsi_ready)
		op13_panel_power_mode(p);
	seq_printf(m, "panel_control %u\ndsi_faulted %u\npanel_asleep %u\nbrightness %u\n"
		   "panel_power_mode %d\ndsi_commands %lld\npanel_sleeps %lld\npanel_wakes %lld\n",
		   p->dsi_ready, p->dsi_faulted, p->asleep, p->level, p->power_mode,
		   atomic64_read(&p->dsi_cmds), atomic64_read(&p->sleeps), atomic64_read(&p->wakes));
	mutex_unlock(&p->hw_lock);
	timing = kmalloc_array(TIMING_SAMPLES, sizeof(*timing), GFP_KERNEL);
	if (!timing)
		return -ENOMEM;
	spin_lock_irqsave(&p->timing_lock, flags);
	memcpy(timing, p->timing, sizeof(p->timing));
	count = p->timing_count;
	spin_unlock_irqrestore(&p->timing_lock, flags);
	seq_puts(m, "transfer start_ns armed_ns finished_ns last_te_ns\n");
	for (i = count > TIMING_SAMPLES ? count - TIMING_SAMPLES : 0; i < count; i++) {
		struct op13_timing *t = &timing[i % TIMING_SAMPLES];
		seq_printf(m, "%llu %llu %llu %llu %llu\n", i, t->start,
			   t->armed, t->finished, t->last_te);
	}
	kfree(timing);
	return 0;
}

static int op13_probe(struct platform_device *pdev)
{
	struct op13_scanout *p;
	struct device_node *np;
	struct platform_device *owner;
	void *blank;
	u32 sync, line;
	int ret;
	ret = verify_bypass();
	if (ret)
		return ret;
	np = of_find_node_by_path("/soc@0/display-subsystem@ae00000");
	if (!np)
		return -ENODEV;
	owner = of_find_device_by_node(np);
	of_node_put(np);
	if (!owner)
		return -ENODEV;
	device_lock(&owner->dev);
	if (!owner->dev.driver || strcmp(owner->dev.driver->name, "oneplus13-display-probe") ||
	    !pm_runtime_active(&owner->dev) || !owner->dev.driver->suppress_bind_attrs) {
		ret = -ENODEV;
		goto put_owner;
	}
	if (!try_module_get(owner->dev.driver->owner)) {
		ret = -ENODEV;
		goto put_owner;
	}
	pdev->dev.dma_mask = &pdev->dev.coherent_dma_mask;
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto module_put_owner;
	p = devm_drm_dev_alloc(&pdev->dev, &op13_driver, struct op13_scanout, drm);
	if (IS_ERR(p)) {
		ret = PTR_ERR(p);
		goto module_put_owner;
	}
	p->regs = devm_ioremap(&pdev->dev, 0xae00000, 0x93800);
	if (!p->regs) {
		ret = -ENOMEM;
		goto module_put_owner;
	}
	if (readl(p->regs) != 0xc0000000 ||
	    readl(p->regs + 0x5014) != ORIGINAL_FB || readl(p->regs + 0x7014) != ORIGINAL_FB ||
	    readl(p->regs + 0x5000) != 0x0c6002d0 || readl(p->regs + 0x7000) != 0x0c6002d0 ||
	    readl(p->regs + 0x5008) || readl(p->regs + 0x7008) != 720 ||
	    readl(p->regs + 0x5024) != PITCH || readl(p->regs + 0x7024) != PITCH ||
	    readl(p->regs + 0x5034) != 0x03020001 || readl(p->regs + 0x7034) != 0x03020001 ||
	    readl(p->regs + 0x16014) != 0xf0020000 || readl(p->regs + 0x16018) ||
	    readl(p->regs + 0x362b4) != 0x80000001 ||
	    readl(p->regs + 0x6a404) != 0x00281127 || readl(p->regs + 0x6b404) != 0x00281127 ||
	    readl(p->regs + 0x1010) || readl(p->regs + 0x36800)) {
		ret = -EINVAL;
		goto module_put_owner;
	}
	blank = dmam_alloc_coherent(&pdev->dev, PITCH * HEIGHT, &p->blank_dma, GFP_KERNEL);
	if (!blank) {
		ret = -ENOMEM;
		goto module_put_owner;
	}
	memset(blank, 0, PITCH * HEIGHT);
	p->irq = platform_get_irq(owner, 0);
	if (p->irq < 0) {
		ret = p->irq;
		goto module_put_owner;
	}
	init_completion(&p->done);
	spin_lock_init(&p->timing_lock);
	ret = drmm_mutex_init(&p->drm, &p->hw_lock);
	if (ret)
		goto module_put_owner;
	ret = drmm_mode_config_init(&p->drm);
	if (ret)
		goto module_put_owner;
	p->mode = (struct drm_display_mode) {
		.clock = 599808, .hdisplay = WIDTH, .hsync_start = 1504,
		.hsync_end = 1512, .htotal = 1562, .vdisplay = HEIGHT,
		.vsync_start = 3184, .vsync_end = 3188, .vtotal = 3200,
		.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC,
		.type = DRM_MODE_TYPE_DRIVER, .width_mm = 71, .height_mm = 157,
	};
	drm_mode_set_name(&p->mode);
	p->drm.mode_config.min_width = p->drm.mode_config.max_width = WIDTH;
	p->drm.mode_config.min_height = p->drm.mode_config.max_height = HEIGHT;
	p->drm.mode_config.preferred_depth = 24;
	p->drm.mode_config.funcs = &op13_mode_funcs;
	p->drm.mode_config.helper_private = &op13_mode_helpers;
	ret = drm_connector_init(&p->drm, &p->connector, &op13_connector_funcs,
				 DRM_MODE_CONNECTOR_DSI);
	if (ret)
		goto module_put_owner;
	drm_connector_helper_add(&p->connector, &op13_connector_helpers);
	ret = drm_simple_display_pipe_init(&p->drm, &p->pipe, &op13_pipe_funcs,
		op13_formats, ARRAY_SIZE(op13_formats), op13_modifiers, &p->connector);
	if (ret)
		goto module_put_owner;
	ret = drm_vblank_init(&p->drm, 1);
	if (ret)
		goto module_put_owner;
	drm_mode_config_reset(&p->drm);
	ret = devm_request_irq(&pdev->dev, p->irq, op13_irq, 0, "oneplus13-frame-done", p);
	if (ret)
		goto module_put_owner;
	/* Drain the inherited automatic transport before accepting commits. */
	sync = readl(p->regs + 0x36288);
	writel(sync & ~BIT(20), p->regs + 0x36288);
	writel(0, p->regs + 0x362b4);
	ret = readl_poll_timeout(p->regs + 0x362b0, line,
		!(line & 0xffff) || (line & 0xffff) >= HEIGHT, 100, 50000);
	writel(sync, p->regs + 0x36288);
	if (ret)
		goto restore_auto;
	writel(FRAME_DONE, p->regs + 0x1018);
	writel(BIT(2), p->regs + 0x36808);
	writel(BIT(2), p->regs + 0x36800);
	writel(FRAME_DONE, p->regs + 0x1010);
	ret = op13_transfer(p, ORIGINAL_FB, NULL);
	if (ret)
		goto restore_auto;
	ret = aperture_remove_conflicting_devices(ORIGINAL_FB, 0x2b00000, op13_driver.name);
	if (ret)
		goto restore_auto;
	if (panel && op13_dsi_init(p, &pdev->dev))
		drm_warn(&p->drm, "panel control unavailable; retained panel only\n");
	drm_debugfs_add_file(&p->drm, "oneplus13_scanout", op13_stats, NULL);
	ret = drm_dev_register(&p->drm, 0);
	if (ret)
		goto restore_auto;
	platform_set_drvdata(pdev, p);
	/* No unload/unbind until reboot: the retained display has no full reset
	 * or power-down implementation yet. Keep its power owner pinned too. */
	__module_get(THIS_MODULE);
	device_unlock(&owner->dev);
	put_device(&owner->dev);
	drm_info(&p->drm, "120 Hz DMA KMS, on-demand hardware completion; retained panel\n");
	return 0;
restore_auto:
	writel(0, p->regs + 0x1010);
	writel(0, p->regs + 0x36800);
	readl(p->regs + 0x1010);
	synchronize_irq(p->irq);
	writel(0x80000001, p->regs + 0x362b4);
module_put_owner:
	module_put(owner->dev.driver->owner);
put_owner:
	device_unlock(&owner->dev);
	put_device(&owner->dev);
	return ret;
}

static struct platform_driver op13_platform_driver = {
	.probe = op13_probe,
	.driver = { .name = "oneplus13-scanout", .suppress_bind_attrs = true },
};
static int __init op13_init(void)
{
	struct platform_device *pdev;
	int ret;
	if (!enable || !of_machine_is_compatible("oneplus,cph2655-omarchy-probe"))
		return -ENODEV;
	ret = platform_driver_register(&op13_platform_driver);
	if (ret)
		return ret;
	pdev = platform_device_register_simple("oneplus13-scanout", -1, NULL, 0);
	if (IS_ERR(pdev)) {
		ret = PTR_ERR(pdev);
		goto unregister;
	}
	if (platform_get_drvdata(pdev))
		return 0;
	platform_device_unregister(pdev);
	ret = -ENODEV;
unregister:
	platform_driver_unregister(&op13_platform_driver);
	return ret;
}
module_init(op13_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Experimental OnePlus 13 DMA scanout with retained panel state");
