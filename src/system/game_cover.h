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

#ifndef __game_cover_h__
#define __game_cover_h__

#include <system/games.h>

#include <vector>

namespace game_cover
{
	struct progress
	{
		bool running;
		size_t total;
		size_t done;
		size_t found;
	};

	/* fetches the covers of the games that have no picture, in a thread of
	   its own; false when there is nothing to fetch or it runs already */
	bool start(const std::vector<games::system> &systems, const std::vector<games::game> &list);
	/* after the cover it is at */
	void stop();
	progress state();
}

#endif
