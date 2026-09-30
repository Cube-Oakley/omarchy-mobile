// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal NXP TFA9874 codec support for the OnePlus 7 Pro (guacamole).
 *
 * The device has no internal audio DSP.  These settings reproduce the
 * hardware and speaker profile from the stock guacamole tfa98xx container, while
 * external Qualcomm ADSP speaker protection remains a separate concern.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <sound/soc.h>

#define TFA9874_SYSTEM_CONTROL		0x00
#define TFA9874_MANAGER_CONTROL		0x01
#define TFA9874_AUDIO_CONTROL		0x02
#define TFA9874_REVISION_REG		0x03
#define TFA9874_STATUS_FLAGS0		0x10
#define TFA9874_STATUS_FLAGS1		0x11
#define TFA9874_STATUS_FLAGS3		0x13
#define TFA9874_BATTERY_VOLTAGE		0x15
#define TFA9874_TEMPERATURE		0x16
#define TFA9874_VDDP_VOLTAGE		0x17
#define TFA9874_TDM_CONFIG0		0x20
#define TFA9874_REVISION_0C74		0x0c74

#define TFA9874_PWDN			BIT(0)
#define TFA9874_AMPE			BIT(3)
#define TFA9874_DCA			BIT(4)
#define TFA9874_MANSCONF			BIT(2)
#define TFA9874_TDME			BIT(4)
#define TFA9874_PLLS			BIT(6)
#define TFA9874_CLKS			BIT(7)

/* Field encoding used by the NXP container: register, shift, width - 1. */
#define TFA9874_FIELD(reg, shift, width)	(((reg) << 8) | ((shift) << 4) | ((width) - 1))

struct tfa9874_field_value {
	u16 field;
	u16 value;
};

struct tfa9874_reg_value {
	u8 reg;
	u16 value;
};

struct tfa9874 {
	struct device *dev;
	struct regmap *regmap;
	struct mutex lock;
	u8 slot;
	bool configured;
	bool active;
	bool receiver;
	/* The lower amp's Loudspeaker Switch: off keeps it silent while its
	 * stream runs, so a call on the earpiece stays on the earpiece. */
	bool enabled;
	bool streaming;
};

static const struct tfa9874_reg_value tfa9874_0c74_optimal[] = {
	{ 0x02, 0x22c8 },
	{ 0x52, 0x57dc },
	{ 0x53, 0x003e },
	{ 0x56, 0x0400 },
	{ 0x61, 0x0110 },
	{ 0x6f, 0x00a5 },
	{ 0x70, 0x07f8 },
	{ 0x73, 0x0047 },
	{ 0x74, 0x5098 },
	{ 0x75, 0x8d28 },
	{ 0x80, 0x0000 },
	{ 0x83, 0x0799 },
	{ 0x84, 0x0081 },
};

