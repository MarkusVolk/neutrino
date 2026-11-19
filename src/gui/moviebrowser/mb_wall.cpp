/*
	Movie browser: the recordings as a wall of covers on shelves

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

#include "mb_wall.h"

#include <global.h>
#include <neutrino.h>
#include <cs_api.h>
#include <driver/fontrenderer.h>
#include <driver/pictureviewer/pictureviewer.h>
#include <gui/color.h>
#include <gui/widget/icons.h>
#include <system/locals.h>
#include <system/recordings.h>
#include <system/helpers.h>
#include <system/settings.h>

#include <algorithm>
#include <map>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern const char *GetGenre(const unsigned char contentClassification);
extern CPictureViewer *g_PicViewer;

/* covers kept decoded at tile size; a screen shows about fourteen */
static const size_t COVER_CACHE = 48;
/* the shelves "continue" and "newest" stop at a row's worth or two */
static const size_t RECENT_TILES = 12;

static Font *titleFont() { return g_Font[SNeutrinoSettings::FONT_TYPE_MENU]; }
static Font *captionFont() { return g_Font[SNeutrinoSettings::FONT_TYPE_MOVIEBROWSER_LIST]; }
static Font *lineFont() { return g_Font[SNeutrinoSettings::FONT_TYPE_MOVIEBROWSER_INFO]; }

static std::string titleOf(const MI_MOVIE_INFO *m)
{
	if (!m->epgTitle.empty())
		return m->epgTitle;
	std::string name = m->file.getFileName();
	size_t dot = name.rfind('.');
	return dot == std::string::npos ? name : name.substr(0, dot);
}

static std::string seriesOf(const MI_MOVIE_INFO *m)
{
	return recordings::series_of(m->serieName, recordings::numbered(m->epgInfo1, m->epgInfo2), titleOf(m));
}

static std::string episodeOf(const MI_MOVIE_INFO *m)
{
	return recordings::episode_name(titleOf(m), m->serieName, m->epgInfo1);
}

static std::string dateOf(const MI_MOVIE_INFO *m, bool year = false)
{
	char buf[32];
	struct tm tm;
	time_t t = m->file.Time;
	localtime_r(&t, &tm);
	strftime(buf, sizeof(buf), year ? "%d.%m.%Y %H:%M" : "%d.%m. %H:%M", &tm);
	return buf;
}

static std::string counted(neutrino_locale_t one, neutrino_locale_t many, size_t n)
{
	if (n == 1)
		return g_Locale->getText(one);
	char buf[64];
	snprintf(buf, sizeof(buf), g_Locale->getText(many), (int) n);
	return buf;
}

static CMovieWall::Tile single(MI_MOVIE_INFO *m)
{
	std::string episode = episodeOf(m);
	return { m->serieName.empty() ? titleOf(m) : m->serieName, episode.empty() ? dateOf(m) : episode, { m }, false };
}

static CMovieWall::Tile groupTile(const std::string &name, const std::vector<MI_MOVIE_INFO *> &list, bool series)
{
	return { name, series ? counted(LOCALE_MOVIEBROWSER_WALL_EPISODE, LOCALE_MOVIEBROWSER_WALL_EPISODES, list.size())
			: counted(LOCALE_MOVIEBROWSER_WALL_PARTS, LOCALE_MOVIEBROWSER_WALL_PARTS, list.size()), list, true };
}

/* recordings of the same series or title as one tile, in the order they
   come */
static std::vector<CMovieWall::Tile> titleTiles(const std::vector<MI_MOVIE_INFO *> &list)
{
	std::vector<std::pair<std::string, std::vector<MI_MOVIE_INFO *>>> groups;
	std::map<std::string, size_t> index;
	for (MI_MOVIE_INFO *m : list) {
		std::string series = seriesOf(m);
		std::string key = series.empty() ? titleOf(m) : series;
		auto it = index.find(key);
		if (it == index.end()) {
			index[key] = groups.size();
			groups.push_back({ key, { m } });
		} else
			groups[it->second].second.push_back(m);
	}
	std::vector<CMovieWall::Tile> tiles;
	for (auto &g : groups)
		tiles.push_back(g.second.size() == 1 ? single(g.second.front()) : groupTile(g.first, g.second, !seriesOf(g.second.front()).empty()));
	return tiles;
}

