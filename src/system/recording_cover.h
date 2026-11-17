/*
	Recording covers: a picture beside each recording, the poster of the
	film or series from TMDB, else the picture of the programme in the media
	library of ARD or arte, else a frame of the recording itself

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

#ifndef __recording_cover_h__
#define __recording_cover_h__

#include <string>

namespace recording_cover
{
	struct job
	{
		std::string ts;		/* the recording */
		std::string title;
		std::string channel;	/* ARD and arte ones are asked in their media library */
		int year;		/* 0 if unknown */
		int length;		/* minutes, 0 if unknown */
		bool start;		/* the recording only begins: no frame yet */
	};

	/* fetched in a thread of its own, one recording after the other;
	   nothing happens when the recording has a cover already */
	void queue(const job &j);
	/* a recording on the disk, described by its .xml; one still being
	   recorded gets no frame yet */
	void queue(const std::string &ts, bool running = false);
}

#endif
