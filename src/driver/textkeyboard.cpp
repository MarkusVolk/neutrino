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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/input.h>
#include <xkbcommon/xkbcommon.h>

#include <driver/textkeyboard.h>

#if HAVE_GENERIC_HARDWARE
#include <glfb.h>
extern GLFramebuffer *glfb;
#endif

/* the layout switched to last, for the next keyboard and the next dialog */
static xkb_layout_index_t layoutIndex = 0;

static bool has_key(const unsigned long *bits, int key)
{
	const int n = 8 * sizeof(long);
	return bits[key / n] & (1UL << (key % n));
}

/* a full keyboard has letters, space and shift. A remote control has not,
 * so it stays with neutrino and its keys keep working. */
static bool is_keyboard(int fd)
{
	unsigned long bits[KEY_MAX / (8 * sizeof(long)) + 1];
	memset(bits, 0, sizeof(bits));
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0)
		return false;
	for (int k = KEY_Q; k <= KEY_P; k++)
		if (!has_key(bits, k))
			return false;
	for (int k = KEY_A; k <= KEY_L; k++)
		if (!has_key(bits, k))
			return false;
	for (int k = KEY_Z; k <= KEY_M; k++)
		if (!has_key(bits, k))
			return false;
	return has_key(bits, KEY_SPACE) && has_key(bits, KEY_LEFTSHIFT);
}

static std::string vconsole(const char *key)
{
	FILE *f = fopen("/etc/vconsole.conf", "r");
	if (!f)
		return "";
	std::string value;
	char line[256];
	size_t len = strlen(key);
	while (fgets(line, sizeof(line), f))
	{
		if (strncmp(line, key, len) || line[len] != '=')
			continue;
		value = line + len + 1;
		value.erase(value.find_last_not_of(" \t\r\n\"'") + 1);
		value.erase(0, value.find_first_not_of(" \t\"'"));
	}
	fclose(f);
	return value;
}

CTextKeyboard::CTextKeyboard()
{
	opened = false;
	pipefd[0] = pipefd[1] = -1;
	inputChanged = 0;
	xkbContext = NULL;
	xkbKeymap = NULL;
	xkbState = NULL;
	switched = false;
}

CTextKeyboard::~CTextKeyboard()
{
	close();
	if (xkbState)
		xkb_state_unref(xkbState);
	if (xkbKeymap)
		xkb_keymap_unref(xkbKeymap);
	if (xkbContext)
		xkb_context_unref(xkbContext);
}

void CTextKeyboard::open()
{
	if (opened)
		return;
	opened = true;
	pending.clear();
#if HAVE_GENERIC_HARDWARE
	if (glfb && pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) == 0)
		glfb->setTerminalFd(pipefd[1]);
#endif
	openKeyboards();
}

void CTextKeyboard::close()
{
	if (!opened)
		return;
	opened = false;
#if HAVE_GENERIC_HARDWARE
	if (glfb && pipefd[1] >= 0)
		glfb->setTerminalFd(-1);
#endif
	if (pipefd[0] >= 0)
		::close(pipefd[0]);
	if (pipefd[1] >= 0)
		::close(pipefd[1]);
	pipefd[0] = pipefd[1] = -1;
	closeKeyboards();
	pending.clear();
}

/* the layout of the console, as systemd keeps it in vconsole.conf */
bool CTextKeyboard::setupKeymap()
{
	if (xkbKeymap)
		return true;
	if (!xkbContext)
		xkbContext = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!xkbContext)
		return false;
	std::string layout = vconsole("XKBLAYOUT");
	std::string variant = vconsole("XKBVARIANT");
	std::string model = vconsole("XKBMODEL");
	std::string options = vconsole("XKBOPTIONS");
	if (layout.empty())
	{
		layout = vconsole("KEYMAP");
		layout = layout.substr(0, layout.find('-'));
	}
	struct xkb_rule_names names;
	memset(&names, 0, sizeof(names));
	names.layout = layout.empty() ? NULL : layout.c_str();
	names.variant = variant.empty() ? NULL : variant.c_str();
	names.model = model.empty() ? NULL : model.c_str();
	names.options = options.empty() ? NULL : options.c_str();
	xkbKeymap = xkb_keymap_new_from_names(xkbContext, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!xkbKeymap)
	{
		memset(&names, 0, sizeof(names));
		xkbKeymap = xkb_keymap_new_from_names(xkbContext, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
	}
	if (!xkbKeymap)
		return false;
	for (size_t start = 0; start <= layout.size(); )
	{
		size_t end = layout.find(',', start);
		if (end == std::string::npos)
			end = layout.size();
		std::string name = layout.substr(start, end - start);
		for (size_t i = 0; i < name.size(); i++)
			name[i] = toupper((unsigned char)name[i]);
		layouts.push_back(name);
		start = end + 1;
	}
	printf("[textkeyboard] layout %s\n", layout.empty() ? "default" : layout.c_str());
	return true;
}

