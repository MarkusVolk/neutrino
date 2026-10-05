/*
	Commands the assistant understands by itself, for a box without a
	language model: a sentence is matched against patterns and runs a tool

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

#ifndef __assistant_commands__
#define __assistant_commands__

#include <string>

class CAssistantCommands
{
	public:
		/* false when the text is no command we know; the answer may as well say why a command failed */
		static bool run(const std::string &text, std::string &answer);
};

#endif
