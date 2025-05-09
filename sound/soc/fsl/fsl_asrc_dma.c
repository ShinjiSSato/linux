// SPDX-License-Identifier: GPL-2.0
//
// Freescale ASRC ALSA SoC Platform (DMA) driver
//
// Copyright (C) 2014 Freescale Semiconductor, Inc.
//
// Author: Nicolin Chen <nicoleotsuka@gmail.com>

#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/dma/imx-dma.h>
#include <sound/dmaengine_pcm.h>
#include <sound/pcm_params.h>

#include "fsl_asrc_common.h"

#define FSL_ASRC_DMABUF_SIZE	(256 * 1024)

static struct snd_pcm_hardware snd_imx_hardware = {
	.info = SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID,
	.buffer_bytes_max = FSL_ASRC_DMABUF_SIZE,
	.period_bytes_min = 128,
	.period_bytes_max = 65535, /* Limited by SDMA engine */
	.periods_min = 2,
	.periods_max = 255,
	.fifo_size = 0,
};

static bool filter(struct dma_chan *chan, void *param)
{
	if (!imx_dma_is_general_purpose(chan))
		return false;

	chan->private = param;

	return true;
}

static void fsl_asrc_dma_complete(void *arg)
{
	struct snd_pcm_substream *substream = arg;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct fsl_asrc_pair *pair = runtime->private_data;

	pair->pos += snd_pcm_lib_period_bytes(substream);
	if (pair->pos >= snd_pcm_lib_buffer_bytes(substream))
		pair->pos = 0;

	snd_pcm_period_elapsed(substream);
}

static int fsl_asrc_dma_prepare_and_submit(struct snd_pcm_substream *substream,
					   struct snd_soc_component *component)
{
	u8 dir = substream->stream == SNDRV_PCM_STREAM_PLAYBACK ? OUT : IN;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct fsl_asrc_pair *pair = runtime->private_data;
	struct device *dev = component->dev;
	unsigned long flags = DMA_CTRL_ACK;

	/* Prepare and submit Front-End DMA channel */
	if (!substream->runtime->no_period_wakeup)
		flags |= DMA_PREP_INTERRUPT;

	pair->pos = 0;
	pair->desc[!dir] = dmaengine_prep_dma_cyclic(
			pair->dma_chan[!dir], runtime->dma_addr,
			snd_pcm_lib_buffer_bytes(substream),
			snd_pcm_lib_period_bytes(substream),
			dir == OUT ? DMA_MEM_TO_DEV : DMA_DEV_TO_MEM, flags);
	if (!pair->desc[!dir]) {
		dev_err(dev, "failed to prepare slave DMA for Front-End\n");
		return -ENOMEM;
	}

	pair->desc[!dir]->callback = fsl_asrc_dma_complete;
	pair->desc[!dir]->callback_param = substream;

	dmaengine_submit(pair->desc[!dir]);

	/* Prepare and submit Back-End DMA channel */
	pair->desc[dir] = dmaengine_prep_dma_cyclic(
			pair->dma_chan[dir], 0xffff, 64, 64, DMA_DEV_TO_DEV, 0);
	if (!pair->desc[dir]) {
		dev_err(dev, "failed to prepare slave DMA for Back-End\n");
		return -ENOMEM;
	}

	dmaengine_submit(pair->desc[dir]);

	return 0;
}

