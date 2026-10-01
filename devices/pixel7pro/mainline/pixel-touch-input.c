// SPDX-License-Identifier: GPL-2.0-only
/* Temporary cheetah touch probe. Sends GET_APPLICATION_INFO,
 * GET_TOUCH_REPORT_CONFIG and DISABLE_REPORT for the heat map, a runtime
 * setting the vendor driver also toggles; no firmware/configuration writes.
 * After an error (protocol, controller reset, AOC taking the bus) the worker
 * resets the controller and continues instead of stopping.
 * SPI reads clock 0xff as required by Google's Synaptics transport.
 * hwspi=1 moves the transfers onto the SPI0 controller at 9.98 MHz and irq=1
 * sleeps on the attention interrupt; without them the pins are bit-banged
 * and polled. Restores pins, clocks and the USI on unload or timeout.
 * sleep=1 (the display hook writes it at screen-off) holds the controller in
 * reset, as the bootloader leaves it, until sleep=0 restarts it: about 5 mA
 * less in system sleep (docs/suspend-20260930.md).
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/input/mt.h>
#include <linux/freezer.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/unaligned.h>

static bool probe;
module_param(probe, bool, 0400);
MODULE_PARM_DESC(probe, "Explicit opt-in to bounded reset/read/restore probe");
static void __iomem *peri, *far, *hsi;
static const unsigned long bases[]={0x10c40000,0x180e0000,0x11840000};
static u32 con, dat, pud, rdat, hcon, hdat;
static int claimed;
static bool changed;
static struct input_dev *input;
static struct task_struct *worker;
static unsigned int seconds=300;
module_param(seconds, uint, 0400);
static unsigned int half_period_us=1;
module_param(half_period_us, uint, 0400);
MODULE_PARM_DESC(half_period_us, "SCLK half period; 0 relies on register access time");
/* Poll the attention line this often for a second after any report, then
 * fall back to the 8 ms idle poll.
 */
static unsigned int active_poll_us=1000;
module_param(active_poll_us, uint, 0400);
static unsigned int retry_us=500;
module_param(retry_us, uint, 0400);
MODULE_PARM_DESC(retry_us, "Delay before re-reading a packet without its 0xa5 marker");
static bool heatmap;
module_param(heatmap, bool, 0400);
MODULE_PARM_DESC(heatmap, "Keep the controller's heat map report ($c3); off halves each frame");
static int restart;
module_param(restart, int, 0600);
MODULE_PARM_DESC(restart, "Write 1 to reset and restart the controller, as after an error");
static bool sleep_req;
static DECLARE_WAIT_QUEUE_HEAD(sleep_wq);
static int sleep_set(const char *val, const struct kernel_param *kp)
{
    int ret=param_set_bool(val, kp);
    if (!ret) wake_up(&sleep_wq);
    return ret;
}
static const struct kernel_param_ops sleep_ops = { .set = sleep_set, .get = param_get_bool };
module_param_cb(sleep, &sleep_ops, &sleep_req, 0644);
MODULE_PARM_DESC(sleep, "Write 1 to hold the controller in reset (screen off), 0 to restart it");
static bool hwspi;
module_param(hwspi, bool, 0400);
MODULE_PARM_DESC(hwspi, "Use the SPI0 controller (PERIC1 USI0) instead of GPIO bit-banging");
static u64 transfer_ns, transfer_bytes, read_retries, recoveries, spi_timeouts;
/* SPI0: stock DT spi@10D10000 (samsung,exynos-spi, 64-entry FIFO) on PERIC1
 * USI0, sysreg_peric1 0x10c21000 offset 0. Clock: PLL_SHARED2 798.72 MHz / 2
 * reaches USI0 undivided (399.36 MHz); DIV_CLK_PERIC1_USI0_USI (CMU_PERIC1
 * 0x1808) at 10 and the controller's fixed 4 give 9.98 MHz, under the
 * S3908's 10 MHz. Registers follow mainline spi-s3c64xx (gs101 port: 32-bit
 * I/O only) and exynos-usi. CS stays a GPIO, as in the stock DT.
 */
