/*
	Movie Database - TMDb

	Copyright (C) 2015-2020 TangoCash

	License: GPLv2

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation;

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program. If not, see <http://www.gnu.org/licenses/>.
*/


#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <fstream>
#include <set>
#include <sstream>
#include <vector>
#include <string>

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

#include <neutrino.h>
#include <system/settings.h>
#include <system/set_threadname.h>
#include <system/debug.h>
#include <driver/screen_max.h>
#include <global.h>
#include "mdb-tmdb.h"

CTMDB *CTMDB::getInstance()
{
	static CTMDB *tmdb = NULL;
	if (!tmdb)
		tmdb = new CTMDB();
	return tmdb;
}

CTMDB::CTMDB()
{
	key = g_settings.tmdb_api_key;
	posterfile = "/tmp/tmdb.jpg";
	hintbox = NULL;
}

CTMDB::~CTMDB()
{
	cleanup();
}

void CTMDB::setTitle(std::string epgtitle)
{
	minfo.epgtitle = epgtitle;

	hintbox = new CHintBox(LOCALE_MESSAGEBOX_INFO, LOCALE_TMDB_READ_DATA);
	hintbox->paint();

	std::string lang = Lang2ISO639_1(g_settings.language);
	getMovieDetails(lang);
	if ((minfo.result < 1 || minfo.overview.empty()) && lang != "en")
		getMovieDetails("en", true);

	if (hintbox)
	{
		hintbox->hide();
		delete hintbox;
		hintbox = NULL;
	}
}

bool CTMDB::getData(std::string url, Json::Value *root)
{
	std::string answer;
	if (!getUrl(url, answer))
		return false;

	std::string errMsg = "";
	bool ok = parseJsonFromString(answer, root, &errMsg);
	if (!ok)
	{
		printf("Failed to parse JSON\n");
		printf("%s\n", errMsg.c_str());
		return false;
	}
	return true;
}

bool CTMDB::getMovieDetails(std::string lang, bool second)
{
	printf("[TMDB]: %s\n", __func__);
	Json::Value root;
	const std::string urlapi = "http://api.themoviedb.org/3/";
	std::string url	= urlapi + "search/multi?api_key=" + key + "&language=" + lang + "&query=" + encodeUrl(minfo.epgtitle);
	if (!(getData(url, &root)))
		return false;

	minfo.result = root.get("total_results", 0).asInt();
	if (minfo.result == 0)
	{
		std::string title = minfo.epgtitle;
		size_t pos1 = title.find_last_of("(");
		size_t pos2 = title.find_last_of(")");
		if (pos1 != std::string::npos && pos2 != std::string::npos && pos2 > pos1)
		{
			printf("[TMDB]: second try\n");
			title.replace(pos1, pos2 - pos1 + 1, "");
			url	= urlapi + "search/multi?api_key=" + key + "&language=" + lang + "&query=" + encodeUrl(title);
			if (!(getData(url, &root)))
				return false;

			minfo.result = root.get("total_results", 0).asInt();
		}
	}
	printf("[TMDB]: results: %d\n", minfo.result);

	if (minfo.result > 0)
	{
		Json::Value elements = root["results"];
		int use_result = 0;

		if ((minfo.result > 1) && (!second))
			selectResult(elements, minfo.result, use_result);

		if (!second)
		{
			minfo.id = elements[use_result].get("id", -1).asInt();
			minfo.media_type = elements[use_result].get("media_type", "").asString();
		}
		if (minfo.id > -1)
		{
			url = urlapi + minfo.media_type + "/" + to_string(minfo.id) + "?api_key=" + key + "&language=" + lang + "&append_to_response=credits";
			if (!(getData(url, &root)))
				return false;

			minfo.overview = root.get("overview", "").asString();
			minfo.poster_path = root.get("poster_path", "").asString();
			minfo.original_title = root.get("original_title", "").asString();;
			minfo.release_date = root.get("release_date", "").asString();;
			minfo.vote_average = root.get("vote_average", "").asString();;
			minfo.vote_count = root.get("vote_count", 0).asInt();;
			minfo.runtime = root.get("runtime", 0).asInt();;
			if (minfo.media_type == "tv")
			{
				minfo.original_title = root.get("original_name", "").asString();;
				minfo.episodes = root.get("number_of_episodes", 0).asInt();;
				minfo.seasons = root.get("number_of_seasons", 0).asInt();;
				minfo.release_date = root.get("first_air_date", "").asString();;
				elements = root["episode_run_time"];
				minfo.runtimes = elements[0].asString();
				for (unsigned int i = 1; i < elements.size(); i++)
				{
					minfo.runtimes +=  + ", " + elements[i].asString();
				}
			}

			elements = root["genres"];
			minfo.genres = elements[0].get("name", "").asString();
			for (unsigned int i = 1; i < elements.size(); i++)
			{
				minfo.genres += ", " + elements[i].get("name", "").asString();
			}

			elements = root["credits"]["cast"];
			minfo.cast.clear();
			for (unsigned int i = 0; i < elements.size() && i < 10; i++)
			{
				minfo.cast +=  "  " + elements[i].get("character", "").asString() + " (" + elements[i].get("name", "").asString() + ")\n";
				// dprintf(DEBUG_NORMAL, "[TMDB]: %s (%s)\n", elements[i].get("character","").asString().c_str(), elements[i].get("name","").asString().c_str());
			}

			unlink(posterfile.c_str());
			if (hasPoster())
			{
				getBigPoster(posterfile.c_str());
				// dprintf(DEBUG_NORMAL, "[TMDB]: %s (%s) %s\n %s\n %d\n", minfo.epgtitle.c_str(), minfo.original_title.c_str(), minfo.release_date.c_str(), minfo.overview.c_str(), minfo.found);
			}
			return true;
		}
	}
	else
		return false;

	return false;
}

