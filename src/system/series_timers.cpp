/*
	Series timers: rules that record every broadcast of a series

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
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "series_timers.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <json/json.h>

#include <global.h>
#include <neutrino.h>
#include <driver/movieinfo.h>
#include <eitd/sectionsd.h>
#include <gui/timerlist.h>
#include <system/recordings.h>
#include <system/set_threadname.h>
#include <timerdclient/timerdclient.h>
#include <zapit/getservices.h>

#define RULES_FILE	CONFIGDIR "/series-timers.json"
#define MAX_RULES	200
#define LOOK_EVERY	3600	/* seconds; new EPG and a new rule look sooner */
#define PLACEHOLDER_FROM	3	/* broadcasts with one long text */
#define MAX_GENERIC	50

extern bool timeset; /* sectionsd.cpp */

namespace
{

struct Stored
{
	series_timers::rule rule;
	/* the starts of the broadcasts it has set a timer for: one of them
	   without its timer was deleted by the user and is not set again */
	std::vector<time_t> set;
	/* the starts of the broadcasts it has set a timer for itself, which
	   go with the rule */
	std::vector<time_t> made;
	/* long texts that tell no episode, see placeholder() */
	std::vector<std::string> generic;
};

/* what tells one episode from the next */
struct Episode
{
	std::string number;	/* from the title, where it has one */
	std::string info1;
	std::string info2;
};

struct Recorded
{
	std::string title;
	Episode episode;
};

std::mutex lock;		/* the rules, their file and the worker's time */
std::mutex looking;		/* one look at the EPG at a time */
std::condition_variable wake;
std::vector<Stored> stored;
bool loaded = false;
bool running = false;
std::chrono::steady_clock::time_point due;
unsigned int serial = 0;

std::string trimmed(const std::string &s)
{
	size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos)
		return "";
	return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

bool sameTitle(const std::string &a, const std::string &b)
{
	return strcasecmp(series_timers::name(a).c_str(), series_timers::name(b).c_str()) == 0;
}

/* the number of the episode a title carries: "199", "3/6", or nothing */
std::string numberIn(const std::string &title)
{
	std::string rest = trimmed(title).substr(series_timers::name(title).size());
	std::string number;
	for (size_t i = 0; i < rest.size(); i++)
		if (isdigit((unsigned char) rest[i]) || (rest[i] == '/' && !number.empty()))
			number += rest[i];
	return number;
}

std::string text(const Json::Value &v)
{
	return v.isString() ? v.asString() : "";
}

/* under the lock */
void load()
{
	if (loaded)
		return;
	loaded = true;
	std::ifstream in(RULES_FILE);
	if (!in)
		return;
	Json::CharReaderBuilder b;
	Json::Value root;
	std::string errs;
	if (!Json::parseFromStream(b, in, &root, &errs) || !root.isObject() || !root["rules"].isArray())
		return;
	for (const Json::Value &v : root["rules"]) {
		if (!v.isObject())
			continue;
		Stored s;
		s.rule.id = text(v["id"]);
		s.rule.title = text(v["title"]);
		s.rule.channel_id = strtoull(text(v["channel_id"]).c_str(), NULL, 16);
		s.rule.created = v["created"].isIntegral() ? (time_t) v["created"].asInt64() : 0;
		s.rule.timers = 0;
		s.rule.conflicts = 0;
		if (v["set"].isArray())
			for (const Json::Value &t : v["set"])
				if (t.isIntegral())
					s.set.push_back((time_t) t.asInt64());
		if (v["made"].isArray())
			for (const Json::Value &t : v["made"])
				if (t.isIntegral())
					s.made.push_back((time_t) t.asInt64());
		if (v["generic"].isArray())
			for (const Json::Value &t : v["generic"])
				if (t.isString())
					s.generic.push_back(t.asString());
		if (!s.rule.id.empty() && !s.rule.title.empty() && s.rule.channel_id)
			stored.push_back(s);
	}
}

/* under the lock */
bool save()
{
	Json::Value root(Json::objectValue);
	root["rules"] = Json::Value(Json::arrayValue);
	for (const Stored &s : stored) {
		char channel[20];
		snprintf(channel, sizeof(channel), "%" PRIx64, (uint64_t) s.rule.channel_id);
		Json::Value v(Json::objectValue);
		v["id"] = s.rule.id;
		v["title"] = s.rule.title;
		v["channel_id"] = channel;
		v["created"] = (Json::Int64) s.rule.created;
		v["set"] = Json::Value(Json::arrayValue);
		for (time_t t : s.set)
			v["set"].append((Json::Int64) t);
		v["made"] = Json::Value(Json::arrayValue);
		for (time_t t : s.made)
			v["made"].append((Json::Int64) t);
		v["generic"] = Json::Value(Json::arrayValue);
		for (const std::string &t : s.generic)
			v["generic"].append(t);
		root["rules"].append(v);
	}
	Json::StreamWriterBuilder w;
	w["indentation"] = "\t";
	std::string json = Json::writeString(w, root) + "\n";
	std::string tmp = std::string(RULES_FILE) + ".new";
	int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
	if (fd < 0)
		return false;
	bool ok = write(fd, json.data(), json.size()) == (ssize_t) json.size();
	ok &= fsync(fd) == 0;
	ok &= close(fd) == 0;
	if (!ok || rename(tmp.c_str(), RULES_FILE) != 0) {
		unlink(tmp.c_str());
		return false;
	}
	return true;
}

/* the recordings there are, read once in a look and only when a broadcast
   would get a timer: a sleeping disk is not woken every hour */
const std::vector<Recorded> &recordings_known(std::vector<Recorded> &cache, bool &read)
{
	if (read)
		return cache;
	read = true;
	std::vector<std::string> rels;
	recordings::files(rels);
	CMovieInfo info;
	for (size_t i = 0; i < rels.size(); i++) {
		MI_MOVIE_INFO mi;
		info.clearMovieInfo(&mi);
		mi.file.Name = recordings::dir() + "/" + rels[i];
		if (!info.loadMovieInfo(&mi) || mi.epgTitle.empty())
			continue;
		Recorded r;
		r.title = mi.epgTitle;
		r.episode.number = numberIn(mi.epgTitle);
		r.episode.info1 = trimmed(mi.epgInfo1);
		r.episode.info2 = trimmed(mi.epgInfo2);
		cache.push_back(r);
	}
	return cache;
}

/* the texts of an episode in a few letters, to remember them by */
std::string mark(const Episode &e)
{
	uint64_t hash = 0xcbf29ce484222325ULL;
	std::string text = e.info1 + "\n" + e.info2;
	for (size_t i = 0; i < text.size(); i++)
		hash = (hash ^ (unsigned char) text[i]) * 0x100000001b3ULL;
	char hex[20];
	snprintf(hex, sizeof(hex), "%016" PRIx64, hash);
	return hex;
}

/* The same episode again. The second title names the episode where the
   broadcasts of the series differ in it; where they all carry the same one
   ("Die Trödel-Show") or none, the long text has to be the same as well,
   and must not be one of those a channel puts on any episode it has no
   words for. A broadcast with neither text is told from no other. Rather
   an episode twice than one missed. */
bool repeatOf(const Episode &e, const Episode &known, bool distinct, const std::vector<std::string> &generic)
{
	/* numbered by the channel: nothing tells better */
	if (!e.number.empty() && !known.number.empty())
		return e.number == known.number;
	if (e.info1.empty() && e.info2.empty())
		return false;
	if (e.info1 != known.info1)
		return false;
	if (distinct && !e.info1.empty())
		return true;
	if (e.info2.empty() || e.info2 != known.info2)
		return false;
	return std::find(generic.begin(), generic.end(), mark(e)) == generic.end();
}

struct Outcome
{
	int added;
	int timers;
	int conflicts;
};

/* sets the timers the EPG gives for one rule; its list of starts and of
   texts that tell no episode are brought up to date */
Outcome look(Stored &rule_stored, std::vector<Recorded> &cache, bool &cache_read)
{
	const series_timers::rule &r = rule_stored.rule;
	std::vector<time_t> &set = rule_stored.set;
	std::vector<std::string> &generic = rule_stored.generic;
	Outcome out = { 0, 0, 0 };
	time_t now = time(NULL);
	set.erase(std::remove_if(set.begin(), set.end(), [now](time_t t) { return t <= now; }), set.end());
	std::vector<time_t> &made = rule_stored.made;
	made.erase(std::remove_if(made.begin(), made.end(), [now](time_t t) { return t <= now; }), made.end());

	t_channel_id epg_channel = 0;
	{
		CServiceManager::ChannelGuard guard;
		CZapitChannel *channel = CServiceManager::getInstance()->FindChannel(r.channel_id);
		if (channel)
			epg_channel = channel->getEpgID();
	}
	if (!epg_channel)
		return out;

	/* the search finds the title within others too */
	CChannelEventList found, events;
	CEitManager::getInstance()->getEventsServiceKey(epg_channel, found, 1, r.title);
	for (size_t i = 0; i < found.size(); i++)
		if (found[i].eventID && found[i].startTime > now && sameTitle(found[i].description, r.title))
			events.push_back(found[i]);
	std::sort(events.begin(), events.end(), [](const CChannelEvent &a, const CChannelEvent &b) { return a.startTime < b.startTime; });

	std::vector<Episode> episodes(events.size());
	std::set<std::string> second_titles;
	for (size_t i = 0; i < events.size(); i++) {
		CEPGData epg;
		episodes[i].number = numberIn(events[i].description);
		if (CEitManager::getInstance()->getEPGid(events[i].eventID, events[i].startTime, &epg)) {
			episodes[i].info1 = trimmed(epg.info1);
			episodes[i].info2 = trimmed(epg.info2);
		}
		if (!episodes[i].info1.empty())
			second_titles.insert(episodes[i].info1);
	}
	bool distinct = second_titles.size() > 1;
	/* one long text on three broadcasts and more is not an episode shown
	   that often but what the channel writes when it has nothing to say;
	   remembered, as later there may be only two of them to see */
	if (!distinct) {
		std::map<std::string, int> shared;
		for (size_t i = 0; i < episodes.size(); i++) {
			if (episodes[i].info2.empty())
				continue;
			std::string m = mark(episodes[i]);
			if (++shared[m] == PLACEHOLDER_FROM && std::find(generic.begin(), generic.end(), m) == generic.end()) {
				if (generic.size() >= MAX_GENERIC)
					generic.erase(generic.begin());
				generic.push_back(m);
			}
		}
	}

	/* the timers there are: the times they take on this channel, and the
	   episodes of the series they record on any */
	CTimerdClient timerd;
	CTimerd::TimerList timers;
	timerd.getTimerList(timers);
	std::vector<Episode> known;
	std::vector<std::pair<time_t, time_t> > taken;
	std::set<time_t> timed;
	for (size_t i = 0; i < timers.size(); i++) {
		const CTimerd::responseGetTimer &t = timers[i];
		if (t.eventType != CTimerd::TIMER_RECORD && t.eventType != CTimerd::TIMER_IMMEDIATE_RECORD)
			continue;
		if (t.channel_id == r.channel_id) {
			taken.push_back(std::make_pair(t.alarmTime, t.stopTime));
			if (t.epg_starttime)
				timed.insert(t.epg_starttime);
		}
		CEPGData epg;
		if (t.epg_id && CEitManager::getInstance()->getEPGid(t.epg_id, t.epg_starttime, &epg) && sameTitle(epg.title, r.title)) {
			Episode e;
			e.number = numberIn(epg.title);
			e.info1 = trimmed(epg.info1);
			e.info2 = trimmed(epg.info2);
			known.push_back(e);
			if (t.channel_id == r.channel_id && t.alarmTime > now)
				out.timers++;
		}
	}

	for (size_t i = 0; i < events.size(); i++) {
		time_t start = events[i].startTime;
		time_t stop = start + events[i].duration;
		/* a timer for it, also one set by hand or from before the EPG
		   moved the broadcast a little */
		bool has_timer = timed.count(start) > 0;
		for (size_t t = 0; t < taken.size() && !has_timer; t++)
			has_timer = std::min(stop, taken[t].second) - std::max(start, taken[t].first) > (time_t) events[i].duration / 2;
		if (has_timer) {
			if (std::find(set.begin(), set.end(), start) == set.end())
				set.push_back(start);
			continue;
		}
		if (std::find(set.begin(), set.end(), start) != set.end())
			continue;
		bool repeat = false;
		for (size_t k = 0; k < known.size() && !repeat; k++)
			repeat = repeatOf(episodes[i], known[k], distinct, generic);
		if (repeat)
			continue;
		const std::vector<Recorded> &recorded = recordings_known(cache, cache_read);
		for (size_t k = 0; k < recorded.size() && !repeat; k++)
			repeat = sameTitle(recorded[k].title, r.title) && repeatOf(episodes[i], recorded[k].episode, distinct, generic);
		if (repeat)
			continue;
		if (!timerConflicts(start, stop, r.channel_id).empty()) {
			out.conflicts++;
			continue;
		}
		time_t announce = start - (ANNOUNCETIME + 120);
		int id = timerd.addRecordTimerEvent(r.channel_id, start, stop, events[i].eventID, start, announce,
				TIMERD_APIDS_CONF, true, announce > now, "", true);
		if (id <= 0)
			continue;
		printf("[series] \"%s\": timer for %s", r.title.c_str(), ctime(&start));
		set.push_back(start);
		made.push_back(start);
		known.push_back(episodes[i]);
		taken.push_back(std::make_pair(start, stop));
		out.added++;
		out.timers++;
	}
	return out;
}

/* the record timers a rule has set on its channel for these starts, which
   have not begun; a recording that runs is left to end */
void deleteTimers(t_channel_id channel_id, const std::vector<time_t> &starts, time_t keep = 0)
{
	if (starts.empty())
		return;
	CTimerdClient timerd;
	CTimerd::TimerList timers;
	timerd.getTimerList(timers);
	for (size_t i = 0; i < timers.size(); i++) {
		const CTimerd::responseGetTimer &t = timers[i];
		if (t.eventType != CTimerd::TIMER_RECORD || t.channel_id != channel_id ||
		    t.eventState == CTimerd::TIMERSTATE_ISRUNNING ||
		    t.epg_starttime == keep ||
		    std::find(starts.begin(), starts.end(), t.epg_starttime) == starts.end())
			continue;
		timerd.removeTimerEvent(t.eventID);
	}
}

/* looks for the rule of an id, or for all of them; under looking */
void lookAt(const std::string &id, int *added)
{
	/* without the clock every broadcast in the EPG is a coming one */
	if (!timeset)
		return;
	std::vector<Stored> todo;
	{
		std::lock_guard<std::mutex> g(lock);
		load();
		for (const Stored &s : stored)
			if (id.empty() || s.rule.id == id)
				todo.push_back(s);
	}
	std::vector<Recorded> cache;
	bool cache_read = false;
	for (Stored &s : todo) {
		Stored before = s;
		Outcome out = look(s, cache, cache_read);
		if (added)
			*added += out.added;
		bool gone = true;
		{
			std::lock_guard<std::mutex> g(lock);
			for (Stored &now_stored : stored) {
				if (now_stored.rule.id != s.rule.id)
					continue;
				gone = false;
				now_stored.rule.timers = out.timers;
				now_stored.rule.conflicts = out.conflicts;
				if (s.set != before.set || s.made != before.made || s.generic != before.generic) {
					now_stored.set = s.set;
					now_stored.made = s.made;
					now_stored.generic = s.generic;
					save();
				}
			}
		}
		/* deleted while it looked: what it set meanwhile goes as well */
		if (gone)
			deleteTimers(s.rule.channel_id, s.made);
	}
}

void run()
{
	set_threadname("n:series");
	std::unique_lock<std::mutex> g(lock);
	for (;;) {
		while (std::chrono::steady_clock::now() < due)
			wake.wait_until(g, due);
		due = std::chrono::steady_clock::now() + std::chrono::seconds(LOOK_EVERY);
		g.unlock();
		{
			std::lock_guard<std::mutex> one(looking);
			lookAt("", NULL);
		}
		g.lock();
	}
}

} /* namespace */

