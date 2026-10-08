/*
 * HEIF, HEIC and AVIF pictures through libheif
 *
 * libheif turns the picture the way the camera held it.
 *
 * License: GPL
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MM 02139, USA.
 */
#include <config.h>
#include "pv_config.h"
#ifdef FBV_SUPPORT_HEIF
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <libheif/heif.h>
#include "pictureviewer.h"

int fh_heif_id(const char *name)
{
	unsigned char id[64];
	FILE *f = fopen(name, "rb");
	if (!f)
		return 0;
	size_t n = fread(id, 1, sizeof(id), f);
	fclose(f);
	if (n < 12)
		return 0;
	enum heif_filetype_result r = heif_check_filetype(id, (int)n);
	return r == heif_filetype_yes_supported || r == heif_filetype_maybe;
}

static struct heif_image_handle *primary(const char *name, struct heif_context **ctx)
{
	*ctx = heif_context_alloc();
	if (!*ctx)
		return NULL;
	struct heif_error err = heif_context_read_from_file(*ctx, name, NULL);
	if (err.code != heif_error_Ok)
	{
		heif_context_free(*ctx);
		*ctx = NULL;
		return NULL;
	}
	struct heif_image_handle *handle = NULL;
	err = heif_context_get_primary_image_handle(*ctx, &handle);
	if (err.code != heif_error_Ok)
	{
		heif_context_free(*ctx);
		*ctx = NULL;
		return NULL;
	}
	return handle;
}

int fh_heif_getsize(const char *name, int *x, int *y, int /*wanted_width*/, int /*wanted_height*/)
{
	struct heif_context *ctx;
	struct heif_image_handle *handle = primary(name, &ctx);
	if (!handle)
		return FH_ERROR_FORMAT;
	*x = heif_image_handle_get_width(handle);
	*y = heif_image_handle_get_height(handle);
	heif_image_handle_release(handle);
	heif_context_free(ctx);
	return FH_ERROR_OK;
}

int fh_heif_load(const char *name, unsigned char **buffer, int *x, int *y)
{
	struct heif_context *ctx;
	struct heif_image_handle *handle = primary(name, &ctx);
	if (!handle)
		return FH_ERROR_FORMAT;
	struct heif_image *img = NULL;
	struct heif_error err = heif_decode_image(handle, &img, heif_colorspace_RGB, heif_chroma_interleaved_RGB, NULL);
	if (err.code != heif_error_Ok || !img)
	{
		heif_image_handle_release(handle);
		heif_context_free(ctx);
		return FH_ERROR_FORMAT;
	}
	int w = heif_image_get_width(img, heif_channel_interleaved);
	int h = heif_image_get_height(img, heif_channel_interleaved);
	int stride = 0;
	const uint8_t *plane = heif_image_get_plane_readonly(img, heif_channel_interleaved, &stride);
	int ret = FH_ERROR_FORMAT;
	if (plane && w > 0 && h > 0)
	{
		if (w > *x || h > *y)
		{
			free(*buffer);
			*buffer = (unsigned char *)malloc((size_t)w * h * 3);
		}
		if (*buffer)
		{
			for (int row = 0; row < h; row++)
				memcpy(*buffer + (size_t)row * w * 3, plane + (size_t)row * stride, (size_t)w * 3);
			*x = w;
			*y = h;
			ret = FH_ERROR_OK;
		}
		else
			ret = FH_ERROR_MALLOC;
	}
	heif_image_release(img);
	heif_image_handle_release(handle);
	heif_context_free(ctx);
	return ret;
}
#endif
