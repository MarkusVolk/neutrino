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

#ifndef __series_timers_h__
#define __series_timers_h__

#include <time.h>

#include <string>
#include <vector>

#include <zapit/client/zapittypes.h>

/*
 * A rule names a title and a channel. Whenever the EPG has grown, every
 * coming broadcast of that title on that channel gets a record timer, but
 * not a repeat of an episode that is recorded or has its timer already,
 * and not what no tuner is free for beside the timers there are.
 */
namespace series_timers
{
	struct rule
	{
		std::string id;		// stays the same over restarts
		std::string title;	// name() of the EPG title, compared without case
		t_channel_id channel_id;
		time_t created;
		int timers;		// record timers of the series waiting, at the last look
		int conflicts;		// broadcasts left out at the last look: no tuner free
	};

	/* the series in the title of a broadcast: without the number of the
	   episode some channels put there, "Hubert ohne Staller (199)",
	   "Lost Places - Folge 119". Rules are kept and compared by it. */
	std::string name(const std::string &title);
	std::vector<rule> list();
	/* the rule for a title on a channel; false when there is none */
	bool find(const std::string &title, t_channel_id channel_id, rule &r);
	/* a new rule, with the timers the EPG gives set at once and counted in
	   added; false with the rule there is already in r, or with no id in r
	   when it could not be made */
	bool add(const std::string &title, t_channel_id channel_id, rule &r, int &added);
	/* the timers it has set stay */
	bool remove(const std::string &id);
	/* looks at the EPG in delay seconds at the latest, and every hour then */
	void kick(int delay);
}

#endif