std::string series_timers::name(const std::string &title)
{
	std::string t = trimmed(title);
	for (size_t before = 0; before != t.size();) {
		before = t.size();
		/* "(199)", "(3/6)" */
		size_t open = t.rfind('(');
		if (open != std::string::npos && open > 0 && t.size() - open > 2 && t[t.size() - 1] == ')'
		    && t.find_first_not_of("0123456789/", open + 1) == t.size() - 1)
			t = trimmed(t.substr(0, open));
		/* "- Folge 119" */
		size_t last = t.find_last_not_of("0123456789");
		if (last != std::string::npos && last + 1 < t.size()) {
			std::string head = trimmed(t.substr(0, last + 1));
			if (head.size() > 6 && strcasecmp(head.c_str() + head.size() - 5, "Folge") == 0) {
				head = trimmed(head.substr(0, head.size() - 5));
				if (head.size() > 3 && head.compare(head.size() - 3, 3, "\xe2\x80\x93") == 0)	/* – */
					head.erase(head.size() - 3);
				else if (!head.empty() && strchr("-:,", head[head.size() - 1]))
					head.erase(head.size() - 1);
				t = trimmed(head);
			}
		}
	}
	return t.empty() ? trimmed(title) : t;
}

std::vector<series_timers::rule> series_timers::list()
{
	std::lock_guard<std::mutex> g(lock);
	load();
	std::vector<rule> out;
	for (const Stored &s : stored)
		out.push_back(s.rule);
	return out;
}

