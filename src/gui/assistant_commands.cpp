/*
	Commands the assistant understands by itself, for a box without a
	language model: a sentence is matched against patterns and runs a tool

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

#include "assistant_commands.h"
#include "assistant_tools.h"

#include <global.h>
#include <neutrino.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum
{
	CMD_HELP,
	CMD_STATUS,
	CMD_ZAP,
	CMD_EPG,
	CMD_EPG_TONIGHT,
	CMD_SEARCH,
	CMD_RECORD,
	CMD_REMIND,
	CMD_TIMERS,
	CMD_TIMER_REMOVE,
	CMD_VOLUME_UP,
	CMD_VOLUME_DOWN,
	CMD_VOLUME,
	CMD_MUTE,
	CMD_UNMUTE,
	CMD_STANDBY,
	CMD_CHANNELS
};

/*
	A pattern is a sentence in lower case; a * stands for what the command
	is about. The first pattern that fits is taken, so of two that could
	both fit the longer one comes first.
*/
static const struct
{
	const char *pattern;
	int command;
} patterns[] =
{
	{ "hilfe",				CMD_HELP },
	{ "befehle",				CMD_HELP },
	{ "was kannst du",			CMD_HELP },
	{ "help",				CMD_HELP },
	{ "commands",				CMD_HELP },
	{ "what can you do",			CMD_HELP },

	{ "ton aus",				CMD_MUTE },
	{ "ton ausschalten",			CMD_MUTE },
	{ "stumm",				CMD_MUTE },
	{ "stummschalten",			CMD_MUTE },
	{ "mute",				CMD_MUTE },
	{ "ton an",				CMD_UNMUTE },
	{ "ton ein",				CMD_UNMUTE },
	{ "ton einschalten",			CMD_UNMUTE },
	{ "ton wieder an",			CMD_UNMUTE },
	{ "unmute",				CMD_UNMUTE },
	{ "lauter",				CMD_VOLUME_UP },
	{ "mach lauter",			CMD_VOLUME_UP },
	{ "louder",				CMD_VOLUME_UP },
	{ "volume up",				CMD_VOLUME_UP },
	{ "leiser",				CMD_VOLUME_DOWN },
	{ "mach leiser",			CMD_VOLUME_DOWN },
	{ "quieter",				CMD_VOLUME_DOWN },
	{ "volume down",			CMD_VOLUME_DOWN },
	{ "lautst\xc3\xa4rke auf *",		CMD_VOLUME },
	{ "lautst\xc3\xa4rke *",		CMD_VOLUME },
	{ "volume to *",			CMD_VOLUME },
	{ "volume *",				CMD_VOLUME },

	{ "standby",				CMD_STANDBY },
	{ "ausschalten",			CMD_STANDBY },
	{ "schalte aus",			CMD_STANDBY },
	{ "schalt aus",				CMD_STANDBY },
	{ "gute nacht",				CMD_STANDBY },
	{ "turn off",				CMD_STANDBY },
	{ "switch off",				CMD_STANDBY },
	{ "power off",				CMD_STANDBY },
	{ "good night",				CMD_STANDBY },

	{ "timer",				CMD_TIMERS },
	{ "timerliste",				CMD_TIMERS },
	{ "timer anzeigen",			CMD_TIMERS },
	{ "zeige timer",			CMD_TIMERS },
	{ "zeig timer",				CMD_TIMERS },
	{ "geplante aufnahmen",			CMD_TIMERS },
	{ "was wird aufgenommen",		CMD_TIMERS },
	{ "timers",				CMD_TIMERS },
	{ "show timers",			CMD_TIMERS },
	{ "list timers",			CMD_TIMERS },
	{ "l\xc3\xb6sche timer *",		CMD_TIMER_REMOVE },
	{ "l\xc3\xb6sch timer *",		CMD_TIMER_REMOVE },
	{ "timer * l\xc3\xb6schen",		CMD_TIMER_REMOVE },
	{ "delete timer *",			CMD_TIMER_REMOVE },
	{ "remove timer *",			CMD_TIMER_REMOVE },
	{ "cancel timer *",			CMD_TIMER_REMOVE },

	{ "sender",				CMD_CHANNELS },
	{ "senderliste",			CMD_CHANNELS },
	{ "kan\xc3\xa4le",			CMD_CHANNELS },
	{ "kanalliste",				CMD_CHANNELS },
	{ "welche sender gibt es",		CMD_CHANNELS },
	{ "channels",				CMD_CHANNELS },
	{ "channel list",			CMD_CHANNELS },
	{ "list channels",			CMD_CHANNELS },
	{ "sender mit *",			CMD_CHANNELS },
	{ "suche sender *",			CMD_CHANNELS },
	{ "kan\xc3\xa4le mit *",		CMD_CHANNELS },
	{ "channels with *",			CMD_CHANNELS },

	{ "status",				CMD_STATUS },
	{ "was l\xc3\xa4uft",			CMD_STATUS },
	{ "was l\xc3\xa4uft gerade",		CMD_STATUS },
	{ "was l\xc3\xa4uft jetzt",		CMD_STATUS },
	{ "was l\xc3\xa4uft da",		CMD_STATUS },
	{ "was l\xc3\xa4uft danach",		CMD_STATUS },
	{ "was kommt gerade",			CMD_STATUS },
	{ "was kommt jetzt",			CMD_STATUS },
	{ "was kommt danach",			CMD_STATUS },
	{ "was kommt als n\xc3\xa4" "chstes",	CMD_STATUS },
	{ "what's on",				CMD_STATUS },
	{ "whats on",				CMD_STATUS },
	{ "what is on",				CMD_STATUS },
	{ "what's on now",			CMD_STATUS },
	{ "what is on now",			CMD_STATUS },
	{ "what's next",			CMD_STATUS },
	{ "whats next",				CMD_STATUS },
	{ "what is next",			CMD_STATUS },

	{ "heute abend",			CMD_EPG_TONIGHT },
	{ "was kommt heute abend",		CMD_EPG_TONIGHT },
	{ "was l\xc3\xa4uft heute abend",	CMD_EPG_TONIGHT },
	{ "tonight",				CMD_EPG_TONIGHT },
	{ "what's on tonight",			CMD_EPG_TONIGHT },
	{ "what is on tonight",			CMD_EPG_TONIGHT },
	{ "was kommt heute abend auf *",	CMD_EPG_TONIGHT },
	{ "was kommt heute abend im *",		CMD_EPG_TONIGHT },
	{ "was l\xc3\xa4uft heute abend auf *",	CMD_EPG_TONIGHT },
	{ "was l\xc3\xa4uft heute abend im *",	CMD_EPG_TONIGHT },
	{ "what's on tonight on *",		CMD_EPG_TONIGHT },
	{ "what is on tonight on *",		CMD_EPG_TONIGHT },

	{ "epg",				CMD_EPG },
	{ "programm",				CMD_EPG },
	{ "programm\xc3\xbc" "bersicht",	CMD_EPG },
	{ "guide",				CMD_EPG },
	{ "was l\xc3\xa4uft gerade auf *",	CMD_EPG },
	{ "was l\xc3\xa4uft jetzt auf *",	CMD_EPG },
	{ "was l\xc3\xa4uft gerade im *",	CMD_EPG },
	{ "was l\xc3\xa4uft jetzt im *",	CMD_EPG },
	{ "was l\xc3\xa4uft auf *",		CMD_EPG },
	{ "was l\xc3\xa4uft im *",		CMD_EPG },
	{ "was kommt gerade auf *",		CMD_EPG },
	{ "was kommt gerade im *",		CMD_EPG },
	{ "was kommt auf *",			CMD_EPG },
	{ "was kommt im *",			CMD_EPG },
	{ "programm von *",			CMD_EPG },
	{ "programm auf *",			CMD_EPG },
	{ "epg von *",				CMD_EPG },
	{ "epg for *",				CMD_EPG },
	{ "epg *",				CMD_EPG },
	{ "guide for *",			CMD_EPG },
	{ "what's on on *",			CMD_EPG },
	{ "what is on on *",			CMD_EPG },
	{ "what's on *",			CMD_EPG },
	{ "whats on *",				CMD_EPG },
	{ "what is on *",			CMD_EPG },

	{ "nimm das auf",			CMD_RECORD },
	{ "nimm das hier auf",			CMD_RECORD },
	{ "nimm die sendung auf",		CMD_RECORD },
	{ "nimm auf",				CMD_RECORD },
	{ "aufnehmen",				CMD_RECORD },
	{ "das aufnehmen",			CMD_RECORD },
	{ "aufnahme",				CMD_RECORD },
	{ "record",				CMD_RECORD },
	{ "record this",			CMD_RECORD },
	{ "record that",			CMD_RECORD },
	{ "nimm * auf",				CMD_RECORD },
	{ "aufnahme von *",			CMD_RECORD },
	{ "* aufnehmen",			CMD_RECORD },
	{ "record *",				CMD_RECORD },
	{ "erinnere mich an *",			CMD_REMIND },
	{ "erinner mich an *",			CMD_REMIND },
	{ "remind me of *",			CMD_REMIND },
	{ "remind me about *",			CMD_REMIND },

	{ "suche nach *",			CMD_SEARCH },
	{ "such nach *",			CMD_SEARCH },
	{ "suche *",				CMD_SEARCH },
	{ "such *",				CMD_SEARCH },
	{ "finde *",				CMD_SEARCH },
	{ "wann kommt *",			CMD_SEARCH },
	{ "wann l\xc3\xa4uft *",		CMD_SEARCH },
	{ "search for *",			CMD_SEARCH },
	{ "search *",				CMD_SEARCH },
	{ "find *",				CMD_SEARCH },
	{ "when is * on",			CMD_SEARCH },
	{ "when is *",				CMD_SEARCH },

	{ "schalte um auf *",			CMD_ZAP },
	{ "schalte um zu *",			CMD_ZAP },
	{ "schalte auf *",			CMD_ZAP },
	{ "schalte zu *",			CMD_ZAP },
	{ "schalt um auf *",			CMD_ZAP },
	{ "schalt auf *",			CMD_ZAP },
	{ "schalt zu *",			CMD_ZAP },
	{ "umschalten auf *",			CMD_ZAP },
	{ "wechsle auf *",			CMD_ZAP },
	{ "wechsle zu *",			CMD_ZAP },
	{ "wechsel auf *",			CMD_ZAP },
	{ "wechsel zu *",			CMD_ZAP },
	{ "gehe auf *",				CMD_ZAP },
	{ "gehe zu *",				CMD_ZAP },
	{ "geh auf *",				CMD_ZAP },
	{ "geh zu *",				CMD_ZAP },
	{ "* einschalten",			CMD_ZAP },
	{ "kanal *",				CMD_ZAP },
	{ "switch to *",			CMD_ZAP },
	{ "change to *",			CMD_ZAP },
	{ "go to *",				CMD_ZAP },
	{ "zap to *",				CMD_ZAP },
	{ "tune to *",				CMD_ZAP },
	{ "put on *",				CMD_ZAP },
	{ "watch *",				CMD_ZAP },
	{ "channel *",				CMD_ZAP }
};