static int fsl_asrc_dma_trigger(struct snd_soc_component *component,
				struct snd_pcm_substream *substream, int cmd)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct fsl_asrc_pair *pair = runtime->private_data;
	int ret;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		ret = fsl_asrc_dma_prepare_and_submit(substream, component);
		if (ret)
			return ret;
		dma_async_issue_pending(pair->dma_chan[IN]);
		dma_async_issue_pending(pair->dma_chan[OUT]);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		dmaengine_terminate_async(pair->dma_chan[OUT]);
		dmaengine_terminate_async(pair->dma_chan[IN]);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int fsl_asrc_dma_hw_params(struct snd_soc_component *component,
				  struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *params)
{
	enum dma_slave_buswidth buswidth = DMA_SLAVE_BUSWIDTH_2_BYTES;
	enum sdma_peripheral_type be_peripheral_type = IMX_DMATYPE_SSI;
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	bool tx = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
	struct snd_dmaengine_dai_dma_data *dma_params_fe = NULL;
	struct snd_dmaengine_dai_dma_data *dma_params_be = NULL;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct fsl_asrc_pair *pair = runtime->private_data;
	struct dma_chan *tmp_chan = NULL, *be_chan = NULL;
	struct snd_soc_component *component_be = NULL;
	struct fsl_asrc *asrc = pair->asrc;
	struct dma_slave_config config_fe = {}, config_be = {};
	struct sdma_peripheral_config audio_config;
	enum asrc_pair_index index = pair->index;
	struct device *dev = component->dev;
	struct device_node *of_dma_node;
	int stream = substream->stream;
	struct imx_dma_data *tmp_data;
	struct snd_soc_dpcm *dpcm;
	struct device *dev_be;
	u8 dir = tx ? OUT : IN;
	dma_cap_mask_t mask;
	int ret, width;

	/* Fetch the Back-End dma_data from DPCM */
	for_each_dpcm_be(rtd, stream, dpcm) {
		struct snd_soc_pcm_runtime *be = dpcm->be;
		struct snd_pcm_substream *substream_be;
		struct snd_soc_dai *dai_cpu = snd_soc_rtd_to_cpu(be, 0);
		struct snd_soc_dai *dai_codec = snd_soc_rtd_to_codec(be, 0);
		struct snd_soc_dai *dai;

		if (dpcm->fe != rtd)
			continue;

		/*
		 * With audio graph card, original cpu dai is changed to codec
		 * device in backend, so if cpu dai is dummy device in backend,
		 * get the codec dai device, which is the real hardware device
		 * connected.
		 */
		if (!snd_soc_dai_is_dummy(dai_cpu))
			dai = dai_cpu;
		else
			dai = dai_codec;

		substream_be = snd_soc_dpcm_get_substream(be, stream);
		dma_params_be = snd_soc_dai_get_dma_data(dai, substream_be);
		dev_be = dai->dev;
		break;
	}

	if (!dma_params_be) {
		dev_err(dev, "failed to get the substream of Back-End\n");
		return -EINVAL;
	}

	/* Override dma_data of the Front-End and config its dmaengine */
	dma_params_fe = snd_soc_dai_get_dma_data(snd_soc_rtd_to_cpu(rtd, 0), substream);
	dma_params_fe->addr = asrc->paddr + asrc->get_fifo_addr(!dir, index);
	dma_params_fe->maxburst = dma_params_be->maxburst;

	pair->dma_chan[!dir] = asrc->get_dma_channel(pair, !dir);
	if (!pair->dma_chan[!dir]) {
		dev_err(dev, "failed to request DMA channel\n");
		return -EINVAL;
	}

	ret = snd_dmaengine_pcm_prepare_slave_config(substream, params, &config_fe);
	if (ret) {
		dev_err(dev, "failed to prepare DMA config for Front-End\n");
		return ret;
	}

	ret = dmaengine_slave_config(pair->dma_chan[!dir], &config_fe);
	if (ret) {
		dev_err(dev, "failed to config DMA channel for Front-End\n");
		return ret;
	}

	/* Request and config DMA channel for Back-End */
	dma_cap_zero(mask);
	dma_cap_set(DMA_SLAVE, mask);
	dma_cap_set(DMA_CYCLIC, mask);

	/*
	 * The Back-End device might have already requested a DMA channel,
	 * so try to reuse it first, and then request a new one upon NULL.
	 */
	component_be = snd_soc_lookup_component_nolocked(dev_be, SND_DMAENGINE_PCM_DRV_NAME);
	if (component_be) {
		be_chan = soc_component_to_pcm(component_be)->chan[substream->stream];
		tmp_chan = be_chan;
	}
	if (!tmp_chan) {
		tmp_chan = dma_request_chan(dev_be, tx ? "tx" : "rx");
		if (IS_ERR(tmp_chan)) {
			dev_err(dev, "failed to request DMA channel for Back-End\n");
			return -EINVAL;
		}
	}

	/*
	 * An EDMA DEV_TO_DEV channel is fixed and bound with DMA event of each
	 * peripheral, unlike SDMA channel that is allocated dynamically. So no
	 * need to configure dma_request and dma_request2, but get dma_chan of
	 * Back-End device directly via dma_request_chan.
	 */
	if (!asrc->use_edma) {
		/* Get DMA request of Back-End */
		tmp_data = tmp_chan->private;
		pair->dma_data.dma_request = tmp_data->dma_request;
		be_peripheral_type = tmp_data->peripheral_type;
		if (!be_chan)
			dma_release_channel(tmp_chan);

		/* Get DMA request of Front-End */
		tmp_chan = asrc->get_dma_channel(pair, dir);
		tmp_data = tmp_chan->private;
		pair->dma_data.dma_request2 = tmp_data->dma_request;
		pair->dma_data.peripheral_type = tmp_data->peripheral_type;
		pair->dma_data.priority = tmp_data->priority;
		dma_release_channel(tmp_chan);

		of_dma_node = pair->dma_chan[!dir]->device->dev->of_node;
		pair->dma_chan[dir] =
			__dma_request_channel(&mask, filter, &pair->dma_data,
					      of_dma_node);
		pair->req_dma_chan = true;
	} else {
		pair->dma_chan[dir] = tmp_chan;
		/* Do not flag to release if we are reusing the Back-End one */
		pair->req_dma_chan = !be_chan;
	}

	if (!pair->dma_chan[dir]) {
		dev_err(dev, "failed to request DMA channel for Back-End\n");
		return -EINVAL;
	}

	width = snd_pcm_format_physical_width(asrc->asrc_format);
	if (width < 8 || width > 64)
		return -EINVAL;
	else if (width == 8)
		buswidth = DMA_SLAVE_BUSWIDTH_1_BYTE;
	else if (width == 16)
		buswidth = DMA_SLAVE_BUSWIDTH_2_BYTES;
	else if (width == 24)
		buswidth = DMA_SLAVE_BUSWIDTH_3_BYTES;
	else if (width <= 32)
		buswidth = DMA_SLAVE_BUSWIDTH_4_BYTES;
	else
		buswidth = DMA_SLAVE_BUSWIDTH_8_BYTES;

	config_be.direction = DMA_DEV_TO_DEV;
	config_be.src_addr_width = buswidth;
	config_be.src_maxburst = dma_params_be->maxburst;
	config_be.dst_addr_width = buswidth;
	config_be.dst_maxburst = dma_params_be->maxburst;

	memset(&audio_config, 0, sizeof(audio_config));
	config_be.peripheral_config = &audio_config;
	config_be.peripheral_size  = sizeof(audio_config);

	if (tx && (be_peripheral_type == IMX_DMATYPE_SSI_DUAL ||
		   be_peripheral_type == IMX_DMATYPE_SPDIF))
		audio_config.n_fifos_dst = 2;
	if (!tx && (be_peripheral_type == IMX_DMATYPE_SSI_DUAL ||
		    be_peripheral_type == IMX_DMATYPE_SPDIF))
		audio_config.n_fifos_src = 2;

	if (tx) {
		config_be.src_addr = asrc->paddr + asrc->get_fifo_addr(OUT, index);
		config_be.dst_addr = dma_params_be->addr;
	} else {
		config_be.dst_addr = asrc->paddr + asrc->get_fifo_addr(IN, index);
		config_be.src_addr = dma_params_be->addr;
	}

	ret = dmaengine_slave_config(pair->dma_chan[dir], &config_be);
	if (ret) {
		dev_err(dev, "failed to config DMA channel for Back-End\n");
		if (pair->req_dma_chan)
			dma_release_channel(pair->dma_chan[dir]);
		return ret;
	}

	return 0;
}