/* Device-common values from stock guacamole tfa98xx.cnt. */
static const struct tfa9874_field_value tfa9874_device_fields[] = {
	{ TFA9874_FIELD(0x20, 12, 4), 0x2 },	/* TDMNBCK */
	{ TFA9874_FIELD(0x21, 4, 5), 0x1f },	/* TDMSLLN */
	{ TFA9874_FIELD(0x22, 2, 5), 0x1f },	/* TDMSSIZE */
	{ TFA9874_FIELD(0x23, 3, 1), 0x1 },	/* TDMCSE */
	{ TFA9874_FIELD(0x23, 4, 1), 0x0 },	/* TDMVSE */
	{ TFA9874_FIELD(0x68, 0, 3), 0x3 },	/* TDMSRCMAP */
	{ TFA9874_FIELD(0x04, 0, 2), 0x1 },	/* REFCKEXT */
	{ TFA9874_FIELD(0x23, 1, 1), 0x1 },	/* TDMDCE */
	{ TFA9874_FIELD(0x61, 6, 4), 0x4 },	/* TDMSPKG */
	{ TFA9874_FIELD(0x61, 2, 4), 0xf },	/* TDMDCG */
	{ TFA9874_FIELD(0x68, 3, 2), 0x1 },	/* TDMSRCAS */
	{ TFA9874_FIELD(0x68, 5, 2), 0x2 },	/* TDMSRCBS */
	{ TFA9874_FIELD(0x02, 5, 6), 0x16 },	/* FRACTDEL */
	{ TFA9874_FIELD(0x52, 5, 8), 0xbe },	/* AMPGAIN */
	{ TFA9874_FIELD(0x53, 5, 1), 0x1 },
	{ TFA9874_FIELD(0x64, 14, 2), 0x2 },	/* LPM1MODE */
	{ TFA9874_FIELD(0x56, 9, 1), 0x0 },
	{ TFA9874_FIELD(0x6f, 0, 3), 0x5 },	/* DELCURCOMP */
	{ TFA9874_FIELD(0x6f, 7, 3), 0x1 },	/* LVLCLPPWM */
	{ TFA9874_FIELD(0x70, 7, 2), 0x3 },	/* DCCV */
	{ TFA9874_FIELD(0x74, 3, 1), 0x1 },	/* DCTRACK */
	{ TFA9874_FIELD(0x74, 9, 5), 0x8 },	/* DCHOLD */
	{ TFA9874_FIELD(0x75, 15, 1), 0x1 },	/* DCTRIPHYSTE */
	{ TFA9874_FIELD(0x74, 15, 1), 0x0 },
	{ TFA9874_FIELD(0x80, 0, 2), 0x0 },
	{ TFA9874_FIELD(0x83, 0, 6), 0x19 },
	{ TFA9874_FIELD(0x83, 6, 5), 0x1e },
	{ TFA9874_FIELD(0x84, 5, 4), 0x4 },
	{ TFA9874_FIELD(0x51, 5, 1), 0x0 },	/* HPFBYP */
	{ TFA9874_FIELD(0x58, 4, 5), 0x1 },
	{ TFA9874_FIELD(0x6f, 5, 1), 0x1 },	/* ENCURCOMP */
	{ TFA9874_FIELD(0x85, 1, 1), 0x1 },
	{ TFA9874_FIELD(0x85, 3, 1), 0x0 },
	{ TFA9874_FIELD(0x85, 4, 1), 0x0 },
	{ TFA9874_FIELD(0x88, 0, 2), 0x2 },
	{ TFA9874_FIELD(0x84, 4, 1), 0x0 },
	{ TFA9874_FIELD(0xc4, 13, 1), 0x0 },
	{ TFA9874_FIELD(0xb0, 5, 1), 0x0 },
	{ TFA9874_FIELD(0x74, 4, 5), 0xa },	/* DCTRIP */
	{ TFA9874_FIELD(0x75, 3, 5), 0x5 },	/* DCTRIP2 */
	{ TFA9874_FIELD(0x75, 8, 5), 0x10 },	/* DCTRIPT */
	{ TFA9874_FIELD(0x76, 3, 6), 0x1e },	/* DCVOF */
	{ TFA9874_FIELD(0x76, 9, 6), 0x3f },	/* DCVOS */
	{ TFA9874_FIELD(0x02, 0, 4), 0x8 },	/* AUDFS: 48 kHz */
	{ TFA9874_FIELD(0x02, 4, 1), 0x0 },	/* INPLEV */
	{ TFA9874_FIELD(0x62, 14, 2), 0x0 },	/* LNMODE */
};

static bool tfa9874_writeable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case 0x00 ... 0x02:
	case 0x04:
	case 0x0f:
	case 0x20 ... 0x23:
	case 0x26 ... 0x27:
	case 0x51 ... 0x53:
	case 0x56:
	case 0x58:
	case 0x61 ... 0x62:
	case 0x64:
	case 0x68:
	case 0x6f ... 0x70:
	case 0x73 ... 0x76:
	case 0x80:
	case 0x83 ... 0x85:
	case 0x88:
	case 0xa0 ... 0xa1:
	case 0xb0:
	case 0xc4:
		return true;
	default:
		return false;
	}
}

