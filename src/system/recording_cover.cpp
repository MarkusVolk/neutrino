/*
	Recording covers: a picture beside each recording, the poster of the
	film or series from TMDB, else the picture of the programme in the media
	library of ARD, ZDF or arte, else a frame of the recording itself

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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "recording_cover.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

#include <ctype.h>
#include <string.h>

#include <global.h>
#include <driver/movieinfo.h>
#include <gui/moviedb/mdb-tmdb.h>
#include <gui/pictureviewer.h>
#include <system/helpers.h>
#include <system/helpers-json.h>
#include <system/recordings.h>
#include <system/set_threadname.h>

extern CPictureViewer *g_PicViewer; /* neutrino.cpp */

namespace
{

std::mutex lock;
std::condition_variable wake;
std::deque<recording_cover::job> jobs;
bool running = false;
/* what TMDB said when a recording began, to put it in its .xml again at the
   end: the recorder may have saved its own version meanwhile; worker only */
std::map<std::string, tmdbPoster> began;

/* DVB content 0x23: documentary, 0x10: film */
#define GENRE_DOCUMENTARY 0x23
#define GENRE_MOVIE 0x10

/* what TMDB knows goes where the EPG said nothing (a recording begun
   before the EPG was there): a series is named as one, so that its
   episodes go together, a documentary or a film gets that genre, here and
   in the moviebrowser; what the EPG or a hand gave stays */
void markInfo(const recording_cover::job &j, const tmdbPoster &found)
{
	MI_MOVIE_INFO mi;
	CMovieInfo info;
	info.clearMovieInfo(&mi);
	mi.file.Name = j.ts;
	if (!info.loadMovieInfo(&mi))
		return;
	bool changed = false;
	if (found.type == "tv" && mi.serieName.empty()) {
		mi.serieName = found.name;
		changed = true;
	}
	if (found.documentary && mi.genreMajor == 0) {
		mi.genreMajor = GENRE_DOCUMENTARY;
		changed = true;
	}
	if (found.type == "movie" && mi.genreMajor == 0) {
		mi.genreMajor = GENRE_MOVIE;
		changed = true;
	}
	if (changed)
		info.saveMovieInfo(mi);
}

bool usable(const std::string &file)
{
	struct stat st;
	return stat(file.c_str(), &st) == 0 && st.st_size > 0;
}

/* ---- the media libraries of ARD, ZDF and arte ---- */

std::string lower(std::string s)
{
	for (size_t i = 0; i < s.size(); i++)
		s[i] = tolower((unsigned char) s[i]);
	return s;
}

/* the words of a channel name: "ZDFneo HD", "BR Fernsehen Süd HD" */
bool channelWord(const std::string &channel, bool (*match)(const std::string &))
{
	std::string word;
	std::string name = lower(channel) + " ";
	for (size_t i = 0; i < name.size(); i++) {
		if (isalnum((unsigned char) name[i])) {
			word += name[i];
			continue;
		}
		if (!word.empty() && match(word))
			return true;
		word.clear();
	}
	return false;
}

bool zdfWord(const std::string &w)
{
	return w.compare(0, 3, "zdf") == 0;
}

bool ardWord(const std::string &w)
{
	static const char *words[] = { "erste", "ard", "br", "hr", "mdr", "ndr", "bremen", "rbb", "sr", "swr", "wdr",
		"one", "tagesschau24", "phoenix", "kika", "alpha", "3sat", NULL };
	for (unsigned int i = 0; words[i]; i++)
		if (w == words[i])
			return true;
	return false;
}

/* a title as its words alone, lower case: what stands between them
   differs ("Das Ordnungsamt - Klemmbrett statt Knarre" in the EPG, with
   "–" in the library) */
std::string words(const std::string &s)
{
	std::string out;
	bool gap = false;
	for (size_t i = 0; i < s.size(); i++) {
		unsigned char ch = s[i], next = i + 1 < s.size() ? s[i + 1] : 0;
		std::string letter;
		if (ch == 0xe2 && next == 0x80)		/* dashes, quotes, ellipsis */
			i += 2;
		else if (ch == 0xc2)			/* middle dot, guillemets, no-break space */
			i++;
		else if (ch == 0xc3 && next >= 0x80) {
			letter = std::string(1, (char) ch) + (char) (next <= 0x9e && next != 0x97 ? next + 0x20 : next);
			i++;
		} else if (ch >= 0x80)
			letter = std::string(1, (char) ch);
		else if (isalnum(ch))
			letter = std::string(1, (char) tolower(ch));
		if (letter.empty()) {
			gap = true;
			continue;
		}
		if (gap && !out.empty())
			out += ' ';
		gap = false;
		out += letter;
	}
	return out;
}

/* words() without the blanks */
std::string plain(const std::string &s)
{
	std::string out = words(s);
	out.erase(std::remove(out.begin(), out.end(), ' '), out.end());
	return out;
}

/* "Sag die Wahrheit (795)", "Golf von Kalifornien (3/4)", "Time (S01/E01)":
   the title without the number of the part */
std::string withoutPart(const std::string &t)
{
	size_t open = t.rfind(" (");
	if (open == std::string::npos || open == 0 || t[t.size() - 1] != ')')
		return t;
	bool digit = false;
	for (size_t i = open + 2; i < t.size() - 1; i++) {
		if (isdigit((unsigned char) t[i]))
			digit = true;
		else if (!strchr("SE/ ", t[i]))
			return t;
	}
	return digit ? t.substr(0, open) : t;
}

/* words() without the article in front */
std::string withoutArticle(const std::string &w)
{
	size_t blank = w.find(' ');
	std::string first = w.substr(0, blank);
	if (blank != std::string::npos && (first == "der" || first == "die" || first == "das"))
		return w.substr(blank + 1);
	return w;
}

/* the name of the programme the library calls it by: the title itself, or
   failing that one that begins with it ("tagesthemen mit Gebärdensprache",
   but not "extra 3" for "Extra"), one the title begins with ("Bayerischer
   Kabarettpreis 2026", "Die Tagesschau vor 20 Jahren") or one that ends
   with it after a dash ("Heiter bis tödlich – Hauptstadtrevier") */
int nameMatch(const std::string &name, const std::string &title)
{
	std::string n = words(withoutPart(name)), t = words(withoutPart(title));
	if (n.empty() || t.empty())
		return 0;
	if (withoutArticle(n) == withoutArticle(t))
		return 2;
	if (n.compare(0, t.size() + 1, t + " ") == 0)
		return n.find_first_not_of("0123456789", t.size() + 1) == std::string::npos ? 0 : 1;
	if (n.size() >= 4 && (t.compare(0, n.size() + 1, n + " ") == 0
			      || withoutArticle(t).compare(0, n.size() + 1, n + " ") == 0))
		return 1;
	size_t dash = name.rfind(" \xe2\x80\x93 ");
	if (dash == std::string::npos)
		dash = name.rfind(" - ");
	if (t.size() >= 10 && dash != std::string::npos && words(withoutPart(name.substr(dash))) == t)
		return 1;
	return 0;
}

/* a picture of a library, and how good it is */
struct libPicture
{
	std::string url;
	int match;		/* of nameMatch(), 0 for none */
	bool upright;
	const char *source;
	libPicture() : match(0), upright(false), source(NULL) {}
};

/* "Narrentanz (2026)", "Narrentanz (2026) (Audiodeskription)": the title alone */
std::string episodeTitle(std::string t)
{
	size_t open = t.find(" (");
	return open == std::string::npos ? t : t.substr(0, open);
}

/* a picture address of the ARD image service at a width */
std::string ardImage(std::string src, const char *width)
{
	size_t w = src.find("{width}");
	if (w != std::string::npos)
		return src.replace(w, 7, width);
	if (src.find("w=") == std::string::npos)
		src += std::string(src.find('?') == std::string::npos ? "?" : (src[src.size() - 1] == '&' ? "" : "&")) + "w=" + width;
	return src;
}

/* what the search of the ARD media library finds for a query: shows
   ("grouping") or single videos ("vod") */
bool ardSearch(const char *kind, const std::string &query, Json::Value &teasers)
{
	std::string url = std::string("https://api.ardmediathek.de/page-gateway/widgets/ard/search/") + kind
		+ "?pageSize=10&searchString=" + encodeUrl(query);
	std::string answer, err;
	Json::Value root;
	if (!getUrl(url, answer, AGENT, 20) || !parseJsonFromString(answer, &root, &err))
		return false;
	teasers = root["teasers"];
	/* nothing is found for "Tagesschau-Nachrichten" */
	std::string apart = query;
	std::replace(apart.begin(), apart.end(), '-', ' ');
	if (teasers.empty() && apart != query)
		return ardSearch(kind, apart, teasers);
	return true;
}

void ardTake(const Json::Value &teaser, int match, libPicture &best)
{
	const Json::Value &images = teaser["images"];
	bool upright = images.isMember("aspect3x4");
	std::string src = images[upright ? "aspect3x4" : "aspect16x9"].get("src", "").asString();
	if (match <= best.match || src.empty())
		return;
	best.url = ardImage(src, upright ? "600" : "768");
	best.match = match;
	best.upright = upright;
	best.source = "ARD";
}

/* shorter videos are trailers and clips */
#define ARD_VIDEO_MIN	(5 * 60)

/* the show of this title, else the video of a programme that is none
   ("Krieg im Weltraum · Wie verwundbar ist Deutschland?") */
bool ardPicture(const std::string &title, libPicture &best)
{
	std::string query = withoutPart(title);
	Json::Value teasers;
	if (ardSearch("grouping", query, teasers))
		for (unsigned int i = 0; i < teasers.size(); i++)
			if (teasers[i].get("type", "").asString() == "show")
				ardTake(teasers[i], nameMatch(teasers[i].get("shortTitle", "").asString(), query), best);
	if (best.match < 2 && ardSearch("vod", query, teasers))
		for (unsigned int i = 0; i < teasers.size(); i++) {
			if (teasers[i].get("duration", 0).asInt() < ARD_VIDEO_MIN)
				continue;
			std::string name = teasers[i].get("longTitle", "").asString();
			int match = nameMatch(episodeTitle(name), query);
			/* "Wildesten Weihnachtshäuser - Folge 1: ..." */
			size_t dash = name.find(" - Folge ");
			if (match < 2 && dash != std::string::npos && nameMatch(name.substr(0, dash), query) == 2)
				match = 2;
			if (match == 2)
				ardTake(teasers[i], match, best);
		}
	/* "37°: Pflege lernen fern der Heimat": the show in front */
	size_t colon = query.find(": ");
	if (!best.match && colon != std::string::npos && ardSearch("grouping", query.substr(0, colon), teasers))
		for (unsigned int i = 0; i < teasers.size(); i++)
			if (teasers[i].get("type", "").asString() == "show"
			    && nameMatch(teasers[i].get("shortTitle", "").asString(), query.substr(0, colon)) == 2)
				ardTake(teasers[i], 1, best);
	return best.match > 0;
}

/* the picture of one episode of an ARD series: the upright poster its
   editorial page shows (the one of Tatort has one for each film), else the
   wide picture of the episode the search finds */
bool ardEpisodePicture(const std::string &series, const std::string &episode, std::string &picture)
{
	std::string slug;
	for (size_t i = 0; i < series.size(); i++) {
		unsigned char ch = series[i];
		if (isalnum(ch))
			slug += (char) tolower(ch);
		else if (!slug.empty() && slug[slug.size() - 1] != '-')
			slug += '-';
	}
	while (!slug.empty() && slug[slug.size() - 1] == '-')
		slug.erase(slug.size() - 1);
	std::string want = plain(episode);
	std::string answer, err;
	Json::Value root;
	std::string url = "https://api.ardmediathek.de/page-gateway/pages/ard/editorial/" + slug + "?embedded=true";
	if (!slug.empty() && getUrl(url, answer, AGENT, 20) && parseJsonFromString(answer, &root, &err)) {
		const Json::Value &widgets = root["widgets"];
		for (unsigned int w = 0; w < widgets.size(); w++) {
			const Json::Value &teasers = widgets[w]["teasers"];
			for (unsigned int i = 0; i < teasers.size(); i++) {
				const Json::Value &t = teasers[i];
				std::string title = t.get("longTitle", t.get("shortTitle", "")).asString();
				std::string src = t["images"]["aspect3x4"].get("src", "").asString();
				if (!src.empty() && plain(episodeTitle(title)) == want) {
					picture = ardImage(src, "600");
					return true;
				}
			}
		}
	}
	/* the search does not find every episode by series and title together
	   ("Tatort MagicMom") */
	std::string queries[] = { series + " " + episode, episode };
	for (unsigned int q = 0; q < 2; q++) {
		Json::Value teasers;
		if (!ardSearch("vod", queries[q], teasers))
			continue;
		for (unsigned int i = 0; i < teasers.size(); i++) {
			const Json::Value &t = teasers[i];
			/* "Polizeiruf 110 - Klassiker" too */
			std::string show = t["show"].get("title", "").asString();
			if (words(show).compare(0, words(series).size(), words(series)) != 0
			    || t.get("duration", 0).asInt() < ARD_VIDEO_MIN)
				continue;
			std::string title = episodeTitle(t.get("longTitle", "").asString());
			if (plain(title) != want && plain(title) != plain(series + episode))
				continue;
			libPicture found;
			ardTake(t, 2, found);
			if (found.match) {
				picture = found.url;
				return true;
			}
		}
	}
	return false;
}

/* the rubric pages of the ARD media library show some thousands of films,
   documentaries and shows with an upright poster each; of them the
   parts of the titles and the picture are kept for a day, in a file too, so
   that a restart does not load them again; worker only */
#define ARD_POSTERS_FILE	CONFIGDIR "/ard-posters"
#define ARD_POSTERS_AGE		(24 * 3600)
struct ardPoster
{
	std::vector<std::string> titles;	/* plain, without a year */
	std::string picture;
};
std::vector<ardPoster> ardPosters;
time_t ardPostersTime = 0;

/* "Dream Scenario | Düstere Komödie", "Das Lied der Tochter · Wolfsland":
   each part may be the title */
void titleParts(const std::string &teaser, std::vector<std::string> &parts)
{
	std::string rest = teaser;
	for (;;) {
		size_t bar = rest.find(" | ");
		size_t dot = rest.find(" \xc2\xb7 ");
		size_t cut = std::min(bar, dot);
		std::string part = plain(episodeTitle(rest.substr(0, cut)));
		if (!part.empty() && std::find(parts.begin(), parts.end(), part) == parts.end())
			parts.push_back(part);
		if (cut == std::string::npos)
			return;
		rest = rest.substr(cut + (cut == bar ? 3 : 4));
	}
}

/* the pages with posters: the start pages of ARD and of its channels, and
   the rubrics as the library lists them under "Alle Rubriken" (films,
   series, documentaries, classical music, ...), else a few known ones */
std::vector<std::string> ardRubrics()
{
	static const char *channels[] = { "ard", "daserste", "br", "hr", "mdr", "ndr", "radiobremen", "rbb", "sr", "swr",
		"wdr", "one", "tagesschau24", "phoenix", "kika", "alpha", "3sat", NULL };
	std::vector<std::string> rubrics;
	for (unsigned int i = 0; channels[i]; i++)
		rubrics.push_back(std::string(channels[i]) + "/home");
	size_t homes = rubrics.size();
	std::string url = "https://api.ardmediathek.de/page-gateway/pages/ard/compilation/alle-rubriken?embedded=true", answer, err;
	Json::Value root;
	if (getUrl(url, answer, AGENT, 20) && parseJsonFromString(answer, &root, &err)) {
		const Json::Value &widgets = root["widgets"];
		for (unsigned int w = 0; w < widgets.size(); w++) {
			const Json::Value &teasers = widgets[w]["teasers"];
			for (unsigned int i = 0; i < teasers.size(); i++) {
				std::string href = teasers[i]["links"]["target"].get("href", "").asString();
				size_t at = href.find("/editorial/");
				if (at == std::string::npos)
					continue;
				std::string slug = href.substr(at + 11, href.find('?', at) == std::string::npos ? std::string::npos : href.find('?', at) - at - 11);
				if (!slug.empty() && std::find(rubrics.begin(), rubrics.end(), "ard/editorial/" + slug) == rubrics.end())
					rubrics.push_back("ard/editorial/" + slug);
			}
		}
	}
	if (rubrics.size() == homes) {
		static const char *known[] = { "filme", "serien", "dokus", "kinderfamilie", "shows_comedy", "kultur", "wissen",
			"klassik", "sport", NULL };
		for (unsigned int i = 0; known[i]; i++)
			rubrics.push_back(std::string("ard/editorial/") + known[i]);
	}
	return rubrics;
}

void loadArdPosters()
{
	std::vector<std::string> rubrics = ardRubrics();
	std::vector<ardPoster> posters;
	for (size_t r = 0; r < rubrics.size(); r++) {
		std::string url = "https://api.ardmediathek.de/page-gateway/pages/" + rubrics[r] + "?embedded=true";
		std::string answer, err;
		Json::Value root;
		if (!getUrl(url, answer, AGENT, 20) || !parseJsonFromString(answer, &root, &err))
			continue;
		const Json::Value &widgets = root["widgets"];
		for (unsigned int w = 0; w < widgets.size(); w++) {
			const Json::Value &teasers = widgets[w]["teasers"];
			for (unsigned int i = 0; i < teasers.size(); i++) {
				const Json::Value &t = teasers[i];
				/* a video's upright picture is a cut of its wide one */
				std::string src = t["images"]["aspect3x4"].get("src", "").asString();
				if (src.empty() || t.get("type", "").asString() == "ondemand")
					continue;
				ardPoster p;
				titleParts(t.get("longTitle", "").asString(), p.titles);
				titleParts(t.get("shortTitle", "").asString(), p.titles);
				p.picture = ardImage(src, "600");
				posters.push_back(p);
			}
		}
	}
	if (posters.empty())
		return;
	ardPosters.swap(posters);
	ardPostersTime = time(NULL);
	/* a line per poster: the picture, then its titles, by tabs */
	std::string tmp = std::string(ARD_POSTERS_FILE) + ".part";
	FILE *f = fopen(tmp.c_str(), "w");
	if (!f)
		return;
	for (size_t i = 0; i < ardPosters.size(); i++) {
		fputs(ardPosters[i].picture.c_str(), f);
		for (size_t t = 0; t < ardPosters[i].titles.size(); t++)
			fprintf(f, "\t%s", ardPosters[i].titles[t].c_str());
		fputc('\n', f);
	}
	if (fclose(f) == 0)
		rename(tmp.c_str(), ARD_POSTERS_FILE);
	else
		unlink(tmp.c_str());
}

/* the list a run before kept, while it is young enough */
void readArdPosters()
{
	struct stat st;
	if (stat(ARD_POSTERS_FILE, &st) != 0 || time(NULL) - st.st_mtime > ARD_POSTERS_AGE)
		return;
	FILE *f = fopen(ARD_POSTERS_FILE, "r");
	if (!f)
		return;
	std::vector<ardPoster> posters;
	char *line = NULL;
	size_t n = 0;
	while (getline(&line, &n, f) > 0) {
		std::string l = line;
		while (!l.empty() && (l[l.size() - 1] == '\n' || l[l.size() - 1] == '\r'))
			l.erase(l.size() - 1);
		ardPoster p;
		size_t tab = l.find('\t');
		p.picture = l.substr(0, tab);
		while (tab != std::string::npos) {
			size_t next = l.find('\t', tab + 1);
			p.titles.push_back(l.substr(tab + 1, next == std::string::npos ? std::string::npos : next - tab - 1));
			tab = next;
		}
		if (!p.picture.empty() && !p.titles.empty())
			posters.push_back(p);
	}
	free(line);
	fclose(f);
	if (!posters.empty()) {
		ardPosters.swap(posters);
		ardPostersTime = st.st_mtime;
	}
}

bool ardRubricPicture(const std::string &title, std::string &picture)
{
	if (!ardPostersTime)
		readArdPosters();
	if (time(NULL) - ardPostersTime > ARD_POSTERS_AGE)
		loadArdPosters();
	std::string want = plain(title);
	for (size_t i = 0; i < ardPosters.size(); i++)
		for (size_t t = 0; t < ardPosters[i].titles.size(); t++)
			if (ardPosters[i].titles[t] == want) {
				picture = ardPosters[i].picture;
				return true;
			}
	return false;
}

bool arteWord(const std::string &w)
{
	return w == "arte";
}

/* arte's search, open like the one of ARD; its upright size is a poster of
   its own, not a cut of the wide picture */
bool artePicture(const std::string &title, libPicture &best)
{
	std::string answer;
	std::string url = "https://api.arte.tv/api/emac/v4/de/web/pages/SEARCH/?mainZonePage=1&query=" + encodeUrl(title);
	Json::Value root;
	std::string err;
	if (!getUrl(url, answer, AGENT, 20) || !parseJsonFromString(answer, &root, &err))
		return false;
	const Json::Value &zones = root["zones"];
	for (unsigned int z = 0; z < zones.size(); z++) {
		const Json::Value &data = zones[z]["content"]["data"];
		for (unsigned int i = 0; i < data.size(); i++) {
			int m = nameMatch(data[i].get("title", "").asString(), title);
			std::string pic = data[i]["mainImage"].get("url", "").asString();
			size_t size = pic.find("__SIZE__");
			if (size == std::string::npos)
				continue;
			pic.replace(size, 8, "300x450");
			if (m > best.match) {
				best.url = pic;
				best.match = m;
				best.upright = true;
				best.source = "arte";
			}
		}
	}
	return best.match > 0;
}

std::string zdfToken;	/* worker only */

/* the token zdf.de hands its own pages, for the API */
bool zdfFetchToken()
{
	std::string url = "https://www.zdf.de/", page;
	zdfToken.clear();
	if (!getUrl(url, page, AGENT, 20))
		return false;
	size_t pos = page.find("videoToken");
	pos = pos == std::string::npos ? pos : page.find("apiToken", pos);
	if (pos == std::string::npos)
		return false;
	pos += 8;
	while (pos < page.size() && !isalnum((unsigned char) page[pos]))
		pos++;
	size_t end = pos;
	while (end < page.size() && isalnum((unsigned char) page[end]))
		end++;
	zdfToken = page.substr(pos, end - pos);
	return zdfToken.size() >= 16;
}

/* what the search of the ZDF media library finds for a query */
bool zdfSearch(const std::string &query, Json::Value &results)
{
	std::string answer;
	for (int attempt = 0; attempt < 2 && answer.empty(); attempt++) {
		if ((zdfToken.empty() || attempt) && !zdfFetchToken())
			return false;
		std::string url = "https://api.zdf.de/search/documents?limit=10&q=" + encodeUrl(query);
		std::string header = "Api-Auth: Bearer " + zdfToken;
		getUrl(url, answer, AGENT, 20, header.c_str());
	}
	Json::Value root;
	std::string err;
	if (answer.empty() || !parseJsonFromString(answer, &root, &err))
		return false;
	results = root["http://zdf.de/rels/search/results"];
	return true;
}

/* the picture of one episode of a ZDF series, found by series and episode
   title together and taken only when the episode title is the very one */
bool zdfEpisodePicture(const std::string &series, const std::string &episode, std::string &picture)
{
	Json::Value results;
	if (episode.empty() || episode.size() > 80 || !zdfSearch(series + " " + episode, results))
		return false;
	for (unsigned int i = 0; i < results.size(); i++) {
		const Json::Value &t = results[i]["http://zdf.de/rels/target"];
		if (t.get("contentType", "").asString() != "episode"
		    || nameMatch(t.get("teaserHeadline", "").asString(), episode) < 2)
			continue;
		picture = t["teaserImageRef"]["layouts"].get("768x432", "").asString();
		if (!picture.empty())
			return true;
	}
	return false;
}

bool zdfPicture(const std::string &title, libPicture &best)
{
	Json::Value results;
	if (!zdfSearch(title, results))
		return false;
	for (unsigned int i = 0; i < results.size(); i++) {
		const Json::Value &t = results[i]["http://zdf.de/rels/target"];
		/* "default" is the page of a series without a brand, such as the
		   ones of ZDFinfo, "topic" the one of a documentary series of the
		   new pages ("Natur macht Geschichte"); an episode or a topic counts
		   only by its very title */
		std::string type = t.get("contentType", "").asString();
		bool exact = type == "episode" || type == "topic";
		if (type != "brand" && type != "default" && !exact)
			continue;
		int m = nameMatch(t.get("teaserHeadline", "").asString(), title);
		if (exact && m < 2)
			continue;
		/* its upright formats are cut out of the wide one, the title cut off */
		std::string pic = t["teaserImageRef"]["layouts"].get("768x432", "").asString();
		if (m > best.match && !pic.empty()) {
			best.url = pic;
			best.match = m;
			best.source = "ZDF";
		}
	}
	return best.match > 0;
}

/* a channel with a media library of its own, where TMDB comes later */
bool publicChannel(const std::string &channel)
{
	return channelWord(channel, zdfWord) || channelWord(channel, arteWord) || channelWord(channel, ardWord);
}

/* the picture of the programme in the media library of its channel: ARD
   and arte, and ZDF with the token of its own pages; the search of ARD
   knows the programmes of ZDF too */
bool libraryPicture(const recording_cover::job &j, libPicture &best)
{
	if (channelWord(j.channel, zdfWord))
		return zdfPicture(j.title, best) || ardPicture(j.title, best);
	if (channelWord(j.channel, arteWord))
		return artePicture(j.title, best);
	if (channelWord(j.channel, ardWord))
		return ardPicture(j.title, best);
	return false;
}

/* "Spielfilm Brasilien/Frankreich 2024", "Krimi, Österreich 2025": the year
   at the end of the first line */
int yearOf(const std::string &text)
{
	std::string line = text.substr(0, text.find('\n'));
	line = trim(line);
	if (line.size() < 6 || line.size() > 100 || line[line.size() - 5] != ' ')
		return 0;
	std::string last = line.substr(line.size() - 4);
	if (last.find_first_not_of("0123456789") != std::string::npos)
		return 0;
	int year = atoi(last.c_str());
	return year > 1900 && year < 2100 ? year : 0;
}

/* a film of TMDB is the one broadcast when it came out about the year the
   EPG names: without one the title alone is any show's ("SUPER.MARKT",
   "Shift") */
bool sameYear(int epg, int film)
{
	return epg > 0 && film > 0 && abs(epg - film) <= 2;
}

/* a picture as JPEG, whatever a library hands out: a PNG with an alpha
   channel takes ten times the room */
bool asJpeg(const std::string &file)
{
	unsigned char magic[2] = { 0, 0 };
	FILE *f = fopen(file.c_str(), "rb");
	if (!f)
		return false;
	size_t n = fread(magic, 1, 2, f);
	fclose(f);
	if (n == 2 && magic[0] == 0xff && magic[1] == 0xd8)
		return true;
	std::string jpeg = file + ".jpeg";
	my_system(14, "nice", "-n", "19", "ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-i", file.c_str(),
		  "-f", "mjpeg", jpeg.c_str());
	bool ok = usable(jpeg) && rename(jpeg.c_str(), file.c_str()) == 0;
	if (!ok)
		unlink(jpeg.c_str());
	return ok;
}

/* a wide picture made upright (2:3, as the posters of TMDB): whole in the
   middle of a blurred and darkened copy of itself, so that the covers of
   the moviebrowser keep one shape while one scrolls through them */
bool upright(const std::string &file)
{
	int w = 0, h = 0;
	g_PicViewer->getSize(file.c_str(), &w, &h);
	if (w <= h)
		return true;
	std::string out = file + ".upright";
	my_system(17, "nice", "-n", "19", "ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-i", file.c_str(),
		  "-filter_complex",
		  "[0:v]scale=600:900:force_original_aspect_ratio=increase,crop=600:900,boxblur=30:2,"
		  "colorchannelmixer=rr=0.6:gg=0.6:bb=0.6[bg];[0:v]scale=600:-2[fg];[bg][fg]overlay=(W-w)/2:(H-h)/2",
		  "-frames:v", "1", "-f", "mjpeg", out.c_str());
	bool ok = usable(out) && rename(out.c_str(), file.c_str()) == 0;
	if (!ok)
		unlink(out.c_str());
	return ok;
}

/* the frame a tenth into the recording, but not in the first minute, where
   the programme before or a trailer may still run; the start if the file is
   shorter than that */
bool still(const recording_cover::job &j, const std::string &tmp)
{
	int at = j.length > 0 ? j.length * 6 : 0;
	if (at < 60)
		at = 60;
	std::string seek[] = { to_string(at), "5" };
	for (unsigned int i = 0; i < 2; i++) {
		unlink(tmp.c_str());
		my_system(19, "nice", "-n", "19", "ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-ss", seek[i].c_str(),
			  "-i", j.ts.c_str(), "-frames:v", "1", "-vf", "scale=480:-2", "-f", "mjpeg", tmp.c_str());
		if (usable(tmp))
			return true;
	}
	return false;
}

void fetch(const recording_cover::job &j)
{
	std::string base = recordings::without_ts(j.ts);
	std::string cover = base + ".jpg";
	std::map<std::string, tmdbPoster>::iterator known = began.find(j.ts);
	if (!j.start && known != began.end()) {
		markInfo(j, known->second);
		began.erase(known);
	}
	/* a cover there is, from before covers were made upright, is only
	   made upright */
	if (access(cover.c_str(), F_OK) == 0) {
		upright(cover);
		return;
	}
	if (access(j.ts.c_str(), R_OK) != 0)
		return;
	std::string tmp = base + ".jpg.part";
	tmdbPoster found;
	const char *source = NULL;
	bool library = publicChannel(j.channel);
	int year = j.year ? j.year : yearOf(j.episode);
	if (!year)
		year = yearOf(j.text);
	bool tmdb = g_settings.tmdb_enabled && CTMDB::findPoster(j.title, library ? year : j.year, found);
	if (tmdb && library && found.type == "movie" && !sameYear(year, found.year))
		tmdb = false;
	if (tmdb) {
		markInfo(j, found);
		if (j.start)
			began[j.ts] = found;
	}
	std::string picture;
	/* "Tatort: Schweigen", "Schlepper - Der Usedom-Krimi": the episode of
	   the series TMDB found */
	std::string series, episode;
	if (tmdb && found.type == "tv" && j.title.size() > found.name.size()) {
		series = found.name;
		bool first = j.title.compare(0, found.name.size(), found.name) == 0;
		bool last = !first && j.title.compare(j.title.size() - found.name.size(), found.name.size(), found.name) == 0;
		if (first)
			episode = j.title.substr(found.name.size());
		else if (last)
			episode = j.title.substr(0, j.title.size() - found.name.size());
		for (bool cut = true; cut && !episode.empty();) {
			size_t at = first ? 0 : episode.size() - 1;
			cut = episode[at] == ' ' || episode[at] == ':' || episode[at] == '-';
			if (cut)
				episode.erase(at, 1);
			else if (episode.size() >= 3 && episode.compare(first ? 0 : episode.size() - 3, 3, "\xe2\x80\x93") == 0) {	/* – */
				episode.erase(first ? 0 : episode.size() - 3, 3);
				cut = true;
			}
		}
	}
	size_t colon = j.title.find(": ");
	if (episode.empty() && colon != std::string::npos && colon >= 3) {
		series = j.title.substr(0, colon);
		episode = j.title.substr(colon + 2);
	}
	if (channelWord(j.channel, ardWord)) {
		/* from an ARD channel: the poster of this film on a start or
		   rubric page of the library, else on the page of its series,
		   before the poster of the series */
		if (ardRubricPicture(withoutPart(j.title), picture) || (!episode.empty() && ardRubricPicture(episode, picture))) {
			unlink(tmp.c_str());
			if (downloadUrl(picture, tmp, AGENT, 30) && usable(tmp) && asJpeg(tmp))
				source = "ARD rubric";
		}
		if (!source && !episode.empty() && ardEpisodePicture(series, episode, picture)) {
			unlink(tmp.c_str());
			if (downloadUrl(picture, tmp, AGENT, 30) && usable(tmp) && asJpeg(tmp))
				source = "ARD episode";
		}
	}
	/* from a ZDF channel: the picture of this episode in the library */
	if (!source && channelWord(j.channel, zdfWord) && zdfEpisodePicture(j.title, j.episode, picture)) {
		unlink(tmp.c_str());
		if (downloadUrl(picture, tmp, AGENT, 30) && usable(tmp) && asJpeg(tmp))
			source = "ZDF episode";
	}
	auto tmdbCover = [&] {
		unlink(tmp.c_str());
		if (downloadUrl(found.url, tmp, AGENT, 30) && usable(tmp))
			source = "TMDB";
	};
	if (!source && library) {
		/* the media library of the channel knows its own programmes better
		   than TMDB, whose titles may be another one's ("nano" of 3sat and
		   a telenovela); only a film's poster is better than a wide picture */
		libPicture lib;
		libraryPicture(j, lib);
		auto libraryCover = [&] {
			unlink(tmp.c_str());
			if (lib.match && downloadUrl(lib.url, tmp, AGENT, 30) && usable(tmp) && asJpeg(tmp))
				source = lib.source;
		};
		if (lib.upright && lib.match == 2)
			libraryCover();
		if (!source && tmdb && found.type == "movie")
			tmdbCover();
		if (!source)
			libraryCover();
	}
	if (!source && tmdb)
		tmdbCover();
	if (!source && !j.start && still(j, tmp))
		source = "frame";
	if (source && upright(tmp) && rename(tmp.c_str(), cover.c_str()) == 0)
		printf("[covers] %s: %s\n", j.ts.c_str(), source);
	else {
		unlink(tmp.c_str());
		if (!j.start)
			printf("[covers] %s: no cover\n", j.ts.c_str());
	}
}

void run()
{
	set_threadname("n:covers");
	std::unique_lock<std::mutex> g(lock);
	for (;;) {
		while (jobs.empty())
			wake.wait(g);
		recording_cover::job j = jobs.front();
		jobs.pop_front();
		g.unlock();
		fetch(j);
		g.lock();
	}
}

} /* namespace */

void recording_cover::queue(const job &j)
{
	{
		std::lock_guard<std::mutex> g(lock);
		for (size_t i = 0; i < jobs.size(); i++)
			if (jobs[i].ts == j.ts && jobs[i].start == j.start)
				return;
		jobs.push_back(j);
		if (!running) {
			running = true;
			std::thread(run).detach();
		}
	}
	wake.notify_one();
}

void recording_cover::queue(const std::string &ts, bool running)
{
	MI_MOVIE_INFO mi;
	CMovieInfo info;
	info.clearMovieInfo(&mi);
	mi.file.Name = ts;
	job j;
	j.ts = ts;
	j.year = 0;
	j.length = 0;
	j.start = running;
	if (info.loadMovieInfo(&mi)) {
		j.title = mi.epgTitle;
		j.episode = mi.epgInfo1;
		j.text = mi.epgInfo2;
		j.channel = mi.channelName;
		j.year = mi.productionDate;
		j.length = mi.length;
	}
	if (j.title.empty()) {
		std::string name = recordings::without_ts(ts);
		j.title = name.substr(name.rfind('/') + 1);
	}
	queue(j);
}