static void addShelf(std::vector<CMovieWall::Shelf> &shelves, const std::string &title, std::vector<CMovieWall::Tile> tiles)
{
	if (!tiles.empty())
		shelves.push_back({ title, std::move(tiles), 0, 0 });
}

static std::vector<CMovieWall::Shelf> wallOf(const std::vector<MI_MOVIE_INFO *> &movies)
{
	std::vector<MI_MOVIE_INFO *> newest = movies;
	std::stable_sort(newest.begin(), newest.end(), [](const MI_MOVIE_INFO *a, const MI_MOVIE_INFO *b) { return a->file.Time > b->file.Time; });

	std::vector<MI_MOVIE_INFO *> started;
	for (MI_MOVIE_INFO *m : newest)
		if (m->bookmarks.lastPlayStop > 0)
			started.push_back(m);
	std::stable_sort(started.begin(), started.end(), [](const MI_MOVIE_INFO *a, const MI_MOVIE_INFO *b) { return a->dateOfLastPlay > b->dateOfLastPlay; });

	std::vector<MI_MOVIE_INFO *> films, docs, other;
	std::vector<std::pair<std::string, std::vector<MI_MOVIE_INFO *>>> series, genres;
	auto into = [](std::vector<std::pair<std::string, std::vector<MI_MOVIE_INFO *>>> &groups, const std::string &key, MI_MOVIE_INFO *m) {
		for (auto &g : groups)
			if (g.first == key) {
				g.second.push_back(m);
				return;
			}
		groups.push_back({ key, { m } });
	};
	for (MI_MOVIE_INFO *m : newest) {
		std::string name = seriesOf(m);
		if (recordings::documentary(m->genreMajor))
			docs.push_back(m);
		else if (!name.empty())
			into(series, name, m);
		else if (recordings::genre_class(m->genreMajor) == 1)
			films.push_back(m);
		else if (recordings::genre_class(m->genreMajor))
			into(genres, GetGenre(m->genreMajor & 0xf0), m);
		else
			other.push_back(m);
	}
	std::stable_sort(genres.begin(), genres.end(), [](const auto &a, const auto &b) { return a.second.size() > b.second.size(); });

	std::vector<CMovieWall::Shelf> shelves;
	std::vector<CMovieWall::Tile> tiles;
	for (size_t i = 0; i < started.size() && i < RECENT_TILES; i++)
		tiles.push_back(single(started[i]));
	addShelf(shelves, g_Locale->getText(LOCALE_MOVIEBROWSER_WALL_CONTINUE), std::move(tiles));
	tiles.clear();
	for (size_t i = 0; i < newest.size() && i < RECENT_TILES; i++)
		tiles.push_back(single(newest[i]));
	addShelf(shelves, g_Locale->getText(LOCALE_MOVIEBROWSER_WALL_NEWEST), std::move(tiles));
	addShelf(shelves, g_Locale->getText(LOCALE_MOVIEBROWSER_WALL_MOVIES), titleTiles(films));
	tiles.clear();
	for (auto &s : series)
		tiles.push_back(groupTile(s.first, s.second, true));
	addShelf(shelves, g_Locale->getText(LOCALE_MOVIEBROWSER_WALL_SERIES), std::move(tiles));
	addShelf(shelves, g_Locale->getText(LOCALE_MOVIEBROWSER_WALL_DOCUMENTARIES), titleTiles(docs));
	for (auto &g : genres)
		addShelf(shelves, g.first, titleTiles(g.second));
	addShelf(shelves, g_Locale->getText(LOCALE_MOVIEBROWSER_WALL_OTHER), titleTiles(other));
	return shelves;
}

void CMovieWall::setArea(int ax, int ay, int aw, int ah)
{
	x = ax;
	y = ay;
	width = aw;
	height = ah;
	layout();
}

/* two shelves on screen, the covers upright as the posters are, and a line
   below them with what the tile in the cursor is */
void CMovieWall::layout()
{
	int margin = OFFSET_INNER_MID;
	rows = 2;
	info_h = captionFont()->getHeight() + margin;
	shelf_h = (height - margin - info_h) / rows;
	int w = (shelf_h - titleFont()->getHeight() - captionFont()->getHeight() - lineFont()->getHeight() - 2 * margin) * 2 / 3;
	int h = w * 3 / 2;
	if (w != tile_w || h != tile_h)
		covers.clear();
	tile_w = std::max(w, 1);
	tile_h = std::max(h, 1);
	per_row = std::max(1, (width - margin) / (tile_w + margin));
}