static bool tfa9874_readable_reg(struct device *dev, unsigned int reg)
{
	if (tfa9874_writeable_reg(dev, reg))
		return true;

	switch (reg) {
	case TFA9874_REVISION_REG:
	case TFA9874_STATUS_FLAGS0 ... TFA9874_STATUS_FLAGS1:
	case TFA9874_STATUS_FLAGS3:
	case TFA9874_BATTERY_VOLTAGE ... TFA9874_VDDP_VOLTAGE:
	case 0xfb:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config tfa9874_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.val_format_endian = REGMAP_ENDIAN_BIG,
	.max_register = 0xfb,
	.readable_reg = tfa9874_readable_reg,
	.writeable_reg = tfa9874_writeable_reg,
	.cache_type = REGCACHE_NONE,
};

static int tfa9874_update_field(struct tfa9874 *tfa, u16 field, u16 value)
{
	u8 reg = field >> 8;
	u8 shift = (field >> 4) & 0xf;
	u8 width = (field & 0xf) + 1;
	u16 mask = GENMASK(shift + width - 1, shift);

	return regmap_update_bits(tfa->regmap, reg, mask, value << shift);
}

static int tfa9874_safe_off(struct tfa9874 *tfa)
{
	int ret;

	ret = regmap_update_bits(tfa->regmap, TFA9874_SYSTEM_CONTROL,
				 TFA9874_AMPE | TFA9874_DCA, 0);
	if (ret)
		return ret;

	ret = regmap_update_bits(tfa->regmap, TFA9874_SYSTEM_CONTROL,
				 TFA9874_PWDN, TFA9874_PWDN);
	if (!ret)
		tfa->active = false;

	return ret;
}

static int tfa9874_configure(struct tfa9874 *tfa)
{
	unsigned int hidden_key;
	unsigned int i;
	int ret;

	if (tfa->configured)
		return 0;

	ret = tfa9874_safe_off(tfa);
	if (ret)
		return ret;

	/* Exact 0x0c74 unlock and optimal-settings sequence from stock guacamole. */
	ret = regmap_write(tfa->regmap, 0x0f, 0x5a6b);
	if (ret)
		return ret;
	ret = regmap_read(tfa->regmap, 0xfb, &hidden_key);
	if (ret)
		return ret;
	ret = regmap_write(tfa->regmap, 0xa0, hidden_key ^ 0x005a);
	if (ret)
		return ret;
	ret = regmap_write(tfa->regmap, 0xa1, 0x005a);
	if (ret)
		return ret;
	ret = regmap_write(tfa->regmap, 0x0f, 0x0000);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(tfa9874_0c74_optimal); i++) {
		ret = regmap_write(tfa->regmap, tfa9874_0c74_optimal[i].reg,
				   tfa9874_0c74_optimal[i].value);
		if (ret)
			return ret;
	}

	/* The production container repeats these three key writes. */
	ret = regmap_write(tfa->regmap, 0x0f, 0x5a6b);
	if (ret)
		return ret;
	ret = regmap_write(tfa->regmap, 0xa1, 0x005a);
	if (ret)
		return ret;
	ret = regmap_write(tfa->regmap, 0xa0, 0x005a);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(tfa9874_device_fields); i++) {
		ret = tfa9874_update_field(tfa, tfa9874_device_fields[i].field,
					   tfa9874_device_fields[i].value);
		if (ret)
			return ret;
	}

	/* Stereo slot and current-limit values differ between the two chips. */
	ret = tfa9874_update_field(tfa, TFA9874_FIELD(0x26, 0, 4), tfa->slot);
	if (ret)
		return ret;
	ret = tfa9874_update_field(tfa, TFA9874_FIELD(0x26, 4, 4),
				    !tfa->slot);
	if (ret)
		return ret;
	ret = tfa9874_update_field(tfa, TFA9874_FIELD(0x26, 12, 4),
				    tfa->slot);
	if (ret)
		return ret;
	ret = tfa9874_update_field(tfa, TFA9874_FIELD(0x27, 0, 4),
				    !tfa->slot);
	if (ret)
		return ret;
	ret = tfa9874_update_field(tfa, TFA9874_FIELD(0x70, 3, 4),
				    tfa->slot ? 0xa : 0xb);
	if (ret)
		return ret;

	ret = regmap_update_bits(tfa->regmap, TFA9874_TDM_CONFIG0,
				 TFA9874_TDME, TFA9874_TDME);
	if (ret)
		return ret;

	/* The upper amp's stock receiver profile: boost off and receiver gain.
	 * This is a separate path from its louder stereo/media speaker profile. */
	if (tfa->receiver) {
		ret = tfa9874_update_field(tfa, TFA9874_FIELD(0x23, 1, 1), 0);
		if (!ret)
			ret = tfa9874_update_field(tfa, TFA9874_FIELD(0x61, 6, 4), 0xd);
		if (!ret)
			ret = tfa9874_update_field(tfa, TFA9874_FIELD(0x02, 4, 1), 1);
		if (!ret)
			ret = tfa9874_update_field(tfa, TFA9874_FIELD(0x62, 14, 2), 3);
		if (ret)
			return ret;
	}

	ret = tfa9874_safe_off(tfa);
	if (ret)
		return ret;

	tfa->configured = true;
	dev_info(tfa->dev, "stock guacamole speaker profile prepared on TDM slot %u; output muted\n",
		 tfa->slot);

	return 0;
}

