/*
	Games: the systems the box has an emulator for and the games it found
	for them

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

#include "games.h"

#include <global.h>
#include <system/helpers.h>
#include <system/settings.h>

#include <algorithm>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define RECENT_FILE CONFIGDIR "/games.recent"
static const size_t RECENT_GAMES = 24;

static std::vector<std::string> filesWith(const std::string &dir, const std::string &ext)
{
	std::vector<std::string> names;
	DIR *d = opendir(dir.c_str());
	if (!d)
		return names;
	while (struct dirent *e = readdir(d)) {
		std::string name = e->d_name;
		size_t dot = name.rfind('.');
		if (dot != std::string::npos && dot > 0 && strcasecmp(name.c_str() + dot + 1, ext.c_str()) == 0)
			names.push_back(name.substr(0, dot));
	}
	closedir(d);
	std::sort(names.begin(), names.end());
	return names;
}

std::vector<games::system> games::systems()
{
	std::vector<system> list;
	for (const std::string &id : filesWith(GAMESYSTEMDIR, "cfg")) {
		system s;
		s.id = id;
		s.index = 100;
		std::ifstream in(std::string(GAMESYSTEMDIR "/") + id + ".cfg");
		std::string line;
		while (getline(in, line)) {
			size_t eq = line.find('=');
			if (eq == std::string::npos || line[0] == '#')
				continue;
			std::string key = line.substr(0, eq), value = line.substr(eq + 1);
			if (key == "name")
				s.name = value;
			else if (key == "dir")
				s.dir = value;
			else if (key == "ext")
				s.ext = value;
			else if (key == "names")
				s.names = value;
			else if (key == "prefix")
				s.prefix = value;
			else if (key == "covers")
				s.covers = value;
			else if (key == "run")
				s.run = value;
			else if (key == "index")
				s.index = atoi(value.c_str());
		}
		if (!s.name.empty() && !s.dir.empty() && !s.ext.empty() && !s.run.empty())
			list.push_back(s);
	}
	std::stable_sort(list.begin(), list.end(), [](const system &a, const system &b) { return a.index < b.index; });
	return list;
}

std::string games::directory()
{
	std::string rec = settingsText(g_settings.network_nfs_recordingdir);
	while (rec.size() > 1 && rec.back() == '/')
		rec.pop_back();
	std::string above = rec.substr(0, rec.rfind('/') == std::string::npos ? 0 : rec.rfind('/'));
	for (const std::string &dir : { rec + "/games", above + "/games" }) {
		struct stat st;
		if (stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
			return dir;
	}
	return "";
}

static std::string trimmed(const std::string &line, size_t from, size_t to)
{
	while (from < to && line[from] == ' ')
		from++;
	while (to > from && line[to - 1] == ' ')
		to--;
	return line.substr(from, to - from);
}

/* the columns of a line of the table */
static std::vector<std::string> columns(const std::string &line)
{
	std::vector<std::string> c;
	size_t from = 1;
	for (size_t bar; (bar = line.find('|', from)) != std::string::npos; from = bar + 1)
		c.push_back(trimmed(line, from, bar));
	return c;
}

static bool hidden(const std::vector<std::string> &c)
{
	return c[1].find("NW") != std::string::npos || c[7] == "BIOS only" || c[7].compare(0, 8, "System -") == 0;
}

static void readNames(const games::system &s, std::map<std::string, games::game> &found)
{
	std::ifstream in(s.names);
	std::string line;
	std::map<std::string, bool> shown;
	bool listed = false;
	while (getline(in, line)) {
		if (line.empty() || line[0] != '|')
			continue;
		listed = true;
		size_t bar = line.find('|', 1);
		if (bar == std::string::npos)
			continue;
		auto it = found.find(trimmed(line, 1, bar));
		if (it == found.end())
			continue;
		std::vector<std::string> c = columns(line);
		if (c.size() < 8)
			continue;
		bool &show = shown[it->first];
		if (hidden(c) || show)
			continue;
		show = true;
		it->second.title = c[2];
		it->second.year = c[4];
		it->second.maker = c[5];
		it->second.hardware = c[6];
	}
	if (!listed)
		return;
	for (auto it = found.begin(); it != found.end();)
		it = shown[it->first] ? std::next(it) : found.erase(it);
}

std::vector<games::game> games::find(const std::vector<system> &list)
{
	std::vector<game> all;
	std::string base = directory();
	if (base.empty())
		return all;
	for (const system &s : list) {
		std::map<std::string, game> found;
		for (const std::string &name : filesWith(base + "/" + s.dir, s.ext)) {
			game g;
			g.system = s.id;
			g.id = s.prefix + name;
			g.file = base + "/" + s.dir + "/" + name + "." + s.ext;
			g.title = name;
			found[g.id] = g;
		}
		if (!s.names.empty() && !found.empty())
			readNames(s, found);
		for (auto &f : found)
			all.push_back(f.second);
	}
	std::stable_sort(all.begin(), all.end(), [](const game &a, const game &b) { return strcasecmp(a.title.c_str(), b.title.c_str()) < 0; });
	return all;
}

bool games::available()
{
	std::string base = directory();
	if (base.empty())
		return false;
	for (const system &s : systems())
		if (!filesWith(base + "/" + s.dir, s.ext).empty())
			return true;
	return false;
}

std::vector<std::string> games::recent()
{
	std::vector<std::string> list;
	std::ifstream in(RECENT_FILE);
	std::string line;
	while (getline(in, line))
		if (!line.empty())
			list.push_back(line);
	return list;
}

void games::played(const game &g)
{
	std::vector<std::string> list = recent();
	std::string id = g.system + "/" + g.id;
	list.erase(std::remove(list.begin(), list.end(), id), list.end());
	list.insert(list.begin(), id);
	if (list.size() > RECENT_GAMES)
		list.resize(RECENT_GAMES);
	std::string tmp = RECENT_FILE ".tmp";
	std::ofstream out(tmp);
	for (const std::string &l : list)
		out << l << "\n";
	out.close();
	if (!out || rename(tmp.c_str(), RECENT_FILE) != 0)
		unlink(tmp.c_str());
}

std::string games::picture(const game &g)
{
	std::string base = g.file.substr(0, g.file.rfind('.'));
	for (const char *ext : { ".jpg", ".png" })
		if (access((base + ext).c_str(), R_OK) == 0)
			return base + ext;
	return "";
}