#define SPI_BASE        0x10d10000
#define CMU_PERIC1      0x10c00000
#define SYSREG_PERIC1   0x10c21000
#define SPI_CH_CFG      0x00
#define SPI_MODE_CFG    0x08
#define SPI_CS_REG      0x0c
#define SPI_INT_EN      0x10
#define SPI_STATUS      0x14
#define SPI_TX_DATA     0x18
#define SPI_RX_DATA     0x1c
#define SPI_PENDING_CLR 0x24
#define SPI_CH_SW_RST   BIT(5)
#define SPI_CH_RX_ON    BIT(1)
#define SPI_CH_TX_ON    BIT(0)
#define SPI_RX_LVL(v)   (((v) >> 15) & 0x1ff)
#define USI_CON         0xc4
#define USI_OPTION      0xc8
#define USI0_DIV        0x1808
/* Attention line gpa7-0: FAR_ALIVE wake-up EINT (exynos7 layout: ECON 0x700,
 * EMASK 0x900, EPEND 0xa00, gpa7 at eint offset 4; vendor pinctrl-gs201.c),
 * GIC SPI 52 through the stock DT gpa7 node. Level low, like the stock
 * irq-gpio flags. Only pin 0's bits are changed.
 */
static bool use_irq;
module_param_named(irq, use_irq, bool, 0400);
MODULE_PARM_DESC(irq, "Sleep until the attention interrupt instead of polling");
#define GPA7_CON   0x20
#define GPA7_ECON  0x704
#define GPA7_EMASK 0x904
#define GPA7_EPEND 0xa04
static int attn_irq;
static bool attn_disabled;
static DECLARE_COMPLETION(attn);
static u32 gpa7_con_saved, gpa7_emask_saved;
static u64 attn_irqs;
static void __iomem *spi, *cmu, *sysreg;
static u32 div_saved, swconf_saved, usicon_saved, usiopt_saved;
static bool spi_ready;
static void restore_pins(void);
static const u8 expected_config[128] = {
  0x10,8,0x1b,56,0x1e,8,0x17,8,0x18,8,4,1,6,4,7,4,
  8,16,9,16,10,16,11,8,12,8,0xd2,8,0xd3,8,0xd1,8,3,0
};
static void bits(void __iomem *r, u32 mask, u32 value)
{
    writel((readl(r) & ~mask) | (value & mask), r);
    readl(r);
}
/* Mode 0: MOSI changes with SCLK low, MISO is sampled after the rising edge.
 * Output bits of the SPI bank come from a shadow taken with CS asserted; only
 * this driver drives these pins while it owns the bus. Device-memory accesses
 * to one peripheral stay ordered, so no read-back is needed between edges.
 */
static u32 shadow;
static void half_period(void)
{
    if (half_period_us) udelay(half_period_us);
}
static u8 transfer_byte(u8 tx)
{
    u8 v=0;
    for (int i=0;i<8;i++) {
        shadow = (shadow & ~3) | ((tx & (0x80 >> i)) ? 2 : 0);
        writel(shadow, peri+4);
        half_period();
        writel(shadow | 1, peri+4);
        half_period();
        v=(v<<1)|((readl(peri+4)>>2)&1);
    }
    writel(shadow, peri+4);
    return v;
}
/* Full duplex through the FIFOs, up to 64 bytes a round. A stalled round
 * reads as 0xff, which the framing checks reject.
 */
