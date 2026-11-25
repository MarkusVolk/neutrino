/*
	Game wall: the games of the box as a wall of covers

	License: GPL

	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public License as published
	by the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <config.h>

#include "gamewall.h"

#include <global.h>
#include <neutrino.h>
#include <driver/fontrenderer.h>
#include <driver/screen_max.h>
#include <gui/color.h>
#include <gui/components/cc.h>
#include <gui/infoviewer.h>
#include <gui/pluginlist.h>
#include <gui/plugins.h>
#include <gui/widget/buttons.h>
#include <gui/widget/icons.h>
#include <gui/widget/msgbox.h>
#include <gui/widget/remote_list.h>
#include <system/game_cover.h>
#include <system/helpers.h>
#include <system/settings.h>

#include <algorithm>
#include <map>
#include <strings.h>
#include <unistd.h>

extern CPlugins *g_Plugins;
extern CInfoViewer *g_InfoViewer;

/* hardware with fewer games has no shelf of its own */
static const size_t MIN_SHELF = 4;
static const size_t RECENT_TILES = 12;

/* the title without what the table says about the version */
static std::string shortTitle(const std::string &title)
{
	size_t end = title.find(" (");
	return end == std::string::npos || end == 0 ? title : title.substr(0, end);
}

static std::string joined(const std::string &a, const std::string &b)
{
	return a.empty() || b.empty() ? a + b : a + " \xc2\xb7 " + b;
}

static std::string hardwareOf(const games::game &g)
{
	return g.hardware.substr(0, g.hardware.find(" / "));
}

CCoverWall::Tile CGameWall::gameTile(size_t item) const
{
	if (item >= list.size()) {
		const Plugin &p = plugins[item - list.size()];
		return { p.name, "", { item }, false };
	}
	const games::game &g = list[item];
	return { shortTitle(g.title), joined(g.year, g.maker), { item }, false };
}

CCoverWall::Tile CGameWall::part(size_t item) const
{
	return gameTile(item);
}

/* a shelf for each initial, programs among the games */
std::vector<CCoverWall::Shelf> CGameWall::byTitle() const
{
	std::vector<std::pair<std::string, size_t>> titles;
	for (size_t i = 0; i < list.size() + plugins.size(); i++)
		titles.push_back({ i < list.size() ? list[i].title : plugins[i - list.size()].name, i });
	std::stable_sort(titles.begin(), titles.end(), [](const auto &a, const auto &b) { return strcasecmp(a.first.c_str(), b.first.c_str()) < 0; });

	std::vector<std::pair<std::string, std::vector<Tile>>> letters;
	for (const auto &title : titles) {
		const std::string &t = title.first;
		std::string letter = t.empty() || !isalpha((unsigned char) t[0]) ? "0-9" : std::string(1, toupper((unsigned char) t[0]));
		auto it = std::find_if(letters.begin(), letters.end(), [&letter](const auto &l) { return l.first == letter; });
		if (it == letters.end())
			it = letters.insert(letters.end(), { letter, {} });
		it->second.push_back(gameTile(title.second));
	}
	std::vector<Shelf> shelves;
	for (auto &l : letters)
		addShelf(shelves, l.first, std::move(l.second));
	return shelves;
}

/* played last, then the systems; one with many games has a shelf for each
   hardware they ran on */