static void tfa9874_log_status(struct tfa9874 *tfa, const char *stage)
{
	unsigned int status0, status1, status3, temp;
	int ret;

	ret = regmap_read(tfa->regmap, TFA9874_STATUS_FLAGS0, &status0);
	ret |= regmap_read(tfa->regmap, TFA9874_STATUS_FLAGS1, &status1);
	ret |= regmap_read(tfa->regmap, TFA9874_STATUS_FLAGS3, &status3);
	ret |= regmap_read(tfa->regmap, TFA9874_TEMPERATURE, &temp);
	if (ret)
		dev_warn(tfa->dev, "%s status read failed: %d\n", stage, ret);
	else
		dev_info(tfa->dev,
			 "%s status0=%04x status1=%04x status3=%04x temp=%04x\n",
			 stage, status0, status1, status3, temp);
}

static int tfa9874_startup(struct snd_pcm_substream *substream,
			   struct snd_soc_dai *dai)
{
	struct tfa9874 *tfa = snd_soc_component_get_drvdata(dai->component);
	int ret;

	mutex_lock(&tfa->lock);
	ret = tfa9874_configure(tfa);
	mutex_unlock(&tfa->lock);

	return ret;
}

/* Called with tfa->lock held. */
static int tfa9874_power_on(struct tfa9874 *tfa)
{
	unsigned int status;
	int ret;

	ret = tfa9874_configure(tfa);
	if (ret)
		return ret;

	ret = regmap_update_bits(tfa->regmap, TFA9874_MANAGER_CONTROL,
				 TFA9874_MANSCONF, TFA9874_MANSCONF);
	if (ret)
		goto fail_off;
	ret = regmap_update_bits(tfa->regmap, TFA9874_SYSTEM_CONTROL,
				 TFA9874_PWDN | TFA9874_DCA | TFA9874_AMPE,
				 tfa->receiver ? 0 : TFA9874_DCA);
	if (ret)
		goto fail_off;

	ret = regmap_read_poll_timeout(tfa->regmap, TFA9874_STATUS_FLAGS1,
				       status, (status & (TFA9874_PLLS | TFA9874_CLKS)) ==
				       (TFA9874_PLLS | TFA9874_CLKS), 500, 20000);
	if (ret) {
		dev_err(tfa->dev, "audio clocks did not become stable (status1=%04x)\n",
			status);
		goto fail_off;
	}

	ret = regmap_update_bits(tfa->regmap, TFA9874_SYSTEM_CONTROL,
				 TFA9874_AMPE, TFA9874_AMPE);
	if (ret)
		goto fail_off;

	usleep_range(1000, 1500);
	tfa->active = true;
	tfa9874_log_status(tfa, "unmuted");
	return 0;

fail_off:
	tfa9874_safe_off(tfa);
	return ret;
}

static int tfa9874_mute_stream(struct snd_soc_dai *dai, int mute, int stream)
{
	struct tfa9874 *tfa = snd_soc_component_get_drvdata(dai->component);
	int ret;

	if (stream != SNDRV_PCM_STREAM_PLAYBACK)
		return 0;

	mutex_lock(&tfa->lock);
	tfa->streaming = !mute;
	if (mute) {
		if (tfa->active)
			tfa9874_log_status(tfa, "pre-mute");
		ret = tfa9874_safe_off(tfa);
	} else if (!tfa->enabled) {
		ret = tfa9874_safe_off(tfa);
	} else {
		ret = tfa9874_power_on(tfa);
	}
	mutex_unlock(&tfa->lock);
	return ret;
}

