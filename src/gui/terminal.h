/*
	Terminal for plugins that run a text user interface, drawn on the OSD

	License: GPL

	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public
	License as published by the Free Software Foundation; either
	version 2 of the License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
	General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef __terminal_h__
#define __terminal_h__

#include <stdint.h>
#include <sys/types.h>
#include <string>
#include <vector>
#include <time.h>
#include <libtsm.h>
#include <driver/textkeyboard.h>

class CFrameBuffer;
class FBFontRenderClass;
class Font;

class CTerminal
{
	public:
		CTerminal(const std::string &program);
		~CTerminal();
		/* runs the program until it ends or the menu key is pressed,
		 * returns its exit status or -1 */
		int exec();

	private:
		std::string program;
		CFrameBuffer *frameBuffer;
		FBFontRenderClass *fontRenderer;
		Font *font;
		struct tsm_screen *screen;
		struct tsm_vte *vte;
		pid_t pid;
		int master;
		CTextKeyboard keyboard;
		std::string layoutName;
		int64_t layoutShown; /* until when the layout switched to is shown */
		int x, y, cols, rows, cellWidth, cellHeight;
		tsm_age_t age;
		unsigned int cursorX, cursorY;
		unsigned int leftX, leftY; /* where the cursor was at the last draw */
		bool dirty;
		bool scrolled; /* the scroll-back buffer is shown */

		bool setupFont();
		bool start();
		void stop(int *status);
		bool readOutput();
		void drawLayout();
		bool readKeyboard();
		bool rcKey(uint32_t msg);
		void typeText();
		void scrollBack(bool up, unsigned int lines = 0);
		void scrollBottom();
		void draw();
		bool drawShape(uint32_t c, int px, int py, int w, uint32_t fg, uint32_t bg);
		void repaint();
		int drawCell(const uint32_t *ch, size_t len, unsigned int width,
			     unsigned int posx, unsigned int posy,
			     const struct tsm_screen_attr *attr, tsm_age_t cellAge);
		void waitForKey();

		static void write_cb(struct tsm_vte *vte, const char *u8, size_t len, void *data);
		static int draw_cb(struct tsm_screen *screen, uint64_t id, const uint32_t *ch,
				   size_t len, unsigned int width, unsigned int posx,
				   unsigned int posy, const struct tsm_screen_attr *attr,
				   tsm_age_t age, void *data);
};

#endif