std::vector<CCoverWall::Shelf> CGameWall::bySystem() const
{
	std::vector<Shelf> shelves;
	std::vector<Tile> tiles;
	for (const std::string &id : games::recent())
		for (size_t i = 0; i < list.size() && tiles.size() < RECENT_TILES; i++)
			if (list[i].system + "/" + list[i].id == id)
				tiles.push_back(gameTile(i));
	addShelf(shelves, g_Locale->getText(LOCALE_GAMEWALL_RECENT), std::move(tiles));

	for (const games::system &s : systems) {
		std::map<std::string, std::vector<Tile>> hardware;
		for (size_t i = 0; i < list.size(); i++)
			if (list[i].system == s.id)
				hardware[hardwareOf(list[i])].push_back(gameTile(i));
		size_t kinds = hardware.size() - hardware.count("");
		std::vector<std::pair<std::string, std::vector<Tile>>> big;
		std::vector<Tile> rest;
		for (auto &h : hardware) {
			if (kinds > 1 && !h.first.empty() && h.second.size() >= MIN_SHELF)
				big.push_back({ h.first, std::move(h.second) });
			else
				rest.insert(rest.end(), h.second.begin(), h.second.end());
		}
		std::stable_sort(big.begin(), big.end(), [](const auto &a, const auto &b) { return a.second.size() > b.second.size(); });
		std::stable_sort(rest.begin(), rest.end(), [](const Tile &a, const Tile &b) { return a.items.front() < b.items.front(); });
		for (auto &b : big)
			addShelf(shelves, s.name + ": " + b.first, std::move(b.second));
		addShelf(shelves, big.empty() ? s.name : s.name + ": " + g_Locale->getText(LOCALE_GAMEWALL_OTHER), std::move(rest));
	}

	tiles.clear();
	for (size_t i = 0; i < plugins.size(); i++)
		tiles.push_back(gameTile(list.size() + i));
	addShelf(shelves, g_Locale->getText(LOCALE_GAMEWALL_PROGRAMS), std::move(tiles));
	return shelves;
}

void CGameWall::build(const std::vector<games::system> &sys, const std::vector<games::game> &found,
		      const std::vector<Plugin> &plug, bool by_title)
{
	std::string was;
	if (game())
		was = game()->file;
	else if (plugin())
		was = plugin()->name;
	systems = sys;
	list = found;
	plugins = plug;
	show(by_title ? byTitle() : bySystem(), [this, &was](size_t item) {
		return was == (item < list.size() ? list[item].file : plugins[item - list.size()].name);
	});
}

const games::game *CGameWall::game() const
{
	const Tile *t = tile();
	return t && t->items.front() < list.size() ? &list[t->items.front()] : NULL;
}

const CGameWall::Plugin *CGameWall::plugin() const
{
	const Tile *t = tile();
	return t && t->items.front() >= list.size() ? &plugins[t->items.front() - list.size()] : NULL;
}

std::string CGameWall::picture(size_t item) const
{
	if (item < list.size())
		return games::picture(list[item]);
	const Plugin &p = plugins[item - list.size()];
	std::string cover = games::picture(p.cover);
	return cover.empty() ? p.picture : cover;
}

std::string CGameWall::info(const Tile &t) const
{
	size_t item = t.items.front();
	if (item >= list.size())
		return joined(t.caption, plugins[item - list.size()].description);
	const games::game &g = list[item];
	return joined(joined(g.title, g.year), joined(g.maker, g.hardware));
}

std::string CGameWall::details() const
{
	const Tile *t = tile();
	if (!t)
		return "";
	if (plugin())
		return plugin()->name + "\n" + plugin()->description;
	const games::game &g = *game();
	std::string text = g.title;
	for (const std::string &line : { g.year, g.maker, g.hardware, g.file })
		if (!line.empty())
			text += "\n" + line;
	return text;
}

const char *CGameWall::emptyText() const
{
	return g_Locale->getText(LOCALE_GAMEWALL_EMPTY);
}

/* the program's own icon where the system has one */
static std::string programIcon(const std::string &file)
{
	std::string icon = "/usr/share/icons/hicolor/scalable/apps/" + file + ".svg";
	if (access(icon.c_str(), R_OK) != 0)
		icon = "/usr/share/icons/hicolor/256x256/apps/" + file + ".png";
	return access(icon.c_str(), R_OK) == 0 ? icon : "";
}

static std::vector<CGameWall::Plugin> gamePlugins()
{
	std::vector<CGameWall::Plugin> list;
	std::string dir = games::directory();
	for (int i = 0; i < g_Plugins->getNumberOfPlugins(); i++) {
		if (g_Plugins->getType(i) != CPlugins::P_TYPE_GAME || g_Plugins->isHidden(i))
			continue;
		std::string file = g_Plugins->getFileName(i);
		std::string picture = programIcon(file);
		if (picture.empty()) {
			picture = g_Plugins->getHintIcon(i);
			if (picture.find('/') == std::string::npos)
				picture = CFrameBuffer::getInstance()->getIconPath(picture);
		}
		games::game cover;
		cover.title = g_Plugins->getCover(i).empty() ? g_Plugins->getName(i) : g_Plugins->getCover(i);
		cover.file = dir + "/programs/" + file + ".game";
		list.push_back({ i, g_Plugins->getName(i), g_Plugins->getDescription(i), picture, cover,
				 dir.empty() ? "" : g_Plugins->getCovers(i) });
	}
	return list;
}

