/*
	Recordings: the .ts files below the record directory, with Neutrino's
	.xml beside them

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

#include "recordings.h"

#include <global.h>
#include <driver/movieinfo.h>
#include <gui/movieplayer.h>
#include <gui/pictureviewer.h>
#include <system/helpers.h>

extern CPictureViewer *g_PicViewer; /* neutrino.cpp */
#include <system/main_call.h>
#include <zapit/client/zapittools.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static std::string hex_of(const std::string &s)
{
	std::string out;
	char b[3];
	for (size_t i = 0; i < s.size(); i++) {
		snprintf(b, sizeof(b), "%02x", (unsigned char) s[i]);
		out += b;
	}
	return out;
}

static bool unhex(const std::string &s, std::string &out)
{
	out.clear();
	if (s.empty() || s.size() % 2 || s.size() > 2048)
		return false;
	for (size_t i = 0; i < s.size(); i += 2) {
		unsigned int b;
		if (!isxdigit((unsigned char) s[i]) || !isxdigit((unsigned char) s[i + 1]) || sscanf(s.c_str() + i, "%2x", &b) != 1)
			return false;
		if (b == 0)
			return false;
		out += (char) b;
	}
	return true;
}

std::string recordings::dir()
{
	std::string d = g_settings.network_nfs_recordingdir;
	while (d.size() > 1 && d[d.size() - 1] == '/')
		d.erase(d.size() - 1);
	return d;
}

/* the record directory and the directories in it, but no hidden one (.timeshift) */
void recordings::files(std::vector<std::string> &rels)
{
	std::string base = dir();
	std::vector<std::string> dirs(1, "");
	for (size_t d = 0; d < dirs.size(); d++) {
		DIR *dh = opendir((base + "/" + dirs[d]).c_str());
		if (!dh)
			continue;
		while (struct dirent *e = readdir(dh)) {
			std::string name = e->d_name;
			if (name.empty() || name[0] == '.')
				continue;
			std::string rel = dirs[d].empty() ? name : dirs[d] + "/" + name;
			struct stat st;
			if (stat((base + "/" + rel).c_str(), &st) != 0)
				continue;
			if (S_ISDIR(st.st_mode) && dirs[d].empty())
				dirs.push_back(rel);
			else if (S_ISREG(st.st_mode) && name.size() > 3 && name.compare(name.size() - 3, 3, ".ts") == 0)
				rels.push_back(rel);
		}
		closedir(dh);
	}
}

bool recordings::find(const std::string &id, std::string &rel, std::string &path)
{
	if (!unhex(id, rel) || rel.find("..") != std::string::npos || rel[0] == '/'
			|| rel.size() < 4 || rel.compare(rel.size() - 3, 3, ".ts") != 0)
		return false;
	path = dir() + "/" + rel;
	struct stat st;
	return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string recordings::without_ts(const std::string &path)
{
	return path.substr(0, path.size() - 3);
}

/* a file the recorder still writes to */
bool recordings::busy(const std::string &path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0 && time(NULL) - st.st_mtime < 30;
}

/* the parts of a second title, between "║" or "|", trimmed and not empty */
static std::vector<std::string> info_parts(const std::string &info1)
{
	std::string parts = info1;
	for (size_t at = parts.find("\xe2\x95\x91"); at != std::string::npos; at = parts.find("\xe2\x95\x91"))	/* ║ */
		parts.replace(at, 3, "|");
	std::vector<std::string> out;
	for (size_t start = 0; start <= parts.size();) {
		size_t end = parts.find('|', start);
		std::string part = parts.substr(start, end == std::string::npos ? std::string::npos : end - start);
		size_t a = part.find_first_not_of(' '), b = part.find_last_not_of(' ');
		if (a != std::string::npos)
			out.push_back(part.substr(a, b - a + 1));
		if (end == std::string::npos)
			break;
		start = end + 1;
	}
	return out;
}

/* the title without the number of its part in brackets at the end */
std::string recordings::without_part(const std::string &t)
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

/* whether a programme is an episode: the EPG numbers it (season and
   episode, or a part number in brackets in the second title), or its title
   carries the number of a part and it is no longer than an episode; a film
   in parts runs longer. DVB has no mark for a series, so this is what
   tells one */
bool recordings::numbered(const std::string &title, const std::string &info1, const std::string &info2, int length)
{
	if (length <= 70 && without_part(title) != title)
		return true;
	const char *words[] = { "staffel", "folge", "episode", "season" };
	for (const std::string *text : { &info1, &info2 }) {
		std::string t = *text;
		for (size_t i = 0; i < t.size(); i++)
			t[i] = tolower((unsigned char) t[i]);
		for (const char *w : words) {
			size_t len = strlen(w);
			for (size_t at = t.find(w); at != std::string::npos; at = t.find(w, at + len)) {
				size_t d = at + len;
				while (d < t.size() && t[d] == ' ')
					d++;
				if ((at == 0 || !isalnum((unsigned char) t[at - 1])) && d > at + len && d < t.size() && isdigit((unsigned char) t[d]))
					return true;
			}
		}
		for (size_t at = 0; at + 3 < t.size(); at++) {
			if (t[at] != 's' || (at > 0 && isalnum((unsigned char) t[at - 1])) || !isdigit((unsigned char) t[at + 1]))
				continue;
			size_t e = at + 1;
			while (e < t.size() && isdigit((unsigned char) t[e]))
				e++;
			while (e < t.size() && t[e] == ' ')
				e++;
			if (e + 1 < t.size() && t[e] == 'e' && isdigit((unsigned char) t[e + 1]))
				return true;
		}
	}
	/* a number in brackets alone as one part of the second title */
	for (const std::string &part : info_parts(info1))
		if (part.size() > 2 && part.front() == '(' && part.back() == ')'
		    && part.find_first_not_of("0123456789", 1) == part.size() - 1)
			return true;
	return false;
}

int recordings::genre_class(int genre)
{
	return genre >= 0x10 && genre < 0xb0 ? genre >> 4 : 0;
}

/* education and science, or the documentary of the news group */
bool recordings::documentary(int genre)
{
	return genre_class(genre) == 0x9 || genre == 0x23;
}

/* named so (by TMDB or by hand), or numbered as an episode by the EPG; DVB
   has no other mark for a series */
std::string recordings::series_of(const std::string &serie, bool episode, const std::string &title)
{
	return !serie.empty() ? serie : episode ? without_part(title) : "";
}

/* letters and digits only, in lower case, for comparing names; of the
   characters beyond ASCII the punctuation of Latin-1 (U+00A0 to U+00BF) and
   the general punctuation and box drawing (dashes, quotes, "║") go too */
static std::string plain(const std::string &s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); i++) {
		unsigned char c = s[i], n = i + 1 < s.size() ? s[i + 1] : 0;
		if (c == 0xc2 && n >= 0xa0 && n <= 0xbf)
			i += 1;
		else if (c == 0xe2 && (n == 0x80 || n == 0x81 || n == 0x94 || n == 0x95))
			i += 2;
		else if (c >= 0x80 || isalnum(c))
			out += (char) tolower(c);
	}
	return out;
}