std::string CTMDB::getEPGText()
{
	std::string epgtext("");

	epgtext += "Vote: " + minfo.vote_average.substr(0, 3) + "/10; Votecount: " + to_string(minfo.vote_count) + "\n";
	epgtext += "\n";
	epgtext += minfo.overview + "\n";
	epgtext += "\n";
	if (minfo.media_type == "tv")
		epgtext += g_Locale->getString(LOCALE_EPGVIEWER_LENGTH) + ": " + minfo.runtimes + "\n";
	else
		epgtext += g_Locale->getString(LOCALE_EPGVIEWER_LENGTH) + ": " + to_string(minfo.runtime) + "\n";
	epgtext += g_Locale->getString(LOCALE_EPGVIEWER_GENRE) + ": " + minfo.genres + "\n";
	epgtext += g_Locale->getString(LOCALE_EPGEXTENDED_ORIGINAL_TITLE) + ": " + minfo.original_title + "\n";
	epgtext += g_Locale->getString(LOCALE_EPGEXTENDED_YEAR_OF_PRODUCTION) + ": " + minfo.release_date.substr(0, 4) + "\n";
	if (minfo.media_type == "tv")
		epgtext += "Seasons/Episodes: " + to_string(minfo.seasons) + "/" + to_string(minfo.episodes) + "\n";
	if (!minfo.cast.empty())
		epgtext += g_Locale->getString(LOCALE_EPGEXTENDED_ACTORS) + ":\n" + minfo.cast + "\n";

	return epgtext;
}

std::string CTMDB::getMovieText()
{
	std::string movietext("");

	if (!minfo.overview.empty())
	{
		movietext = minfo.overview + "\n";
		if (!minfo.cast.empty())
		{
			movietext += "\n";
			movietext += g_Locale->getString(LOCALE_EPGEXTENDED_ACTORS) + ":\n";
			movietext += minfo.cast;
		}
	}

	return movietext;
}

void CTMDB::cleanup()
{
	if (access(posterfile.c_str(), F_OK) == 0)
		unlink(posterfile.c_str());
}

void CTMDB::selectResult(Json::Value elements, int results, int &use_result)
{
	if (hintbox)
	{
		hintbox->hide();
		delete hintbox;
		hintbox = NULL;
	}

	int select = 0;

	CMenuWidget *m = new CMenuWidget(LOCALE_TMDB_READ_DATA, NEUTRINO_ICON_TMDB);
	CMenuSelectorTarget *selector = new CMenuSelectorTarget(&select);

	// we don't show introitems, so we add a separator for a smoother view
	m->addItem(GenericMenuSeparator);
	CMenuForwarder *mf;
	int counter = std::min(results, 10);
	for (int i = 0; i != counter; i++)
	{
		if (elements[i].get("media_type", "").asString() == "movie")
			mf = new CMenuForwarder(elements[i].get("title", "").asString(), true, NULL, selector, to_string(i).c_str());
		else
			mf = new CMenuForwarder(elements[i].get("name", "").asString(), true, NULL, selector, to_string(i).c_str());
		m->addItem(mf);
	}

	m->enableSaveScreen();
	m->exec(NULL, "");
	if (!m->gotAction())
		return;
	delete selector;
	delete m;
	use_result = select;
}

/* lower case, letters and digits only: "Der Herr der Ringe: Die Gefährten"
   and "Der Herr der Ringe - Die Gefährten" are the same title */
static std::string titleKey(const std::string &title)
{
	std::string key;
	for (size_t i = 0; i < title.size(); i++) {
		unsigned char ch = title[i];
		if (ch == 0xc3 && i + 1 < title.size()) {
			unsigned char next = title[i + 1];
			if (next >= 0x80 && next <= 0x9e && next != 0x97)
				next += 0x20;
			key += (char)ch;
			key += (char)next;
			i++;
		} else if (ch >= 0x80)
			key += (char)ch;
		else if (isalnum(ch))
			key += (char)tolower(ch);
	}
	return key;
}

