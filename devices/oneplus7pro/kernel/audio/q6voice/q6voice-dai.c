// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2012-2017, The Linux Foundation. All rights reserved.
// Copyright (c) 2020, Stephan Gerhold

#include <linux/limits.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <sound/soc.h>
#include <dt-bindings/sound/qcom,q6afe.h>
#include <dt-bindings/sound/qcom,q6voice.h>
#include "q6voice.h"

#define DRV_NAME	"q6voice-dai"

static enum q6voice_path_type q6voice_get_path(unsigned int dai_id)
{
	switch (dai_id) {
	case CS_VOICE:
		return Q6VOICE_PATH_VOICE;
	case VOICEMMODE1:
		return Q6VOICE_PATH_VOICEMMODE1;
	}

	return Q6VOICE_PATH_COUNT;
}

static int q6voice_dai_startup(struct snd_pcm_substream *substream,
			       struct snd_soc_dai *dai)
{
	struct q6voice *v = snd_soc_dai_get_drvdata(dai);
	enum q6voice_path_type path = q6voice_get_path(dai->driver->id);

	if (path == Q6VOICE_PATH_COUNT) {
		dev_err(dai->dev, "Invalid DAI ID %u\n", dai->driver->id);
		return -EINVAL;
	}

	return q6voice_start(v, path, substream->stream);
}

static void q6voice_dai_shutdown(struct snd_pcm_substream *substream,
				 struct snd_soc_dai *dai)
{
	struct q6voice *v = snd_soc_dai_get_drvdata(dai);
	enum q6voice_path_type path = q6voice_get_path(dai->driver->id);

	if (path == Q6VOICE_PATH_COUNT) {
		dev_err(dai->dev, "Invalid DAI ID %u\n", dai->driver->id);
		return;
	}

	q6voice_stop(v, path, substream->stream);
}

static const struct snd_soc_dai_ops q6voice_dai_ops = {
	.startup = q6voice_dai_startup,
	.shutdown = q6voice_dai_shutdown,
};

static struct snd_soc_dai_driver q6voice_dais[] = {
	{
		.id = CS_VOICE,
		.name = "CS-VOICE",
		/* The constraints here are not really meaningful... */
		.playback = {
			.stream_name =	"CS-VOICE Playback",
			.formats =	SNDRV_PCM_FMTBIT_S16_LE,
			.rates =	SNDRV_PCM_RATE_8000,
			.rate_min =	8000,
			.rate_max =	8000,
			.channels_min =	1,
			.channels_max =	1,
		},
		.capture = {
			.stream_name =	"CS-VOICE Capture",
			.formats =	SNDRV_PCM_FMTBIT_S16_LE,
			.rates =	SNDRV_PCM_RATE_8000,
			.rate_min =	8000,
			.rate_max =	8000,
			.channels_min =	1,
			.channels_max =	1,
		},
		.ops = &q6voice_dai_ops,
	},
	{
		.id = VOICEMMODE1,
		.name = "VOICEMMODE1",
		.playback = {
			.stream_name =	"VOICEMMODE1 Playback",
			.formats =	SNDRV_PCM_FMTBIT_S16_LE,
			.rates =	SNDRV_PCM_RATE_8000,
			.rate_min =	8000,
			.rate_max =	8000,
			.channels_min =	1,
			.channels_max =	1,
		},
		.capture = {
			.stream_name =	"VOICEMMODE1 Capture",
			.formats =	SNDRV_PCM_FMTBIT_S16_LE,
			.rates =	SNDRV_PCM_RATE_8000,
			.rate_min =	8000,
			.rate_max =	8000,
			.channels_min =	1,
			.channels_max =	1,
		},
		.ops = &q6voice_dai_ops,
	},
};

/* FIXME: Use codec2codec instead */
static struct snd_pcm_hardware q6voice_dai_hardware = {
	.info =			SNDRV_PCM_INFO_INTERLEAVED,
	.buffer_bytes_max =	4096 * 2,
	.period_bytes_min =	2048,
	.period_bytes_max =	4096,
	.periods_min =		2,
	.periods_max =		4,
	.fifo_size =		0,
};

static int q6voice_dai_open(struct snd_soc_component *component,
			    struct snd_pcm_substream *substream)
{
	substream->runtime->hw = q6voice_dai_hardware;
	return 0;
}

static int q6voice_get_mixer(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *ucontrol, bool capture)
{
	struct snd_soc_dapm_context *dapm = snd_soc_dapm_kcontrol_to_dapm(kcontrol);
	struct snd_soc_component *c = snd_soc_dapm_to_component(dapm);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct q6voice *v = snd_soc_component_get_drvdata(c);
	enum q6voice_path_type path = q6voice_get_path(mc->shift);

	if (path == Q6VOICE_PATH_COUNT) {
		dev_err(c->dev, "Invalid DAI ID %u\n", mc->shift);
		return -EINVAL;
	}

	ucontrol->value.integer.value[0] =
		q6voice_get_port(v, path, capture) == mc->reg;
	return 0;
}

static int q6voice_put_mixer(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *ucontrol, bool capture)
{
	struct snd_soc_dapm_context *dapm = snd_soc_dapm_kcontrol_to_dapm(kcontrol);
	struct snd_soc_component *c = snd_soc_dapm_to_component(dapm);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct q6voice *v = snd_soc_component_get_drvdata(c);
	bool val = !!ucontrol->value.integer.value[0];
	enum q6voice_path_type path = q6voice_get_path(mc->shift);

	if (path == Q6VOICE_PATH_COUNT) {
		dev_err(c->dev, "Invalid DAI ID %u\n", mc->shift);
		return -EINVAL;
	}

	if (val)
		q6voice_set_port(v, path, capture, mc->reg);
	else if (q6voice_get_port(v, path, capture) == mc->reg)
		q6voice_set_port(v, path, capture, 0);

	snd_soc_dapm_mixer_update_power(dapm, kcontrol, val, NULL);
	return 1;
}

static int q6voice_get_mixer_capture(struct snd_kcontrol *kcontrol,
				     struct snd_ctl_elem_value *ucontrol)
{
	return q6voice_get_mixer(kcontrol, ucontrol, true);
}

static int q6voice_get_mixer_playback(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *ucontrol)
{
	return q6voice_get_mixer(kcontrol, ucontrol, false);
}

static int q6voice_put_mixer_capture(struct snd_kcontrol *kcontrol,
				     struct snd_ctl_elem_value *ucontrol)
{
	return q6voice_put_mixer(kcontrol, ucontrol, true);
}

static int q6voice_put_mixer_playback(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *ucontrol)
{
	return q6voice_put_mixer(kcontrol, ucontrol, false);
}

static int q6voice_get_topology_kctrl(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *c = snd_kcontrol_chip(kcontrol);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct q6voice *v = snd_soc_component_get_drvdata(c);
	enum q6voice_path_type path = q6voice_get_path(mc->reg);
	bool capture = !!mc->shift;

	if (path >= Q6VOICE_PATH_COUNT) {
		dev_err(c->dev, "Invalid DAI ID %u\n", mc->reg);
		return -EINVAL;
	}

	ucontrol->value.integer.value[0] =
		q6voice_get_topology(v, path, capture);

	return 0;
}

static int q6voice_put_topology_kctrl(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *c = snd_kcontrol_chip(kcontrol);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct q6voice *v = snd_soc_component_get_drvdata(c);
	u32 val = ucontrol->value.integer.value[0];
	enum q6voice_path_type path = q6voice_get_path(mc->reg);
	bool capture = !!mc->shift;

	if (path >= Q6VOICE_PATH_COUNT) {
		dev_err(c->dev, "Invalid DAI ID %u\n", mc->reg);
		return -EINVAL;
	}

	q6voice_set_topology(v, path, capture, val);

	return 1;
}

static const struct snd_kcontrol_new q6voice_kcontrols[] = {
	SOC_SINGLE_EXT("VoiceMMode1 TX Topology", VOICEMMODE1, 1, S32_MAX, 0,
		       q6voice_get_topology_kctrl, q6voice_put_topology_kctrl),
	SOC_SINGLE_EXT("VoiceMMode1 RX Topology", VOICEMMODE1, 0, S32_MAX, 0,
		       q6voice_get_topology_kctrl, q6voice_put_topology_kctrl),
};