static bool all_of(const std::string &s, size_t from, size_t to, int (*is)(int))
{
	if (from >= to)
		return false;
	for (size_t i = from; i < to; i++)
		if (!is((unsigned char) s[i]))
			return false;
	return true;
}

/* "S17E4", "S1 E2" or "(12)" */
static bool episode_number(const std::string &part)
{
	if (part.size() > 2 && part.front() == '(' && part.back() == ')')
		return all_of(part, 1, part.size() - 1, isdigit);
	if (part.size() < 4 || tolower((unsigned char) part[0]) != 's')
		return false;
	size_t e = 1;
	while (e < part.size() && isdigit((unsigned char) part[e]))
		e++;
	if (e == 1)
		return false;
	size_t n = e;
	while (n < part.size() && part[n] == ' ')
		n++;
	return n < part.size() && tolower((unsigned char) part[n]) == 'e' && all_of(part, n + 1, part.size(), isdigit);
}

/* a year, an age like "12+" or a country like "GB": no episode name */
static bool aside(const std::string &part)
{
	if (part.size() == 4 && all_of(part, 0, 4, isdigit))
		return true;
	if (part.size() > 1 && part.back() == '+' && all_of(part, 0, part.size() - 1, isdigit))
		return true;
	return part.size() <= 3 && all_of(part, 0, part.size(), isupper);
}

/* "Tatort: Ein Freund, ein guter Freund" gives its part after the series,
   "S17E4 ║ Ein letzter Tropfen ║ 2015 ║ 12+ ║ GB" the number and the first
   part that is no year, age or country and does not just name the series
   again */
std::string recordings::episode_name(const std::string &title, const std::string &serie, const std::string &info1)
{
	std::string name;
	if (!serie.empty() && title.size() > serie.size() && title.compare(0, serie.size(), serie) == 0) {
		name = title.substr(serie.size());
		size_t a = name.find_first_not_of(' ');
		name = a == std::string::npos ? "" : name.substr(a);
		for (const char *sep : { ":", "-", "\xe2\x80\x93" })	/* – */
			if (name.compare(0, strlen(sep), sep) == 0) {
				name = name.substr(strlen(sep));
				break;
			}
		a = name.find_first_not_of(' ');
		name = a == std::string::npos ? "" : name.substr(a);
	}
	std::string number;
	std::string title_plain = plain(title), serie_plain = plain(serie);
	for (const std::string &part : info_parts(info1)) {
		if (episode_number(part)) {
			if (number.empty())
				number = part.front() == '(' ? part.substr(1, part.size() - 2) : part;
		} else if (!aside(part) && name.empty()) {
			std::string p = plain(part);
			if (p.find(title_plain) == std::string::npos && (serie_plain.empty() || p.find(serie_plain) == std::string::npos))
				name = part;
		}
	}
	if (!number.empty() && all_of(number, 0, number.size(), isdigit))
		number = "#" + number;
	if (number.empty() || name.empty())
		return number + name;
	return number + " \xc2\xb7 " + name;	/* · */
}