static int fsl_asrc_dma_hw_free(struct snd_soc_component *component,
				struct snd_pcm_substream *substream)
{
	bool tx = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct fsl_asrc_pair *pair = runtime->private_data;
	u8 dir = tx ? OUT : IN;

	if (pair->dma_chan[!dir])
		dma_release_channel(pair->dma_chan[!dir]);

	/* release dev_to_dev chan if we aren't reusing the Back-End one */
	if (pair->dma_chan[dir] && pair->req_dma_chan)
		dma_release_channel(pair->dma_chan[dir]);

	pair->dma_chan[!dir] = NULL;
	pair->dma_chan[dir] = NULL;

	return 0;
}

static int fsl_asrc_dma_startup(struct snd_soc_component *component,
				struct snd_pcm_substream *substream)
{
	bool tx = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_dmaengine_dai_dma_data *dma_data;
	struct device *dev = component->dev;
	struct fsl_asrc *asrc = dev_get_drvdata(dev);
	struct fsl_asrc_pair *pair;
	struct dma_chan *tmp_chan = NULL;
	u8 dir = tx ? OUT : IN;
	bool release_pair = true;
	int ret = 0;

	ret = snd_pcm_hw_constraint_integer(substream->runtime,
					    SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0) {
		dev_err(dev, "failed to set pcm hw params periods\n");
		return ret;
	}

