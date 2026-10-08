/*
 * JPEG XL pictures through libjxl
 *
 * libjxl turns the picture the way the camera held it.
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
#ifdef FBV_SUPPORT_JXL
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <jxl/decode.h>
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

int fh_jxl_id(const char *name)
{
	unsigned char id[16];
	FILE *f = fopen(name, "rb");
	if (!f)
		return 0;
	size_t n = fread(id, 1, sizeof(id), f);
	fclose(f);
	JxlSignature sig = JxlSignatureCheck(id, n);
	return sig == JXL_SIG_CODESTREAM || sig == JXL_SIG_CONTAINER;
}

/* decodes the picture into buffer when one is given, otherwise only its size */
static int decode(const char *name, unsigned char **buffer, int *x, int *y)
{
	std::vector<unsigned char> data;
	if (!read_file(name, data))
		return FH_ERROR_FILE;
	JxlDecoder *dec = JxlDecoderCreate(NULL);
	if (!dec)
		return FH_ERROR_MALLOC;
	int ret = FH_ERROR_FORMAT;
	int events = JXL_DEC_BASIC_INFO | (buffer ? JXL_DEC_FULL_IMAGE : 0);
	if (JxlDecoderSubscribeEvents(dec, events) != JXL_DEC_SUCCESS
	    || JxlDecoderSetInput(dec, &data[0], data.size()) != JXL_DEC_SUCCESS)
	{
		JxlDecoderDestroy(dec);
		return FH_ERROR_FORMAT;
	}
	JxlDecoderCloseInput(dec);
	JxlPixelFormat format = { 3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0 };
	JxlBasicInfo info;
	memset(&info, 0, sizeof(info));
	for (;;)
	{
		JxlDecoderStatus st = JxlDecoderProcessInput(dec);
		if (st == JXL_DEC_BASIC_INFO)
		{
			if (JxlDecoderGetBasicInfo(dec, &info) != JXL_DEC_SUCCESS || !info.xsize || !info.ysize)
				break;
			if (!buffer)
			{
				*x = info.xsize;
				*y = info.ysize;
				ret = FH_ERROR_OK;
				break;
			}
		}
		else if (st == JXL_DEC_NEED_IMAGE_OUT_BUFFER)
		{
			size_t size = 0;
			if (JxlDecoderImageOutBufferSize(dec, &format, &size) != JXL_DEC_SUCCESS)
				break;
			if ((int)info.xsize > *x || (int)info.ysize > *y)
			{
				free(*buffer);
				*buffer = (unsigned char *)malloc(size);
				if (!*buffer)
				{
					ret = FH_ERROR_MALLOC;
					break;
				}
			}
			if (JxlDecoderSetImageOutBuffer(dec, &format, *buffer, size) != JXL_DEC_SUCCESS)
				break;
		}
		else if (st == JXL_DEC_FULL_IMAGE)
		{
			*x = info.xsize;
			*y = info.ysize;
			ret = FH_ERROR_OK;
			break;
		}
		else
			break;
	}
	JxlDecoderDestroy(dec);
	return ret;
}

int fh_jxl_getsize(const char *name, int *x, int *y, int /*wanted_width*/, int /*wanted_height*/)
{
	return decode(name, NULL, x, y);
}

int fh_jxl_load(const char *name, unsigned char **buffer, int *x, int *y)
{
	return decode(name, buffer, x, y);
}
#endif
