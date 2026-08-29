#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <string.h>

#include <lvgl.h>
#include "libs/tjpgd/tjpgd.h"

#include "art.h"
#include "icons.h"
#include "metadata.h"
#include "sd_card.h"

LOG_MODULE_REGISTER(ART, LOG_LEVEL_INF);

/* TJpgDec working pool: prepare area + one MCU of RGB888 output.
 * Kept small: the helix MP3 decoder allocates its buffers from the
 * same heap, so art decode (which can run during playback) must
 * leave headroom. 6 KB covers prepare + 4:2:0 MCU at JD_SZBUF=512.
 */
#define ART_JPEG_POOL_SIZE 6144

/* 4x4 ordered (Bayer) dither matrix, 0..15 */
static const uint8_t bayer4[16] = {
	0, 8, 2, 10,
	12, 4, 14, 6,
	3, 11, 1, 9,
	15, 7, 13, 5,
};

/* TJpgDec has a single device pointer shared by both callbacks */
struct art_ctx {
	struct fs_file_t *file;
	size_t remaining;
	/* dither output */
	uint8_t *canvas;
	int scale_q16; /* source -> dest scale, 16.16 */
	int off_x;
	int off_y;
};

static size_t art_in_func(JDEC *jd, uint8_t *buff, size_t n)
{
	struct art_ctx *ctx = (struct art_ctx *)jd->device;

	if (ctx->remaining < n)
	{
		n = ctx->remaining;
	}

	if (buff == NULL)
	{
		/* skip n bytes (decompressed data not needed) */
		if (fs_seek(ctx->file, (off_t)n, FS_SEEK_CUR))
		{
			return 0;
		}
		ctx->remaining -= n;
		return n;
	}

	if (n == 0)
	{
		return 0;
	}

	ssize_t got = fs_read(ctx->file, buff, n);

	if (got <= 0)
	{
		return 0;
	}
	ctx->remaining -= (size_t)got;
	return (size_t)got;
}

static int art_out_func(JDEC *jd, void *data, JRECT *rect)
{
	struct art_ctx *ctx = (struct art_ctx *)jd->device;
	uint8_t *src = (uint8_t *)data;
	int src_w = rect->right - rect->left + 1;
	int dx0, dx1, dy0, dy1;

	/* dest rect covered by this source rect (nearest mapping) */
	dy0 = ctx->off_y + (((int)rect->top * ctx->scale_q16) >> 16);
	dy1 = ctx->off_y + (((int)rect->bottom * ctx->scale_q16) >> 16);
	dx0 = ctx->off_x + (((int)rect->left * ctx->scale_q16) >> 16);
	dx1 = ctx->off_x + (((int)rect->right * ctx->scale_q16) >> 16);

	if (dy0 < 0)
	{
		dy0 = 0;
	}
	if (dx0 < 0)
	{
		dx0 = 0;
	}
	if (dy1 >= ART_H)
	{
		dy1 = ART_H - 1;
	}
	if (dx1 >= ART_W)
	{
		dx1 = ART_W - 1;
	}

	for (int dy = dy0; dy <= dy1; dy++)
	{
		/* inverse-map the dest row back to a source row, clamped
		 * into the rect being delivered
		 */
		int sy = (int)(((int64_t)(dy - ctx->off_y) << 16) / ctx->scale_q16);
		int sy0 = (int)rect->top;
		int sy1 = (int)rect->bottom;

		if (sy < sy0)
		{
			sy = sy0;
		}
		if (sy > sy1)
		{
			sy = sy1;
		}

		for (int dx = dx0; dx <= dx1; dx++)
		{
			int sx = (int)(((int64_t)(dx - ctx->off_x) << 16) / ctx->scale_q16);
			const uint8_t *px;
			uint8_t gray;
			uint8_t th;

			if (sx < (int)rect->left)
			{
				sx = rect->left;
			}
			if (sx > (int)rect->right)
			{
				sx = rect->right;
			}

			px = src + ((sy - (int)rect->top) * src_w + (sx - (int)rect->left)) * 3;
			gray = (uint8_t)((px[0] * 77 + px[1] * 151 + px[2] * 28) >> 8);

			/* ordered dither: ink where the sample is darker
			 * than the threshold. bayer4 is 0..15, scale to the
			 * 0..255 gray domain with a +8 midpoint so solid
			 * black and solid white both stay solid
			 */
			th = (uint8_t)(bayer4[((dy & 3) << 2) | (dx & 3)] * 16 + 8);
			if (gray < th)
			{
				ctx->canvas[dy * ART_STRIDE + (dx >> 3)] &=
					(uint8_t)~(0x80 >> (dx & 7));
			}
		}
	}

	return 1;
}