static void spi_transfer(const u8 *tx, u8 *rx, unsigned int count)
{
    writel(SPI_CH_SW_RST, spi+SPI_CH_CFG);
    writel(0, spi+SPI_CH_CFG);
    /* Manual chip select: the controller only clocks while its own select is
     * active (mainline writes 0 here); the pad itself is the GPIO.
     */
    writel(0, spi+SPI_CS_REG);
    writel(SPI_CH_TX_ON|SPI_CH_RX_ON, spi+SPI_CH_CFG);
    for (unsigned int done=0; done<count; ) {
        unsigned int n=min(count-done, 64U), got=0;
        u64 limit=ktime_get_ns()+5*NSEC_PER_MSEC;
        for (unsigned int i=0; i<n; i++)
            writel(tx ? tx[done+i] : 0xff, spi+SPI_TX_DATA);
        while (got < n) {
            unsigned int lvl=SPI_RX_LVL(readl(spi+SPI_STATUS));
            if (!lvl) {
                if (ktime_get_ns() > limit) break;
                cpu_relax();
                continue;
            }
            while (lvl-- && got < n) {
                u8 v=readl(spi+SPI_RX_DATA);
                if (rx) rx[done+got]=v;
                got++;
            }
        }
        if (got < n) {
            if (!spi_timeouts++)
                pr_err("pixel-touch: SPI stalled %u/%u: STATUS=%08x CH=%08x MODE=%08x CS=%08x USI_CON=%08x OPT=%08x SW_CONF=%08x DIV=%08x\n",
                    got, n, readl(spi+SPI_STATUS), readl(spi+SPI_CH_CFG), readl(spi+SPI_MODE_CFG),
                    readl(spi+SPI_CS_REG), readl(spi+USI_CON), readl(spi+USI_OPTION), readl(sysreg),
                    readl(cmu+USI0_DIV));
            if (rx) memset(rx+done+got, 0xff, n-got);
        }
        done+=n;
    }
    writel(0, spi+SPI_CH_CFG);
    writel(BIT(0), spi+SPI_CS_REG);
}
static void transfer(const u8 *tx, u8 *rx, unsigned int count)
{
    u64 start=ktime_get_ns();
    if (spi_ready) {
        bits(peri+4,8,0);
        spi_transfer(tx, rx, count);
        bits(peri+4,8,8);
        transfer_ns += ktime_get_ns()-start;
        transfer_bytes += count;
        return;
    }
    bits(peri+4,8,0);
    shadow = readl(peri+4) & ~1;
    for (unsigned int i=0; i<count; i++) {
        u8 value = transfer_byte(tx ? tx[i] : 0xff);
        if (rx) rx[i] = value;
    }
    bits(peri+4,8,8);
    transfer_ns += ktime_get_ns()-start;
    transfer_bytes += count;
}
/* Vendor TCM v1 retries reads with a missing marker up to ten times,
 * separated by 5-10 ms. A low attention line alone does not guarantee that
 * the SPI response is ready. Never accept a packet without its framing.
 * Touch reports retry sooner (retry_us) over the same 100 ms budget.
 */
static int read_packet(u8 *buf, unsigned int count)
{
    unsigned int tries = retry_us >= 5000 ? 10 : min(200U, 100000U / max(retry_us, 100U));
    for (unsigned int retry=0; retry<tries; retry++) {
        if (readl(far+0x44)&0x80) return -EBUSY;
        transfer(NULL,buf,count);
        if (buf[0]==0xa5) return 0;
        read_retries++;
        pr_debug_ratelimited("pixel-touch: read not ready len=%u prefix=%*ph retry=%u\n",
            count, min(count,4U),buf,retry+1);
        if (retry_us >= 5000) usleep_range(5000,10000);
        else usleep_range(retry_us, retry_us + retry_us / 2);
    }
    return -EPROTO;
}
/* Wait for the attention line after a command, as the vendor TCM v1 polling
 * path does. The preceding response's attention level can still be low right
 * after a write, so the first check comes 10 ms later.
 */
static int wait_attention(void)
{
    msleep(10);
    for (int i=0; i<100; i++) {
        if (readl(far+0x44)&0x80) return -EBUSY;
        if (!(readl(far+0x24)&1)) return 0;
        msleep(5);
    }
    return -ETIMEDOUT;
}
/* Send a TCM v1 command and read its status response into body (259 bytes).
 * Touch or heat-map reports queued ahead of it (a finger may be down) are
 * read and dropped. Returns the response payload length or an error.
 */
