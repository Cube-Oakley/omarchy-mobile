// SPDX-License-Identifier: GPL-2.0-only
/* Experimental CPH2655 S3910 TouchComm v1 transport/input.
 * Protocol reference: OnePlusOSS modules/devicetree d50b305f7da9e14715a25120a4ac7b1a4b8b97c3,
 * vendor/oplus/kernel/touchpanel/synaptics_hbp/tcm/.
 * No flash, persistent configuration, bootloader or firmware-download commands.
 * Default: identify/application-info only. input_enabled=1 opts into volatile
 * local-coordinate reporting. Polling is for bring-up, not suspend/power parity.
 */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/input/mt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#define MAX_PAYLOAD 4096
static bool input_enabled;
module_param(input_enabled, bool, 0444);
MODULE_PARM_DESC(input_enabled, "Enable volatile coordinate reports after chip identification");

struct tcm {
    struct spi_device *spi;
    struct gpio_desc *reset, *enable, *attn;
    struct input_dev *input;
    struct delayed_work work;
    u8 *rx, *tx, *payload;
    unsigned int length, slots, max_x, max_y, errors;
    u8 code;
};

static int read_bytes(struct tcm *t, unsigned int len)
{
    struct spi_transfer x = { .tx_buf = t->tx, .rx_buf = t->rx, .len = len };
    if (len > MAX_PAYLOAD + 3) return -EMSGSIZE;
    memset(t->tx, 0xff, len);
    return spi_sync_transfer(t->spi, &x, 1);
}

static int read_message(struct tcm *t)
{
    int ret = read_bytes(t, 4);
    if (ret) return ret;
    if (t->rx[0] != 0xa5) return -EPROTO;
    t->code = t->rx[1];
    t->length = get_unaligned_le16(t->rx + 2);
    if (t->code == 0 || t->code == 2) { t->length = 0; return 0; }
    if (t->length > MAX_PAYLOAD) return -EMSGSIZE;
    if (!t->length) return 0;
    ret = read_bytes(t, t->length + 3);
    if (ret) return ret;
    if (t->rx[0] != 0xa5 || t->rx[1] != 3 || t->rx[t->length + 2] != 0x5a)
        return -EPROTO;
    memcpy(t->payload, t->rx + 2, t->length);
    return 0;
}

static int command(struct tcm *t, u8 cmd, const u8 *data, unsigned int len)
{
    int ret, i;
    /* Explicitly exclude firmware/flash and persistent config operations. */
    switch (cmd) {
    case 0x02: case 0x05: case 0x06: case 0x20: case 0x24: case 0x26: break;
    default: return -EPERM;
    }
    if (len > 512) return -EMSGSIZE;
    t->tx[0] = cmd;
    put_unaligned_le16(len, t->tx + 1);
    if (len) memcpy(t->tx + 3, data, len);
    ret = spi_write(t->spi, t->tx, len + 3);
    if (ret) return ret;
    for (i = 0; i < 100; i++) {
        msleep(20);
        ret = read_message(t);
        if (ret) return ret;
        if (t->code == 1 || (cmd == 2 && t->code == 0x10)) return 0;
        if (t->code > 2 && t->code < 0x10) return -EIO;
    }
    return -ETIMEDOUT;
}

static void release_contacts(struct tcm *t)
{
    input_mt_sync_frame(t->input);
    input_sync(t->input);
}

static int report_contacts(struct tcm *t)
{
    unsigned int i, seen = 0;
    if (t->length != t->slots * 5) return -EPROTO;
    /* Validate the whole frame before emitting any input events. */
    for (i = 0; i < t->slots; i++) {
        const u8 *p = t->payload + i * 5;
        unsigned int slot = p[0] & 15, kind = p[0] >> 4;
        if (slot >= t->slots || (seen & BIT(slot))) return -EPROTO;
        seen |= BIT(slot);
        if ((kind == 1 || kind == 2) &&
            (get_unaligned_le16(p + 1) > t->max_x || get_unaligned_le16(p + 3) > t->max_y))
            return -ERANGE;
    }
    for (i = 0; i < t->slots; i++) {
        const u8 *p = t->payload + i * 5;
        unsigned int kind = p[0] >> 4;
        bool active = kind == 1 || kind == 2;
        input_mt_slot(t->input, p[0] & 15);
        input_mt_report_slot_state(t->input, MT_TOOL_FINGER, active);
        if (active) {
            input_report_abs(t->input, ABS_MT_POSITION_X, get_unaligned_le16(p + 1));
            input_report_abs(t->input, ABS_MT_POSITION_Y, get_unaligned_le16(p + 3));
        }
    }
    release_contacts(t);
    return 0;
}

static void poll_touch(struct work_struct *work)
{
    struct tcm *t = container_of(to_delayed_work(work), struct tcm, work);
    int ret = 0, active = gpiod_get_value_cansleep(t->attn);
    if (active < 0) ret = active;
    else if (active) {
        ret = read_message(t);
        if (!ret && t->code == 0x11) ret = report_contacts(t);
        else if (!ret && t->code == 0x10) ret = -ECONNRESET;
    }
    if (ret) {
        release_contacts(t);
        dev_warn_ratelimited(&t->spi->dev, "touch read/report failed: %d (code %02x, len %u)\n", ret, t->code, t->length);
        if (++t->errors >= 20) {
            dev_err(&t->spi->dev, "stopping after repeated touch errors\n");
            return;
        }
    } else if (active) t->errors = 0;
    schedule_delayed_work(&t->work, msecs_to_jiffies(8));
}

static void stop_work(void *data)
{
    struct tcm *t = data;
    cancel_delayed_work_sync(&t->work);
}