recordings::entry recordings::describe(const std::string &rel)
{
	std::string path = dir() + "/" + rel;
	struct stat st;
	stat(path.c_str(), &st);
	MI_MOVIE_INFO mi;
	CMovieInfo info;
	info.clearMovieInfo(&mi);
	mi.file.Name = path;
	bool have_info = info.loadMovieInfo(&mi);

	std::string name = rel.substr(rel.rfind('/') == std::string::npos ? 0 : rel.rfind('/') + 1);
	entry e;
	e.id = hex_of(rel);
	e.file = rel;
	e.title = have_info && !mi.epgTitle.empty() ? mi.epgTitle : without_ts(name);
	e.info = have_info ? mi.epgInfo1 : "";
	e.channel = have_info ? mi.channelName : "";
	e.length = have_info ? mi.length : 0;
	e.genre = have_info ? mi.genreMajor : 0;
	e.serie = have_info ? mi.serieName : "";
	e.episode = have_info && numbered(e.title, mi.epgInfo1, mi.epgInfo2, mi.length);
	e.episode_name = episode_name(e.title, e.serie, e.info);
	e.time = st.st_mtime;
	e.size = st.st_size;
	std::string cover = without_ts(path) + ".jpg";
	struct stat cst;
	e.cover = stat(cover.c_str(), &cst) == 0 && access(cover.c_str(), R_OK) == 0;
	e.cover_time = e.cover ? cst.st_mtime : 0;
	int cw = 0, ch = 0;
	if (e.cover)
		g_PicViewer->getSize(cover.c_str(), &cw, &ch);
	e.cover_wide = cw > ch;
	e.busy = busy(path);
	return e;
}

/* the file name part a title gives, as the recorder makes it */
static std::string file_part(const std::string &s)
{
	std::string t = UTF8_TO_FILESYSTEM_ENCODING(s.c_str());
	std::vector<char> buf(t.begin(), t.end());
	buf.push_back(0);
	ZapitTools::replace_char(buf.data());
	return buf.data();
}

static bool fail(recordings::failure &why, int status, const char *code, const std::string &detail)
{
	why.status = status;
	why.code = code;
	why.detail = detail;
	return false;
}

bool recordings::rename(std::string &rel, const std::string &new_title, failure &why)
{
	std::string title = new_title;
	title = trim(title);
	if (title.empty())
		return fail(why, 400, "bad-string", "title is empty");
	std::string path = dir() + "/" + rel;
	if (busy(path))
		return fail(why, 409, "busy", "the recording is still being written");
	MI_MOVIE_INFO mi;
	CMovieInfo info;
	info.clearMovieInfo(&mi);
	mi.file.Name = path;
	bool have_info = info.loadMovieInfo(&mi);

	/* Neutrino names a recording channel_title_date_time: the title part
	   changes, the channel and the time stay and keep the name its own */
	std::string folder = path.substr(0, path.rfind('/') + 1);
	std::string name = without_ts(path.substr(folder.size()));
	std::string stamp;
	if (name.size() > 16 && name[name.size() - 16] == '_' && name[name.size() - 7] == '_')
		stamp = name.substr(name.size() - 16);
	std::string channel = have_info && !mi.channelName.empty() ? file_part(mi.channelName) + "_" : "";
	if (stamp.empty()) {
		struct stat st;
		stat(path.c_str(), &st);
		char buf[32];
		struct tm tm;
		localtime_r(&st.st_mtime, &tm);
		strftime(buf, sizeof(buf), "_%Y%m%d_%H%M%S", &tm);
		stamp = buf;
	}
	std::string base = folder + channel + file_part(title) + stamp;
	std::string old_base = without_ts(path);
	if (base != old_base) {
		if (access((base + ".ts").c_str(), F_OK) == 0)
			return fail(why, 409, "exists", "a recording of that name is there already");
		if (::rename(path.c_str(), (base + ".ts").c_str()) != 0)
			return fail(why, 500, "not-renamed", std::string("the recording could not be renamed: ") + strerror(errno));
		::rename((old_base + ".jpg").c_str(), (base + ".jpg").c_str());
	}
	mi.file.Name = base + ".ts";
	mi.epgTitle = title;
	if (mi.tfile == old_base + ".jpg")
		mi.tfile = base + ".jpg";
	if (!info.saveMovieInfo(mi))
		return fail(why, 500, "not-saved", "the title could not be written");
	if (base != old_base)
		unlink((old_base + ".xml").c_str());
	rel = (base + ".ts").substr(dir().size() + 1);
	return true;
}

bool recordings::play(const std::string &rel, failure &why)
{
	if (CMoviePlayerGui::getInstance().Playing())
		return fail(why, 409, "busy", "the box plays something already");
	std::string title = describe(rel).title;
	std::string path = dir() + "/" + rel;
	/* the player runs in the main loop until the recording ends */
	if (!main_call::post([title, path] {
			std::string t = title, f = path;
			CMoviePlayerGui::getInstance().SetFile(t, f);
			CMoviePlayerGui::getInstance().exec(NULL, "http");
		}))
		return fail(why, 503, "busy", "the box did not take the playback");
	return true;
}
