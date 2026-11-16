/*
	Slots: the multiboot slots of a box, listed, written and started through
	the box's slot-update program

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

#ifndef __system_slots__
#define __system_slots__

#include <functional>
#include <string>
#include <vector>

/*
 * What a slot is and how it is written and started is the box's business:
 * its image ships slot-update, which Neutrino only calls. A box without it
 * has no slots for Neutrino.
 */
namespace slots
{
	struct Slot
	{
		enum State { EMPTY, WRITTEN, RUNNING };
		int number;
		State state;
		bool next;		/* starts at the next restart */
		std::string version;	/* the build date, as the online lists give it */
		std::string name;	/* the image, empty when the slot does not say */
	};

	enum Result { DONE, RESTART, NO_ROOM, FAILED };

	/* percent and the step: prepare, unzip, kernel or rootfs */
	typedef std::function<void(int, const std::string &)> progress_fn;

	bool available();
	std::vector<Slot> list();
	/* sets the slot that starts and restarts into it; the running one only
	   becomes the slot that starts */
	bool start(int number, std::string &error);
	/* writes the image into the slot, the running one after a restart;
	   boot starts another slot when it is written */
	Result write(const std::string &zip, int number, bool boot, const progress_fn &progress,
		     std::string &error);
}

#endif