static int command(u8 cmd, const u8 *payload, unsigned int plen, u8 *body)
{
    u8 request[3+4], header[4];
    unsigned int len;
    int ret;
    if (plen > 4) return -EINVAL;
    request[0]=cmd; request[1]=plen; request[2]=0;
    if (plen) memcpy(request+3, payload, plen);
    transfer(request, NULL, 3+plen);
    for (int packet=0; packet<16; packet++) {
        ret=wait_attention();
        if (ret) return ret;
        ret=read_packet(header,sizeof(header));
        if (ret) return ret;
        len=get_unaligned_le16(header+2);
        if (header[0] != 0xa5 || len > 256) return -EPROTO;
        /* A zero-length message has no continuation packet. */
        if (len) {
            ret=read_packet(body,len+3);
            if (ret) return ret;
            if (body[0] != 0xa5 || body[1] != 3 || body[len+2] != 0x5a) return -EPROTO;
        }
        if (header[1] == 0x01) return len;
        if (header[1] == 0x10) return -EPROTO;
        if (header[1] < 0x10) {
            pr_err("pixel-touch: command %02x status %02x\n", cmd, header[1]);
            return -EIO;
        }
    }
    return -ETIMEDOUT;
}
static int get_info(u8 cmd)
{
    u8 body[259];
    int len=command(cmd, NULL, 0, body);
    if (len < 0) return len;
    print_hex_dump(KERN_INFO, "pixel-touch-probe: payload ", DUMP_PREFIX_OFFSET, 16, 1, body+2, len, false);
    if (cmd == 0x20 && (len != 46 || get_unaligned_le16(body+4) != 0 ||
        get_unaligned_le16(body+34) != 1439 || get_unaligned_le16(body+36) != 3119 ||
        get_unaligned_le16(body+38) != 10)) return -EPROTO;
    if (cmd == 0x25 && (len != sizeof(expected_config) ||
        memcmp(body+2, expected_config, sizeof(expected_config)))) return -EPROTO;
    return 0;
}
static int report_touch(const u8 *p, unsigned int len)
{
    unsigned long seen=0;
    unsigned int count;
    if (len < 11) return -EPROTO;
    count=p[10];
    if (count > 10 || len != 11+12*count) return -EPROTO;
    /* Validate complete packet before publishing any input events. */
    for (unsigned int n=0; n<count; n++) {
        const u8 *o=p+11+n*12;
        unsigned int id=o[0]&15;
        if (id >= 10 || (seen & BIT(id)) || get_unaligned_le16(o+1)>1439 ||
            get_unaligned_le16(o+3)>3119) return -EPROTO;
        seen |= BIT(id);
    }
    for (unsigned int n=0; n<count; n++) {
        const u8 *o=p+11+n*12;
        unsigned int id=o[0]&15, type=o[0]>>4;
        bool active=type==1 || type==2;
        input_mt_slot(input,id);
        input_mt_report_slot_state(input,MT_TOOL_FINGER,active);
        if (active) {
            input_report_abs(input,ABS_MT_POSITION_X,get_unaligned_le16(o+1));
            input_report_abs(input,ABS_MT_POSITION_Y,get_unaligned_le16(o+3));
            pr_debug_ratelimited("pixel-touch: contact slot=%u x=%u y=%u\n",id,
                get_unaligned_le16(o+1),get_unaligned_le16(o+3));
        }
    }
    input_mt_sync_frame(input);
    input_sync(input);
    return 0;
}
static irqreturn_t attn_handler(int irq, void *unused)
{
    disable_irq_nosync(irq);
    WRITE_ONCE(attn_disabled, true);
    attn_irqs++;
    complete(&attn);
    return IRQ_HANDLED;
}
static void idle_wait(unsigned long active_until)
{
    if (attn_irq > 0) {
        /* The line is high (idle) here. Re-arm, then sleep until it drops;
         * a level interrupt fires at once if it already has. The timeout
         * only lets the worker notice a stop or freeze request. The sleep
         * is interruptible (a kthread gets no signals) so an idle panel
         * does not count as load.
         */
        reinit_completion(&attn);
        if (READ_ONCE(attn_disabled)) {
            writel(BIT(0), far+GPA7_EPEND);
            WRITE_ONCE(attn_disabled, false);
            enable_irq(attn_irq);
        }
        wait_for_completion_interruptible_timeout(&attn, msecs_to_jiffies(200));
        return;
    }
    if (active_poll_us && time_before(jiffies, active_until))
        usleep_range(active_poll_us, active_poll_us + active_poll_us / 2);
    else
        msleep(8);
}
static void spi_teardown(void)
{
    if (!spi_ready) return;
    writel(SPI_CH_SW_RST, spi+SPI_CH_CFG);
    writel(usiopt_saved, spi+USI_OPTION);
    writel(usicon_saved, spi+USI_CON);
    writel(swconf_saved, sysreg);
    writel(div_saved, cmu+USI0_DIV);
    spi_ready=false;
}
/* Divider, USI mode and SPI controller. Also after a deep sleep, which
 * resets PERIC1's USI0 and leaves the controller reading 0xff. */
