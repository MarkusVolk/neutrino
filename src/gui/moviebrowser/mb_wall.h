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
#include <gui/coverwall.h>

/* the shelves are those of the web interface's wall: what to continue
   watching, the newest, films, series, documentaries, the genres and the
   rest; a series, or a title recorded more than once, is one tile that
   opens its recordings */
class CMovieWall : public CCoverWall
{
	public:
		/* the shelves of these recordings; the cursor stays on the same
		   recording where it can */
		void build(const std::vector<MI_MOVIE_INFO *> &movies);
		/* the recording under the cursor, NULL on a group */
		MI_MOVIE_INFO *selected() const;

	protected:
		std::string picture(size_t item) const override;
		std::string info(const Tile &t) const override;
		Tile part(size_t item) const override;
		void paintBadge(const Tile &t, int x, int y) const override;
		const char *emptyText() const override;

	private:
		std::vector<MI_MOVIE_INFO *> movies;

		Tile single(size_t item) const;
		Tile groupTile(const std::string &name, const std::vector<size_t> &list, bool series) const;
		std::vector<Tile> titleTiles(const std::vector<size_t> &list) const;
		std::vector<Shelf> wall() const;
};

#endif
