/*
	Framebuffer acceleration hardware abstraction functions.
	The infrastructure for hardware dependent framebuffer acceleration
	functions are implemented in this class, hardware specific stuff
	is in derived classes.

	(C) 2017 Stefan Seyfried

	License: GPL

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <driver/fb_generic.h>
#include <driver/fb_accel.h>

#include <algorithm>
#include <cstring>

CFbAccel::CFbAccel()
{
};

CFbAccel::~CFbAccel()
{
};

void CFbAccel::paintBoxRel(const int x, const int y, const int dx, const int dy, const fb_pixel_t col, int radius, int type)
{
	/* draw a filled rectangle (with additional round corners) */
	if (!getActive())
		return;

	checkFbArea(x, y, dx, dy, true);

	int top = 0, bottom = 0;
	if (type && radius > 0)
		paintCornerLines(x, y, dx, dy, col, radius, type, top, bottom);
	paintRect(x, y + top, dx, dy - top - bottom, col);
	checkFbArea(x, y, dx, dy, false);
	if (top || bottom)
		mark(x, y, x+dx, y+dy);
}

/* the part of the rectangle that is on the screen, filled by the CPU */
void CFbAccel::fillRect(const int x, const int y, const int dx, const int dy, const fb_pixel_t col)
{
	const int x0 = std::max(x, 0), x1 = std::min(x + dx, (int)xRes);
	const int y0 = std::max(y, 0), y1 = std::min(y + dy, (int)yRes);
	if (x0 >= x1 || y0 >= y1)
		return;

	/* fill the first line, then replicate it */
	fb_pixel_t *first = getFrameBufferPointer() + swidth * y0 + x0;
	std::fill(first, first + (x1 - x0), col);
	fb_pixel_t *fbp = first + swidth;
	for (int line = y0 + 1; line < y1; line++, fbp += swidth)
		memcpy(fbp, first, (x1 - x0) * sizeof(fb_pixel_t));
}

void CFbAccel::paintRect(const int x, const int y, const int dx, const int dy, const fb_pixel_t col)
{
	if (dx < 1 || dy < 1)
		return;

	fillRect(x, y, dx, dy, col);
	mark(x, y, x+dx, y+dy);
	blit();
}
