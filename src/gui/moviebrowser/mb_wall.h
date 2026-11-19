/*
	Movie browser: the recordings as a wall of covers on shelves

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

#ifndef __mb_wall_h__
#define __mb_wall_h__

#include <driver/movieinfo.h>
#include <driver/rcinput.h>

#include <list>
#include <memory>
#include <string>
#include <vector>

/* the shelves are those of the web interface's wall: what to continue
   watching, the newest, films, series, documentaries, the genres and the
   rest; a series, or a title recorded more than once, is one tile that
   opens its recordings */
class CMovieWall
{
	public:
		struct Tile
		{
			std::string caption;
			std::string line2;
			std::vector<MI_MOVIE_INFO *> items;
			bool group;
		};
		struct Shelf
		{
			std::string title;
			std::vector<Tile> tiles;
			size_t first;	/* the first tile on screen */
			size_t cursor;
		};

		void setArea(int x, int y, int w, int h);
		/* the shelves of these recordings; the cursor stays on the same
		   recording where it can */
		void build(const std::vector<MI_MOVIE_INFO *> &movies);
		void paint();
		void hide();
		/* arrows and pages; true when it moved */
		bool key(neutrino_msg_t msg);

		/* the recording under the cursor, NULL on a group */
		MI_MOVIE_INFO *selected() const;
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

		const Tile *tile() const;
		void layout();
		void keepVisible();
		void paintTile(const Tile &t, int tx, int ty, bool cursor);
		void paintInfo();
		const Cover *cover(const std::string &path);
		static std::string coverOf(const MI_MOVIE_INFO *m);
};

#endif