void art_render_fallback(uint8_t *canvas)
{
	memset(canvas, 0xff, ART_STRIDE * ART_H);

	for (int y = 0; y < icon_note.header.h; y++)
	{
		int dy = (ART_H - icon_note.header.h) / 2 + y;

		for (int x = 0; x < ART_W; x++)
		{
			int sx = x - (ART_W - icon_note.header.w) / 2;

			if (sx < 0 || sx >= icon_note.header.w)
			{
				continue;
			}
			if (!(icon_note.data[y * icon_note.header.stride + (sx >> 3)] &
				(0x80 >> (sx & 7))))
			{
				/* glyph bits are 0 (ink) in the I1 icons */
				canvas[dy * ART_STRIDE + (x >> 3)] &=
					(uint8_t)~(0x80 >> (x & 7));
			}
		}
	}
}

static int render_jpeg(const char *path, uint8_t *canvas)
{
	struct fs_file_t file;
	size_t offset;
	size_t len;
	struct art_ctx ctx;
	JDEC jdec;
	JRESULT res;
	uint8_t *pool;
	int scale_q16;
	int dest_w;
	int dest_h;
	int ret = -1;

	pool = k_malloc(ART_JPEG_POOL_SIZE);
	if (pool == NULL)
	{
		LOG_ERR("Art decode pool alloc failed");
		return -ENOMEM;
	}

	if (metadata_open_art(path, &file, &offset, &len))
	{
		k_free(pool);
		return -ENOENT;
	}

	ctx.file = &file;
	ctx.remaining = len;
	ctx.canvas = canvas;

	res = jd_prepare(&jdec, art_in_func, pool, ART_JPEG_POOL_SIZE, &ctx);
	if (res != JDR_OK || jdec.width == 0 || jdec.height == 0)
	{
		LOG_INF("Art prepare failed: %d", res);
		goto out;
	}

	LOG_INF("Art: %ux%u jpeg, %u bytes", jdec.width, jdec.height, (unsigned)len);

	/* Fit into the canvas without upscaling */
	scale_q16 = MIN(((int)ART_W << 16) / (int)jdec.width,
		((int)ART_H << 16) / (int)jdec.height);
	if (scale_q16 > (1 << 16))
	{
		scale_q16 = 1 << 16;
	}
	dest_w = ((int)jdec.width * scale_q16) >> 16;
	dest_h = ((int)jdec.height * scale_q16) >> 16;
	if (dest_w > ART_W)
	{
		dest_w = ART_W;
	}
	if (dest_h > ART_H)
	{
		dest_h = ART_H;
	}

	/* paper background; dithering clears bits for ink */
	memset(canvas, 0xff, ART_STRIDE * ART_H);
	ctx.scale_q16 = scale_q16;
	ctx.off_x = (ART_W - dest_w) / 2;
	ctx.off_y = (ART_H - dest_h) / 2;

	res = jd_decomp(&jdec, art_out_func, 0);
	if (res != JDR_OK)
	{
		LOG_INF("Art decompress failed: %d", res);
		goto out;
	}

	ret = 0;

out:
	sd_card_close(&file);
	k_free(pool);
	return ret;
}

int art_render(const char *path, uint8_t *canvas)
{
	if (render_jpeg(path, canvas) == 0)
	{
		return 0;
	}

	art_render_fallback(canvas);
	return -ENOENT;
}