bool series_timers::find(const std::string &title, t_channel_id channel_id, rule &r)
{
	std::lock_guard<std::mutex> g(lock);
	load();
	for (const Stored &s : stored) {
		if (s.rule.channel_id == channel_id && sameTitle(s.rule.title, title)) {
			r = s.rule;
			return true;
		}
	}
	return false;
}

bool series_timers::add(const std::string &title, t_channel_id channel_id, rule &r, int &added)
{
	added = 0;
	r.id.clear();
	std::string series = name(title);
	if (series.empty() || !channel_id)
		return false;
	if (find(series, channel_id, r))
		return false;
	{
		std::lock_guard<std::mutex> g(lock);
		if (stored.size() >= MAX_RULES)
			return false;
		char id[24];
		snprintf(id, sizeof(id), "%lx%02x", (unsigned long) time(NULL), serial++ & 0xff);
		Stored s;
		s.rule.id = id;
		s.rule.title = series;
		s.rule.channel_id = channel_id;
		s.rule.created = time(NULL);
		s.rule.timers = 0;
		s.rule.conflicts = 0;
		stored.push_back(s);
		save();
		r = s.rule;
	}
	{
		std::lock_guard<std::mutex> one(looking);
		lookAt(r.id, &added);
	}
	rule now_rule;
	if (find(series, channel_id, now_rule))
		r = now_rule;
	kick(LOOK_EVERY);
	return true;
}

bool series_timers::remove(const std::string &id, time_t keep)
{
	Stored gone;
	{
		std::lock_guard<std::mutex> g(lock);
		load();
		size_t i = 0;
		while (i < stored.size() && stored[i].rule.id != id)
			i++;
		if (i == stored.size())
			return false;
		gone = stored[i];
		stored.erase(stored.begin() + i);
		save();
	}
	deleteTimers(gone.rule.channel_id, gone.made, keep);
	return true;
}

void series_timers::kick(int delay)
{
	{
		std::lock_guard<std::mutex> g(lock);
		std::chrono::steady_clock::time_point at = std::chrono::steady_clock::now() + std::chrono::seconds(delay);
		if (!running) {
			running = true;
			due = at;
			std::thread(run).detach();
		} else if (at < due)
			due = at;
	}
	wake.notify_one();
}
