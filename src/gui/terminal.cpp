/*
	Terminal for plugins that run a text user interface, drawn on the OSD

	The program runs on a pseudo terminal, libtsm keeps the screen and turns
	keys into what the program reads. On generic hardware the keyboard comes
	from libstb-hal as typed characters, the remote control works everywhere.

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
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <linux/input.h>
#include <xkbcommon/xkbcommon.h>
#include <vector>

#include <global.h>
#include <neutrino.h>
#include <driver/abstime.h>
#include <driver/fontrenderer.h>
#include <driver/framebuffer.h>
#include <driver/neutrinofonts.h>
#include <driver/rcinput.h>
#include <gui/infoviewer.h>
#include <gui/terminal.h>
#include <gui/widget/keyboard_input.h>

#if HAVE_GENERIC_HARDWARE
#include <glfb.h>
extern GLFramebuffer *glfb;
#endif

extern std::string font_file_monospace;

#define TERM_ROWS 40

/* the colours of foot */
static uint8_t palette[TSM_COLOR_NUM][3] = {
	[TSM_COLOR_BLACK]		= { 0x24, 0x24, 0x24 },
	[TSM_COLOR_RED]			= { 0xf6, 0x2b, 0x5a },
	[TSM_COLOR_GREEN]		= { 0x47, 0xb4, 0x13 },
	[TSM_COLOR_YELLOW]		= { 0xe3, 0xc4, 0x01 },
	[TSM_COLOR_BLUE]		= { 0x24, 0xac, 0xd4 },
	[TSM_COLOR_MAGENTA]		= { 0xf2, 0xaf, 0xfd },
	[TSM_COLOR_CYAN]		= { 0x13, 0xc2, 0x99 },
	[TSM_COLOR_LIGHT_GREY]		= { 0xe6, 0xe6, 0xe6 },
	[TSM_COLOR_DARK_GREY]		= { 0x61, 0x61, 0x61 },
	[TSM_COLOR_LIGHT_RED]		= { 0xff, 0x4d, 0x51 },
	[TSM_COLOR_LIGHT_GREEN]		= { 0x35, 0xd4, 0x50 },
	[TSM_COLOR_LIGHT_YELLOW]	= { 0xe9, 0xe8, 0x36 },
	[TSM_COLOR_LIGHT_BLUE]		= { 0x5d, 0xc5, 0xf8 },
	[TSM_COLOR_LIGHT_MAGENTA]	= { 0xfe, 0xab, 0xf2 },
	[TSM_COLOR_LIGHT_CYAN]		= { 0x24, 0xdf, 0xc4 },
	[TSM_COLOR_WHITE]		= { 0xff, 0xff, 0xff },
	[TSM_COLOR_FOREGROUND]		= { 0xff, 0xff, 0xff },
	[TSM_COLOR_BACKGROUND]		= { 0x24, 0x24, 0x24 },
};

static fb_pixel_t pixel(uint8_t r, uint8_t g, uint8_t b)
{
	return 0xff000000 | (r << 16) | (g << 8) | b;
}

CTerminal::CTerminal(const std::string &Program)
{
	program = Program;
	frameBuffer = CFrameBuffer::getInstance();
	fontRenderer = NULL;
	font = NULL;
	screen = NULL;
	vte = NULL;
	pid = -1;
	master = -1;
	keys[0] = keys[1] = -1;
	inputChanged = 0;
	xkbContext = NULL;
	xkbKeymap = NULL;
	xkbState = NULL;
	layoutShown = 0;
	x = y = cols = rows = cellWidth = cellHeight = 0;
	age = 0;
	cursorX = cursorY = 0;
	leftX = leftY = 0;
	dirty = true;
}

CTerminal::~CTerminal()
{
	if (vte)
		tsm_vte_unref(vte);
	if (screen)
		tsm_screen_unref(screen);
	closeKeyboards();
	if (xkbState)
		xkb_state_unref(xkbState);
	if (xkbKeymap)
		xkb_keymap_unref(xkbKeymap);
	if (xkbContext)
		xkb_context_unref(xkbContext);
	delete font;
	delete fontRenderer;
}

