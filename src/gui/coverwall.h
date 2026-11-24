/*
	A wall of covers on shelves

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

#ifndef __coverwall_h__
#define __coverwall_h__

#include <driver/framebuffer.h>
#include <driver/rcinput.h>

#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>

/* shelves of tiles with a picture; a tile stands for one item or is a group
   that opens its items. What the items are is up to the derived class, the
   wall knows their numbers only */
class CCoverWall
{
	public:
		struct Tile
		{
			std::string caption;
			std::string line2;
			std::vector<size_t> items;
			bool group;
		};
		struct Shelf
		{
			std::string title;
			std::vector<Tile> tiles;
			size_t first;	/* the first tile on screen */
			size_t cursor;
		};

		virtual ~CCoverWall() {}

		void setArea(int x, int y, int w, int h);
		void paint();
		void hide();
		/* arrows and pages; true when it moved */
		bool key(neutrino_msg_t msg);

		bool onGroup() const;
		bool inGroup() const { return !group.empty(); }
		/* the group under the cursor as the only shelf, and back */
		void enter();
		void leave();

		/* the tiles one after the other, for a remote client */
		size_t count() const;
		std::string text(size_t i) const;
		size_t current() const;
		void select(size_t i);

	protected:
		/* the picture of an item, empty when it has none */
		virtual std::string picture(size_t item) const = 0;
		/* the line below the shelves */
		virtual std::string info(const Tile &t) const = 0;
		/* the tile of an item in its open group */
		virtual Tile part(size_t item) const = 0;
		/* on top of a picture */
		virtual void paintBadge(const Tile &, int, int) const {}
		virtual const char *emptyText() const = 0;

		static void addShelf(std::vector<Shelf> &shelves, const std::string &title, std::vector<Tile> tiles);
		const Tile *tile() const;
		/* new shelves; the cursor goes to the tile with the item that same()
		   knows, on the shelf it was on */
		void show(const std::vector<Shelf> &wall, const std::function<bool(size_t)> &same);

	private:
		struct Cover
		{
			std::string path;
			time_t mtime;
			int w, h;
			std::shared_ptr<fb_pixel_t> pixels;
		};

		int x = 0, y = 0, width = 0, height = 0;
		int tile_w = 0, tile_h = 0, shelf_h = 0, per_row = 0, rows = 0, info_h = 0;
		std::vector<Shelf> shelves;
		size_t shelf = 0, first_shelf = 0;
		/* the open group and the wall it was opened from */
		std::string group;
		std::vector<Shelf> outside;
		size_t outside_shelf = 0, outside_first = 0;
		std::list<Cover> covers;	/* most recently used first */

		void layout();
		void keepVisible();
		std::vector<Shelf> groupShelves(const Tile &t) const;
		void paintTile(const Tile &t, int tx, int ty, bool cursor);
		void paintInfo();
		const Cover *cover(const std::string &path);
};

#endif