const CMovieWall::Tile *CMovieWall::tile() const
{
	if (shelf >= shelves.size() || shelves[shelf].cursor >= shelves[shelf].tiles.size())
		return NULL;
	return &shelves[shelf].tiles[shelves[shelf].cursor];
}

MI_MOVIE_INFO *CMovieWall::selected() const
{
	const Tile *t = tile();
	return t && !t->group ? t->items.front() : NULL;
}

bool CMovieWall::onGroup() const
{
	const Tile *t = tile();
	return t && t->group;
}

/* a group's recordings, newest first, a row of tiles per shelf */
static std::vector<CMovieWall::Shelf> groupShelves(const CMovieWall::Tile &t, size_t per_row)
{
	std::vector<CMovieWall::Shelf> shelves;
	per_row = std::max<size_t>(per_row, 1);
	for (size_t i = 0; i < t.items.size(); i++) {
		if (i % per_row == 0)
			shelves.push_back({ i ? "" : t.caption, {}, 0, 0 });
		MI_MOVIE_INFO *m = t.items[i];
		std::string episode = episodeOf(m);
		shelves.back().tiles.push_back({ episode.empty() ? titleOf(m) : episode, dateOf(m), { m }, false });
	}
	return shelves;
}

void CMovieWall::build(const std::vector<MI_MOVIE_INFO *> &movies)
{
	const Tile *t = tile();
	MI_MOVIE_INFO *was = t ? t->items.front() : NULL;
	std::string was_shelf = shelf < shelves.size() ? shelves[shelf].title : "";

	std::vector<Shelf> wall = wallOf(movies);
	if (inGroup()) {
		std::vector<Shelf> inside;
		for (Shelf &s : wall)
			for (Tile &g : s.tiles)
				if (g.group && g.caption == group && inside.empty())
					inside = groupShelves(g, per_row);
		if (inside.empty()) {
			group.clear();
			shelves = wall;
			shelf = outside_shelf;
			first_shelf = outside_first;
		} else {
			outside = wall;
			shelves = inside;
		}
	} else
		shelves = wall;

	/* back to the recording the cursor was on, on the shelf it was on */
	if (shelf >= shelves.size())
		shelf = shelves.empty() ? 0 : shelves.size() - 1;
	for (size_t s = 0; s < shelves.size() && was; s++)
		for (size_t i = 0; i < shelves[s].tiles.size(); i++)
			if (std::find(shelves[s].tiles[i].items.begin(), shelves[s].tiles[i].items.end(), was) != shelves[s].tiles[i].items.end()
			    && (shelves[s].title == was_shelf || s == shelf)) {
				shelf = s;
				shelves[s].cursor = i;
				was = NULL;
				break;
			}
	keepVisible();
}

void CMovieWall::enter()
{
	const Tile *t = tile();
	if (!t || !t->group)
		return;
	group = t->caption;
	outside = shelves;
	outside_shelf = shelf;
	outside_first = first_shelf;
	shelves = groupShelves(*t, per_row);
	shelf = first_shelf = 0;
}

void CMovieWall::leave()
{
	if (!inGroup())
		return;
	group.clear();
	shelves = outside;
	shelf = outside_shelf;
	first_shelf = outside_first;
	keepVisible();
}

void CMovieWall::keepVisible()
{
	if (shelf < first_shelf)
		first_shelf = shelf;
	if (shelf >= first_shelf + rows)
		first_shelf = shelf - rows + 1;
	if (shelf >= shelves.size())
		return;
	Shelf &s = shelves[shelf];
	if (s.cursor < s.first)
		s.first = s.cursor;
	if (s.cursor >= s.first + per_row)
		s.first = s.cursor - per_row + 1;
}