static void power_off(void *data)
{
    struct tcm *t = data;
    gpiod_set_value_cansleep(t->reset, 1);
    gpiod_set_value_cansleep(t->enable, 0);
}

static int tcm_probe(struct spi_device *spi)
{
    struct device *dev = &spi->dev;
    struct tcm *t;
    unsigned int max_config;
    int ret;
    u8 config[512] = { 2, 6, 4, 7, 4, 8, 16, 9, 16, 3, 0 };
    const u8 hbp_report = 0x23, touch_report = 0x11, lbp[] = { 0x11, 2, 0 };
    if (!of_machine_is_compatible("oneplus,dodge")) return -ENODEV;
    t = devm_kzalloc(dev, sizeof(*t), GFP_KERNEL);
    if (!t) return -ENOMEM;
    t->spi = spi;
    t->rx = devm_kmalloc(dev, MAX_PAYLOAD + 3, GFP_KERNEL);
    t->tx = devm_kmalloc(dev, MAX_PAYLOAD + 3, GFP_KERNEL);
    t->payload = devm_kmalloc(dev, MAX_PAYLOAD, GFP_KERNEL);
    if (!t->rx || !t->tx || !t->payload) return -ENOMEM;
    t->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
    if (IS_ERR(t->reset)) return dev_err_probe(dev, PTR_ERR(t->reset), "reset GPIO\n");
    t->enable = devm_gpiod_get(dev, "enable", GPIOD_OUT_LOW);
    if (IS_ERR(t->enable)) return dev_err_probe(dev, PTR_ERR(t->enable), "enable GPIO\n");
    t->attn = devm_gpiod_get(dev, "interrupt", GPIOD_IN);
    if (IS_ERR(t->attn)) return dev_err_probe(dev, PTR_ERR(t->attn), "attention GPIO\n");
    ret = devm_regulator_get_enable(dev, "vdd");
    if (ret) return dev_err_probe(dev, ret, "1.8V supply\n");
    ret = devm_add_action_or_reset(dev, power_off, t);
    if (ret) return ret;
    gpiod_set_value_cansleep(t->enable, 1);
    msleep(200);
    gpiod_set_value_cansleep(t->reset, 1);
    msleep(10);
    gpiod_set_value_cansleep(t->reset, 0);
    msleep(80);
    ret = read_message(t);
    if (!ret && t->code != 0x10) ret = command(t, 2, NULL, 0);
    if (ret) return dev_err_probe(dev, ret, "identify transport (code %02x)\n", t->code);
    if (t->length < 24 || t->payload[1] != 1 || memcmp(t->payload + 2, "S3910", 5))
        return dev_err_probe(dev, -ENODEV, "unexpected chip or firmware mode\n");
    dev_info(dev, "TCM v1 %.16s application build %u; input_enabled=%d\n",
             t->payload + 2, get_unaligned_le32(t->payload + 18), input_enabled);
    ret = command(t, 0x20, NULL, 0);
    if (ret) return dev_err_probe(dev, ret, "application info\n");
    if (t->length < 40 || get_unaligned_le16(t->payload + 2)) return -EPROTO;
    max_config = get_unaligned_le16(t->payload + 12);
    t->max_x = get_unaligned_le16(t->payload + 32);
    t->max_y = get_unaligned_le16(t->payload + 34);
    t->slots = get_unaligned_le16(t->payload + 36);
    dev_info(dev, "application geometry %ux%u, %u slots, report config %u\n", t->max_x, t->max_y, t->slots, max_config);
    if (!input_enabled) return 0;
    if (!t->slots || t->slots > 10 || !t->max_x || !t->max_y || max_config < 11 || max_config > sizeof(config)) return -EINVAL;
    ret = command(t, 0x06, &hbp_report, 1);
    if (!ret) ret = command(t, 0x24, lbp, sizeof(lbp));
    if (!ret) ret = command(t, 0x26, config, max_config);
    if (!ret) ret = command(t, 0x05, &touch_report, 1);
    if (ret) return dev_err_probe(dev, ret, "volatile touch report setup\n");
    t->input = devm_input_allocate_device(dev);
    if (!t->input) return -ENOMEM;
    t->input->name = "OnePlus 13 S3910 native touch";
    t->input->id.bustype = BUS_SPI;
    input_set_abs_params(t->input, ABS_MT_POSITION_X, 0, t->max_x, 0, 0);
    input_set_abs_params(t->input, ABS_MT_POSITION_Y, 0, t->max_y, 0, 0);
    ret = input_mt_init_slots(t->input, t->slots, INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
    if (ret) return ret;
    ret = input_register_device(t->input);
    if (ret) return ret;
    INIT_DELAYED_WORK(&t->work, poll_touch);
    ret = devm_add_action_or_reset(dev, stop_work, t);
    if (ret) return ret;
    schedule_delayed_work(&t->work, msecs_to_jiffies(8));
    return 0;
}

static const struct of_device_id tcm_of_match[] = {
    { .compatible = "omarchy,s3910-probe" }, { }
};
MODULE_DEVICE_TABLE(of, tcm_of_match);
static const struct spi_device_id tcm_spi_ids[] = {
    { "s3910-probe", 0 }, { }
};
MODULE_DEVICE_TABLE(spi, tcm_spi_ids);
static struct spi_driver tcm_driver = {
    .driver = { .name = "oneplus13-tcm", .of_match_table = tcm_of_match },
    .probe = tcm_probe,
    .id_table = tcm_spi_ids,
};
module_spi_driver(tcm_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Bounded OnePlus 13 S3910 native touch bring-up");