static int spi_program(void)
{
    u32 v;
    writel((div_saved & ~0xf) | 9, cmu+USI0_DIV);
    if (readl_poll_timeout(cmu+USI0_DIV, v, !(v & BIT(16)), 1, 1000)) {
        writel(div_saved, cmu+USI0_DIV);
        return -ETIMEDOUT;
    }
    writel((swconf_saved & ~7) | BIT(1), sysreg);
    writel(usicon_saved & ~BIT(0), spi+USI_CON);
    udelay(1);
    writel((usiopt_saved & ~BIT(2)) | BIT(1), spi+USI_OPTION);
    writel(SPI_CH_SW_RST, spi+SPI_CH_CFG);
    udelay(1);
    writel(0, spi+SPI_CH_CFG);
    writel(0, spi+SPI_MODE_CFG);
    writel(BIT(0), spi+SPI_CS_REG);
    writel(0, spi+SPI_INT_EN);
    writel(0x1f, spi+SPI_PENDING_CLR);
    writel(0, spi+SPI_PENDING_CLR);
    spi_ready=true;
    return 0;
}
static int spi_setup(void)
{
    int ret;
    div_saved=readl(cmu+USI0_DIV);
    swconf_saved=readl(sysreg);
    /* The bootloader leaves USI0 unconfigured and undivided. */
    if ((div_saved & 0xf) || (swconf_saved & 7)) {
        pr_err("pixel-touch: SPI0 handoff differs (div=%08x swconf=%08x)\n", div_saved, swconf_saved);
        return -EINVAL;
    }
    usicon_saved=readl(spi+USI_CON);
    usiopt_saved=readl(spi+USI_OPTION);
    ret=spi_program();
    if (ret) return ret;
    pr_info("pixel-touch: SPI0 controller at 9.98 MHz (USI0 div 10)\n");
    return 0;
}
/* TBN_BUS_OWNER_AP=0, with aoc2ap low as acknowledgement. Then CS inactive,
 * MOSI high (read filler), SCLK mode-0 low; MISO input.
 */
static int bus_acquire(void)
{
    changed=true;
    bits(hsi+0x24,1,0); bits(hsi+0x20,0xf,1);
    if (readl(far+0x44)&0x80) return -EBUSY;
    bits(peri+4,0xf,0xa); bits(peri+8,0xffff,0x100);
    bits(peri,0xffff,0x1011);
    if (hwspi) {
        int ret=spi_setup();
        if (ret) return ret;
        /* SCLK, MOSI and MISO to the controller (stock function 3). */
        bits(peri,0xfff,0x333);
    }
    return 0;
}
/* Reset the controller with the stock 20/200 ms timings, check its identify
 * report and application info, and turn off the heat map report.
 */