/* said for a channel without being its name */
static const struct
{
	const char *said;
	const char *channel;
} aliases[] =
{
	{ "ersten",	"das erste" },
	{ "erste",	"das erste" },
	{ "ard",	"das erste" },
	{ "zweiten",	"zdf" },
	{ "zweite",	"zdf" }
};

static bool is_filler(const std::string &word)
{
	static const char *const fillers[] = { "bitte", "mal", "doch", "please" };
	for (size_t i = 0; i < sizeof(fillers) / sizeof(fillers[0]); i++)
		if (word == fillers[i])
			return true;
	return false;
}

/* lower case, one space between words, no commas, no full stop at the end, no words that say nothing */
static std::string normalize(const std::string &text)
{
	std::string in = CAssistantTools::lower(text);
	while (!in.empty() && strchr(".!? \t\r\n", in[in.size() - 1]))
		in.erase(in.size() - 1);

	std::string out, word;
	for (size_t i = 0; i <= in.size(); i++)
	{
		char c = i < in.size() ? in[i] : ' ';
		if (c != ' ' && c != '\t' && c != '\n' && c != ',')
		{
			word += c;
			continue;
		}
		if (!word.empty() && !is_filler(word))
			out += (out.empty() ? "" : " ") + word;
		word.clear();
	}
	return out;
}