	pair = kzalloc(sizeof(*pair) + asrc->pair_priv_size, GFP_KERNEL);
	if (!pair)
		return -ENOMEM;

	pair->asrc = asrc;
	pair->private = (void *)pair + sizeof(struct fsl_asrc_pair);

	runtime->private_data = pair;

	/* Request a dummy pair, which will be released later.
	 * Request pair function needs channel num as input, for this
	 * dummy pair, we just request "1" channel temporarily.
	 */
	ret = asrc->request_pair(1, pair);
	if (ret < 0) {
		dev_err(dev, "failed to request asrc pair\n");
		goto req_pair_err;
	}

	/* Request a dummy dma channel, which will be released later. */
	tmp_chan = asrc->get_dma_channel(pair, dir);
	if (!tmp_chan) {
		dev_err(dev, "failed to get dma channel\n");
		ret = -EINVAL;
		goto dma_chan_err;
	}

	dma_data = snd_soc_dai_get_dma_data(snd_soc_rtd_to_cpu(rtd, 0), substream);

	/* Refine the snd_imx_hardware according to caps of DMA. */
	ret = snd_dmaengine_pcm_refine_runtime_hwparams(substream,
							dma_data,
							&snd_imx_hardware,
							tmp_chan);
	if (ret < 0) {
		dev_err(dev, "failed to refine runtime hwparams\n");
		goto out;
	}

	release_pair = false;
	snd_soc_set_runtime_hwparams(substream, &snd_imx_hardware);

out:
	dma_release_channel(tmp_chan);

dma_chan_err:
	asrc->release_pair(pair);

req_pair_err:
	if (release_pair)
		kfree(pair);

	return ret;
}

static int fsl_asrc_dma_shutdown(struct snd_soc_component *component,
				 struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct fsl_asrc_pair *pair = runtime->private_data;
	struct fsl_asrc *asrc;

	if (!pair)
		return 0;

	asrc = pair->asrc;

	if (asrc->pair[pair->index] == pair)
		asrc->pair[pair->index] = NULL;

	kfree(pair);

	return 0;
}

static snd_pcm_uframes_t
fsl_asrc_dma_pcm_pointer(struct snd_soc_component *component,
			 struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct fsl_asrc_pair *pair = runtime->private_data;

	return bytes_to_frames(substream->runtime, pair->pos);
}

static int fsl_asrc_dma_pcm_new(struct snd_soc_component *component,
				struct snd_soc_pcm_runtime *rtd)
{
	struct snd_card *card = rtd->card->snd_card;
	struct snd_pcm *pcm = rtd->pcm;
	int ret;

	ret = dma_coerce_mask_and_coherent(card->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(card->dev, "failed to set DMA mask\n");
		return ret;
	}

	return snd_pcm_set_fixed_buffer_all(pcm, SNDRV_DMA_TYPE_DEV,
					    card->dev, FSL_ASRC_DMABUF_SIZE);
}

struct snd_soc_component_driver fsl_asrc_component = {
	.name		= DRV_NAME,
	.hw_params	= fsl_asrc_dma_hw_params,
	.hw_free	= fsl_asrc_dma_hw_free,
	.trigger	= fsl_asrc_dma_trigger,
	.open		= fsl_asrc_dma_startup,
	.close		= fsl_asrc_dma_shutdown,
	.pointer	= fsl_asrc_dma_pcm_pointer,
	.pcm_construct	= fsl_asrc_dma_pcm_new,
	.legacy_dai_naming = 1,
};
EXPORT_SYMBOL_GPL(fsl_asrc_component);