void CTextKeyboard::openKeyboards()
{
	closeKeyboards();
	struct stat st;
	inputChanged = stat("/dev/input", &st) == 0 ? st.st_mtime : 0;
	DIR *dir = opendir("/dev/input");
	if (!dir)
		return;
	struct dirent *d;
	while ((d = readdir(dir)) != NULL)
	{
		if (strncmp(d->d_name, "event", 5))
			continue;
		std::string path = std::string("/dev/input/") + d->d_name;
		int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (!is_keyboard(fd) || ioctl(fd, EVIOCGRAB, 1) < 0)
		{
			::close(fd);
			continue;
		}
		keyboards.push_back(fd);
	}
	closedir(dir);
	if (keyboards.empty())
		return;
	if (!setupKeymap())
	{
		printf("[textkeyboard] no keymap\n");
		closeKeyboards();
		return;
	}
	/* no modifier stays held from before, the layout chosen last does */
	if (xkbState)
		xkb_state_unref(xkbState);
	xkbState = xkb_state_new(xkbKeymap);
	if (!xkbState)
	{
		closeKeyboards();
		return;
	}
	xkb_state_update_mask(xkbState, 0, 0, 0, 0, 0, layoutIndex);
	/* modifiers that are held right now count, the others do not */
	static const int modifiers[] = { KEY_LEFTSHIFT, KEY_RIGHTSHIFT, KEY_LEFTCTRL, KEY_RIGHTCTRL,
					 KEY_LEFTALT, KEY_RIGHTALT, KEY_LEFTMETA, KEY_RIGHTMETA };
	for (size_t i = 0; i < keyboards.size(); i++)
	{
		unsigned long held[KEY_MAX / (8 * sizeof(long)) + 1];
		memset(held, 0, sizeof(held));
		if (ioctl(keyboards[i], EVIOCGKEY(sizeof(held)), held) < 0)
			continue;
		for (size_t m = 0; m < sizeof(modifiers) / sizeof(modifiers[0]); m++)
			if (has_key(held, modifiers[m]))
				xkb_state_update_key(xkbState, modifiers[m] + 8, XKB_KEY_DOWN);
	}
	printf("[textkeyboard] %d keyboards\n", (int)keyboards.size());
}

void CTextKeyboard::closeKeyboards()
{
	for (size_t i = 0; i < keyboards.size(); i++)
	{
		ioctl(keyboards[i], EVIOCGRAB, 0);
		::close(keyboards[i]);
	}
	keyboards.clear();
}