bool CTerminal::setupFont()
{
	std::string ttf = CNeutrinoFonts::getInstance()->getShellTTF();
	if (ttf.empty())
		ttf = font_file_monospace;

	x = frameBuffer->getScreenX();
	y = frameBuffer->getScreenY();
	int width = frameBuffer->getScreenWidth();
	int height = frameBuffer->getScreenHeight();

	/* the line height of a font is more than its size, so the size is
	 * found in two steps */
	int size = height / TERM_ROWS;
	for (int i = 0; i < 2; i++)
	{
		delete font;
		font = NULL;
		delete fontRenderer;
		fontRenderer = new FBFontRenderClass();
		const char *style = fontRenderer->AddFont(ttf.c_str());
		if (!style)
			return false;
		font = fontRenderer->getFont(fontRenderer->getFamily(ttf.c_str()).c_str(), style, size);
		if (!font)
			return false;
		cellHeight = font->getHeight();
		if (cellHeight <= 0)
			return false;
		size = size * (height / TERM_ROWS) / cellHeight;
	}
	cellWidth = font->getRenderWidth("M");
	if (cellWidth <= 0)
		return false;
	cols = width / cellWidth;
	rows = height / cellHeight;
	/* centre the cells in the visible area */
	x += (width - cols * cellWidth) / 2;
	y += (height - rows * cellHeight) / 2;
	printf("[terminal] %s: %dx%d cells of %dx%d\n", ttf.c_str(), cols, rows, cellWidth, cellHeight);
	return true;
}

bool CTerminal::start()
{
	std::string home;
	const char *h = getenv("HOME");
	if (h && *h)
		home = h;
	else
	{
		struct passwd *pw = getpwuid(getuid());
		home = pw ? pw->pw_dir : "/";
	}

	/* everything the child needs is made before fork(), the child only
	 * calls what is safe in a copy of a threaded process */
	std::vector<std::string> env;
	/* the keyboard types UTF-8; in a locale with another character set,
	 * such as the C locale neutrino runs in, programs would not take
	 * umlauts and the like */
	const char *ctype = getenv("LC_ALL");
	if (!ctype || !*ctype)
		ctype = getenv("LC_CTYPE");
	if (!ctype || !*ctype)
		ctype = getenv("LANG");
	bool utf8 = ctype && (strcasestr(ctype, "UTF-8") || strcasestr(ctype, "utf8"));
	for (char **e = environ; *e; e++)
		if (strncmp(*e, "TERM=", 5) && strncmp(*e, "COLORTERM=", 10) &&
		    strncmp(*e, "HOME=", 5) && strncmp(*e, "LINES=", 6) && strncmp(*e, "COLUMNS=", 8) &&
		    (utf8 || (strncmp(*e, "LC_ALL=", 7) && strncmp(*e, "LC_CTYPE=", 9))))
			env.push_back(*e);
	if (!utf8)
		env.push_back("LC_CTYPE=C.UTF-8");
	env.push_back("TERM=xterm-256color");
	env.push_back("COLORTERM=truecolor");
	env.push_back("HOME=" + home);
	std::vector<char *> envp;
	for (size_t i = 0; i < env.size(); i++)
		envp.push_back((char *)env[i].c_str());
	envp.push_back(NULL);
	const char *argv[] = { "/bin/sh", program.c_str(), NULL };

	struct winsize ws;
	memset(&ws, 0, sizeof(ws));
	ws.ws_row = rows;
	ws.ws_col = cols;
	ws.ws_xpixel = cols * cellWidth;
	ws.ws_ypixel = rows * cellHeight;

	pid = forkpty(&master, NULL, NULL, &ws);
	if (pid < 0)
	{
		printf("[terminal] forkpty: %m\n");
		return false;
	}
	if (pid == 0)
	{
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = SIG_DFL;
		for (int i = 1; i < NSIG; i++)
			sigaction(i, &sa, NULL);
		close_range(3, ~0U, 0);
		if (chdir(home.c_str()))
			(void)!chdir("/");
		execve(argv[0], (char * const *)argv, envp.data());
		_exit(127);
	}
	fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
	fcntl(master, F_SETFD, FD_CLOEXEC);
	return true;
}

