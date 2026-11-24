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

#ifndef __games_h__
#define __games_h__

#include <string>
#include <vector>

namespace games
{
	/* GAMESYSTEMDIR/<id>.cfg */
	struct system
	{
		std::string id;
		std::string name;
		std::string dir;	/* below the games directory */
		std::string ext;	/* of its files, "zip,smc" */
		std::string names;	/* table of titles: | id | status | title | parent | year | maker | hardware | remarks | */
		std::string prefix;	/* of the ids in that table */
		std::string covers;	/* the collection its covers come from */
		bool unlisted;		/* games the table does not know are shown too */
		std::string run;	/* started with the id and the file of a game */
		int index;
	};
	struct game
	{
		std::string system;
		std::string id;
		std::string file;
		std::string title;
		std::string year;
		std::string maker;
		std::string hardware;
	};

	std::vector<system> systems();
	/* the directory with one directory per system, empty if there is none */
	std::string directory();
	/* the games of these systems by title; what the table of a system
	   marks as a BIOS or as not working is left out, and what it does not
	   know, unless the system says so */
	std::vector<game> find(const std::vector<system> &list);
	/* anything to show, without reading the tables */
	bool available();

	/* ids as "system/game", the one played last first */
	std::vector<std::string> recent();
	void played(const game &g);

	/* beside the game, empty when there is none */
	std::string picture(const game &g);
}

#endif