bool CMovieWall::key(neutrino_msg_t msg)
{
	if (shelves.empty())
		return false;
	Shelf &s = shelves[shelf];
	if (msg == CRCInput::RC_left) {
		if (s.cursor > 0)
			s.cursor--;
	} else if (msg == CRCInput::RC_right) {
		if (s.cursor + 1 < s.tiles.size())
			s.cursor++;
	} else if (msg == CRCInput::RC_up) {
		if (shelf > 0)
			shelf--;
	} else if (msg == CRCInput::RC_down) {
		if (shelf + 1 < shelves.size())
			shelf++;
	} else if (msg == (neutrino_msg_t) g_settings.key_pageup) {
		s.cursor = s.cursor > (size_t) per_row ? s.cursor - per_row : 0;
	} else if (msg == (neutrino_msg_t) g_settings.key_pagedown) {
		s.cursor = std::min(s.cursor + per_row, s.tiles.size() - 1);
	} else
		return false;
	/* a shelf of a group continues the row above it, column for column */
	if (inGroup() && (msg == CRCInput::RC_up || msg == CRCInput::RC_down))
		shelves[shelf].cursor = std::min(s.cursor, shelves[shelf].tiles.size() - 1);
	keepVisible();
	paint();
	return true;
}

std::string CMovieWall::coverOf(const MI_MOVIE_INFO *m)
{
	std::string base = m->file.Name;
	size_t dot = base.rfind('.');
	if (dot != std::string::npos && dot > base.rfind('/'))
		base.erase(dot);
	for (const char *ext : { ".jpg", ".png" })
		if (access((base + ext).c_str(), R_OK) == 0)
			return base + ext;
	return "";
}

/* the picture fitted into a tile, decoded once */
const CMovieWall::Cover *CMovieWall::cover(const std::string &path)
{
	struct stat st;
	if (path.empty() || stat(path.c_str(), &st) != 0)
		return NULL;
	for (auto it = covers.begin(); it != covers.end(); ++it)
		if (it->path == path && it->mtime == st.st_mtime) {
			covers.splice(covers.begin(), covers, it);
			return &covers.front();
		}
	int w = 0, h = 0;
	g_PicViewer->getSize(path.c_str(), &w, &h);
	if (w < 1 || h < 1)
		return NULL;
	int fw = tile_w, fh = h * tile_w / w;
	if (fh > tile_h) {
		fh = tile_h;
		fw = w * tile_h / h;
	}
	fb_pixel_t *pixels = g_PicViewer->getImage(path, fw, fh);
	if (!pixels)
		return NULL;
	covers.push_front({ path, st.st_mtime, fw, fh, std::shared_ptr<fb_pixel_t>(pixels, [](fb_pixel_t *p) { cs_free_uncached(p); }) });
	if (covers.size() > COVER_CACHE)
		covers.pop_back();
	return &covers.front();
}

void CMovieWall::paintTile(const Tile &t, int tx, int ty, bool cursor)
{
	CFrameBuffer *fb = CFrameBuffer::getInstance();
	int frame = fb->scale2Res(3);
	if (cursor)
		fb->paintBoxFrame(tx - frame, ty - frame, tile_w + 2 * frame, tile_h + 2 * frame, frame, COL_MENUCONTENTSELECTED_PLUS_0, RADIUS_SMALL);

	const Cover *c = NULL;
	for (const MI_MOVIE_INFO *m : t.items)
		if ((c = cover(coverOf(m))))
			break;
	if (c) {
		if (c->w < tile_w || c->h < tile_h)
			fb->paintBoxRel(tx, ty, tile_w, tile_h, COL_MENUCONTENTDARK_PLUS_0);
		fb->blitBox2FB(c->pixels.get(), c->w, c->h, tx + (tile_w - c->w) / 2, ty + (tile_h - c->h) / 2);
	} else {
		fb->paintBoxRel(tx, ty, tile_w, tile_h, COL_MENUCONTENTDARK_PLUS_0, RADIUS_SMALL);
		int lh = captionFont()->getHeight();
		captionFont()->RenderString(tx + OFFSET_INNER_SMALL, ty + (tile_h + lh) / 2, tile_w - 2 * OFFSET_INNER_SMALL, t.caption, COL_MENUCONTENTDARK_TEXT);
	}
	for (const MI_MOVIE_INFO *m : t.items)
		if (recordings::busy(m->file.Name)) {
			fb->paintIcon(NEUTRINO_ICON_REC, tx + OFFSET_INNER_SMALL, ty + OFFSET_INNER_SMALL);
			break;
		}

	int cy = ty + tile_h + OFFSET_INNER_SMALL;
	int ch = captionFont()->getHeight(), lh = lineFont()->getHeight();
	fb_pixel_t colour = COL_MENUCONTENT_TEXT;
	if (cursor) {
		fb->paintBoxRel(tx - frame, cy, tile_w + 2 * frame, ch + lh, COL_MENUCONTENTSELECTED_PLUS_0, RADIUS_SMALL);
		colour = COL_MENUCONTENTSELECTED_TEXT;
	}
	captionFont()->RenderString(tx, cy + ch, tile_w, t.caption, colour);
	lineFont()->RenderString(tx, cy + ch + lh, tile_w, t.line2, cursor ? colour : COL_MENUCONTENTINACTIVE_TEXT);
}

