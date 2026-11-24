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
#include <gui/widget/icons.h>
#include <system/locals.h>
#include <system/recordings.h>

#include <algorithm>
#include <map>
#include <numeric>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

extern const char *GetGenre(const unsigned char contentClassification);

/* the shelves "continue" and "newest" stop at a row's worth or two */
static const size_t RECENT_TILES = 12;

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

CCoverWall::Tile CMovieWall::single(size_t item) const
{
	const MI_MOVIE_INFO *m = movies[item];
	std::string episode = episodeOf(m);
	return { m->serieName.empty() ? titleOf(m) : m->serieName, episode.empty() ? dateOf(m) : episode, { item }, false };
}

CCoverWall::Tile CMovieWall::groupTile(const std::string &name, const std::vector<size_t> &list, bool series) const
{
	return { name, series ? counted(LOCALE_MOVIEBROWSER_WALL_EPISODE, LOCALE_MOVIEBROWSER_WALL_EPISODES, list.size())
			: counted(LOCALE_MOVIEBROWSER_WALL_PARTS, LOCALE_MOVIEBROWSER_WALL_PARTS, list.size()), list, true };
}

/* recordings of the same series or title as one tile, in the order they
   come */
std::vector<CCoverWall::Tile> CMovieWall::titleTiles(const std::vector<size_t> &list) const
{
	std::vector<std::pair<std::string, std::vector<size_t>>> groups;
	std::map<std::string, size_t> index;
	for (size_t i : list) {
		const MI_MOVIE_INFO *m = movies[i];
		std::string series = seriesOf(m);
		std::string key = series.empty() ? titleOf(m) : series;
		auto it = index.find(key);
		if (it == index.end()) {
			index[key] = groups.size();
			groups.push_back({ key, { i } });
		} else
			groups[it->second].second.push_back(i);
	}
	std::vector<Tile> tiles;
	for (auto &g : groups)
		tiles.push_back(g.second.size() == 1 ? single(g.second.front()) : groupTile(g.first, g.second, !seriesOf(movies[g.second.front()]).empty()));
	return tiles;
}

std::vector<CCoverWall::Shelf> CMovieWall::wall() const
{
	std::vector<size_t> newest(movies.size());
	std::iota(newest.begin(), newest.end(), 0);
	std::stable_sort(newest.begin(), newest.end(), [this](size_t a, size_t b) { return movies[a]->file.Time > movies[b]->file.Time; });

	std::vector<size_t> started;
	for (size_t i : newest)
		if (movies[i]->bookmarks.lastPlayStop > 0)
			started.push_back(i);
	std::stable_sort(started.begin(), started.end(), [this](size_t a, size_t b) { return movies[a]->dateOfLastPlay > movies[b]->dateOfLastPlay; });

	std::vector<size_t> films, docs, other;
	std::vector<std::pair<std::string, std::vector<size_t>>> series, genres;
	auto into = [](std::vector<std::pair<std::string, std::vector<size_t>>> &groups, const std::string &key, size_t i) {
		for (auto &g : groups)
			if (g.first == key) {
				g.second.push_back(i);
				return;
			}
		groups.push_back({ key, { i } });
	};
	for (size_t i : newest) {
		const MI_MOVIE_INFO *m = movies[i];
		std::string name = seriesOf(m);
		if (recordings::documentary(m->genreMajor))
			docs.push_back(i);
		else if (!name.empty())
			into(series, name, i);
		else if (recordings::genre_class(m->genreMajor) == 1)
			films.push_back(i);
		else if (recordings::genre_class(m->genreMajor))
			into(genres, GetGenre(m->genreMajor & 0xf0), i);
		else
			other.push_back(i);
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

CCoverWall::Tile CMovieWall::part(size_t item) const
{
	const MI_MOVIE_INFO *m = movies[item];
	std::string episode = episodeOf(m);
	return { episode.empty() ? titleOf(m) : episode, dateOf(m), { item }, false };
}

MI_MOVIE_INFO *CMovieWall::selected() const
{
	const Tile *t = tile();
	return t && !t->group ? movies[t->items.front()] : NULL;
}

void CMovieWall::build(const std::vector<MI_MOVIE_INFO *> &list)
{
	const Tile *t = tile();
	const MI_MOVIE_INFO *was = t ? movies[t->items.front()] : NULL;
	movies = list;
	show(wall(), [this, was](size_t item) { return movies[item] == was; });
}

std::string CMovieWall::picture(size_t item) const
{
	std::string base = movies[item]->file.Name;
	size_t dot = base.rfind('.');
	if (dot != std::string::npos && dot > base.rfind('/'))
		base.erase(dot);
	for (const char *ext : { ".jpg", ".png" })
		if (access((base + ext).c_str(), R_OK) == 0)
			return base + ext;
	return "";
}

void CMovieWall::paintBadge(const Tile &t, int x, int y) const
{
	for (size_t item : t.items)
		if (recordings::busy(movies[item]->file.Name)) {
			CFrameBuffer::getInstance()->paintIcon(NEUTRINO_ICON_REC, x, y);
			break;
		}
}

const char *CMovieWall::emptyText() const
{
	return g_Locale->getText(LOCALE_MOVIEBROWSER_WALL_EMPTY);
}

/* the whole name, the episode, when and how long, which a tile cuts short */
std::string CMovieWall::info(const Tile &t) const
{
	std::string info = t.caption;
	auto add = [&info](const std::string &part) {
		if (!part.empty() && info.find(part) == std::string::npos)
			info += " \xc2\xb7 " + part;
	};
	if (t.group)
		add(t.line2);
	else {
		const MI_MOVIE_INFO *m = movies[t.items.front()];
		add(titleOf(m));
		add(episodeOf(m));
		add(dateOf(m, true));
		if (m->length > 0) {
			char buf[32];
			snprintf(buf, sizeof(buf), g_Locale->getText(LOCALE_MOVIEBROWSER_WALL_MINUTES), m->length);
			add(buf);
		}
	}
	return info;
}
