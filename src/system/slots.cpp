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

#include <stdlib.h>

#include <sstream>

#include <system/helpers.h>
#include <system/slots.h>

namespace
{

const std::string &program()
{
	static const std::string path = find_executable("slot-update");
	return path;
}

}

namespace slots
{

bool available()
{
	return !program().empty() && !list().empty();
}

std::vector<Slot> list()
{
	std::vector<Slot> result;
	if (program().empty())
		return result;
	run_program({ program(), "-l" }, [&result](const std::string &line) {
		std::istringstream in(line);
		Slot slot;
		std::string state, next;
		if (!(in >> slot.number >> state >> next >> slot.version))
			return;
		std::getline(in >> std::ws, slot.name);
		if (state == "running")
			slot.state = Slot::RUNNING;
		else if (state == "written")
			slot.state = Slot::WRITTEN;
		else if (state == "empty")
			slot.state = Slot::EMPTY;
		else
			return;
		slot.next = next == "1";
		if (slot.version == "-")
			slot.version.clear();
		if (slot.name == "-")
			slot.name.clear();
		result.push_back(slot);
	}, "", "slots");
	return result;
}

bool start(int number, std::string &error)
{
	error.clear();
	int ret = run_program({ program(), "-q", "-s", to_string(number), "-b" }, [&error](const std::string &line) {
		if (line.compare(0, 6, "error ") == 0)
			error = line.substr(6);
	}, "", "slots");
	return ret == 0;
}

Result write(const std::string &zip, int number, bool boot, const progress_fn &progress, std::string &error)
{
	std::vector<std::string> argv = { program(), "-q", "-s", to_string(number) };
	if (boot)
		argv.push_back("-b");
	argv.push_back(zip);

	Result result = FAILED;
	error.clear();
	int ret = run_program(argv, [&](const std::string &line) {
		if (line.compare(0, 9, "progress ") == 0) {
			std::istringstream in(line.substr(9));
			int percent;
			std::string step;
			if (in >> percent >> step && progress)
				progress(percent, step);
		} else if (line == "done")
			result = DONE;
		else if (line == "restart")
			result = RESTART;
		else if (line.compare(0, 6, "error ") == 0)
			error = line.substr(6);
	}, "", "slots");
	if (ret == 2)
		return NO_ROOM;
	if (ret != 0 && result != RESTART)
		return FAILED;
	return result;
}

}