static bool match(const std::string &text, const char *pattern, std::string &arg)
{
	const char *star = strchr(pattern, '*');
	if (!star)
		return text == pattern;

	size_t head = star - pattern;
	size_t tail = strlen(star + 1);
	if (text.size() <= head + tail || text.compare(0, head, pattern, head) ||
	    text.compare(text.size() - tail, tail, star + 1))
		return false;
	arg = text.substr(head, text.size() - head - tail);
	return true;
}

static bool is_number(const std::string &text)
{
	return !text.empty() && text.size() <= 5 && text.find_first_not_of("0123456789") == std::string::npos;
}

/* "das erste" is a channel, "das zdf" is the channel zdf */
static std::string channel_name(const std::string &said)
{
	static const char *const articles[] = { "das ", "dem ", "den ", "der ", "die ", "the " };
	for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++)
		if (said == aliases[i].said)
			return aliases[i].channel;
	if (CAssistantTools::hasChannel(said))
		return said;
	for (size_t i = 0; i < sizeof(articles) / sizeof(articles[0]); i++)
	{
		size_t len = strlen(articles[i]);
		if (!said.compare(0, len, articles[i]) && said.size() > len)
		{
			std::string rest = said.substr(len);
			for (size_t j = 0; j < sizeof(aliases) / sizeof(aliases[0]); j++)
				if (rest == aliases[j].said)
					return aliases[j].channel;
			/* the name of a channel may begin with what looks like an article */
			return CAssistantTools::hasChannel(rest) ? rest : said;
		}
	}
	return said;
}

