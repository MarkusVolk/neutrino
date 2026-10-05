/*
	Keyboards that type text, for the terminal and the on-screen keyboard

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

#ifndef __textkeyboard_h__
#define __textkeyboard_h__

#include <stdint.h>
#include <time.h>
#include <deque>
#include <string>
#include <vector>

struct xkb_context;
struct xkb_keymap;
struct xkb_state;

#define TEXT_KEY_SHIFT	1
#define TEXT_KEY_CTRL	2
#define TEXT_KEY_ALT	4
#define TEXT_KEY_LOGO	8

/* a key as an XKB keysym, the character it types or 0, and the modifiers */
struct text_key
{
	uint32_t keysym;
	uint32_t unicode;
	unsigned int mods;
};

/* While it is open, every input device that is a full keyboard is taken
 * from neutrino and its keys are turned into characters with xkbcommon, in
 * the layout of /etc/vconsole.conf. A remote control stays with neutrino.
 * On generic hardware the keyboard of the window comes from libstb-hal as
 * well, for a desktop, where the devices cannot be opened. */
class CTextKeyboard
{
	public:
		CTextKeyboard();
		~CTextKeyboard();
		void open();
		void close();
		bool read(struct text_key &key);
		/* the layout Alt+Shift switched to since the last call, or "" */
		std::string switchedLayout();

	private:
		bool opened;
		int pipefd[2];
		std::vector<int> keyboards;
		time_t inputChanged;
		struct xkb_context *xkbContext;
		struct xkb_keymap *xkbKeymap;
		struct xkb_state *xkbState;
		std::vector<std::string> layouts;
		std::deque<struct text_key> pending;
		bool switched;

		bool setupKeymap();
		void openKeyboards();
		void closeKeyboards();
		void readKeyboards();
		void readWindow();
};

#endif
