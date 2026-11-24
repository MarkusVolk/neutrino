/*
	Game wall: the games of the box as a wall of covers

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

#ifndef __gamewall_h__
#define __gamewall_h__

#include <gui/coverwall.h>
#include <gui/widget/menue.h>
#include <system/games.h>

class CGameWall : public CCoverWall
{
	public:
		struct Plugin
		{
			int number;
			std::string name;
			std::string description;
			std::string picture;
		};

		void build(const std::vector<games::system> &systems, const std::vector<games::game> &list,
			   const std::vector<Plugin> &plugins, bool by_title);
		/* what the cursor is on, the other one is NULL */
		const games::game *game() const;
		const Plugin *plugin() const;
		/* the whole text of what the cursor is on */
		std::string details() const;

	protected:
		std::string picture(size_t item) const override;
		std::string info(const Tile &t) const override;
		Tile part(size_t item) const override;
		const char *emptyText() const override;

	private:
		std::vector<games::system> systems;
		/* items are games first, then plugins */
		std::vector<games::game> list;
		std::vector<Plugin> plugins;

		Tile gameTile(size_t item) const;
		std::vector<Shelf> byTitle() const;
		std::vector<Shelf> bySystem() const;
};

/* "Games" of the main menu: the wall, or the list of game plugins where no
   system has a game */
class CGames : public CMenuTarget
{
	public:
		int exec(CMenuTarget *parent, const std::string &actionKey);
		/* worth an entry in the menu */
		static bool available();

	private:
		bool by_title = false;

		int show();
		void start(const games::game &g);
};

#endif
