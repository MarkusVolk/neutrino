/*
	Framebuffer acceleration hardware abstraction functions.
	This implements stuff needed by the GL Framebuffer.
	Not really accelerated ;-)

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

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <inttypes.h>
#include <cstring>

#include <glfb.h>
extern GLFramebuffer *glfb;

#include <driver/abstime.h>
#include <system/set_threadname.h>
#include <gui/color.h>
#include <gui/color_custom.h>
#include <gui/osd_helpers.h>

#include <algorithm>
#include <cstdlib>

#define LOGTAG "[fb_glfb] "

CFbAccelGLFB::CFbAccelGLFB()
{
	fb_name = "GL ramebuffer";
	blit_wanted = false;
	memset(damage, 0, sizeof(damage));
}

void CFbAccelGLFB::init(const char *)
{
	int tr = 0xff;

	fd = -1;
	if (!glfb) {
		fprintf(stderr, LOGTAG "init: GL Framebuffer is not set up? we are doomed...\n");
		return;
	}
	screeninfo = glfb->getScreenInfo();
	stride = 4 * screeninfo.xres;
	swidth = screeninfo.xres;
	available = glfb->getOSDBuffer()->size(); /* allocated in glfb constructor */
	lbb = lfb = reinterpret_cast<fb_pixel_t*>(glfb->getOSDBuffer()->data());

	memset(lfb, 0, available);
	setMode(screeninfo.xres, screeninfo.yres, 8 * sizeof(fb_pixel_t));

	blit_thread = false;

	/* Google Material Colors */
	paletteSetColor(0x01,			0x010101, tr); // what's this?
	paletteSetColor(COL_WHITE0,		0xFFFFFF, tr);
	paletteSetColor(COL_BLACK0,		0x000000, tr);
	paletteSetColor(COL_DARK_RED0,		0xb71c1c, tr); // red		900
	paletteSetColor(COL_RED0,		0xf44336, tr); // red		500
	paletteSetColor(COL_LIGHT_RED0,		0xe57373, tr); // red		300
	paletteSetColor(COL_DARK_GREEN0,	0x1b5e20, tr); // green		900
	paletteSetColor(COL_GREEN0,		0x4caf50, tr); // green		500
	paletteSetColor(COL_LIGHT_GREEN0,	0x81c784, tr); // green		300
	paletteSetColor(COL_DARK_YELLOW0,	0xf9a825, tr); // yellow	800
	paletteSetColor(COL_YELLOW0,		0xffeb3b, tr); // yellow	500
	paletteSetColor(COL_LIGHT_YELLOW0,	0xfff176, tr); // yellow	300
	paletteSetColor(COL_DARK_BLUE0,		0x1a237e, tr); // indigo	900
	paletteSetColor(COL_BLUE0,		0x3f51b5, tr); // indigo	500
	paletteSetColor(COL_LIGHT_BLUE0,	0x7986cb, tr); // indigo	300
	paletteSetColor(COL_DARK_GRAY0,		0x424242, tr); // grey		800
	paletteSetColor(COL_GRAY0,		0x9e9e9e, tr); // grey		500
	paletteSetColor(COL_LIGHT_GRAY0,	0xe0e0e0, tr); // grey		300

	paletteSetColor(COL_BACKGROUND,		0x000000, 0x0);

	paletteSet();

	useBackground(false);
	m_transparent = m_transparent_default;

	/* start the autoblit-thread (run() function) */
	OpenThreads::Thread::start();
};

CFbAccelGLFB::~CFbAccelGLFB()
{
	if (blit_thread)
	{
		blit_thread = false;
		blit(); /* wakes up the thread */
		OpenThreads::Thread::join();
	}
}

void CFbAccelGLFB::blit2FB(void *fbbuff, uint32_t width, uint32_t height, uint32_t xoff, uint32_t yoff, uint32_t xp, uint32_t yp, bool transp)
{
	CFrameBuffer::blit2FB(fbbuff, width, height, xoff, yoff, xp, yp, transp);
	blit();
}