static void tfa9874_shutdown(struct snd_pcm_substream *substream,
			     struct snd_soc_dai *dai)
{
	struct tfa9874 *tfa = snd_soc_component_get_drvdata(dai->component);

	mutex_lock(&tfa->lock);
	tfa9874_safe_off(tfa);
	mutex_unlock(&tfa->lock);
}

static const struct snd_soc_dai_ops tfa9874_dai_ops = {
	.startup = tfa9874_startup,
	.shutdown = tfa9874_shutdown,
	.mute_stream = tfa9874_mute_stream,
	.no_capture_mute = 1,
};

static struct snd_soc_dai_driver tfa9874_dai = {
	.name = "tfa9874-aif",
	.playback = {
		.stream_name = "Speaker Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S24_LE,
	},
	.ops = &tfa9874_dai_ops,
};

static const char * const tfa9874_mode_names[] = { "Receiver", "Speaker" };
static SOC_ENUM_SINGLE_EXT_DECL(tfa9874_mode_enum, tfa9874_mode_names);

static int tfa9874_mode_get(struct snd_kcontrol *kcontrol,
			  struct snd_ctl_elem_value *value)
{
	struct tfa9874 *tfa = snd_soc_component_get_drvdata(snd_soc_kcontrol_component(kcontrol));
	mutex_lock(&tfa->lock);
	value->value.enumerated.item[0] = !tfa->receiver;
	mutex_unlock(&tfa->lock);
	return 0;
}

static int tfa9874_mode_put(struct snd_kcontrol *kcontrol,
			  struct snd_ctl_elem_value *value)
{
	struct tfa9874 *tfa = snd_soc_component_get_drvdata(snd_soc_kcontrol_component(kcontrol));
	unsigned int mode = value->value.enumerated.item[0];
	int ret;
	if (mode > 1)
		return -EINVAL;
	mutex_lock(&tfa->lock);
	if (tfa->active) {
		ret = -EBUSY;
	} else {
		ret = tfa->receiver != !mode;
		if (ret) {
			tfa->receiver = !mode;
			tfa->configured = false;
		}
	}
	mutex_unlock(&tfa->lock);
	return ret;
}

static const struct snd_kcontrol_new tfa9874_upper_controls[] = {
	SOC_ENUM_EXT("Earpiece Mode", tfa9874_mode_enum, tfa9874_mode_get, tfa9874_mode_put),
};

static int tfa9874_switch_get(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *value)
{
	struct tfa9874 *tfa = snd_soc_component_get_drvdata(snd_soc_kcontrol_component(kcontrol));
	mutex_lock(&tfa->lock);
	value->value.integer.value[0] = tfa->enabled;
	mutex_unlock(&tfa->lock);
	return 0;
}

/* Takes effect at once, also while a stream runs (speakerphone in a call). */
static int tfa9874_switch_put(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *value)
{
	struct tfa9874 *tfa = snd_soc_component_get_drvdata(snd_soc_kcontrol_component(kcontrol));
	bool enabled = value->value.integer.value[0];
	int ret = 0;

	mutex_lock(&tfa->lock);
	if (tfa->enabled != enabled) {
		tfa->enabled = enabled;
		if (tfa->streaming)
			ret = enabled ? tfa9874_power_on(tfa) : tfa9874_safe_off(tfa);
		if (!ret)
			ret = 1;
	}
	mutex_unlock(&tfa->lock);
	return ret;
}

static const struct snd_kcontrol_new tfa9874_lower_controls[] = {
	SOC_SINGLE_BOOL_EXT("Loudspeaker Switch", 0, tfa9874_switch_get, tfa9874_switch_put),
};

static int tfa9874_component_probe(struct snd_soc_component *component)
{
	struct tfa9874 *tfa = dev_get_drvdata(component->dev);
	snd_soc_component_set_drvdata(component, tfa);
	if (!tfa->slot)
		return snd_soc_add_component_controls(component, tfa9874_upper_controls,
			ARRAY_SIZE(tfa9874_upper_controls));
	return snd_soc_add_component_controls(component, tfa9874_lower_controls,
		ARRAY_SIZE(tfa9874_lower_controls));
}

static const struct snd_soc_component_driver tfa9874_component_driver = {
	.name = "tfa9874",
	.probe = tfa9874_component_probe,
	.endianness = 1,
};

