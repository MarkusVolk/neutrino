/*
	Game covers: a picture beside each game, from the collection of
	thumbnails of the libretro project

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

#include "game_cover.h"

#include <global.h>
#include <system/helpers.h>
#include <system/helpers-json.h>
#include <system/set_threadname.h>

#include <atomic>
#include <ctype.h>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <thread>
#include <time.h>
#include <unistd.h>

#define COLLECTIONS "libretro-thumbnails"
/* the list of a collection is asked for again after a month */
#define LIST_AGE (30 * 24 * 3600)

namespace
{

/* the best kind of picture first */
const char *const kinds[] = { "Named_Boxarts", "Named_Titles" };

/* the files of one kind, and by what is left of their names */
struct picture_list
{
	std::set<std::string> names;
	std::map<std::string, std::vector<std::string>> loose;
};

struct job
{
	games::game game;
	std::string collection;
};

std::mutex lock;
game_cover::progress now = { false, 0, 0, 0 };
std::atomic<bool> stopping(false);

/* as the collection names its files */
std::string fileTitle(std::string title)
{
	for (char &c : title)
		if (strchr("&*/:`<>?\\|\"", c))
			c = '_';
	return title;
}

/* the letters and digits of a title, without its version and its article */
std::string looseTitle(const std::string &title)
{
	std::string t;
	for (char c : title.substr(0, title.find_first_of("([")))
		t += tolower((unsigned char) c);
	if (t.compare(0, 9, "disney's ") == 0)
		t.erase(0, 9);
	std::string out, word;
	for (size_t i = 0; i <= t.size(); i++) {
		if (i < t.size() && isalnum((unsigned char) t[i]))
			word += t[i];
		else if (i == t.size() || t[i] != '\'') {
			if (word != "the")
				out += word;
			word.clear();
		}
	}
	return out;
}

bool getJson(std::string url, Json::Value &root)
{
	std::string answer, err;
	return getUrl(url, answer, AGENT, 60) && parseJsonFromString(answer, &root, &err);
}

/* asks for the names of the pictures of a kind; they are kept in file */
bool loadList(const std::string &collection, const std::string &kind, const std::string &file)
{
	Json::Value root;
	if (!getJson("https://api.github.com/repos/" COLLECTIONS "/" + collection + "/git/trees/master", root))
		return false;
	const Json::Value &dirs = root["tree"];
	std::string sha;
	for (Json::ArrayIndex i = 0; dirs.isArray() && i < dirs.size(); i++)
		if (dirs[i]["path"].asString() == kind)
			sha = dirs[i]["sha"].asString();
	if (sha.empty() || sha.find_first_not_of("0123456789abcdef") != std::string::npos)
		return false;
	Json::Value list;
	if (!getJson("https://api.github.com/repos/" COLLECTIONS "/" + collection + "/git/trees/" + sha, list))
		return false;
	std::string tmp = file + ".tmp";
	std::ofstream out(tmp);
	const Json::Value &files = list["tree"];
	for (Json::ArrayIndex i = 0; files.isArray() && i < files.size(); i++)
		out << files[i]["path"].asString() << "\n";
	out.close();
	if (out && rename(tmp.c_str(), file.c_str()) == 0)
		return true;
	unlink(tmp.c_str());
	return false;
}

picture_list readList(const std::string &collection, const std::string &kind)
{
	picture_list l;
	std::string dir = games::directory();
	if (dir.empty())
		return l;
	std::string file = dir + "/." + collection + "." + kind;
	struct stat st;
	if (stat(file.c_str(), &st) != 0 || time(NULL) - st.st_mtime > LIST_AGE)
		loadList(collection, kind, file);
	std::ifstream in(file);
	std::string name;
	while (getline(in, name)) {
		if (name.size() < 5 || name.compare(name.size() - 4, 4, ".png") != 0)
			continue;
		name.erase(name.size() - 4);
		l.names.insert(name);
		std::string loose = looseTitle(name);
		if (!loose.empty())
			l.loose[loose].push_back(name);
	}
	return l;
}

/* the picture for a title, empty when there is none */
std::string match(const picture_list &l, const std::string &title)
{
	std::string exact = fileTitle(title);
	if (l.names.count(exact))
		return exact;
	/* "Puzzle Bobble / Bust-A-Move" is in the collection under one of them */
	for (size_t from = 0; from < title.size();) {
		size_t to = title.find(" / ", from);
		auto it = l.loose.find(looseTitle(title.substr(from, to == std::string::npos ? to : to - from)));
		if (it != l.loose.end()) {
			for (const char *region : { "(World", "(Europe", "(USA" })
				for (const std::string &name : it->second)
					if (name.find(region) != std::string::npos)
						return name;
			return it->second.front();
		}
		from = to == std::string::npos ? title.size() : to + 3;
	}
	auto it = l.loose.find(looseTitle(title));
	return it == l.loose.end() ? "" : it->second.front();
}

std::string escaped(const std::string &s)
{
	std::string out;
	for (unsigned char c : s) {
		char buf[4];
		snprintf(buf, sizeof(buf), "%%%02X", c);
		out += isalnum(c) || strchr("-_.~", c) ? std::string(1, c) : buf;
	}
	return out;
}

bool isPng(const std::string &data)
{
	return data.compare(0, 4, "\x89PNG") == 0;
}

/* the picture as a small JPEG beside the game */
bool fetch(const job &j, const std::string &kind, std::string name)
{
	std::string data;
	/* a picture of the collection may be a link to another one */
	for (int links = 0; links < 3; links++) {
		std::string url = "https://raw.githubusercontent.com/" COLLECTIONS "/" + j.collection + "/master/" + kind + "/" + escaped(name + ".png");
		data.clear();
		if (!getUrl(url, data, AGENT, 60))
			return false;
		if (isPng(data))
			break;
		if (data.size() > 512 || data.size() < 5 || data.compare(data.size() - 4, 4, ".png") != 0)
			return false;
		name = data.substr(0, data.size() - 4);
		name = name.substr(name.rfind('/') + 1);
	}
	if (!isPng(data))
		return false;

	std::string base = j.game.file.substr(0, j.game.file.rfind('.'));
	std::string png = base + ".cover.tmp", jpg = base + ".cover.jpeg";
	std::ofstream out(png, std::ios::binary);
	out << data;
	out.close();
	bool ok = false;
	if (out) {
		my_system(19, "nice", "-n", "19", "ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-f", "png_pipe", "-i", png.c_str(),
			  "-vf", "scale=300:450:force_original_aspect_ratio=decrease", "-frames:v", "1", "-f", "mjpeg", jpg.c_str());
		struct stat st;
		ok = stat(jpg.c_str(), &st) == 0 && st.st_size > 0 && games::picture(j.game).empty() &&
		     rename(jpg.c_str(), (base + ".jpg").c_str()) == 0;
	}
	unlink(png.c_str());
	if (!ok)
		unlink(jpg.c_str());
	return ok;
}

void run(std::vector<job> jobs)
{
	set_threadname("n:gamecovers");
	std::map<std::string, picture_list> lists;
	for (const job &j : jobs) {
		if (stopping)
			break;
		bool ok = false;
		for (const char *kind : kinds) {
			std::string key = j.collection + "/" + kind;
			auto it = lists.find(key);
			if (it == lists.end())
				it = lists.insert({ key, readList(j.collection, kind) }).first;
			std::string name = match(it->second, j.game.title);
			if (!name.empty() && (ok = fetch(j, kind, name)))
				break;
		}
		if (!ok)
			printf("[game covers] none for %s\n", j.game.title.c_str());
		std::lock_guard<std::mutex> g(lock);
		now.done++;
		if (ok)
			now.found++;
	}
	std::lock_guard<std::mutex> g(lock);
	now.running = false;
}

} /* namespace */

bool game_cover::start(const std::vector<games::system> &systems, const std::vector<games::game> &list,
		       const std::vector<std::pair<games::game, std::string>> &programs)
{
	std::vector<job> jobs;
	for (const games::game &g : list)
		for (const games::system &s : systems)
			if (s.id == g.system && !s.covers.empty() && games::picture(g).empty())
				jobs.push_back({ g, s.covers });
	for (const auto &p : programs)
		if (!p.second.empty() && games::picture(p.first).empty())
			jobs.push_back({ p.first, p.second });
	std::lock_guard<std::mutex> g(lock);
	if (jobs.empty() || now.running)
		return false;
	now = { true, jobs.size(), 0, 0 };
	stopping = false;
	std::thread(run, std::move(jobs)).detach();
	return true;
}

void game_cover::stop()
{
	stopping = true;
}

game_cover::progress game_cover::state()
{
	std::lock_guard<std::mutex> g(lock);
	return now;
}