#define BLIT_INTERVAL_MIN 40
/* the whole OSD is looked at this often, for what is drawn without mark() and blit() */
#define BLIT_INTERVAL_FULL 2000
void CFbAccelGLFB::run()
{
	printf(LOGTAG "run start\n");
	int64_t last_full = 0, pending_since = 0;
	blit_pending = false;
	blit_thread = true;
	blit_mutex.lock();
	set_threadname("glfb::autoblit");
	while (blit_thread) {
		int64_t wait = BLIT_INTERVAL_FULL;
		if (blit_pending)
			wait = std::max<int64_t>(1, BLIT_INTERVAL_MIN - (time_monotonic_ms() - pending_since));
		blit_cond.wait(&blit_mutex, wait);
		int64_t now = time_monotonic_ms();
		const bool full = now - last_full >= BLIT_INTERVAL_FULL;
		if (!blit_wanted && !blit_pending && !full)
			continue;
		/* the first request opens a window for the rest of the drawing, so
		 * that what is half drawn does not get on the screen */
		if (blit_wanted && !blit_pending)
		{
			blit_pending = true;
			pending_since = now;
			continue;
		}
		if (blit_pending && now - pending_since < BLIT_INTERVAL_MIN)
			continue;
		/* a blit() without a mark() since the last one did not say where it drew */
		int rect[4];
		const bool known = !full && damage[2] > damage[0] && damage[3] > damage[1];
		memcpy(rect, damage, sizeof(rect));
		memset(damage, 0, sizeof(damage));
		blit_wanted = false;
		blit_pending = false;
		blit_mutex.unlock();
		if (glfb)
			glfb->blit(known ? rect : NULL);
		blit_mutex.lock();
		if (!known)
			last_full = now;
	}
	blit_mutex.unlock();
	printf(LOGTAG "run end\n");
}

void CFbAccelGLFB::blit()
{
	blit_mutex.lock();
	blit_wanted = true;
	blit_cond.signal();
	blit_mutex.unlock();
}

void CFbAccelGLFB::mark(int xs, int ys, int xe, int ye)
{
	if (xe <= xs || ye <= ys)
		return;
	OpenThreads::ScopedLock<OpenThreads::Mutex> lock(blit_mutex);
	if (damage[2] <= damage[0] || damage[3] <= damage[1])
	{
		damage[0] = xs;
		damage[1] = ys;
		damage[2] = xe;
		damage[3] = ye;
	}
	else
	{
		damage[0] = std::min(damage[0], xs);
		damage[1] = std::min(damage[1], ys);
		damage[2] = std::max(damage[2], xe);
		damage[3] = std::max(damage[3], ye);
	}
	/* not everything that draws calls blit() afterwards */
	if (!blit_wanted)
	{
		blit_wanted = true;
		blit_cond.signal();
	}
}

/* the size of the OSD; the display's mode is the video system's business */
int CFbAccelGLFB::setMode(unsigned int nxRes, unsigned int nyRes, unsigned int)
{
	if (shadowed())
		shadowEnd();
	if (glfb && nxRes > 0 && nyRes > 0 && (nxRes != screeninfo.xres || nyRes != screeninfo.yres))
	{
		if (!glfb->setOSDResolution(nxRes, nyRes))
		{
			fprintf(stderr, LOGTAG " no OSD of %ux%u\n", nxRes, nyRes);
			return -1;
		}
		screeninfo = glfb->getScreenInfo();
		stride = 4 * screeninfo.xres;
		swidth = screeninfo.xres;
		memset(lfb, 0, available);
	}
	xRes = screeninfo.xres;
	yRes = screeninfo.yres;
	bpp  = screeninfo.bits_per_pixel;
	int needmem = stride * yRes * 2;
	if (available >= needmem)
	{
		backbuffer = lfb + swidth * yRes;
		return 0;
	}
	fprintf(stderr, LOGTAG " not enough FB memory (have %d, need %d)\n", available, needmem);
	backbuffer = lfb; /* will not work well, but avoid crashes */
	return 0;
}

fb_pixel_t * CFbAccelGLFB::getBackBufferPointer() const
{
	return backbuffer;
}

/*
   The GL framebuffer starts with the size libstb-hal was given (GLFB_RESOLUTION,
   1280x720 unless set) and then takes the size of the area it is shown in: the
   window or the screen, fitted to the output's aspect ratio. The OSD is drawn at
   most at 1920x1080, larger areas are scaled by GL, and at least 720 lines high,
   so that a small window still shows text that can be read.
*/
void CFbAccelGLFB::osdSizeFor(int out_w, int out_h, int &w, int &h)
{
	if (out_w <= 0 || out_h <= 0) {
		w = 1280;
		h = 720;
		return;
	}
	double scale = std::min(1.0, std::min(1920.0 / out_w, 1080.0 / out_h));
	double fw = out_w * scale, fh = out_h * scale;
	if (fh < 720) {
		fw = fw * 720 / fh;
		fh = 720;
	}
	w = std::min(1920, (int) (fw + 0.5));
	h = std::min(1080, (int) (fh + 0.5));
	/* a window of nearly 16:9 gets exactly 16:9, not 1919x1080 */
	int w169 = (h * 16 + 4) / 9;
	if (std::abs(w - w169) * 100 <= w169 && w169 <= 1920)
		w = w169;
}

bool CFbAccelGLFB::getOutputSize(int &w, int &h, unsigned &generation)
{
	if (!glfb)
		return false;
	glfb->getOutputSize(w, h, generation);
	return w > 0 && h > 0;
}

bool CFbAccelGLFB::setSource(int fd)
{
	return glfb && glfb->setSource(fd);
}