static int chip_start(void)
{
    u8 reply[64], body[259], id=0xc3;
    int ret;
    bits(peri+0x64,4,0); msleep(20); bits(peri+0x64,4,4); msleep(200);
    pr_info("pixel-touch-probe: after reset IRQ=%08x ACK=%08x\n",readl(far+0x24),readl(far+0x44));
    if (readl(far+0x44)&0x80) return -EBUSY;
    transfer(NULL, reply, sizeof(reply));
    pr_info("pixel-touch-probe: startup reply %*ph\n",(int)sizeof(reply),reply);
    if (reply[0] != 0xa5 || reply[1] != 0x10 || reply[2] != 24 ||
        reply[3] != 0 || reply[4] != 1 || reply[5] != 1 || reply[28] != 0x5a)
        return -EPROTO;
    ret=get_info(0x20);
    if (!ret) ret=get_info(0x25);
    if (ret) return ret;
    if (!heatmap) {
        ret=command(0x06, &id, 1, body);
        if (ret < 0) pr_warn("pixel-touch: heat map stays on (%d)\n", ret);
        else pr_info("pixel-touch: heat map report off\n");
    }
    return 0;
}
static int attn_irq_setup(void)
{
    struct device_node *np=of_find_node_by_path("/pinctrl@180E0000/gpa7");
    int irq, ret;
    if (!np) return -ENODEV;
    irq=irq_of_parse_and_map(np, 0);
    of_node_put(np);
    if (!irq) return -EINVAL;
    gpa7_con_saved=readl(far+GPA7_CON);
    gpa7_emask_saved=readl(far+GPA7_EMASK);
    /* Expect the inspected handoff: input, level low, masked. */
    if ((gpa7_con_saved & 0xf) || (readl(far+GPA7_ECON) & 0xf) || !(gpa7_emask_saved & 1)) {
        pr_err("pixel-touch: attention EINT handoff differs\n");
        return -EINVAL;
    }
    bits(far+GPA7_CON, 0xf, 0xf);
    writel(BIT(0), far+GPA7_EPEND);
    WRITE_ONCE(attn_disabled, true);
    ret=request_irq(irq, attn_handler, IRQF_NO_AUTOEN, "pixel-touch-attn", NULL);
    if (ret) {
        bits(far+GPA7_CON, 0xf, gpa7_con_saved);
        return ret;
    }
    bits(far+GPA7_EMASK, 1, 0);
    attn_irq=irq;
    pr_info("pixel-touch: attention interrupt %d (gpa7-0, level low)\n", irq);
    return 0;
}
static void attn_irq_teardown(void)
{
    if (attn_irq <= 0) return;
    bits(far+GPA7_EMASK, 1, gpa7_emask_saved);
    free_irq(attn_irq, NULL);
    bits(far+GPA7_CON, 0xf, gpa7_con_saved);
    attn_irq=0;
}
static void release_contacts(void)
{
    for (int i=0;i<10;i++) {
        input_mt_slot(input,i);
        input_mt_report_slot_state(input,MT_TOOL_FINGER,false);
    }
    input_mt_sync_frame(input); input_sync(input);
}
/* Sleep in short steps so unloading the module, or a system suspend (the
 * freezer), is never held up. */
static bool stop_during(unsigned int ms)
{
    unsigned long end=jiffies+msecs_to_jiffies(ms);
    while (time_before(jiffies,end)) {
        if (kthread_should_stop()) return true;
        try_to_freeze();
        msleep(50);
    }
    return kthread_should_stop();
}
/* Restart the controller after an error. If AOC holds the bus, hand the pins
 * back as the bootloader left them and wait for it to finish. Retries back
 * off from 0.5 s to 30 s so a persistent fault cannot spin.
 */