/* closing the pseudo terminal hangs up the program; one that ignores
 * that gets a second to end before it is killed */
void CTerminal::stop(int *status)
{
	if (master >= 0)
		close(master);
	master = -1;
	if (pid <= 0)
		return;
	kill(pid, SIGHUP);
	for (int i = 0; i < 20; i++)
	{
		pid_t r = waitpid(pid, status, WNOHANG);
		if (r == pid || (r < 0 && errno != EINTR))
		{
			pid = -1;
			return;
		}
		usleep(50000);
	}
	kill(pid, SIGKILL);
	waitpid(pid, status, 0);
	pid = -1;
}

/* false once the program has closed its side */
bool CTerminal::readOutput()
{
	char buf[16384];
	for (;;)
	{
		ssize_t n = read(master, buf, sizeof(buf));
		if (n > 0)
		{
			tsm_vte_input(vte, buf, n);
			dirty = true;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			return true;
		return false;
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

void CTerminal::readKeys()
{
#if HAVE_GENERIC_HARDWARE
	struct glfb_term_key k;
	while (read(keys[0], &k, sizeof(k)) == sizeof(k))
	{
		unsigned int mods = 0;
		if (k.mods & GLFB_MOD_SHIFT)
			mods |= TSM_SHIFT_MASK;
		if (k.mods & GLFB_MOD_CTRL)
			mods |= TSM_CONTROL_MASK;
		if (k.mods & GLFB_MOD_ALT)
			mods |= TSM_ALT_MASK;
		if (k.code)
		{
			uint32_t sym = keysym(k.code, k.mods);
			if (sym != XKB_KEY_NoSymbol)
				tsm_vte_handle_keyboard(vte, sym, TSM_VTE_INVALID, mods, TSM_VTE_INVALID);
		}
		else if (k.unicode < 0x80)
			tsm_vte_handle_keyboard(vte, k.unicode, k.unicode, mods, k.unicode);
		else
			tsm_vte_handle_keyboard(vte, 0x01000000 | k.unicode, TSM_VTE_INVALID, mods, k.unicode);
	}
#endif
}

/* the layout switched to last, for the next keyboard and the next terminal */
static xkb_layout_index_t layoutIndex = 0;

static bool has_key(const unsigned long *bits, int key)
{
	const int n = 8 * sizeof(long);
	return bits[key / n] & (1UL << (key % n));
}

/* a full keyboard has letters, space and shift. A remote control has not,
 * so it stays with neutrino and its menu key still ends the terminal. */
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

/* the layout of the console, as systemd keeps it in vconsole.conf */
bool CTerminal::setupKeymap()
{
	if (xkbState)
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
	xkbState = xkb_state_new(xkbKeymap);
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
	printf("[terminal] keyboard layout %s\n", layout.empty() ? "default" : layout.c_str());
	return xkbState != NULL;
}

void CTerminal::openKeyboards()
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
		int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (!is_keyboard(fd) || ioctl(fd, EVIOCGRAB, 1) < 0)
		{
			close(fd);
			continue;
		}
		keyboards.push_back(fd);
	}
	closedir(dir);
	if (!keyboards.empty() && !setupKeymap())
	{
		printf("[terminal] no keymap\n");
		closeKeyboards();
	}
	if (xkbState)
	{
		/* no modifier stays held from before, the layout chosen last does */
		xkb_state_unref(xkbState);
		xkbState = xkb_state_new(xkbKeymap);
		if (xkbState)
			xkb_state_update_mask(xkbState, 0, 0, 0, 0, 0, layoutIndex);
	}
	/* modifiers that are held right now count, the others do not */
	static const int modifiers[] = { KEY_LEFTSHIFT, KEY_RIGHTSHIFT, KEY_LEFTCTRL, KEY_RIGHTCTRL,
					 KEY_LEFTALT, KEY_RIGHTALT, KEY_LEFTMETA, KEY_RIGHTMETA };
	for (size_t i = 0; xkbState && i < keyboards.size(); i++)
	{
		unsigned long held[KEY_MAX / (8 * sizeof(long)) + 1];
		memset(held, 0, sizeof(held));
		if (ioctl(keyboards[i], EVIOCGKEY(sizeof(held)), held) < 0)
			continue;
		for (size_t m = 0; m < sizeof(modifiers) / sizeof(modifiers[0]); m++)
			if (has_key(held, modifiers[m]))
				xkb_state_update_key(xkbState, modifiers[m] + 8, XKB_KEY_DOWN);
	}
	printf("[terminal] %d keyboards\n", (int)keyboards.size());
}

void CTerminal::closeKeyboards()
{
	for (size_t i = 0; i < keyboards.size(); i++)
	{
		ioctl(keyboards[i], EVIOCGRAB, 0);
		close(keyboards[i]);
	}
	keyboards.clear();
}

/* false for the menu key, which ends the terminal as on the remote control */
bool CTerminal::readKeyboards()
{
	struct stat st;
	if (stat("/dev/input", &st) == 0 && st.st_mtime != inputChanged)
		openKeyboards();
	struct input_event ev;
	for (size_t i = 0; i < keyboards.size(); i++)
	{
		while (read(keyboards[i], &ev, sizeof(ev)) == sizeof(ev))
		{
			if (ev.type != EV_KEY || !xkbState)
				continue;
			if (ev.code == KEY_MENU)
			{
				if (ev.value == 1)
					return false;
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
					layoutShown = time_monotonic_ms() + 1500;
				}
			}
			xkb_keysym_t sym = xkb_state_key_get_one_sym(xkbState, kc);
			if (sym == XKB_KEY_NoSymbol)
				continue;
			unsigned int mods = 0;
			if (xkb_state_mod_name_is_active(xkbState, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0)
				mods |= TSM_SHIFT_MASK;
			if (xkb_state_mod_name_is_active(xkbState, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0)
				mods |= TSM_CONTROL_MASK;
			if (xkb_state_mod_name_is_active(xkbState, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0)
				mods |= TSM_ALT_MASK;
			if (xkb_state_mod_name_is_active(xkbState, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0)
				mods |= TSM_LOGO_MASK;
			uint32_t uc = xkb_keysym_to_utf32(sym);
			tsm_vte_handle_keyboard(vte, sym, uc && uc < 0x80 ? uc : TSM_VTE_INVALID, mods, uc ? uc : TSM_VTE_INVALID);
		}
	}
	return true;
}

/* text typed on the on-screen keyboard goes to the program as it is */
void CTerminal::typeText()
{
	std::string text;
	closeKeyboards();
#if HAVE_GENERIC_HARDWARE
	if (glfb)
		glfb->setTerminalFd(-1);
#endif
	CKeyboardInput input(std::string("Terminal"), &text, 255, NULL, NULL, std::string("OK sends the text to the program"), std::string(""));
	input.exec(NULL, "");
#if HAVE_GENERIC_HARDWARE
	if (glfb && keys[1] >= 0)
		glfb->setTerminalFd(keys[1]);
#endif
	openKeyboards();
	g_RCInput->clearRCMsg();
	repaint();
	if (!text.empty())
		write_cb(vte, text.data(), text.size(), this);
}

/* the remote control: the cursor keys, OK, exit and the digits are what a
 * text user interface needs most, red opens the on-screen keyboard, green
 * is Ctrl+C and yellow Tab. A keyboard that neutrino reads as a remote
 * control types lower case letters. False for the menu key, which ends. */
bool CTerminal::rcKey(uint32_t msg)
{
	uint32_t sym = XKB_KEY_NoSymbol;
	uint32_t ch = TSM_VTE_INVALID;
	unsigned int mods = 0;
	switch (msg)
	{
		case CRCInput::RC_setup:
			return false;
		case CRCInput::RC_red:
			typeText();
			return true;
		case CRCInput::RC_green:
			ch = 'c';
			mods = TSM_CONTROL_MASK;
			break;
		case CRCInput::RC_yellow:
		case KEY_TAB:			sym = XKB_KEY_Tab; break;
		case KEY_ENTER:			sym = XKB_KEY_Return; break;
		case KEY_BACKSPACE:		sym = XKB_KEY_BackSpace; break;
		case KEY_ESC:			sym = XKB_KEY_Escape; break;
		case CRCInput::RC_up:		sym = XKB_KEY_Up; break;
		case CRCInput::RC_down:		sym = XKB_KEY_Down; break;
		case CRCInput::RC_left:		sym = XKB_KEY_Left; break;
		case CRCInput::RC_right:	sym = XKB_KEY_Right; break;
		case CRCInput::RC_page_up:	sym = XKB_KEY_Prior; break;
		case CRCInput::RC_page_down:	sym = XKB_KEY_Next; break;
		case CRCInput::RC_ok:		sym = XKB_KEY_Return; break;
		case CRCInput::RC_home:
		case CRCInput::RC_back:		sym = XKB_KEY_Escape; break;
		default:
			if (msg >= CRCInput::RC_0 && msg <= CRCInput::RC_9)
				ch = msg == CRCInput::RC_0 ? '0' : '1' + (msg - CRCInput::RC_1);
			else
			{
				const char *u = CRCInput::getUnicodeValue(msg);
				if (*u)
					ch = tolower((unsigned char)*u);
			}
			break;
	}
	if (ch != TSM_VTE_INVALID)
		tsm_vte_handle_keyboard(vte, ch, ch, mods, ch);
	else if (sym != XKB_KEY_NoSymbol)
		tsm_vte_handle_keyboard(vte, sym, TSM_VTE_INVALID, 0, TSM_VTE_INVALID);
	return true;
}

void CTerminal::write_cb(struct tsm_vte *, const char *u8, size_t len, void *data)
{
	CTerminal *t = (CTerminal *)data;
	while (len > 0 && t->master >= 0)
	{
		ssize_t n = write(t->master, u8, len);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN)
		{
			usleep(1000);
			continue;
		}
		if (n <= 0)
			return;
		u8 += n;
		len -= n;
	}
}

int CTerminal::draw_cb(struct tsm_screen *, uint64_t, const uint32_t *ch, size_t len,
		       unsigned int width, unsigned int posx, unsigned int posy,
		       const struct tsm_screen_attr *attr, tsm_age_t cellAge, void *data)
{
	return ((CTerminal *)data)->drawCell(ch, len, width, posx, posy, attr, cellAge);
}

static int utf8(uint32_t c, char *out)
{
	if (c < 0x80)
	{
		out[0] = c;
		return 1;
	}
	if (c < 0x800)
	{
		out[0] = 0xc0 | (c >> 6);
		out[1] = 0x80 | (c & 0x3f);
		return 2;
	}
	if (c < 0x10000)
	{
		out[0] = 0xe0 | (c >> 12);
		out[1] = 0x80 | ((c >> 6) & 0x3f);
		out[2] = 0x80 | (c & 0x3f);
		return 3;
	}
	out[0] = 0xf0 | (c >> 18);
	out[1] = 0x80 | ((c >> 12) & 0x3f);
	out[2] = 0x80 | ((c >> 6) & 0x3f);
	out[3] = 0x80 | (c & 0x3f);
	return 4;
}

/* colour between bg and fg, t from 0 to 1 */
static fb_pixel_t mix(fb_pixel_t bg, fb_pixel_t fg, float t)
{
	fb_pixel_t r = 0xff000000;
	for (int shift = 0; shift < 24; shift += 8)
	{
		int b = (bg >> shift) & 0xff, f = (fg >> shift) & 0xff;
		r |= (fb_pixel_t)(b + (f - b) * t + 0.5f) << shift;
	}
	return r;
}

/* lines, blocks and the powerline separators fill the cell edge to edge,
 * which glyphs of a font do not, so they are drawn here */
bool CTerminal::drawShape(uint32_t c, int px, int py, int w, uint32_t fg, uint32_t bg)
{
	int h = cellHeight;
	switch (c)
	{
		case 0xe0b0: case 0xe0b2: case 0xe0b4: case 0xe0b6:
		{
			bool right = c == 0xe0b0 || c == 0xe0b4;
			bool round = c == 0xe0b4 || c == 0xe0b6;
			for (int r = 0; r < h; r++)
			{
				float dy = (r + 0.5f - h / 2.0f) / (h / 2.0f);
				float e = round ? w * sqrtf(fmaxf(0.0f, 1.0f - dy * dy)) : w * (1.0f - fabsf(dy));
				int full = (int)e;
				if (full > w)
					full = w;
				int x0 = right ? px : px + w - full;
				if (full > 0)
					frameBuffer->paintBoxRel(x0, py + r, full, 1, fg);
				if (full < w && e > full)
					frameBuffer->paintBoxRel(right ? px + full : x0 - 1, py + r, 1, 1, mix(bg, fg, e - full));
			}
			return true;
		}
		case 0x2580:
			frameBuffer->paintBoxRel(px, py, w, h / 2, fg);
			return true;
		case 0x2584:
			frameBuffer->paintBoxRel(px, py + h / 2, w, h - h / 2, fg);
			return true;
		case 0x2588:
			frameBuffer->paintBoxRel(px, py, w, h, fg);
			return true;
		case 0x258c:
			frameBuffer->paintBoxRel(px, py, w / 2, h, fg);
			return true;
		case 0x2590:
			frameBuffer->paintBoxRel(px + w / 2, py, w - w / 2, h, fg);
			return true;
		default:
			break;
	}

	enum { U = 1, D = 2, L = 4, R = 8, HEAVY = 16 };
	int parts;
	switch (c)
	{
		case 0x2500: parts = L | R; break;
		case 0x2501: parts = L | R | HEAVY; break;
		case 0x2502: parts = U | D; break;
		case 0x2503: parts = U | D | HEAVY; break;
		case 0x250c: case 0x256d: parts = D | R; break;
		case 0x2510: case 0x256e: parts = D | L; break;
		case 0x2514: case 0x2570: parts = U | R; break;
		case 0x2518: case 0x256f: parts = U | L; break;
		case 0x251c: parts = U | D | R; break;
		case 0x2524: parts = U | D | L; break;
		case 0x252c: parts = L | R | D; break;
		case 0x2534: parts = L | R | U; break;
		case 0x253c: parts = U | D | L | R; break;
		default: return false;
	}
	int t = w / 10 > 1 ? w / 10 : 1;
	if (parts & HEAVY)
		t *= 2;
	int cx = px + (w - t) / 2;
	int cy = py + (h - t) / 2;
	if (parts & U)
		frameBuffer->paintBoxRel(cx, py, t, cy - py + t, fg);
	if (parts & D)
		frameBuffer->paintBoxRel(cx, cy, t, py + h - cy, fg);
	if (parts & L)
		frameBuffer->paintBoxRel(px, cy, cx - px + t, t, fg);
	if (parts & R)
		frameBuffer->paintBoxRel(cx, cy, px + w - cx, t, fg);
	return true;
}

int CTerminal::drawCell(const uint32_t *ch, size_t len, unsigned int width,
			unsigned int posx, unsigned int posy,
			const struct tsm_screen_attr *attr, tsm_age_t cellAge)
{
	bool cursor = posx == cursorX && posy == cursorY &&
		!(tsm_screen_get_flags(screen) & TSM_SCREEN_HIDE_CURSOR);
	if (cellAge && age && cellAge <= age && !cursor && !(posx == leftX && posy == leftY))
		return 0;
	if (!width)
		return 0;

	fb_pixel_t fg = pixel(attr->fr, attr->fg, attr->fb);
	fb_pixel_t bg = pixel(attr->br, attr->bg, attr->bb);
	if (attr->inverse != cursor)
	{
		fb_pixel_t t = fg;
		fg = bg;
		bg = t;
	}
	int px = x + posx * cellWidth;
	int py = y + posy * cellHeight;
	int w = width * cellWidth;
	frameBuffer->paintBoxRel(px, py, w, cellHeight, bg);
	if (len == 1 && drawShape(ch[0], px, py, w, fg, bg))
		;
	else if (len && ch[0] != ' ')
	{
		char buf[4 * 8 + 1];
		int n = 0;
		for (size_t i = 0; i < len && i < 8; i++)
			n += utf8(ch[i], buf + n);
		buf[n] = 0;
		font->RenderString(px, py + cellHeight, w, buf, fg, 0, Font::IS_UTF8 | Font::FULLBG);
	}
	if (attr->underline)
		frameBuffer->paintBoxRel(px, py + cellHeight - 2, w, 1, fg);
	return 0;
}

/* the layout Alt+Shift switched to, in the top right corner for a moment */
void CTerminal::drawLayout()
{
	std::string name = layoutIndex < layouts.size() ? layouts[layoutIndex] : "";
	if (name.empty())
	{
		const char *n = xkbKeymap ? xkb_keymap_layout_get_name(xkbKeymap, layoutIndex) : NULL;
		name = n ? n : "?";
	}
	int w = font->getRenderWidth(name.c_str()) + 2 * cellWidth;
	int h = cellHeight * 3 / 2;
	int bx = x + cols * cellWidth - w - cellWidth;
	int by = y + cellHeight;
	frameBuffer->paintBoxRel(bx, by, w, h, pixel(palette[TSM_COLOR_BLUE][0], palette[TSM_COLOR_BLUE][1], palette[TSM_COLOR_BLUE][2]), h / 2);
	font->RenderString(bx + cellWidth, by + (h + cellHeight) / 2, w - cellWidth, name.c_str(),
			   pixel(palette[TSM_COLOR_BACKGROUND][0], palette[TSM_COLOR_BACKGROUND][1], palette[TSM_COLOR_BACKGROUND][2]),
			   0, Font::IS_UTF8 | Font::FULLBG);
	frameBuffer->blit();
}

/* everything again, over whatever neutrino drew meanwhile */
void CTerminal::repaint()
{
	frameBuffer->paintBoxRel(0, 0, frameBuffer->getScreenWidth(true), frameBuffer->getScreenHeight(true),
				 pixel(palette[TSM_COLOR_BACKGROUND][0], palette[TSM_COLOR_BACKGROUND][1], palette[TSM_COLOR_BACKGROUND][2]));
	age = 0;
	dirty = true;
}

void CTerminal::draw()
{
	/* the cell the cursor leaves has to be drawn again as well */
	leftX = cursorX;
	leftY = cursorY;
	cursorX = tsm_screen_get_cursor_x(screen);
	cursorY = tsm_screen_get_cursor_y(screen);
	age = tsm_screen_draw(screen, draw_cb, this);
	frameBuffer->blit();
	dirty = false;
}

void CTerminal::waitForKey()
{
	neutrino_msg_t msg;
	neutrino_msg_data_t data;
	int64_t end = time_monotonic_ms() + 30000;
	g_RCInput->clearRCMsg();
	while (time_monotonic_ms() < end)
	{
		g_RCInput->getMsg_ms(&msg, &data, 100);
		if (msg <= CRCInput::RC_MaxRC && !(msg & CRCInput::RC_Release))
			return;
		if (msg > CRCInput::RC_MaxRC && msg != CRCInput::RC_timeout)
			CNeutrinoApp::getInstance()->handleMsg(msg, data);
#if HAVE_GENERIC_HARDWARE
		struct glfb_term_key k;
		if (read(keys[0], &k, sizeof(k)) == sizeof(k))
			return;
#endif
	}
}

int CTerminal::exec()
{
	int status = -1;
	if (!setupFont())
	{
		printf("[terminal] no usable font\n");
		return -1;
	}
	if (tsm_screen_new(&screen, NULL, NULL) < 0 ||
	    tsm_screen_resize(screen, cols, rows) < 0 ||
	    tsm_vte_new(&vte, screen, write_cb, this, NULL, NULL) < 0)
	{
		printf("[terminal] libtsm failed\n");
		return -1;
	}
	tsm_vte_set_backspace_sends_delete(vte, true);
	tsm_vte_set_custom_palette(vte, palette);
	tsm_vte_set_palette(vte, "custom");
	if (pipe2(keys, O_CLOEXEC | O_NONBLOCK))
		keys[0] = keys[1] = -1;
	if (!start())
	{
		close(keys[0]);
		close(keys[1]);
		return -1;
	}
#if HAVE_GENERIC_HARDWARE
	if (glfb && keys[1] >= 0)
		glfb->setTerminalFd(keys[1]);
#endif
	openKeyboards();
	if (g_InfoViewer)
		g_InfoViewer->killTitle();
	g_RCInput->clearRCMsg();
	repaint();

	bool running = true;
	while (running)
	{
		if (!readOutput())
			break;
		readKeys();
		if (!readKeyboards())
		{
			running = false;
			break;
		}
		if (dirty)
			draw();
		if (layoutShown)
		{
			if (time_monotonic_ms() < layoutShown)
				drawLayout();
			else
			{
				layoutShown = 0;
				repaint();
			}
		}

		neutrino_msg_t msg;
		neutrino_msg_data_t data;
		g_RCInput->getMsg_ms(&msg, &data, 20);
		if (msg == CRCInput::RC_timeout)
			continue;
		if (msg <= CRCInput::RC_MaxRC)
		{
			if (!(msg & CRCInput::RC_Release))
				running = rcKey(msg & ~CRCInput::RC_Repeat);
		}
		else if (msg == NeutrinoMessages::EVT_START_PLUGIN)
		{
			/* a plugin started from the web interface would run in here */
			delete[] (unsigned char *)data;
		}
		else
		{
			if (CNeutrinoApp::getInstance()->handleMsg(msg, data) & messages_return::cancel_all)
				running = false;
			else if (msg != NeutrinoMessages::EVT_TIMER)
				repaint();
		}
	}

	closeKeyboards();
	bool ended = running;
	if (ended)
	{
		/* the program ended by itself: it may have said why */
		int r = waitpid(pid, &status, 0);
		pid = -1;
		if (r > 0 && !(WIFEXITED(status) && WEXITSTATUS(status) == 0))
		{
			char line[80];
			snprintf(line, sizeof(line), "\r\n[%s %d]", WIFEXITED(status) ? "exit" : "signal",
				 WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
			tsm_vte_input(vte, line, strlen(line));
			draw();
			waitForKey();
		}
	}
	stop(&status);
#if HAVE_GENERIC_HARDWARE
	if (glfb)
		glfb->setTerminalFd(-1);
#endif
	close(keys[0]);
	close(keys[1]);
	keys[0] = keys[1] = -1;
	g_RCInput->clearRCMsg();
	frameBuffer->paintBackground();
	frameBuffer->blit();
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
