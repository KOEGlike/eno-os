#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <lvgl.h>
#include "draw/lv_image_decoder_private.h"

#include "i1_decoder.h"

LOG_MODULE_REGISTER(I1DEC, LOG_LEVEL_INF);

static lv_result_t i1_decoder_info(lv_image_decoder_t *decoder,
	lv_image_decoder_dsc_t *dsc, lv_image_header_t *header)
{
	const lv_image_dsc_t *image = (const lv_image_dsc_t *)dsc->src;

	if (dsc->src_type != LV_IMAGE_SRC_VARIABLE ||
		image->header.cf != LV_COLOR_FORMAT_I1 || image->data == NULL)
	{
		return LV_RESULT_INVALID;
	}

	*header = image->header;
	if (header->stride == 0)
	{
		header->stride = (header->w + 7) >> 3;
	}

	return LV_RESULT_OK;
}

static lv_result_t i1_decoder_open(lv_image_decoder_t *decoder,
	lv_image_decoder_dsc_t *dsc)
{
	const lv_image_dsc_t *image = (const lv_image_dsc_t *)dsc->src;
	lv_draw_buf_t *buf;
	lv_result_t res;

	if (dsc->src_type != LV_IMAGE_SRC_VARIABLE ||
		image->header.cf != LV_COLOR_FORMAT_I1 || image->data == NULL)
	{
		return LV_RESULT_INVALID;
	}

	/* wrap the constant data in a draw buf, no copy: the decoder
	 * dsc borrows it until close_cb
	 */
	buf = lv_malloc(sizeof(lv_draw_buf_t));
	if (buf == NULL)
	{
		return LV_RESULT_INVALID;
	}

	res = lv_draw_buf_from_image(buf, image);
	if (res != LV_RESULT_OK)
	{
		lv_free(buf);
		return LV_RESULT_INVALID;
	}

	if (buf->header.stride == 0)
	{
		buf->header.stride = (buf->header.w + 7) >> 3;
	}

	dsc->decoded = buf;
	dsc->user_data = buf;

	return LV_RESULT_OK;
}

static void i1_decoder_close(lv_image_decoder_t *decoder,
	lv_image_decoder_dsc_t *dsc)
{
	if (dsc->user_data != NULL)
	{
		lv_free(dsc->user_data);
		dsc->user_data = NULL;
	}
}

void i1_decoder_init(void)
{
	lv_image_decoder_t *dec = lv_image_decoder_create();

	if (dec == NULL)
	{
		LOG_ERR("Could not create I1 decoder");
		return;
	}

	/* created decoders are pushed to the head of the list, so this
	 * one claims I1 variable images before the bin decoder
	 */
	dec->name = "I1_MONO";
	dec->info_cb = i1_decoder_info;
	dec->open_cb = i1_decoder_open;
	dec->close_cb = i1_decoder_close;

	LOG_INF("I1 mono image decoder registered");
}
