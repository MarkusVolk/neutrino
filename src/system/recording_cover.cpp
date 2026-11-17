/*
	Recording covers: a picture beside each recording, the poster of the
	film or series from TMDB or else a frame of the recording itself

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

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>

#include <global.h>
#include <driver/movieinfo.h>
#include <gui/moviedb/mdb-tmdb.h>
#include <system/helpers.h>
#include <system/recordings.h>
#include <system/set_threadname.h>

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
	if (access(cover.c_str(), F_OK) == 0 || access(j.ts.c_str(), R_OK) != 0)
		return;
	std::string tmp = base + ".jpg.part";
	tmdbPoster found;
	const char *source = NULL;
	if (g_settings.tmdb_enabled && CTMDB::findPoster(j.title, j.year, found)) {
		markInfo(j, found);
		if (j.start)
			began[j.ts] = found;
		unlink(tmp.c_str());
		if (downloadUrl(found.url, tmp, AGENT, 30) && usable(tmp))
			source = "TMDB";
	}
	if (!source && !j.start && still(j, tmp))
		source = "frame";
	if (source && rename(tmp.c_str(), cover.c_str()) == 0)
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

void recording_cover::queue(const std::string &ts)
{
	MI_MOVIE_INFO mi;
	CMovieInfo info;
	info.clearMovieInfo(&mi);
	mi.file.Name = ts;
	job j;
	j.ts = ts;
	j.year = 0;
	j.length = 0;
	j.start = false;
	if (info.loadMovieInfo(&mi)) {
		j.title = mi.epgTitle;
		j.year = mi.productionDate;
		j.length = mi.length;
	}
	if (j.title.empty()) {
		std::string name = recordings::without_ts(ts);
		j.title = name.substr(name.rfind('/') + 1);
	}
	queue(j);
}