static bool tmdbSearch(const std::string &url, const std::string &want, std::string &poster, tmdbPoster &found, bool german = false)
{
	std::string answer, err;
	std::string u = url;
	Json::Value root;
	if (!getUrl(u, answer, AGENT, 20) || !parseJsonFromString(answer, &root, &err))
		return false;
	const Json::Value &results = root["results"];
	for (unsigned int i = 0; i < results.size() && i < 10; i++) {
		const Json::Value &r = results[i];
		std::string type = r.get("media_type", "movie").asString();
		if (type != "movie" && type != "tv")
			continue;
		if (german && r.get("original_language", "").asString() != "de")
			continue;
		/* the poster, else the wide backdrop: the same picture for every
		   episode either way */
		std::string path = r.get("poster_path", "").asString();
		if (!path.empty())
			path = "w342" + path;
		else if (!r.get("backdrop_path", "").asString().empty())
			path = "w780" + r.get("backdrop_path", "").asString();
		else
			continue;
		const char *names[] = { "title", "name", "original_title", "original_name" };
		for (unsigned int n = 0; n < 4; n++)
			if (r.isMember(names[n]) && titleKey(r[names[n]].asString()) == want) {
				poster = path;
				found.type = type;
				found.documentary = false;
				found.year = atoi(r.get("release_date", "").asString().c_str());
				const Json::Value &genres = r["genre_ids"];
				for (unsigned int g = 0; g < genres.size(); g++)
					if (genres[g].asInt() == 99)	/* Documentary */
						found.documentary = true;
				return true;
			}
	}
	return false;
}

bool CTMDB::findPoster(const std::string &title, int year, tmdbPoster &found)
{
	std::string key = settingsText(g_settings.tmdb_api_key);
	if (key.empty() || key.compare(0, 4, "XXXX") == 0)
		return false;
	std::string language = settingsText(g_settings.language);
	std::string lang = Lang2ISO639_1(language);
	if (lang.empty())
		lang = "en";

	std::string query = title;
	size_t open = query.find_last_of('(');
	if (open != std::string::npos && open > 0 && query.find(')', open) != std::string::npos)
		query = query.substr(0, open);
	while (!query.empty() && query[query.size() - 1] == ' ')
		query.erase(query.size() - 1);
	std::string want = titleKey(query);
	if (want.empty())
		return false;

	const std::string api = "http://api.themoviedb.org/3/";
	std::string common = "?api_key=" + key + "&language=" + lang + "&query=" + encodeUrl(query);
	std::string poster;
	bool hit = (year > 1900 && tmdbSearch(api + "search/movie" + common + "&year=" + to_string(year), want, poster, found))
		|| tmdbSearch(api + "search/multi" + common, want, poster, found);
	/* "Tatort: Ein Freund, ein guter Freund": the series, then its episode;
	   "Schlepper - Der Usedom-Krimi": the episode, then its series */
	size_t cut = query.find(": ");
	size_t dash = 3;
	if (cut == std::string::npos) {
		cut = query.find(" - ");
		if (cut == std::string::npos) {
			cut = query.find(" \xe2\x80\x93 ");	/* – */
			dash = 5;
		}
	}
	if (!hit && cut != std::string::npos && cut >= 3) {
		std::string series[2] = { query.substr(0, cut), query[cut] == ':' ? "" : query.substr(cut + dash) };
		for (unsigned int s = 0; s < 2 && !hit; s++) {
			if (series[s].size() < 3)
				continue;
			common = "?api_key=" + key + "&language=" + lang + "&query=" + encodeUrl(series[s]);
			hit = tmdbSearch(api + "search/tv" + common, titleKey(series[s]), poster, found);
			if (hit) {
				found.type = "tv";
				query = series[s];
			}
		}
	}
	/* "Marie Brand und der Tote im Trikot": a film of a series, under the
	   series' name; the longest beginning that names one, but no article */
	if (!hit && cut == std::string::npos) {
		std::vector<std::string> words;
		std::istringstream in(query);
		for (std::string w; in >> w;)
			words.push_back(w);
		static const char *small[] = { "der", "die", "das", "den", "dem", "des", "ein", "eine", "einer",
			"the", "a", "an", "de", "het", "een", "la", "le", "il", NULL };
		/* one word alone ("Happy") names a series too easily: a German one only */
		for (size_t n = std::min<size_t>(words.size() - 1, 6); n >= 1 && !hit; n--) {
			std::string series = words[0];
			for (size_t i = 1; i < n; i++)
				series += " " + words[i];
			if (n == 1) {
				std::string w = titleKey(series);
				bool article = w.size() < 4;
				for (unsigned int i = 0; small[i] && !article; i++)
					article = w == small[i];
				if (article)
					break;
			}
			common = "?api_key=" + key + "&language=" + lang + "&query=" + encodeUrl(series);
			if (tmdbSearch(api + "search/tv" + common, titleKey(series), poster, found, n == 1)) {
				hit = true;
				found.type = "tv";
				query = series;
			}
		}
	}
	if (!hit)
		return false;
	found.name = query;
	found.url = "http://image.tmdb.org/t/p/" + poster;
	return true;
}