void CTextKeyboard::readKeyboards()
{
	struct stat st;
	if (stat("/dev/input", &st) == 0 && st.st_mtime != inputChanged)
		openKeyboards();
	struct input_event ev;
	for (size_t i = 0; i < keyboards.size(); i++)
	{
		while (::read(keyboards[i], &ev, sizeof(ev)) == sizeof(ev))
		{
			if (ev.type != EV_KEY || !xkbState)
				continue;
			struct text_key k;
			memset(&k, 0, sizeof(k));
			if (ev.code == KEY_MENU)
			{
				if (ev.value == 1)
				{
					k.keysym = XKB_KEY_Menu;
					pending.push_back(k);
				}
				continue;
			}
			xkb_keycode_t kc = ev.code + 8;
			if (ev.value == 0)
			{
				xkb_state_update_key(xkbState, kc, XKB_KEY_UP);
				continue;
			}
			if (ev.value == 1)
			{
				xkb_state_update_key(xkbState, kc, XKB_KEY_DOWN);
				xkb_layout_index_t now = xkb_state_serialize_layout(xkbState, XKB_STATE_LAYOUT_EFFECTIVE);
				if (now != layoutIndex)
				{
					layoutIndex = now;
					switched = true;
				}
			}
			k.keysym = xkb_state_key_get_one_sym(xkbState, kc);
			if (k.keysym == XKB_KEY_NoSymbol)
				continue;
			if (xkb_state_mod_name_is_active(xkbState, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0)
				k.mods |= TEXT_KEY_SHIFT;
			if (xkb_state_mod_name_is_active(xkbState, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0)
				k.mods |= TEXT_KEY_CTRL;
			if (xkb_state_mod_name_is_active(xkbState, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0)
				k.mods |= TEXT_KEY_ALT;
			if (xkb_state_mod_name_is_active(xkbState, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0)
				k.mods |= TEXT_KEY_LOGO;
			k.unicode = xkb_keysym_to_utf32(k.keysym);
			pending.push_back(k);
		}
	}
}

#if HAVE_GENERIC_HARDWARE
static uint32_t keysym(uint32_t code, uint32_t mods)
{
	switch (code)
	{
		case KEY_ENTER:		return XKB_KEY_Return;
		case KEY_ESC:		return XKB_KEY_Escape;
		case KEY_BACKSPACE:	return XKB_KEY_BackSpace;
		case KEY_TAB:		return (mods & GLFB_MOD_SHIFT) ? XKB_KEY_ISO_Left_Tab : XKB_KEY_Tab;
		case KEY_UP:		return XKB_KEY_Up;
		case KEY_DOWN:		return XKB_KEY_Down;
		case KEY_LEFT:		return XKB_KEY_Left;
		case KEY_RIGHT:		return XKB_KEY_Right;
		case KEY_HOME:		return XKB_KEY_Home;
		case KEY_END:		return XKB_KEY_End;
		case KEY_PAGEUP:	return XKB_KEY_Prior;
		case KEY_PAGEDOWN:	return XKB_KEY_Next;
		case KEY_INSERT:	return XKB_KEY_Insert;
		case KEY_DELETE:	return XKB_KEY_Delete;
		case KEY_F1:		return XKB_KEY_F1;
		case KEY_F2:		return XKB_KEY_F2;
		case KEY_F3:		return XKB_KEY_F3;
		case KEY_F4:		return XKB_KEY_F4;
		case KEY_F5:		return XKB_KEY_F5;
		case KEY_F6:		return XKB_KEY_F6;
		case KEY_F7:		return XKB_KEY_F7;
		case KEY_F8:		return XKB_KEY_F8;
		case KEY_F9:		return XKB_KEY_F9;
		case KEY_F10:		return XKB_KEY_F10;
		case KEY_F11:		return XKB_KEY_F11;
		case KEY_F12:		return XKB_KEY_F12;
		default:		return XKB_KEY_NoSymbol;
	}
}
#endif

/* the keyboard of the window, typed characters from libstb-hal */
void CTextKeyboard::readWindow()
{
#if HAVE_GENERIC_HARDWARE
	if (pipefd[0] < 0)
		return;
	struct glfb_term_key g;
	while (::read(pipefd[0], &g, sizeof(g)) == sizeof(g))
	{
		struct text_key k;
		memset(&k, 0, sizeof(k));
		if (g.mods & GLFB_MOD_SHIFT)
			k.mods |= TEXT_KEY_SHIFT;
		if (g.mods & GLFB_MOD_CTRL)
			k.mods |= TEXT_KEY_CTRL;
		if (g.mods & GLFB_MOD_ALT)
			k.mods |= TEXT_KEY_ALT;
		if (g.code)
		{
			k.keysym = keysym(g.code, g.mods);
			if (k.keysym == XKB_KEY_NoSymbol)
				continue;
			k.unicode = xkb_keysym_to_utf32(k.keysym);
		}
		else
		{
			k.keysym = g.unicode < 0x80 ? g.unicode : 0x01000000 | g.unicode;
			k.unicode = g.unicode;
		}
		pending.push_back(k);
	}
#endif
}

bool CTextKeyboard::read(struct text_key &key)
{
	if (!opened)
		return false;
	if (pending.empty())
	{
		readKeyboards();
		readWindow();
	}
	if (pending.empty())
		return false;
	key = pending.front();
	pending.pop_front();
	return true;
}

std::string CTextKeyboard::switchedLayout()
{
	if (!switched)
		return "";
	switched = false;
	if (layoutIndex < layouts.size() && !layouts[layoutIndex].empty())
		return layouts[layoutIndex];
	const char *n = xkbKeymap ? xkb_keymap_layout_get_name(xkbKeymap, layoutIndex) : NULL;
	return n ? n : "?";
}
