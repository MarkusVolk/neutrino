/*
	Recording covers: a picture beside each recording, the poster of the
	film or series from TMDB, else the picture of the programme in the media
	library of ARD or arte, else a frame of the recording itself

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

/* DVB content 0x23: documentary */
#define GENRE_DOCUMENTARY 0x23

/* what TMDB knows goes where the EPG said nothing: a series is named as
   one, so that its episodes go together, a documentary gets that genre,
   here and in the moviebrowser; what the EPG or a hand gave stays */
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
	if (changed)
		info.saveMovieInfo(mi);
}

bool usable(const std::string &file)
{
	struct stat st;
	return stat(file.c_str(), &st) == 0 && st.st_size > 0;
}

/* ---- the media libraries of ARD and arte ---- */

std::string lower(std::string s)
{
	for (size_t i = 0; i < s.size(); i++)
		s[i] = tolower((unsigned char) s[i]);
	return s;
}

/* the words of a channel name: "Das Erste HD", "BR Fernsehen Süd HD" */
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

bool ardWord(const std::string &w)
{
	static const char *words[] = { "erste", "ard", "br", "hr", "mdr", "ndr", "bremen", "rbb", "sr", "swr", "wdr",
		"one", "tagesschau24", "phoenix", "kika", "alpha", "3sat", NULL };
	for (unsigned int i = 0; words[i]; i++)
		if (w == words[i])
			return true;
	return false;
}

/* the name of the programme the library calls it by: the title itself, or
   failing that one that begins with it ("tagesthemen mit Gebärdensprache") */
int nameMatch(const std::string &name, const std::string &title)
{
	std::string n = lower(name), t = lower(title);
	if (n == t)
		return 2;
	if (n.size() > t.size() && n.compare(0, t.size(), t) == 0 && !isalnum((unsigned char) n[t.size()]))
		return 1;
	return 0;
}

bool ardPicture(const std::string &title, std::string &picture)
{
	std::string answer;
	std::string url = "https://api.ardmediathek.de/page-gateway/widgets/ard/search/grouping?pageSize=10&searchString=" + encodeUrl(title);
	Json::Value root;
	std::string err;
	if (!getUrl(url, answer, AGENT, 20) || !parseJsonFromString(answer, &root, &err))
		return false;
	const Json::Value &teasers = root["teasers"];
	int best = 0;
	for (unsigned int i = 0; i < teasers.size(); i++) {
		const Json::Value &t = teasers[i];
		if (t.get("type", "").asString() != "show")
			continue;
		int m = nameMatch(t.get("shortTitle", "").asString(), title);
		const Json::Value &images = t["images"];
		bool portrait = images.isMember("aspect3x4");
		std::string pic = images[portrait ? "aspect3x4" : "aspect16x9"].get("src", "").asString();
		size_t w = pic.find("{width}");
		if (w != std::string::npos)
			pic.replace(w, 7, portrait ? "600" : "768");
		if (m > best && !pic.empty()) {
			best = m;
			picture = pic;
		}
	}
	return best > 0;
}

/* letters and digits only, lower case: "Schweigen (2024)" against
   "Schweigen" needs the year cut off first */
std::string plain(const std::string &s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); i++) {
		unsigned char ch = s[i];
		if (ch >= 0x80 || isalnum(ch))
			out += (char) tolower(ch);
	}
	return out;
}

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
	answer.clear();
	url = "https://api.ardmediathek.de/page-gateway/widgets/ard/search/vod?pageSize=10&searchString=" + encodeUrl(series + " " + episode);
	if (!getUrl(url, answer, AGENT, 20) || !parseJsonFromString(answer, &root, &err))
		return false;
	const Json::Value &teasers = root["teasers"];
	for (unsigned int i = 0; i < teasers.size(); i++) {
		const Json::Value &t = teasers[i];
		if (plain(t["show"].get("title", "").asString()) != plain(series))
			continue;
		std::string title = episodeTitle(t.get("longTitle", "").asString());
		if (plain(title) != want && plain(title) != plain(series + episode))
			continue;
		const Json::Value &images = t["images"];
		bool upright = images.isMember("aspect3x4");
		std::string src = images[upright ? "aspect3x4" : "aspect16x9"].get("src", "").asString();
		if (!src.empty()) {
			picture = ardImage(src, upright ? "600" : "768");
			return true;
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
bool artePicture(const std::string &title, std::string &picture)
{
	std::string answer;
	std::string url = "https://api.arte.tv/api/emac/v4/de/web/pages/SEARCH/?mainZonePage=1&query=" + encodeUrl(title);
	Json::Value root;
	std::string err;
	if (!getUrl(url, answer, AGENT, 20) || !parseJsonFromString(answer, &root, &err))
		return false;
	const Json::Value &zones = root["zones"];
	int best = 0;
	for (unsigned int z = 0; z < zones.size(); z++) {
		const Json::Value &data = zones[z]["content"]["data"];
		for (unsigned int i = 0; i < data.size(); i++) {
			int m = nameMatch(data[i].get("title", "").asString(), title);
			std::string pic = data[i]["mainImage"].get("url", "").asString();
			size_t size = pic.find("__SIZE__");
			if (size == std::string::npos)
				continue;
			pic.replace(size, 8, "300x450");
			if (m > best) {
				best = m;
				picture = pic;
			}
		}
	}
	return best > 0;
}

/* the picture of the programme in the media library of its channel, where
   the library's search is open: ARD and arte; the one of ZDF is not */
const char *libraryPicture(const recording_cover::job &j, std::string &picture)
{
	if (channelWord(j.channel, arteWord))
		return artePicture(j.title, picture) ? "arte" : NULL;
	if (channelWord(j.channel, ardWord))
		return ardPicture(j.title, picture) ? "ARD" : NULL;
	return NULL;
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
	bool tmdb = g_settings.tmdb_enabled && CTMDB::findPoster(j.title, j.year, found);
	if (tmdb) {
		markInfo(j, found);
		if (j.start)
			began[j.ts] = found;
	}
	std::string picture;
	/* "Tatort: Schweigen", "Schlepper - Der Usedom-Krimi": the episode of
	   the series TMDB found */
	std::string episode;
	if (tmdb && found.type == "tv" && j.title.size() > found.name.size()) {
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
	if (channelWord(j.channel, ardWord)) {
		/* from an ARD channel: the poster of this film on a start or
		   rubric page of the library, else on the page of its series,
		   before the poster of the series */
		if (ardRubricPicture(j.title, picture) || (!episode.empty() && ardRubricPicture(episode, picture))) {
			unlink(tmp.c_str());
			if (downloadUrl(picture, tmp, AGENT, 30) && usable(tmp) && asJpeg(tmp))
				source = "ARD rubric";
		}
		if (!source && !episode.empty() && ardEpisodePicture(found.name, episode, picture)) {
			unlink(tmp.c_str());
			if (downloadUrl(picture, tmp, AGENT, 30) && usable(tmp) && asJpeg(tmp))
				source = "ARD episode";
		}
	}
	if (!source && tmdb) {
		unlink(tmp.c_str());
		if (downloadUrl(found.url, tmp, AGENT, 30) && usable(tmp))
			source = "TMDB";
	}
	const char *library = source ? NULL : libraryPicture(j, picture);
	if (library) {
		unlink(tmp.c_str());
		if (downloadUrl(picture, tmp, AGENT, 30) && usable(tmp) && asJpeg(tmp))
			source = library;
	}
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