static int recover(void)
{
    unsigned int backoff=500;
    int ret;
    for (;;) {
        if (readl(far+0x44)&0x80) {
            restore_pins();
            pr_warn("pixel-touch: AOC holds the touch bus; waiting\n");
            while (readl(far+0x44)&0x80)
                if (stop_during(200)) return -EINTR;
        }
        ret = changed ? 0 : bus_acquire();
        /* After a deep sleep the controller has lost its setup. */
        if (!ret && changed && hwspi && spi_ready) ret=spi_program();
        if (!ret) ret=chip_start();
        if (!ret) return 0;
        pr_warn("pixel-touch: restart failed (%d); retrying in %u ms\n", ret, backoff);
        if (stop_during(backoff)) return -EINTR;
        backoff=min(backoff*2, 30000U);
    }
}
static int poll_touch(void *unused)
{
    unsigned long deadline=jiffies+seconds*HZ, active_until=jiffies;
    unsigned int reports=0;
    int ret=0;
    set_freezable();
    while (!kthread_should_stop() && (!seconds || time_before(jiffies,deadline))) {
        u8 header[4], body[259];
        unsigned int len;
        /* System suspend freezes the worker between reports. */
        try_to_freeze();
        if (READ_ONCE(sleep_req)) {
            release_contacts();
            bits(peri+0x64,4,0);
            pr_info("pixel-touch: controller held in reset (sleep)\n");
            wait_event_freezable(sleep_wq, !READ_ONCE(sleep_req) || kthread_should_stop());
            if (kthread_should_stop()) break;
            ret=recover();
            if (ret) break;
            pr_info("pixel-touch: controller restarted after sleep\n");
            active_until=jiffies;
            continue;
        }
        if (readl(far+0x44)&0x80) { ret=-EBUSY; goto fail; }
        if (READ_ONCE(restart)) { WRITE_ONCE(restart, 0); ret=-ERESTART; goto fail; }
        if (readl(far+0x24)&1) { idle_wait(active_until); continue; }
        ret=read_packet(header,4);
        if (ret) goto fail;
        len=get_unaligned_le16(header+2);
        if (header[0]!=0xa5 || len>256) { ret=-EPROTO; goto fail; }
        if (!len) { idle_wait(active_until); continue; }
        ret=read_packet(body,len+3);
        if (ret) goto fail;
        if (body[0]!=0xa5 || body[1]!=3 || body[len+2]!=0x5a) {
            pr_err("pixel-touch: invalid body header=%*ph len=%u prefix=%*ph tail=%02x\n",
                4,header,len,min(len+3,4U),body,body[len+2]);
            ret=-EPROTO; goto fail;
        }
        active_until = jiffies + HZ;
        if (header[1]==0x11) {
            ret=report_touch(body+2,len);
            if (ret) {
                print_hex_dump(KERN_ERR,"pixel-touch: rejected ",DUMP_PREFIX_OFFSET,16,1,body+2,len,false);
                goto fail;
            }
            reports++;
        } else {
            pr_debug_ratelimited("pixel-touch: report=%02x len=%u\n",header[1],len);
            /* An identify report means the controller reset itself. */
            if (header[1]==0x10) { ret=-ERESTART; goto fail; }
        }
        cond_resched();
        continue;
fail:
        release_contacts();
        recoveries++;
        pr_warn("pixel-touch: error %d after %u reports; restarting the controller\n", ret, reports);
        ret=recover();
        if (ret) break;
        pr_info("pixel-touch: controller restarted (recovery %llu)\n", recoveries);
    }
    release_contacts();
    restore_pins();
    pr_info("pixel-touch: stopped reports=%u error=%d recoveries=%llu; pins restored\n",
        reports, ret, recoveries);
    pr_info("pixel-touch: transfer bytes=%llu total_us=%llu half_period_us=%u\n",
        transfer_bytes, div_u64(transfer_ns,1000),half_period_us);
    pr_info("pixel-touch: read retries=%llu SPI timeouts=%llu attention IRQs=%llu\n",
        read_retries,spi_timeouts,attn_irqs);
    /* Keep task lifetime valid for module_exit's kthread_stop(). */
    while (!kthread_should_stop())
        msleep(100);
    return ret;
}
static int __init touch_probe_init(void)
{
    int ret=-ENODEV;
    if (half_period_us > 10 || active_poll_us > 8000 || retry_us < 100 || retry_us > 10000 || (seconds && seconds < 10) || seconds > 600 || !probe || !of_machine_is_compatible("google,GS201 CHEETAH"))
        return -EINVAL;
    for (int i=0;i<3;i++) {
        char path[48];
        struct device_node *node;
        struct resource resource;
        int err;
        snprintf(path, sizeof(path), "/pinctrl@%lX", bases[i]);
        node = of_find_node_by_path(path);
        if (!node)
            goto done;
        err = of_address_to_resource(node, 0, &resource);
        of_node_put(node);
        if (err || resource.start != bases[i] || resource_size(&resource) != 4096)
            goto done;
        if (!request_mem_region(bases[i],4096,"pixel-touch-gpio-probe"))
            goto done;
        claimed++;
    }
    peri=ioremap(bases[0],4096); far=ioremap(bases[1],4096); hsi=ioremap(bases[2],4096);
    if (!peri || !far || !hsi) goto done;
    if (hwspi) {
        spi=ioremap(SPI_BASE,4096); cmu=ioremap(CMU_PERIC1,0x2000); sysreg=ioremap(SYSREG_PERIC1,4096);
        if (!spi || !cmu || !sysreg) goto done;
    }
    con=readl(peri); dat=readl(peri+4); pud=readl(peri+8);
    rdat=readl(peri+0x64); hcon=readl(hsi+0x20); hdat=readl(hsi+0x24);
    pr_info("pixel-touch-probe: SPI con=%08x dat=%08x reset con=%08x dat=%08x AP con=%08x dat=%08x ACK=%08x IRQ=%08x\n",
        con,dat,readl(peri+0x60),rdat,hcon,hdat,readl(far+0x44),readl(far+0x24));
    /* Require exactly the inspected unbound bootloader state. Never steal a bus. */
    if ((con&0xffff) || (dat&8)!=8 || (readl(peri+0x60)&0xf00)!=0x100 ||
        (rdat&4) || (hcon&0xf) || (hdat&1) || (readl(far+0x44)&0x80)) {
        pr_err("pixel-touch-probe: handoff differs, refusing writes\n"); goto done;
    }
    ret=bus_acquire();
    if (!ret) ret=chip_start();
    if (ret) goto restore;
    input=input_allocate_device();
    if (!input) { ret=-ENOMEM; goto restore; }
    input->name=hwspi ? "Pixel 7 Pro Synaptics S3908" : "Pixel 7 Pro Synaptics S3908 (GPIO development)";
    input->phys="pixel-touch-gpio/input0";
    input->id.bustype=BUS_SPI;
    input_set_abs_params(input,ABS_MT_POSITION_X,0,1439,0,0);
    input_set_abs_params(input,ABS_MT_POSITION_Y,0,3119,0,0);
    ret=input_mt_init_slots(input,10,INPUT_MT_DIRECT|INPUT_MT_DROP_UNUSED);
    if (!ret) ret=input_register_device(input);
    if (ret) { input_free_device(input); input=NULL; goto restore; }
    if (use_irq && attn_irq_setup())
        pr_warn("pixel-touch: no attention interrupt; polling\n");
    worker=kthread_run(poll_touch,NULL,"pixel-touch");
    if (IS_ERR(worker)) {
        ret=PTR_ERR(worker); worker=NULL;
        attn_irq_teardown();
        input_unregister_device(input); input=NULL;
        goto restore;
    }
    pr_info("pixel-touch: physical input enabled for %u seconds\n",seconds);
    return 0;
restore:
    restore_pins();
done:
    if (peri) iounmap(peri);
    if (far) iounmap(far);
    if (hsi) iounmap(hsi);
    if (spi) iounmap(spi);
    if (cmu) iounmap(cmu);
    if (sysreg) iounmap(sysreg);
    for(int i=0;i<claimed;i++) release_mem_region(bases[i],4096);
    return ret;
}
static void restore_pins(void)
{
    if (!changed) return;
    /* Return reset first, then GPIO inputs, before restoring data/pulls. */
    bits(peri+4,8,8); bits(peri+0x64,4,rdat);
    spi_teardown();
    bits(peri,0xffff,con); bits(peri+4,0xf,dat); bits(peri+8,0xffff,pud);
    bits(hsi+0x20,0xf,hcon); bits(hsi+0x24,1,hdat);
    pr_info("pixel-touch-probe: restored SPI con=%08x dat=%08x reset=%08x AP con=%08x dat=%08x\n",
        readl(peri),readl(peri+4),readl(peri+0x64),readl(hsi+0x20),readl(hsi+0x24));
    changed=false;
}
static void __exit touch_probe_exit(void)
{
    if (worker) kthread_stop(worker);
    attn_irq_teardown();
    if (input) input_unregister_device(input);
    restore_pins();
    if (peri) iounmap(peri);
    if (far) iounmap(far);
    if (hsi) iounmap(hsi);
    if (spi) iounmap(spi);
    if (cmu) iounmap(cmu);
    if (sysreg) iounmap(sysreg);
    for(int i=0;i<claimed;i++) release_mem_region(bases[i],4096);
}
module_init(touch_probe_init);
module_exit(touch_probe_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Pixel 7 Pro bounded GPIO touch input experiment");
