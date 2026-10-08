/*
 * WebP pictures through libwebp
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
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#include <config.h>
#include "pv_config.h"
#ifdef FBV_SUPPORT_WEBP
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <webp/decode.h>
#include "pictureviewer.h"

static bool read_file(const char *name, std::vector<unsigned char> &data)
{
	FILE *f = fopen(name, "rb");
	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len <= 0)
	{
		fclose(f);
		return false;
	}
	data.resize(len);
	bool ok = fread(&data[0], 1, len, f) == (size_t)len;
	fclose(f);
	return ok;
}

int fh_webp_id(const char *name)
{
	unsigned char id[12];
	FILE *f = fopen(name, "rb");
	if (!f)
		return 0;
	size_t n = fread(id, 1, sizeof(id), f);
	fclose(f);
	return n == sizeof(id) && !memcmp(id, "RIFF", 4) && !memcmp(id + 8, "WEBP", 4);
}

int fh_webp_getsize(const char *name, int *x, int *y, int /*wanted_width*/, int /*wanted_height*/)
{
	std::vector<unsigned char> data;
	if (!read_file(name, data))
		return FH_ERROR_FILE;
	return WebPGetInfo(&data[0], data.size(), x, y) ? FH_ERROR_OK : FH_ERROR_FORMAT;
}

int fh_webp_load(const char *name, unsigned char **buffer, int *x, int *y)
{
	std::vector<unsigned char> data;
	if (!read_file(name, data))
		return FH_ERROR_FILE;
	int w, h;
	if (!WebPGetInfo(&data[0], data.size(), &w, &h))
		return FH_ERROR_FORMAT;
	if (w > *x || h > *y)
	{
		free(*buffer);
		*buffer = (unsigned char *)malloc((size_t)w * h * 3);
		if (!*buffer)
			return FH_ERROR_MALLOC;
	}
	*x = w;
	*y = h;
	if (!WebPDecodeRGBInto(&data[0], data.size(), *buffer, (size_t)w * h * 3, w * 3))
		return FH_ERROR_FORMAT;
	return FH_ERROR_OK;
}
#endif