static ssize_t diagnostics_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct tfa9874 *tfa = dev_get_drvdata(dev);
	unsigned int system, status0, status1, status3, battery, temp, vddp;
	int ret;

	mutex_lock(&tfa->lock);
	ret = regmap_read(tfa->regmap, TFA9874_SYSTEM_CONTROL, &system);
	ret |= regmap_read(tfa->regmap, TFA9874_STATUS_FLAGS0, &status0);
	ret |= regmap_read(tfa->regmap, TFA9874_STATUS_FLAGS1, &status1);
	ret |= regmap_read(tfa->regmap, TFA9874_STATUS_FLAGS3, &status3);
	ret |= regmap_read(tfa->regmap, TFA9874_BATTERY_VOLTAGE, &battery);
	ret |= regmap_read(tfa->regmap, TFA9874_TEMPERATURE, &temp);
	ret |= regmap_read(tfa->regmap, TFA9874_VDDP_VOLTAGE, &vddp);
	mutex_unlock(&tfa->lock);
	if (ret)
		return ret;

	return sysfs_emit(buf,
		"slot=%u configured=%u active=%u enabled=%u streaming=%u receiver=%u system=%04x status0=%04x status1=%04x status3=%04x battery=%04x temp=%04x vddp=%04x\n",
		tfa->slot, tfa->configured, tfa->active, tfa->enabled, tfa->streaming, tfa->receiver, system, status0,
		status1, status3, battery, temp, vddp);
}
static DEVICE_ATTR_RO(diagnostics);

static struct attribute *tfa9874_attrs[] = {
	&dev_attr_diagnostics.attr,
	NULL,
};
static const struct attribute_group tfa9874_group = {
	.attrs = tfa9874_attrs,
};

static int tfa9874_i2c_probe(struct i2c_client *client)
{
	struct tfa9874 *tfa;
	unsigned int revision;
	int ret;

	if (client->addr != 0x34 && client->addr != 0x35)
		return dev_err_probe(&client->dev, -EINVAL,
				     "unsupported I2C address 0x%02x\n", client->addr);

	tfa = devm_kzalloc(&client->dev, sizeof(*tfa), GFP_KERNEL);
	if (!tfa)
		return -ENOMEM;

	tfa->dev = &client->dev;
	tfa->slot = client->addr == 0x35;
	tfa->receiver = !tfa->slot;
	tfa->enabled = true;
	mutex_init(&tfa->lock);
	tfa->regmap = devm_regmap_init_i2c(client, &tfa9874_regmap_config);
	if (IS_ERR(tfa->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(tfa->regmap),
				     "failed to allocate register map\n");

	ret = regmap_read(tfa->regmap, TFA9874_REVISION_REG, &revision);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to read silicon revision\n");
	if (revision != TFA9874_REVISION_0C74)
		return dev_err_probe(&client->dev, -ENODEV,
				     "unsupported silicon revision 0x%04x\n", revision);

	i2c_set_clientdata(client, tfa);
	ret = devm_device_add_group(&client->dev, &tfa9874_group);
	if (ret)
		return ret;

	dev_info(&client->dev,
		 "TFA9874 revision 0x%04x detected on TDM slot %u; amplifier remains disabled\n",
		 revision, tfa->slot);

	return devm_snd_soc_register_component(&client->dev,
					       &tfa9874_component_driver,
					       &tfa9874_dai, 1);
}

static const struct of_device_id tfa9874_of_match[] = {
	{ .compatible = "nxp,tfa9874" },
	{}
};
MODULE_DEVICE_TABLE(of, tfa9874_of_match);

static const struct i2c_device_id tfa9874_i2c_id[] = {
	{ "tfa9874" },
	{}
};
MODULE_DEVICE_TABLE(i2c, tfa9874_i2c_id);

static struct i2c_driver tfa9874_i2c_driver = {
	.driver = {
		.name = "tfa9874",
		.of_match_table = tfa9874_of_match,
	},
	.probe = tfa9874_i2c_probe,
	.id_table = tfa9874_i2c_id,
};
module_i2c_driver(tfa9874_i2c_driver);

MODULE_DESCRIPTION("NXP TFA9874 amplifier codec driver");
MODULE_AUTHOR("Robin Snyders <robin@snyders.xyz>");
MODULE_LICENSE("GPL");