static std::vector<std::pair<games::game, std::string>> programCovers()
{
	std::vector<std::pair<games::game, std::string>> list;
	for (const CGameWall::Plugin &p : gamePlugins())
		if (!p.covers.empty())
			list.push_back({ p.cover, p.covers });
	if (!list.empty())
		safe_mkdir((games::directory() + "/programs").c_str());
	return list;
}

bool CGames::available()
{
	/* the menu is built before the drives are there */
	return g_Plugins->hasPlugin(CPlugins::P_TYPE_GAME) || !games::systems().empty();
}

int CGames::exec(CMenuTarget *parent, const std::string &)
{
	if (!games::available() && gamePlugins().empty())
		return CPluginList(LOCALE_MAINMENU_GAMES, CPlugins::P_TYPE_GAME).exec(parent, "");
	if (parent)
		parent->hide();
	return show();
}

void CGames::start(const games::game &g)
{
	for (const games::system &s : games::systems()) {
		if (s.id != g.system)
			continue;
		const char *argv[] = { "/bin/sh", s.run.c_str(), g.id.c_str(), g.file.c_str(), NULL };
		printf("[games] %s %s\n", s.run.c_str(), g.id.c_str());
		int status = g_Plugins->runProgram(shortTitle(g.title), argv, games::picture(g), true);
		if (status > 0)
			ShowMsg(LOCALE_MESSAGEBOX_ERROR, g_Locale->getText(LOCALE_GAMEWALL_FAILED), CMsgBox::mbrBack, CMsgBox::mbBack, NEUTRINO_ICON_ERROR);
		else
			games::played(g);
	}
}