/* the whole name, the episode, when and how long, which a tile cuts short */
void CMovieWall::paintInfo()
{
	const Tile *t = tile();
	if (!t)
		return;
	std::string info = t->caption;
	auto add = [&info](const std::string &part) {
		if (!part.empty() && info.find(part) == std::string::npos)
			info += " \xc2\xb7 " + part;
	};
	if (t->group)
		add(t->line2);
	else {
		const MI_MOVIE_INFO *m = t->items.front();
		add(titleOf(m));
		add(episodeOf(m));
		add(dateOf(m, true));
		if (m->length > 0) {
			char buf[32];
			snprintf(buf, sizeof(buf), g_Locale->getText(LOCALE_MOVIEBROWSER_WALL_MINUTES), m->length);
			add(buf);
		}
	}
	int margin = OFFSET_INNER_MID;
	captionFont()->RenderString(x + margin, y + height - margin / 2, width - 2 * margin, info, COL_MENUCONTENT_TEXT);
}

void CMovieWall::paint()
{
	CFrameBuffer *fb = CFrameBuffer::getInstance();
	fb->paintBoxRel(x, y, width, height, COL_MENUCONTENT_PLUS_0);
	int margin = OFFSET_INNER_MID;
	int th = titleFont()->getHeight();
	if (shelves.empty()) {
		titleFont()->RenderString(x + margin, y + margin + th, width - 2 * margin, g_Locale->getText(LOCALE_MOVIEBROWSER_WALL_EMPTY), COL_MENUCONTENTINACTIVE_TEXT);
		return;
	}
	for (int r = 0; r < rows && first_shelf + r < shelves.size(); r++) {
		size_t index = first_shelf + r;
		const Shelf &s = shelves[index];
		int sy = y + margin + r * shelf_h;
		titleFont()->RenderString(x + margin, sy + th, width - 2 * margin, s.title, COL_MENUCONTENT_TEXT);
		if (index == shelf && s.tiles.size() > (size_t) per_row) {
			std::string where = to_string(s.cursor + 1) + "/" + to_string(s.tiles.size());
			int ww = titleFont()->getRenderWidth(where);
			titleFont()->RenderString(x + width - margin - ww, sy + th, ww, where, COL_MENUCONTENTINACTIVE_TEXT);
		}
		for (size_t i = s.first, k = 0; i < s.tiles.size() && k < (size_t) per_row; i++, k++)
			paintTile(s.tiles[i], x + margin + (int) k * (tile_w + margin), sy + th + margin, index == shelf && i == s.cursor);
	}
	paintInfo();
}

void CMovieWall::hide()
{
	CFrameBuffer::getInstance()->paintBackgroundBoxRel(x, y, width, height);
}

size_t CMovieWall::count() const
{
	size_t n = 0;
	for (const Shelf &s : shelves)
		n += s.tiles.size();
	return n;
}

std::string CMovieWall::text(size_t i) const
{
	for (const Shelf &s : shelves) {
		if (i < s.tiles.size()) {
			const Tile &t = s.tiles[i];
			return (s.title.empty() ? "" : s.title + ": ") + t.caption + (t.line2.empty() ? "" : " - " + t.line2);
		}
		i -= s.tiles.size();
	}
	return "";
}

size_t CMovieWall::current() const
{
	size_t n = 0;
	for (size_t s = 0; s < shelf && s < shelves.size(); s++)
		n += shelves[s].tiles.size();
	return shelf < shelves.size() ? n + shelves[shelf].cursor : 0;
}

void CMovieWall::select(size_t i)
{
	for (size_t s = 0; s < shelves.size(); s++) {
		if (i < shelves[s].tiles.size()) {
			shelf = s;
			shelves[s].cursor = i;
			keepVisible();
			return;
		}
		i -= shelves[s].tiles.size();
	}
}