static std::string tonight()
{
	char buf[32];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	if (tm.tm_hour < 20)
	{
		tm.tm_hour = 20;
		tm.tm_min = 0;
	}
	strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
	return buf;
}

static bool tool(const char *name, const Json::Value &input, std::string &answer)
{
	printf("[assistant] %s %s\n", name, json_compact(input).c_str());
	CAssistantTools::call(name, input, answer);
	return true;
}

bool CAssistantCommands::run(const std::string &text, std::string &answer)
{
	std::string said = normalize(text);
	std::string arg;
	int command = -1;
	for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]) && command < 0; i++)
	{
		arg.clear();
		if (match(said, patterns[i].pattern, arg))
			command = patterns[i].command;
	}

	Json::Value input(Json::objectValue);
	int volume = g_settings.current_volume;
	switch (command)
	{
		case CMD_HELP:
			answer = g_Locale->getText(LOCALE_ASSISTANT_HELP);
			return true;
		case CMD_STATUS:
			return tool("tv_status", input, answer);
		case CMD_ZAP:
			input["channel"] = channel_name(arg);
			return tool("tv_zap", input, answer);
		case CMD_EPG_TONIGHT:
			input["from"] = tonight();
		/* fall through */
		case CMD_EPG:
			if (!arg.empty())
				input["channel"] = channel_name(arg);
			input["limit"] = 6;
			return tool("tv_epg", input, answer);
		case CMD_SEARCH:
			input["query"] = arg;
			return tool("tv_epg_search", input, answer);
		case CMD_RECORD:
		case CMD_REMIND:
			input["kind"] = command == CMD_RECORD ? "record" : "zap";
			if (!arg.empty())
				input["title"] = arg;
			return tool("tv_timer_add", input, answer);
		case CMD_TIMERS:
			return tool("tv_timers", input, answer);
		case CMD_TIMER_REMOVE:
			if (!is_number(arg))
				return false;
			input["id"] = atoi(arg.c_str());
			return tool("tv_timer_remove", input, answer);
		case CMD_VOLUME_UP:
		case CMD_VOLUME_DOWN:
			volume += command == CMD_VOLUME_UP ? 10 : -10;
			input["percent"] = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
			return tool("tv_volume", input, answer);
		case CMD_VOLUME:
			/* "40", "40 %", "40 prozent" */
			if (!is_number(arg.substr(0, arg.find_first_of(" %"))))
				return false;
			input["percent"] = atoi(arg.c_str());
			return tool("tv_volume", input, answer);
		case CMD_MUTE:
		case CMD_UNMUTE:
			input["mute"] = command == CMD_MUTE;
			return tool("tv_volume", input, answer);
		case CMD_STANDBY:
			return tool("tv_standby", input, answer);
		case CMD_CHANNELS:
			if (!arg.empty())
				input["query"] = arg;
			return tool("tv_channels", input, answer);
		default:
			break;
	}

	/* nothing but a channel is the wish to see it */
	if (CAssistantTools::hasChannel(said))
	{
		input["channel"] = said;
		return tool("tv_zap", input, answer);
	}
	return false;
}