int CGames::show()
{
	CFrameBuffer *fb = CFrameBuffer::getInstance();
	int w = fb->getWindowWidth(), h = fb->getWindowHeight();
	int x = getScreenStartX(w), y = getScreenStartY(h);
	CComponentsHeader header(x, y, w, 0, LOCALE_MAINMENU_GAMES, NEUTRINO_ICON_GAMES, CComponentsHeader::CC_BTN_EXIT);
	int head_h = header.getHeight();
	struct button_label buttons[] = {
		{ NEUTRINO_ICON_BUTTON_GREEN, LOCALE_GAMEWALL_COVERS },
		{ NEUTRINO_ICON_BUTTON_YELLOW, LOCALE_GAMEWALL_SORT },
		{ NEUTRINO_ICON_BUTTON_OKAY, LOCALE_GAMEWALL_START },
		{ NEUTRINO_ICON_BUTTON_INFO, LOCALE_GAMEWALL_DETAILS },
	};
	const int button_count = sizeof(buttons) / sizeof(buttons[0]);
	CComponentsFooter footer;
	int foot_h = footer.getHeight();

	std::vector<games::system> systems = games::systems();
	std::vector<games::game> found = games::find(systems);
	CGameWall wall;
	wall.setArea(x, y + head_h, w, h - head_h - foot_h);
	wall.build(systems, found, gamePlugins(), by_title);

	/* while covers are fetched the title counts them and the wall shows
	   them as they come */
	game_cover::progress shown = game_cover::state();
	uint32_t cover_timer = 0;
	auto caption = [&shown] {
		std::string text = g_Locale->getText(LOCALE_MAINMENU_GAMES);
		if (shown.running) {
			char buf[128];
			snprintf(buf, sizeof(buf), g_Locale->getText(LOCALE_GAMEWALL_COVERS_PROGRESS), (int) shown.done, (int) shown.total);
			text = joined(text, buf);
		}
		return text;
	};
	auto watch = [&] {
		shown = game_cover::state();
		if (shown.running && !cover_timer)
			cover_timer = g_RCInput->addTimer(2 * 1000 * 1000, false);
		else if (!shown.running && cover_timer) {
			g_RCInput->killTimer(cover_timer);
			cover_timer = 0;
		}
	};
	watch();

	auto paint = [&] {
		header.setCaption(caption());
		header.paint(CC_SAVE_SCREEN_NO);
		wall.paint();
		buttons[0].locale = shown.running ? LOCALE_GAMEWALL_COVERS_STOP : LOCALE_GAMEWALL_COVERS;
		footer.paintButtons(x, y + h - foot_h, w, foot_h, button_count, buttons);
	};
	paint();

	CRemoteList::Access access;
	access.title = [] { return std::string(g_Locale->getText(LOCALE_MAINMENU_GAMES)); };
	access.count = [&wall] { return wall.count(); };
	access.text = [&wall](size_t i) { return wall.text(i); };
	access.selected = [&wall] { return wall.current(); };
	access.select = [&wall](size_t i) { wall.select(i); wall.paint(); };
	CRemoteList remote(access, "starts the game", "green: fetches the missing covers, again: stops, yellow: by system or by title, info: details");

	int ret = menu_return::RETURN_REPAINT;
	bool loop = true;
	int timeout = g_settings.timing[SNeutrinoSettings::TIMING_FILEBROWSER];
	uint64_t timeout_end = CRCInput::calcTimeoutEnd(timeout);
	while (loop) {
		neutrino_msg_t msg;
		neutrino_msg_data_t data;
		g_RCInput->getMsgAbsoluteTimeout(&msg, &data, &timeout_end);
		if (msg <= CRCInput::RC_MaxRC)
			timeout_end = CRCInput::calcTimeoutEnd(timeout);

		if (msg == CRCInput::RC_timeout || CNeutrinoApp::getInstance()->backKey(msg) || msg == CRCInput::RC_home)
			loop = false;
		else if (msg == CRCInput::RC_ok) {
			const games::game *g = wall.game();
			const CGameWall::Plugin *p = wall.plugin();
			if (!g && !p)
				continue;
			if (g) {
				games::game chosen = *g;
				start(chosen);
			} else
				g_Plugins->startPlugin(p->number);
			wall.build(systems, found, gamePlugins(), by_title);
			paint();
			timeout_end = CRCInput::calcTimeoutEnd(timeout);
		} else if (msg == CRCInput::RC_yellow) {
			by_title = !by_title;
			wall.build(systems, found, gamePlugins(), by_title);
			wall.paint();
		} else if (msg == CRCInput::RC_green) {
			if (game_cover::state().running)
				game_cover::stop();
			else if (!game_cover::start(systems, found, programCovers())) {
				ShowMsg(LOCALE_MAINMENU_GAMES, g_Locale->getText(LOCALE_GAMEWALL_COVERS_NONE), CMsgBox::mbrBack, CMsgBox::mbBack, NEUTRINO_ICON_GAMES);
			}
			watch();
			paint();
		} else if (msg == NeutrinoMessages::EVT_TIMER && cover_timer && data == cover_timer) {
			game_cover::progress was = shown;
			watch();
			timeout_end = CRCInput::calcTimeoutEnd(timeout);
			if (shown.found != was.found || shown.running != was.running)
				paint();
			else if (shown.done != was.done) {
				header.setCaption(caption());
				header.paint(CC_SAVE_SCREEN_NO);
			}
		} else if (msg == CRCInput::RC_info) {
			std::string text = wall.details();
			if (!text.empty()) {
				ShowMsg(LOCALE_MAINMENU_GAMES, text, CMsgBox::mbrBack, CMsgBox::mbBack, NEUTRINO_ICON_GAMES);
				paint();
			}
		} else if (wall.key(msg))
			;
		else if (msg == NeutrinoMessages::STANDBY_ON || msg == NeutrinoMessages::LEAVE_ALL ||
			 msg == NeutrinoMessages::SHUTDOWN || msg == NeutrinoMessages::SLEEPTIMER) {
			ret = menu_return::RETURN_EXIT_ALL;
			loop = false;
			g_RCInput->postMsg(msg, data);
		} else if (CNeutrinoApp::getInstance()->handleMsg(msg, data) & messages_return::cancel_all) {
			ret = menu_return::RETURN_EXIT_ALL;
			loop = false;
		}
	}
	if (cover_timer)
		g_RCInput->killTimer(cover_timer);
	fb->paintBackgroundBoxRel(x, y, w, h);
	return ret;
}