static const struct snd_kcontrol_new cs_voice_tx_mixer_controls[] = {
	SOC_SINGLE_EXT("PRI_MI2S_TX", PRIMARY_MI2S_TX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("SEC_MI2S_TX", SECONDARY_MI2S_TX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("TERT_MI2S_TX", TERTIARY_MI2S_TX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUAT_MI2S_TX", QUATERNARY_MI2S_TX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUIN_MI2S_TX", QUINARY_MI2S_TX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("SLIMBUS_0_TX", SLIMBUS_0_TX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("SLIMBUS_1_TX", SLIMBUS_1_TX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("SLIMBUS_2_TX", SLIMBUS_2_TX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("SLIMBUS_3_TX", SLIMBUS_3_TX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("SLIMBUS_4_TX", SLIMBUS_4_TX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("SLIMBUS_5_TX", SLIMBUS_5_TX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("SLIMBUS_6_TX", SLIMBUS_6_TX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("PRIMARY_TDM_TX_0", PRIMARY_TDM_TX_0, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("PRIMARY_TDM_TX_1", PRIMARY_TDM_TX_1, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("PRIMARY_TDM_TX_2", PRIMARY_TDM_TX_2, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("PRIMARY_TDM_TX_3", PRIMARY_TDM_TX_3, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("PRIMARY_TDM_TX_4", PRIMARY_TDM_TX_4, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("PRIMARY_TDM_TX_5", PRIMARY_TDM_TX_5, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("PRIMARY_TDM_TX_6", PRIMARY_TDM_TX_6, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("PRIMARY_TDM_TX_7", PRIMARY_TDM_TX_7, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("SEC_TDM_TX_0", SECONDARY_TDM_TX_0, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("SEC_TDM_TX_1", SECONDARY_TDM_TX_1, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("SEC_TDM_TX_2", SECONDARY_TDM_TX_2, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("SEC_TDM_TX_3", SECONDARY_TDM_TX_3, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("SEC_TDM_TX_4", SECONDARY_TDM_TX_4, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("SEC_TDM_TX_5", SECONDARY_TDM_TX_5, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("SEC_TDM_TX_6", SECONDARY_TDM_TX_6, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("SEC_TDM_TX_7", SECONDARY_TDM_TX_7, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("TERT_TDM_TX_0", TERTIARY_TDM_TX_0, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("TERT_TDM_TX_1", TERTIARY_TDM_TX_1, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("TERT_TDM_TX_2", TERTIARY_TDM_TX_2, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("TERT_TDM_TX_3", TERTIARY_TDM_TX_3, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("TERT_TDM_TX_4", TERTIARY_TDM_TX_4, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("TERT_TDM_TX_5", TERTIARY_TDM_TX_5, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("TERT_TDM_TX_6", TERTIARY_TDM_TX_6, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("TERT_TDM_TX_7", TERTIARY_TDM_TX_7, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUAT_TDM_TX_0", QUATERNARY_TDM_TX_0, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUAT_TDM_TX_1", QUATERNARY_TDM_TX_1, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUAT_TDM_TX_2", QUATERNARY_TDM_TX_2, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUAT_TDM_TX_3", QUATERNARY_TDM_TX_3, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUAT_TDM_TX_4", QUATERNARY_TDM_TX_4, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUAT_TDM_TX_5", QUATERNARY_TDM_TX_5, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUAT_TDM_TX_6", QUATERNARY_TDM_TX_6, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUAT_TDM_TX_7", QUATERNARY_TDM_TX_7, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUIN_TDM_TX_0", QUINARY_TDM_TX_0, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUIN_TDM_TX_1", QUINARY_TDM_TX_1, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUIN_TDM_TX_2", QUINARY_TDM_TX_2, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUIN_TDM_TX_3", QUINARY_TDM_TX_3, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUIN_TDM_TX_4", QUINARY_TDM_TX_4, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUIN_TDM_TX_5", QUINARY_TDM_TX_5, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUIN_TDM_TX_6", QUINARY_TDM_TX_6, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUIN_TDM_TX_7", QUINARY_TDM_TX_7, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("INT0_MI2S_TX", INT0_MI2S_TX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("INT1_MI2S_TX", INT1_MI2S_TX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("INT2_MI2S_TX", INT2_MI2S_TX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("INT3_MI2S_TX", INT3_MI2S_TX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("INT4_MI2S_TX", INT4_MI2S_TX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("INT5_MI2S_TX", INT5_MI2S_TX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("INT6_MI2S_TX", INT6_MI2S_TX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
};

static const struct snd_kcontrol_new voicemmode1_tx_mixer_controls[] = {
	SOC_SINGLE_EXT("PRI_MI2S_TX", PRIMARY_MI2S_TX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("SEC_MI2S_TX", SECONDARY_MI2S_TX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("TERT_MI2S_TX", TERTIARY_MI2S_TX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUAT_MI2S_TX", QUATERNARY_MI2S_TX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUIN_MI2S_TX", QUINARY_MI2S_TX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("SLIMBUS_0_TX", SLIMBUS_0_TX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("SLIMBUS_1_TX", SLIMBUS_1_TX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("SLIMBUS_2_TX", SLIMBUS_2_TX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("SLIMBUS_3_TX", SLIMBUS_3_TX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("SLIMBUS_4_TX", SLIMBUS_4_TX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("SLIMBUS_5_TX", SLIMBUS_5_TX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("SLIMBUS_6_TX", SLIMBUS_6_TX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("PRIMARY_TDM_TX_0", PRIMARY_TDM_TX_0, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("PRIMARY_TDM_TX_1", PRIMARY_TDM_TX_1, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("PRIMARY_TDM_TX_2", PRIMARY_TDM_TX_2, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("PRIMARY_TDM_TX_3", PRIMARY_TDM_TX_3, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("PRIMARY_TDM_TX_4", PRIMARY_TDM_TX_4, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("PRIMARY_TDM_TX_5", PRIMARY_TDM_TX_5, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("PRIMARY_TDM_TX_6", PRIMARY_TDM_TX_6, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("PRIMARY_TDM_TX_7", PRIMARY_TDM_TX_7, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("SEC_TDM_TX_0", SECONDARY_TDM_TX_0, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("SEC_TDM_TX_1", SECONDARY_TDM_TX_1, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("SEC_TDM_TX_2", SECONDARY_TDM_TX_2, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("SEC_TDM_TX_3", SECONDARY_TDM_TX_3, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("SEC_TDM_TX_4", SECONDARY_TDM_TX_4, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("SEC_TDM_TX_5", SECONDARY_TDM_TX_5, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("SEC_TDM_TX_6", SECONDARY_TDM_TX_6, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("SEC_TDM_TX_7", SECONDARY_TDM_TX_7, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("TERT_TDM_TX_0", TERTIARY_TDM_TX_0, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("TERT_TDM_TX_1", TERTIARY_TDM_TX_1, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("TERT_TDM_TX_2", TERTIARY_TDM_TX_2, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("TERT_TDM_TX_3", TERTIARY_TDM_TX_3, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("TERT_TDM_TX_4", TERTIARY_TDM_TX_4, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("TERT_TDM_TX_5", TERTIARY_TDM_TX_5, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("TERT_TDM_TX_6", TERTIARY_TDM_TX_6, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("TERT_TDM_TX_7", TERTIARY_TDM_TX_7, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUAT_TDM_TX_0", QUATERNARY_TDM_TX_0, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUAT_TDM_TX_1", QUATERNARY_TDM_TX_1, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUAT_TDM_TX_2", QUATERNARY_TDM_TX_2, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUAT_TDM_TX_3", QUATERNARY_TDM_TX_3, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUAT_TDM_TX_4", QUATERNARY_TDM_TX_4, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUAT_TDM_TX_5", QUATERNARY_TDM_TX_5, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUAT_TDM_TX_6", QUATERNARY_TDM_TX_6, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUAT_TDM_TX_7", QUATERNARY_TDM_TX_7, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUIN_TDM_TX_0", QUINARY_TDM_TX_0, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUIN_TDM_TX_1", QUINARY_TDM_TX_1, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUIN_TDM_TX_2", QUINARY_TDM_TX_2, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUIN_TDM_TX_3", QUINARY_TDM_TX_3, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUIN_TDM_TX_4", QUINARY_TDM_TX_4, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUIN_TDM_TX_5", QUINARY_TDM_TX_5, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUIN_TDM_TX_6", QUINARY_TDM_TX_6, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("QUIN_TDM_TX_7", QUINARY_TDM_TX_7, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("INT0_MI2S_TX", INT0_MI2S_TX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("INT1_MI2S_TX", INT1_MI2S_TX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("INT2_MI2S_TX", INT2_MI2S_TX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("INT3_MI2S_TX", INT3_MI2S_TX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("INT4_MI2S_TX", INT4_MI2S_TX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("INT5_MI2S_TX", INT5_MI2S_TX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
	SOC_SINGLE_EXT("INT6_MI2S_TX", INT6_MI2S_TX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_capture, q6voice_put_mixer_capture),
};

static const struct snd_kcontrol_new primary_mi2s_rx_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", PRIMARY_MI2S_RX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", PRIMARY_MI2S_RX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
};

static const struct snd_kcontrol_new secondary_mi2s_rx_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", SECONDARY_MI2S_RX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", SECONDARY_MI2S_RX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
};

static const struct snd_kcontrol_new tertiary_mi2s_rx_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", TERTIARY_MI2S_RX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", TERTIARY_MI2S_RX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
};

static const struct snd_kcontrol_new quaternary_mi2s_rx_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", QUATERNARY_MI2S_RX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", QUATERNARY_MI2S_RX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
};

static const struct snd_kcontrol_new quinary_mi2s_rx_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", QUINARY_MI2S_RX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", QUINARY_MI2S_RX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
};

static const struct snd_kcontrol_new slimbus_0_rx_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", SLIMBUS_0_RX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", SLIMBUS_0_RX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
};

static const struct snd_kcontrol_new slimbus_1_rx_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", SLIMBUS_1_RX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", SLIMBUS_1_RX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new slimbus_2_rx_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", SLIMBUS_2_RX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", SLIMBUS_2_RX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new slimbus_3_rx_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", SLIMBUS_3_RX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", SLIMBUS_3_RX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new slimbus_4_rx_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", SLIMBUS_4_RX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", SLIMBUS_4_RX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new slimbus_5_rx_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", SLIMBUS_5_RX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", SLIMBUS_5_RX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new slimbus_6_rx_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", SLIMBUS_6_RX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", SLIMBUS_6_RX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new primary_tdm_rx_0_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", PRIMARY_TDM_RX_0, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", PRIMARY_TDM_RX_0, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new primary_tdm_rx_1_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", PRIMARY_TDM_RX_1, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", PRIMARY_TDM_RX_1, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new primary_tdm_rx_2_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", PRIMARY_TDM_RX_2, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", PRIMARY_TDM_RX_2, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new primary_tdm_rx_3_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", PRIMARY_TDM_RX_3, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", PRIMARY_TDM_RX_3, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new primary_tdm_rx_4_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", PRIMARY_TDM_RX_4, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", PRIMARY_TDM_RX_4, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new primary_tdm_rx_5_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", PRIMARY_TDM_RX_5, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", PRIMARY_TDM_RX_5, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new primary_tdm_rx_6_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", PRIMARY_TDM_RX_6, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", PRIMARY_TDM_RX_6, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new primary_tdm_rx_7_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", PRIMARY_TDM_RX_7, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", PRIMARY_TDM_RX_7, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new secondary_tdm_rx_0_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", SECONDARY_TDM_RX_0, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", SECONDARY_TDM_RX_0, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new secondary_tdm_rx_1_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", SECONDARY_TDM_RX_1, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", SECONDARY_TDM_RX_1, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new secondary_tdm_rx_2_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", SECONDARY_TDM_RX_2, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", SECONDARY_TDM_RX_2, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new secondary_tdm_rx_3_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", SECONDARY_TDM_RX_3, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", SECONDARY_TDM_RX_3, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new secondary_tdm_rx_4_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", SECONDARY_TDM_RX_4, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", SECONDARY_TDM_RX_4, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new secondary_tdm_rx_5_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", SECONDARY_TDM_RX_5, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", SECONDARY_TDM_RX_5, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new secondary_tdm_rx_6_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", SECONDARY_TDM_RX_6, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", SECONDARY_TDM_RX_6, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new secondary_tdm_rx_7_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", SECONDARY_TDM_RX_7, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", SECONDARY_TDM_RX_7, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new tertiary_tdm_rx_0_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", TERTIARY_TDM_RX_0, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", TERTIARY_TDM_RX_0, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new tertiary_tdm_rx_1_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", TERTIARY_TDM_RX_1, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", TERTIARY_TDM_RX_1, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new tertiary_tdm_rx_2_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", TERTIARY_TDM_RX_2, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", TERTIARY_TDM_RX_2, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new tertiary_tdm_rx_3_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", TERTIARY_TDM_RX_3, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", TERTIARY_TDM_RX_3, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new tertiary_tdm_rx_4_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", TERTIARY_TDM_RX_4, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", TERTIARY_TDM_RX_4, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new tertiary_tdm_rx_5_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", TERTIARY_TDM_RX_5, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", TERTIARY_TDM_RX_5, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new tertiary_tdm_rx_6_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", TERTIARY_TDM_RX_6, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", TERTIARY_TDM_RX_6, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new tertiary_tdm_rx_7_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", TERTIARY_TDM_RX_7, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", TERTIARY_TDM_RX_7, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new quaternary_tdm_rx_0_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", QUATERNARY_TDM_RX_0, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", QUATERNARY_TDM_RX_0, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new quaternary_tdm_rx_1_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", QUATERNARY_TDM_RX_1, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", QUATERNARY_TDM_RX_1, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new quaternary_tdm_rx_2_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", QUATERNARY_TDM_RX_2, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", QUATERNARY_TDM_RX_2, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new quaternary_tdm_rx_3_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", QUATERNARY_TDM_RX_3, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", QUATERNARY_TDM_RX_3, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new quaternary_tdm_rx_4_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", QUATERNARY_TDM_RX_4, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", QUATERNARY_TDM_RX_4, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new quaternary_tdm_rx_5_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", QUATERNARY_TDM_RX_5, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", QUATERNARY_TDM_RX_5, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new quaternary_tdm_rx_6_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", QUATERNARY_TDM_RX_6, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", QUATERNARY_TDM_RX_6, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new quaternary_tdm_rx_7_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", QUATERNARY_TDM_RX_7, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", QUATERNARY_TDM_RX_7, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new quinary_tdm_rx_0_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", QUINARY_TDM_RX_0, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", QUINARY_TDM_RX_0, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new quinary_tdm_rx_1_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", QUINARY_TDM_RX_1, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", QUINARY_TDM_RX_1, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new quinary_tdm_rx_2_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", QUINARY_TDM_RX_2, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", QUINARY_TDM_RX_2, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new quinary_tdm_rx_3_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", QUINARY_TDM_RX_3, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", QUINARY_TDM_RX_3, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new quinary_tdm_rx_4_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", QUINARY_TDM_RX_4, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", QUINARY_TDM_RX_4, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new quinary_tdm_rx_5_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", QUINARY_TDM_RX_5, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", QUINARY_TDM_RX_5, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new quinary_tdm_rx_6_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", QUINARY_TDM_RX_6, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", QUINARY_TDM_RX_6, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new quinary_tdm_rx_7_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", QUINARY_TDM_RX_7, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", QUINARY_TDM_RX_7, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback)
};

static const struct snd_kcontrol_new int0_mi2s_rx_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", INT0_MI2S_RX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", INT0_MI2S_RX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
};

static const struct snd_kcontrol_new int1_mi2s_rx_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", INT1_MI2S_RX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", INT1_MI2S_RX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
};

static const struct snd_kcontrol_new int2_mi2s_rx_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", INT2_MI2S_RX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", INT2_MI2S_RX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
};

static const struct snd_kcontrol_new int3_mi2s_rx_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", INT3_MI2S_RX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", INT3_MI2S_RX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
};

static const struct snd_kcontrol_new int4_mi2s_rx_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", INT4_MI2S_RX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", INT4_MI2S_RX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
};

static const struct snd_kcontrol_new int5_mi2s_rx_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", INT5_MI2S_RX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", INT5_MI2S_RX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
};

static const struct snd_kcontrol_new int6_mi2s_rx_mixer_controls[] = {
	SOC_SINGLE_EXT("CS-Voice", INT6_MI2S_RX, CS_VOICE, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
	SOC_SINGLE_EXT("VoiceMMode1", INT6_MI2S_RX, VOICEMMODE1, 1, 0,
		       q6voice_get_mixer_playback, q6voice_put_mixer_playback),
};

static const struct snd_soc_dapm_widget q6voice_dapm_widgets[] = {
	SND_SOC_DAPM_AIF_IN("CS-VOICE_DL1", "CS-VOICE Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("CS-VOICE_UL1", "CS-VOICE Capture", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_MIXER("CS-Voice Capture Mixer", SND_SOC_NOPM, 0, 0,
			   cs_voice_tx_mixer_controls,
			   ARRAY_SIZE(cs_voice_tx_mixer_controls)),
	SND_SOC_DAPM_AIF_IN("VOICEMMODE1_DL1", "VOICEMMODE1 Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("VOICEMMODE1_UL1", "VOICEMMODE1 Capture", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_MIXER("VoiceMMode1 Capture Mixer", SND_SOC_NOPM, 0, 0,
			   voicemmode1_tx_mixer_controls,
			   ARRAY_SIZE(voicemmode1_tx_mixer_controls)),
	SND_SOC_DAPM_MIXER("PRI_MI2S_RX Voice Mixer", SND_SOC_NOPM, 0, 0,
			   primary_mi2s_rx_mixer_controls,
			   ARRAY_SIZE(primary_mi2s_rx_mixer_controls)),
	SND_SOC_DAPM_MIXER("SEC_MI2S_RX Voice Mixer", SND_SOC_NOPM, 0, 0,
			   secondary_mi2s_rx_mixer_controls,
			   ARRAY_SIZE(secondary_mi2s_rx_mixer_controls)),
	SND_SOC_DAPM_MIXER("TERT_MI2S_RX Voice Mixer", SND_SOC_NOPM, 0, 0,
			   tertiary_mi2s_rx_mixer_controls,
			   ARRAY_SIZE(tertiary_mi2s_rx_mixer_controls)),
	SND_SOC_DAPM_MIXER("QUAT_MI2S_RX Voice Mixer", SND_SOC_NOPM, 0, 0,
			   quaternary_mi2s_rx_mixer_controls,
			   ARRAY_SIZE(quaternary_mi2s_rx_mixer_controls)),
	SND_SOC_DAPM_MIXER("QUIN_MI2S_RX Voice Mixer", SND_SOC_NOPM, 0, 0,
			   quinary_mi2s_rx_mixer_controls,
			   ARRAY_SIZE(quinary_mi2s_rx_mixer_controls)),
	SND_SOC_DAPM_MIXER("SLIMBUS_0_RX Voice Mixer", SND_SOC_NOPM, 0, 0,
			   slimbus_0_rx_mixer_controls,
			   ARRAY_SIZE(slimbus_0_rx_mixer_controls)),
	SND_SOC_DAPM_MIXER("SLIMBUS_1_RX Voice Mixer", SND_SOC_NOPM, 0, 0,
			   slimbus_1_rx_mixer_controls,
			   ARRAY_SIZE(slimbus_1_rx_mixer_controls)),
	SND_SOC_DAPM_MIXER("SLIMBUS_2_RX Voice Mixer", SND_SOC_NOPM, 0, 0,
			   slimbus_2_rx_mixer_controls,
			   ARRAY_SIZE(slimbus_2_rx_mixer_controls)),
	SND_SOC_DAPM_MIXER("SLIMBUS_3_RX Voice Mixer", SND_SOC_NOPM, 0, 0,
			   slimbus_3_rx_mixer_controls,
			   ARRAY_SIZE(slimbus_3_rx_mixer_controls)),
	SND_SOC_DAPM_MIXER("SLIMBUS_4_RX Voice Mixer", SND_SOC_NOPM, 0, 0,
			   slimbus_4_rx_mixer_controls,
			   ARRAY_SIZE(slimbus_4_rx_mixer_controls)),
	SND_SOC_DAPM_MIXER("SLIMBUS_5_RX Voice Mixer", SND_SOC_NOPM, 0, 0,
			   slimbus_5_rx_mixer_controls,
			   ARRAY_SIZE(slimbus_5_rx_mixer_controls)),
	SND_SOC_DAPM_MIXER("SLIMBUS_6_RX Voice Mixer", SND_SOC_NOPM, 0, 0,
			   slimbus_6_rx_mixer_controls,
			   ARRAY_SIZE(slimbus_6_rx_mixer_controls)),
	SND_SOC_DAPM_MIXER("PRIMARY_TDM_RX_0 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   primary_tdm_rx_0_mixer_controls,
			   ARRAY_SIZE(primary_tdm_rx_0_mixer_controls)),
	SND_SOC_DAPM_MIXER("PRIMARY_TDM_RX_1 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   primary_tdm_rx_1_mixer_controls,
			   ARRAY_SIZE(primary_tdm_rx_1_mixer_controls)),
	SND_SOC_DAPM_MIXER("PRIMARY_TDM_RX_2 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   primary_tdm_rx_2_mixer_controls,
			   ARRAY_SIZE(primary_tdm_rx_2_mixer_controls)),
	SND_SOC_DAPM_MIXER("PRIMARY_TDM_RX_3 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   primary_tdm_rx_3_mixer_controls,
			   ARRAY_SIZE(primary_tdm_rx_3_mixer_controls)),
	SND_SOC_DAPM_MIXER("PRIMARY_TDM_RX_4 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   primary_tdm_rx_4_mixer_controls,
			   ARRAY_SIZE(primary_tdm_rx_4_mixer_controls)),
	SND_SOC_DAPM_MIXER("PRIMARY_TDM_RX_5 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   primary_tdm_rx_5_mixer_controls,
			   ARRAY_SIZE(primary_tdm_rx_5_mixer_controls)),
	SND_SOC_DAPM_MIXER("PRIMARY_TDM_RX_6 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   primary_tdm_rx_6_mixer_controls,
			   ARRAY_SIZE(primary_tdm_rx_6_mixer_controls)),
	SND_SOC_DAPM_MIXER("PRIMARY_TDM_RX_7 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   primary_tdm_rx_7_mixer_controls,
			   ARRAY_SIZE(primary_tdm_rx_7_mixer_controls)),
	SND_SOC_DAPM_MIXER("SEC_TDM_RX_0 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   secondary_tdm_rx_0_mixer_controls,
			   ARRAY_SIZE(secondary_tdm_rx_0_mixer_controls)),
	SND_SOC_DAPM_MIXER("SEC_TDM_RX_1 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   secondary_tdm_rx_1_mixer_controls,
			   ARRAY_SIZE(secondary_tdm_rx_1_mixer_controls)),
	SND_SOC_DAPM_MIXER("SEC_TDM_RX_2 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   secondary_tdm_rx_2_mixer_controls,
			   ARRAY_SIZE(secondary_tdm_rx_2_mixer_controls)),
	SND_SOC_DAPM_MIXER("SEC_TDM_RX_3 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   secondary_tdm_rx_3_mixer_controls,
			   ARRAY_SIZE(secondary_tdm_rx_3_mixer_controls)),
	SND_SOC_DAPM_MIXER("SEC_TDM_RX_4 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   secondary_tdm_rx_4_mixer_controls,
			   ARRAY_SIZE(secondary_tdm_rx_4_mixer_controls)),
	SND_SOC_DAPM_MIXER("SEC_TDM_RX_5 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   secondary_tdm_rx_5_mixer_controls,
			   ARRAY_SIZE(secondary_tdm_rx_5_mixer_controls)),
	SND_SOC_DAPM_MIXER("SEC_TDM_RX_6 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   secondary_tdm_rx_6_mixer_controls,
			   ARRAY_SIZE(secondary_tdm_rx_6_mixer_controls)),
	SND_SOC_DAPM_MIXER("SEC_TDM_RX_7 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   secondary_tdm_rx_7_mixer_controls,
			   ARRAY_SIZE(secondary_tdm_rx_7_mixer_controls)),
	SND_SOC_DAPM_MIXER("TERT_TDM_RX_0 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   tertiary_tdm_rx_0_mixer_controls,
			   ARRAY_SIZE(tertiary_tdm_rx_0_mixer_controls)),
	SND_SOC_DAPM_MIXER("TERT_TDM_RX_1 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   tertiary_tdm_rx_1_mixer_controls,
			   ARRAY_SIZE(tertiary_tdm_rx_1_mixer_controls)),
	SND_SOC_DAPM_MIXER("TERT_TDM_RX_2 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   tertiary_tdm_rx_2_mixer_controls,
			   ARRAY_SIZE(tertiary_tdm_rx_2_mixer_controls)),
	SND_SOC_DAPM_MIXER("TERT_TDM_RX_3 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   tertiary_tdm_rx_3_mixer_controls,
			   ARRAY_SIZE(tertiary_tdm_rx_3_mixer_controls)),
	SND_SOC_DAPM_MIXER("TERT_TDM_RX_4 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   tertiary_tdm_rx_4_mixer_controls,
			   ARRAY_SIZE(tertiary_tdm_rx_4_mixer_controls)),
	SND_SOC_DAPM_MIXER("TERT_TDM_RX_5 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   tertiary_tdm_rx_5_mixer_controls,
			   ARRAY_SIZE(tertiary_tdm_rx_5_mixer_controls)),
	SND_SOC_DAPM_MIXER("TERT_TDM_RX_6 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   tertiary_tdm_rx_6_mixer_controls,
			   ARRAY_SIZE(tertiary_tdm_rx_6_mixer_controls)),
	SND_SOC_DAPM_MIXER("TERT_TDM_RX_7 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   tertiary_tdm_rx_7_mixer_controls,
			   ARRAY_SIZE(tertiary_tdm_rx_7_mixer_controls)),
	SND_SOC_DAPM_MIXER("QUAT_TDM_RX_0 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   quaternary_tdm_rx_0_mixer_controls,
			   ARRAY_SIZE(quaternary_tdm_rx_0_mixer_controls)),
	SND_SOC_DAPM_MIXER("QUAT_TDM_RX_1 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   quaternary_tdm_rx_1_mixer_controls,
			   ARRAY_SIZE(quaternary_tdm_rx_1_mixer_controls)),
	SND_SOC_DAPM_MIXER("QUAT_TDM_RX_2 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   quaternary_tdm_rx_2_mixer_controls,
			   ARRAY_SIZE(quaternary_tdm_rx_2_mixer_controls)),
	SND_SOC_DAPM_MIXER("QUAT_TDM_RX_3 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   quaternary_tdm_rx_3_mixer_controls,
			   ARRAY_SIZE(quaternary_tdm_rx_3_mixer_controls)),
	SND_SOC_DAPM_MIXER("QUAT_TDM_RX_4 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   quaternary_tdm_rx_4_mixer_controls,
			   ARRAY_SIZE(quaternary_tdm_rx_4_mixer_controls)),
	SND_SOC_DAPM_MIXER("QUAT_TDM_RX_5 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   quaternary_tdm_rx_5_mixer_controls,
			   ARRAY_SIZE(quaternary_tdm_rx_5_mixer_controls)),
	SND_SOC_DAPM_MIXER("QUAT_TDM_RX_6 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   quaternary_tdm_rx_6_mixer_controls,
			   ARRAY_SIZE(quaternary_tdm_rx_6_mixer_controls)),
	SND_SOC_DAPM_MIXER("QUAT_TDM_RX_7 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   quaternary_tdm_rx_7_mixer_controls,
			   ARRAY_SIZE(quaternary_tdm_rx_7_mixer_controls)),
	SND_SOC_DAPM_MIXER("QUIN_TDM_RX_0 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   quinary_tdm_rx_0_mixer_controls,
			   ARRAY_SIZE(quinary_tdm_rx_0_mixer_controls)),
	SND_SOC_DAPM_MIXER("QUIN_TDM_RX_1 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   quinary_tdm_rx_1_mixer_controls,
			   ARRAY_SIZE(quinary_tdm_rx_1_mixer_controls)),
	SND_SOC_DAPM_MIXER("QUIN_TDM_RX_2 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   quinary_tdm_rx_2_mixer_controls,
			   ARRAY_SIZE(quinary_tdm_rx_2_mixer_controls)),
	SND_SOC_DAPM_MIXER("QUIN_TDM_RX_3 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   quinary_tdm_rx_3_mixer_controls,
			   ARRAY_SIZE(quinary_tdm_rx_3_mixer_controls)),
	SND_SOC_DAPM_MIXER("QUIN_TDM_RX_4 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   quinary_tdm_rx_4_mixer_controls,
			   ARRAY_SIZE(quinary_tdm_rx_4_mixer_controls)),
	SND_SOC_DAPM_MIXER("QUIN_TDM_RX_5 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   quinary_tdm_rx_5_mixer_controls,
			   ARRAY_SIZE(quinary_tdm_rx_5_mixer_controls)),
	SND_SOC_DAPM_MIXER("QUIN_TDM_RX_6 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   quinary_tdm_rx_6_mixer_controls,
			   ARRAY_SIZE(quinary_tdm_rx_6_mixer_controls)),
	SND_SOC_DAPM_MIXER("QUIN_TDM_RX_7 Voice Mixer", SND_SOC_NOPM, 0, 0,
			   quinary_tdm_rx_7_mixer_controls,
			   ARRAY_SIZE(quinary_tdm_rx_7_mixer_controls)),
	SND_SOC_DAPM_MIXER("INT0_MI2S_RX Voice Mixer", SND_SOC_NOPM, 0, 0,
			   int0_mi2s_rx_mixer_controls,
			   ARRAY_SIZE(int0_mi2s_rx_mixer_controls)),
	SND_SOC_DAPM_MIXER("INT1_MI2S_RX Voice Mixer", SND_SOC_NOPM, 0, 0,
			   int1_mi2s_rx_mixer_controls,
			   ARRAY_SIZE(int1_mi2s_rx_mixer_controls)),
	SND_SOC_DAPM_MIXER("INT2_MI2S_RX Voice Mixer", SND_SOC_NOPM, 0, 0,
			   int2_mi2s_rx_mixer_controls,
			   ARRAY_SIZE(int2_mi2s_rx_mixer_controls)),
	SND_SOC_DAPM_MIXER("INT3_MI2S_RX Voice Mixer", SND_SOC_NOPM, 0, 0,
			   int3_mi2s_rx_mixer_controls,
			   ARRAY_SIZE(int3_mi2s_rx_mixer_controls)),
	SND_SOC_DAPM_MIXER("INT4_MI2S_RX Voice Mixer", SND_SOC_NOPM, 0, 0,
			   int4_mi2s_rx_mixer_controls,
			   ARRAY_SIZE(int4_mi2s_rx_mixer_controls)),
	SND_SOC_DAPM_MIXER("INT5_MI2S_RX Voice Mixer", SND_SOC_NOPM, 0, 0,
			   int5_mi2s_rx_mixer_controls,
			   ARRAY_SIZE(int5_mi2s_rx_mixer_controls)),
	SND_SOC_DAPM_MIXER("INT6_MI2S_RX Voice Mixer", SND_SOC_NOPM, 0, 0,
			   int6_mi2s_rx_mixer_controls,
			   ARRAY_SIZE(int6_mi2s_rx_mixer_controls)),
};

static const struct snd_soc_dapm_route q6voice_dapm_routes[] = {
	{ "CS-Voice Capture Mixer",	"PRI_MI2S_TX",	"PRI_MI2S_TX" },
	{ "CS-Voice Capture Mixer",	"SEC_MI2S_TX",	"SEC_MI2S_TX" },
	{ "CS-Voice Capture Mixer",	"TERT_MI2S_TX",	"TERT_MI2S_TX" },
	{ "CS-Voice Capture Mixer",	"QUAT_MI2S_TX",	"QUAT_MI2S_TX" },
	{ "CS-Voice Capture Mixer",	"QUIN_MI2S_TX",	"QUIN_MI2S_TX" },
	{ "CS-Voice Capture Mixer",	"SLIMBUS_0_TX",	"SLIMBUS_0_TX" },
	{ "CS-Voice Capture Mixer",	"SLIMBUS_1_TX",	"SLIMBUS_1_TX" },
	{ "CS-Voice Capture Mixer",	"SLIMBUS_2_TX",	"SLIMBUS_2_TX" },
	{ "CS-Voice Capture Mixer",	"SLIMBUS_3_TX",	"SLIMBUS_3_TX" },
	{ "CS-Voice Capture Mixer",	"SLIMBUS_4_TX",	"SLIMBUS_4_TX" },
	{ "CS-Voice Capture Mixer",	"SLIMBUS_5_TX",	"SLIMBUS_5_TX" },
	{ "CS-Voice Capture Mixer",	"SLIMBUS_6_TX",	"SLIMBUS_6_TX" },
	{ "CS-Voice Capture Mixer",	"PRIMARY_TDM_TX_0", "PRIMARY_TDM_TX_0" },
	{ "CS-Voice Capture Mixer",	"PRIMARY_TDM_TX_1", "PRIMARY_TDM_TX_1" },
	{ "CS-Voice Capture Mixer",	"PRIMARY_TDM_TX_2", "PRIMARY_TDM_TX_2" },
	{ "CS-Voice Capture Mixer",	"PRIMARY_TDM_TX_3", "PRIMARY_TDM_TX_3" },
	{ "CS-Voice Capture Mixer",	"PRIMARY_TDM_TX_4", "PRIMARY_TDM_TX_4" },
	{ "CS-Voice Capture Mixer",	"PRIMARY_TDM_TX_5", "PRIMARY_TDM_TX_5" },
	{ "CS-Voice Capture Mixer",	"PRIMARY_TDM_TX_6", "PRIMARY_TDM_TX_6" },
	{ "CS-Voice Capture Mixer",	"PRIMARY_TDM_TX_7", "PRIMARY_TDM_TX_7" },
	{ "CS-Voice Capture Mixer",	"SEC_TDM_TX_0", "SEC_TDM_TX_0" },
	{ "CS-Voice Capture Mixer",	"SEC_TDM_TX_1", "SEC_TDM_TX_1" },
	{ "CS-Voice Capture Mixer",	"SEC_TDM_TX_2", "SEC_TDM_TX_2" },
	{ "CS-Voice Capture Mixer",	"SEC_TDM_TX_3", "SEC_TDM_TX_3" },
	{ "CS-Voice Capture Mixer",	"SEC_TDM_TX_4", "SEC_TDM_TX_4" },
	{ "CS-Voice Capture Mixer",	"SEC_TDM_TX_5", "SEC_TDM_TX_5" },
	{ "CS-Voice Capture Mixer",	"SEC_TDM_TX_6", "SEC_TDM_TX_6" },
	{ "CS-Voice Capture Mixer",	"SEC_TDM_TX_7", "SEC_TDM_TX_7" },
	{ "CS-Voice Capture Mixer",	"TERT_TDM_TX_0", "TERT_TDM_TX_0" },
	{ "CS-Voice Capture Mixer",	"TERT_TDM_TX_1", "TERT_TDM_TX_1" },
	{ "CS-Voice Capture Mixer",	"TERT_TDM_TX_2", "TERT_TDM_TX_2" },
	{ "CS-Voice Capture Mixer",	"TERT_TDM_TX_3", "TERT_TDM_TX_3" },
	{ "CS-Voice Capture Mixer",	"TERT_TDM_TX_4", "TERT_TDM_TX_4" },
	{ "CS-Voice Capture Mixer",	"TERT_TDM_TX_5", "TERT_TDM_TX_5" },
	{ "CS-Voice Capture Mixer",	"TERT_TDM_TX_6", "TERT_TDM_TX_6" },
	{ "CS-Voice Capture Mixer",	"TERT_TDM_TX_7", "TERT_TDM_TX_7" },
	{ "CS-Voice Capture Mixer",	"QUAT_TDM_TX_0", "QUAT_TDM_TX_0" },
	{ "CS-Voice Capture Mixer",	"QUAT_TDM_TX_1", "QUAT_TDM_TX_1" },
	{ "CS-Voice Capture Mixer",	"QUAT_TDM_TX_2", "QUAT_TDM_TX_2" },
	{ "CS-Voice Capture Mixer",	"QUAT_TDM_TX_3", "QUAT_TDM_TX_3" },
	{ "CS-Voice Capture Mixer",	"QUAT_TDM_TX_4", "QUAT_TDM_TX_4" },
	{ "CS-Voice Capture Mixer",	"QUAT_TDM_TX_5", "QUAT_TDM_TX_5" },
	{ "CS-Voice Capture Mixer",	"QUAT_TDM_TX_6", "QUAT_TDM_TX_6" },
	{ "CS-Voice Capture Mixer",	"QUAT_TDM_TX_7", "QUAT_TDM_TX_7" },
	{ "CS-Voice Capture Mixer",	"QUIN_TDM_TX_0", "QUIN_TDM_TX_0" },
	{ "CS-Voice Capture Mixer",	"QUIN_TDM_TX_1", "QUIN_TDM_TX_1" },
	{ "CS-Voice Capture Mixer",	"QUIN_TDM_TX_2", "QUIN_TDM_TX_2" },
	{ "CS-Voice Capture Mixer",	"QUIN_TDM_TX_3", "QUIN_TDM_TX_3" },
	{ "CS-Voice Capture Mixer",	"QUIN_TDM_TX_4", "QUIN_TDM_TX_4" },
	{ "CS-Voice Capture Mixer",	"QUIN_TDM_TX_5", "QUIN_TDM_TX_5" },
	{ "CS-Voice Capture Mixer",	"QUIN_TDM_TX_6", "QUIN_TDM_TX_6" },
	{ "CS-Voice Capture Mixer",	"QUIN_TDM_TX_7", "QUIN_TDM_TX_7" },
	{ "CS-Voice Capture Mixer",	"INT0_MI2S_TX", "INT0_MI2S_TX" },
	{ "CS-Voice Capture Mixer",	"INT1_MI2S_TX", "INT1_MI2S_TX" },
	{ "CS-Voice Capture Mixer",	"INT2_MI2S_TX", "INT2_MI2S_TX" },
	{ "CS-Voice Capture Mixer",	"INT3_MI2S_TX", "INT3_MI2S_TX" },
	{ "CS-Voice Capture Mixer",	"INT4_MI2S_TX", "INT4_MI2S_TX" },
	{ "CS-Voice Capture Mixer",	"INT5_MI2S_TX", "INT5_MI2S_TX" },
	{ "CS-Voice Capture Mixer",	"INT6_MI2S_TX", "INT6_MI2S_TX" },
	{ "CS-VOICE_UL1",		NULL,		"CS-Voice Capture Mixer" },
	{ "VoiceMMode1 Capture Mixer",	"PRI_MI2S_TX",	"PRI_MI2S_TX" },
	{ "VoiceMMode1 Capture Mixer",	"SEC_MI2S_TX",	"SEC_MI2S_TX" },
	{ "VoiceMMode1 Capture Mixer",	"TERT_MI2S_TX",	"TERT_MI2S_TX" },
	{ "VoiceMMode1 Capture Mixer",	"QUAT_MI2S_TX",	"QUAT_MI2S_TX" },
	{ "VoiceMMode1 Capture Mixer",	"QUIN_MI2S_TX",	"QUIN_MI2S_TX" },
	{ "VoiceMMode1 Capture Mixer",	"SLIMBUS_0_TX",	"SLIMBUS_0_TX" },
	{ "VoiceMMode1 Capture Mixer",	"SLIMBUS_1_TX",	"SLIMBUS_1_TX" },
	{ "VoiceMMode1 Capture Mixer",	"SLIMBUS_2_TX",	"SLIMBUS_2_TX" },
	{ "VoiceMMode1 Capture Mixer",	"SLIMBUS_3_TX",	"SLIMBUS_3_TX" },
	{ "VoiceMMode1 Capture Mixer",	"SLIMBUS_4_TX",	"SLIMBUS_4_TX" },
	{ "VoiceMMode1 Capture Mixer",	"SLIMBUS_5_TX",	"SLIMBUS_5_TX" },
	{ "VoiceMMode1 Capture Mixer",	"SLIMBUS_6_TX",	"SLIMBUS_6_TX" },
	{ "VoiceMMode1 Capture Mixer",	"PRIMARY_TDM_TX_0", "PRIMARY_TDM_TX_0" },
	{ "VoiceMMode1 Capture Mixer",	"PRIMARY_TDM_TX_1", "PRIMARY_TDM_TX_1" },
	{ "VoiceMMode1 Capture Mixer",	"PRIMARY_TDM_TX_2", "PRIMARY_TDM_TX_2" },
	{ "VoiceMMode1 Capture Mixer",	"PRIMARY_TDM_TX_3", "PRIMARY_TDM_TX_3" },
	{ "VoiceMMode1 Capture Mixer",	"PRIMARY_TDM_TX_4", "PRIMARY_TDM_TX_4" },
	{ "VoiceMMode1 Capture Mixer",	"PRIMARY_TDM_TX_5", "PRIMARY_TDM_TX_5" },
	{ "VoiceMMode1 Capture Mixer",	"PRIMARY_TDM_TX_6", "PRIMARY_TDM_TX_6" },
	{ "VoiceMMode1 Capture Mixer",	"PRIMARY_TDM_TX_7", "PRIMARY_TDM_TX_7" },
	{ "VoiceMMode1 Capture Mixer",	"SEC_TDM_TX_0", "SEC_TDM_TX_0" },
	{ "VoiceMMode1 Capture Mixer",	"SEC_TDM_TX_1", "SEC_TDM_TX_1" },
	{ "VoiceMMode1 Capture Mixer",	"SEC_TDM_TX_2", "SEC_TDM_TX_2" },
	{ "VoiceMMode1 Capture Mixer",	"SEC_TDM_TX_3", "SEC_TDM_TX_3" },
	{ "VoiceMMode1 Capture Mixer",	"SEC_TDM_TX_4", "SEC_TDM_TX_4" },
	{ "VoiceMMode1 Capture Mixer",	"SEC_TDM_TX_5", "SEC_TDM_TX_5" },
	{ "VoiceMMode1 Capture Mixer",	"SEC_TDM_TX_6", "SEC_TDM_TX_6" },
	{ "VoiceMMode1 Capture Mixer",	"SEC_TDM_TX_7", "SEC_TDM_TX_7" },
	{ "VoiceMMode1 Capture Mixer",	"TERT_TDM_TX_0", "TERT_TDM_TX_0" },
	{ "VoiceMMode1 Capture Mixer",	"TERT_TDM_TX_1", "TERT_TDM_TX_1" },
	{ "VoiceMMode1 Capture Mixer",	"TERT_TDM_TX_2", "TERT_TDM_TX_2" },
	{ "VoiceMMode1 Capture Mixer",	"TERT_TDM_TX_3", "TERT_TDM_TX_3" },
	{ "VoiceMMode1 Capture Mixer",	"TERT_TDM_TX_4", "TERT_TDM_TX_4" },
	{ "VoiceMMode1 Capture Mixer",	"TERT_TDM_TX_5", "TERT_TDM_TX_5" },
	{ "VoiceMMode1 Capture Mixer",	"TERT_TDM_TX_6", "TERT_TDM_TX_6" },
	{ "VoiceMMode1 Capture Mixer",	"TERT_TDM_TX_7", "TERT_TDM_TX_7" },
	{ "VoiceMMode1 Capture Mixer",	"QUAT_TDM_TX_0", "QUAT_TDM_TX_0" },
	{ "VoiceMMode1 Capture Mixer",	"QUAT_TDM_TX_1", "QUAT_TDM_TX_1" },
	{ "VoiceMMode1 Capture Mixer",	"QUAT_TDM_TX_2", "QUAT_TDM_TX_2" },
	{ "VoiceMMode1 Capture Mixer",	"QUAT_TDM_TX_3", "QUAT_TDM_TX_3" },
	{ "VoiceMMode1 Capture Mixer",	"QUAT_TDM_TX_4", "QUAT_TDM_TX_4" },
	{ "VoiceMMode1 Capture Mixer",	"QUAT_TDM_TX_5", "QUAT_TDM_TX_5" },
	{ "VoiceMMode1 Capture Mixer",	"QUAT_TDM_TX_6", "QUAT_TDM_TX_6" },
	{ "VoiceMMode1 Capture Mixer",	"QUAT_TDM_TX_7", "QUAT_TDM_TX_7" },
	{ "VoiceMMode1 Capture Mixer",	"QUIN_TDM_TX_0", "QUIN_TDM_TX_0" },
	{ "VoiceMMode1 Capture Mixer",	"QUIN_TDM_TX_1", "QUIN_TDM_TX_1" },
	{ "VoiceMMode1 Capture Mixer",	"QUIN_TDM_TX_2", "QUIN_TDM_TX_2" },
	{ "VoiceMMode1 Capture Mixer",	"QUIN_TDM_TX_3", "QUIN_TDM_TX_3" },
	{ "VoiceMMode1 Capture Mixer",	"QUIN_TDM_TX_4", "QUIN_TDM_TX_4" },
	{ "VoiceMMode1 Capture Mixer",	"QUIN_TDM_TX_5", "QUIN_TDM_TX_5" },
	{ "VoiceMMode1 Capture Mixer",	"QUIN_TDM_TX_6", "QUIN_TDM_TX_6" },
	{ "VoiceMMode1 Capture Mixer",	"QUIN_TDM_TX_7", "QUIN_TDM_TX_7" },
	{ "VoiceMMode1 Capture Mixer",	"INT0_MI2S_TX",	"INT0_MI2S_TX" },
	{ "VoiceMMode1 Capture Mixer",	"INT1_MI2S_TX",	"INT1_MI2S_TX" },
	{ "VoiceMMode1 Capture Mixer",	"INT2_MI2S_TX",	"INT2_MI2S_TX" },
	{ "VoiceMMode1 Capture Mixer",	"INT3_MI2S_TX",	"INT3_MI2S_TX" },
	{ "VoiceMMode1 Capture Mixer",	"INT4_MI2S_TX",	"INT4_MI2S_TX" },
	{ "VoiceMMode1 Capture Mixer",	"INT5_MI2S_TX",	"INT5_MI2S_TX" },
	{ "VoiceMMode1 Capture Mixer",	"INT6_MI2S_TX",	"INT6_MI2S_TX" },
	{ "VOICEMMODE1_UL1",		NULL,		"VoiceMMode1 Capture Mixer" },

	{ "PRI_MI2S_RX Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "SEC_MI2S_RX Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "TERT_MI2S_RX Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "QUAT_MI2S_RX Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "QUIN_MI2S_RX Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "SLIMBUS_0_RX Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "SLIMBUS_1_RX Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "SLIMBUS_2_RX Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "SLIMBUS_3_RX Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "SLIMBUS_4_RX Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "SLIMBUS_5_RX Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "SLIMBUS_6_RX Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "PRIMARY_TDM_RX_0 Voice Mixer", "CS-Voice",	"CS-VOICE_DL1" },
	{ "PRIMARY_TDM_RX_1 Voice Mixer", "CS-Voice",	"CS-VOICE_DL1" },
	{ "PRIMARY_TDM_RX_2 Voice Mixer", "CS-Voice",	"CS-VOICE_DL1" },
	{ "PRIMARY_TDM_RX_3 Voice Mixer", "CS-Voice",	"CS-VOICE_DL1" },
	{ "PRIMARY_TDM_RX_4 Voice Mixer", "CS-Voice",	"CS-VOICE_DL1" },
	{ "PRIMARY_TDM_RX_5 Voice Mixer", "CS-Voice",	"CS-VOICE_DL1" },
	{ "PRIMARY_TDM_RX_6 Voice Mixer", "CS-Voice",	"CS-VOICE_DL1" },
	{ "PRIMARY_TDM_RX_7 Voice Mixer", "CS-Voice",	"CS-VOICE_DL1" },
	{ "SEC_TDM_RX_0 Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "SEC_TDM_RX_1 Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "SEC_TDM_RX_2 Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "SEC_TDM_RX_3 Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "SEC_TDM_RX_4 Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "SEC_TDM_RX_5 Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "SEC_TDM_RX_6 Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "SEC_TDM_RX_7 Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "TERT_TDM_RX_0 Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "TERT_TDM_RX_1 Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "TERT_TDM_RX_2 Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "TERT_TDM_RX_3 Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "TERT_TDM_RX_4 Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "TERT_TDM_RX_5 Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "TERT_TDM_RX_6 Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "TERT_TDM_RX_7 Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "QUAT_TDM_RX_0 Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "QUAT_TDM_RX_1 Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "QUAT_TDM_RX_2 Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "QUAT_TDM_RX_3 Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "QUAT_TDM_RX_4 Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "QUAT_TDM_RX_5 Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "QUAT_TDM_RX_6 Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "QUAT_TDM_RX_7 Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "QUIN_TDM_RX_0 Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "QUIN_TDM_RX_1 Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "QUIN_TDM_RX_2 Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "QUIN_TDM_RX_3 Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "QUIN_TDM_RX_4 Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "QUIN_TDM_RX_5 Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "QUIN_TDM_RX_6 Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "QUIN_TDM_RX_7 Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "INT0_MI2S_RX Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "INT1_MI2S_RX Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "INT2_MI2S_RX Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "INT3_MI2S_RX Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "INT4_MI2S_RX Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "INT5_MI2S_RX Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },
	{ "INT6_MI2S_RX Voice Mixer",	"CS-Voice",	"CS-VOICE_DL1" },

	{ "PRI_MI2S_RX Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "SEC_MI2S_RX Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "TERT_MI2S_RX Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "QUAT_MI2S_RX Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "QUIN_MI2S_RX Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "SLIMBUS_0_RX Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "SLIMBUS_1_RX Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "SLIMBUS_2_RX Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "SLIMBUS_3_RX Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "SLIMBUS_4_RX Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "SLIMBUS_5_RX Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "SLIMBUS_6_RX Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "PRIMARY_TDM_RX_0 Voice Mixer", "VoiceMMode1", "VOICEMMODE1_DL1" },
	{ "PRIMARY_TDM_RX_1 Voice Mixer", "VoiceMMode1", "VOICEMMODE1_DL1" },
	{ "PRIMARY_TDM_RX_2 Voice Mixer", "VoiceMMode1", "VOICEMMODE1_DL1" },
	{ "PRIMARY_TDM_RX_3 Voice Mixer", "VoiceMMode1", "VOICEMMODE1_DL1" },
	{ "PRIMARY_TDM_RX_4 Voice Mixer", "VoiceMMode1", "VOICEMMODE1_DL1" },
	{ "PRIMARY_TDM_RX_5 Voice Mixer", "VoiceMMode1", "VOICEMMODE1_DL1" },
	{ "PRIMARY_TDM_RX_6 Voice Mixer", "VoiceMMode1", "VOICEMMODE1_DL1" },
	{ "PRIMARY_TDM_RX_7 Voice Mixer", "VoiceMMode1", "VOICEMMODE1_DL1" },
	{ "SEC_TDM_RX_0 Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "SEC_TDM_RX_1 Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "SEC_TDM_RX_2 Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "SEC_TDM_RX_3 Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "SEC_TDM_RX_4 Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "SEC_TDM_RX_5 Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "SEC_TDM_RX_6 Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "SEC_TDM_RX_7 Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "TERT_TDM_RX_0 Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "TERT_TDM_RX_1 Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "TERT_TDM_RX_2 Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "TERT_TDM_RX_3 Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "TERT_TDM_RX_4 Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "TERT_TDM_RX_5 Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "TERT_TDM_RX_6 Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "TERT_TDM_RX_7 Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "QUAT_TDM_RX_0 Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "QUAT_TDM_RX_1 Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "QUAT_TDM_RX_2 Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "QUAT_TDM_RX_3 Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "QUAT_TDM_RX_4 Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "QUAT_TDM_RX_5 Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "QUAT_TDM_RX_6 Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "QUAT_TDM_RX_7 Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "QUIN_TDM_RX_0 Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "QUIN_TDM_RX_1 Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "QUIN_TDM_RX_2 Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "QUIN_TDM_RX_3 Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "QUIN_TDM_RX_4 Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "QUIN_TDM_RX_5 Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "QUIN_TDM_RX_6 Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "QUIN_TDM_RX_7 Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "INT0_MI2S_RX Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "INT1_MI2S_RX Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "INT2_MI2S_RX Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "INT3_MI2S_RX Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "INT4_MI2S_RX Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "INT5_MI2S_RX Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },
	{ "INT6_MI2S_RX Voice Mixer",	"VoiceMMode1",	"VOICEMMODE1_DL1" },

	{ "PRI_MI2S_RX",		NULL,		"PRI_MI2S_RX Voice Mixer" },
	{ "SEC_MI2S_RX",		NULL,		"SEC_MI2S_RX Voice Mixer" },
	{ "TERT_MI2S_RX",		NULL,		"TERT_MI2S_RX Voice Mixer" },
	{ "QUAT_MI2S_RX",		NULL,		"QUAT_MI2S_RX Voice Mixer" },
	{ "QUIN_MI2S_RX",		NULL,		"QUIN_MI2S_RX Voice Mixer" },
	{ "SLIMBUS_0_RX",		NULL,		"SLIMBUS_0_RX Voice Mixer" },
	{ "SLIMBUS_1_RX",		NULL,		"SLIMBUS_1_RX Voice Mixer" },
	{ "SLIMBUS_2_RX",		NULL,		"SLIMBUS_2_RX Voice Mixer" },
	{ "SLIMBUS_3_RX",		NULL,		"SLIMBUS_3_RX Voice Mixer" },
	{ "SLIMBUS_4_RX",		NULL,		"SLIMBUS_4_RX Voice Mixer" },
	{ "SLIMBUS_5_RX",		NULL,		"SLIMBUS_5_RX Voice Mixer" },
	{ "SLIMBUS_6_RX",		NULL,		"SLIMBUS_6_RX Voice Mixer" },
	{ "PRIMARY_TDM_RX_0",		NULL,		"PRIMARY_TDM_RX_0 Voice Mixer" },
	{ "PRIMARY_TDM_RX_1",		NULL,		"PRIMARY_TDM_RX_1 Voice Mixer" },
	{ "PRIMARY_TDM_RX_2",		NULL,		"PRIMARY_TDM_RX_2 Voice Mixer" },
	{ "PRIMARY_TDM_RX_3",		NULL,		"PRIMARY_TDM_RX_3 Voice Mixer" },
	{ "PRIMARY_TDM_RX_4",		NULL,		"PRIMARY_TDM_RX_4 Voice Mixer" },
	{ "PRIMARY_TDM_RX_5",		NULL,		"PRIMARY_TDM_RX_5 Voice Mixer" },
	{ "PRIMARY_TDM_RX_6",		NULL,		"PRIMARY_TDM_RX_6 Voice Mixer" },
	{ "PRIMARY_TDM_RX_7",		NULL,		"PRIMARY_TDM_RX_7 Voice Mixer" },
	{ "SEC_TDM_RX_0",		NULL,		"SEC_TDM_RX_0 Voice Mixer" },
	{ "SEC_TDM_RX_1",		NULL,		"SEC_TDM_RX_1 Voice Mixer" },
	{ "SEC_TDM_RX_2",		NULL,		"SEC_TDM_RX_2 Voice Mixer" },
	{ "SEC_TDM_RX_3",		NULL,		"SEC_TDM_RX_3 Voice Mixer" },
	{ "SEC_TDM_RX_4",		NULL,		"SEC_TDM_RX_4 Voice Mixer" },
	{ "SEC_TDM_RX_5",		NULL,		"SEC_TDM_RX_5 Voice Mixer" },
	{ "SEC_TDM_RX_6",		NULL,		"SEC_TDM_RX_6 Voice Mixer" },
	{ "SEC_TDM_RX_7",		NULL,		"SEC_TDM_RX_7 Voice Mixer" },
	{ "TERT_TDM_RX_0",		NULL,		"TERT_TDM_RX_0 Voice Mixer" },
	{ "TERT_TDM_RX_1",		NULL,		"TERT_TDM_RX_1 Voice Mixer" },
	{ "TERT_TDM_RX_2",		NULL,		"TERT_TDM_RX_2 Voice Mixer" },
	{ "TERT_TDM_RX_3",		NULL,		"TERT_TDM_RX_3 Voice Mixer" },
	{ "TERT_TDM_RX_4",		NULL,		"TERT_TDM_RX_4 Voice Mixer" },
	{ "TERT_TDM_RX_5",		NULL,		"TERT_TDM_RX_5 Voice Mixer" },
	{ "TERT_TDM_RX_6",		NULL,		"TERT_TDM_RX_6 Voice Mixer" },
	{ "TERT_TDM_RX_7",		NULL,		"TERT_TDM_RX_7 Voice Mixer" },
	{ "QUAT_TDM_RX_0",		NULL,		"QUAT_TDM_RX_0 Voice Mixer" },
	{ "QUAT_TDM_RX_1",		NULL,		"QUAT_TDM_RX_1 Voice Mixer" },
	{ "QUAT_TDM_RX_2",		NULL,		"QUAT_TDM_RX_2 Voice Mixer" },
	{ "QUAT_TDM_RX_3",		NULL,		"QUAT_TDM_RX_3 Voice Mixer" },
	{ "QUAT_TDM_RX_4",		NULL,		"QUAT_TDM_RX_4 Voice Mixer" },
	{ "QUAT_TDM_RX_5",		NULL,		"QUAT_TDM_RX_5 Voice Mixer" },
	{ "QUAT_TDM_RX_6",		NULL,		"QUAT_TDM_RX_6 Voice Mixer" },
	{ "QUAT_TDM_RX_7",		NULL,		"QUAT_TDM_RX_7 Voice Mixer" },
	{ "QUIN_TDM_RX_0",		NULL,		"QUIN_TDM_RX_0 Voice Mixer" },
	{ "QUIN_TDM_RX_1",		NULL,		"QUIN_TDM_RX_1 Voice Mixer" },
	{ "QUIN_TDM_RX_2",		NULL,		"QUIN_TDM_RX_2 Voice Mixer" },
	{ "QUIN_TDM_RX_3",		NULL,		"QUIN_TDM_RX_3 Voice Mixer" },
	{ "QUIN_TDM_RX_4",		NULL,		"QUIN_TDM_RX_4 Voice Mixer" },
	{ "QUIN_TDM_RX_5",		NULL,		"QUIN_TDM_RX_5 Voice Mixer" },
	{ "QUIN_TDM_RX_6",		NULL,		"QUIN_TDM_RX_6 Voice Mixer" },
	{ "QUIN_TDM_RX_7",		NULL,		"QUIN_TDM_RX_7 Voice Mixer" },
	{ "INT0_MI2S_RX",		NULL,		"INT0_MI2S_RX Voice Mixer" },
	{ "INT1_MI2S_RX",		NULL,		"INT1_MI2S_RX Voice Mixer" },
	{ "INT2_MI2S_RX",		NULL,		"INT2_MI2S_RX Voice Mixer" },
	{ "INT3_MI2S_RX",		NULL,		"INT3_MI2S_RX Voice Mixer" },
	{ "INT4_MI2S_RX",		NULL,		"INT4_MI2S_RX Voice Mixer" },
	{ "INT5_MI2S_RX",		NULL,		"INT5_MI2S_RX Voice Mixer" },
	{ "INT6_MI2S_RX",		NULL,		"INT6_MI2S_RX Voice Mixer" },
};

static unsigned int q6voice_reg_read(struct snd_soc_component *component,
				     unsigned int reg)
{
	/* default value */
	return 0;
}

static int q6voice_reg_write(struct snd_soc_component *component,
			     unsigned int reg, unsigned int val)
{
	/* dummy */
	return 0;
}

static const struct snd_soc_component_driver q6voice_dai_component = {
	.name = DRV_NAME,
	.open = q6voice_dai_open,

	.controls = q6voice_kcontrols,
	.num_controls = ARRAY_SIZE(q6voice_kcontrols),
	.dapm_widgets = q6voice_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(q6voice_dapm_widgets),
	.dapm_routes = q6voice_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(q6voice_dapm_routes),
	.read = q6voice_reg_read,
	.write = q6voice_reg_write,

	/* Needs to probe after q6afe */
	.probe_order = SND_SOC_COMP_ORDER_LATE,
};

static int q6voice_dai_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct q6voice *v;
	struct device_node *np = dev_of_node(dev);
	bool cvd_v2_3;

	cvd_v2_3 = of_property_read_bool(np, "qcom,cvd-v2.3");

	v = q6voice_create(dev, cvd_v2_3);
	if (IS_ERR(v))
		return PTR_ERR(v);

	dev_set_drvdata(dev, v);

	return devm_snd_soc_register_component(dev, &q6voice_dai_component,
					       q6voice_dais,
					       ARRAY_SIZE(q6voice_dais));
}

static const struct of_device_id q6voice_dai_device_id[] = {
	{ .compatible = "qcom,q6voice-dais" },
	{},
};
MODULE_DEVICE_TABLE(of, q6voice_dai_device_id);

static struct platform_driver q6voice_dai_platform_driver = {
	.driver = {
		.name = "q6voice-dai",
		.of_match_table = of_match_ptr(q6voice_dai_device_id),
	},
	.probe = q6voice_dai_probe,
};
module_platform_driver(q6voice_dai_platform_driver);

MODULE_AUTHOR("Stephan Gerhold <stephan@gerhold.net>");
MODULE_DESCRIPTION("Q6Voice DAI driver");
MODULE_LICENSE("GPL");
