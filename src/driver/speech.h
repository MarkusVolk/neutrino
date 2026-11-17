/*
	Speech output: menus, lists and messages read aloud with eSpeak NG

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

#ifndef __speech_h__
#define __speech_h__

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

class CRemoteUi;

/* What is on top of the screen is spoken when the main loop waits for
 * the next key; any key cuts the sentence off. The main loop calls in,
 * synthesis and sound run in a thread of their own. */
class CSpeech
{
	public:
		static CSpeech *getInstance();
		~CSpeech();

		/* the main loop waits again: speak what changed on top */
		void settled();
		/* a key came: stop speaking */
		void key();
		/* holding help: speech on or off, said either way */
		void toggle();
		/* the voice follows the language of the OSD */
		void languageChanged();

		void zapped();
		/* said when no further step follows for a moment */
		void volume(int value);
		void muted(bool on);

	private:
		CSpeech();
		void say(const std::string &text, int delay_ms = 0, bool always = false);
		void stop();
		void run();

		std::thread thread;
		std::mutex lock;
		std::condition_variable wake;
		std::string pending;
		uint64_t due;
		/* a new sentence or a key ends the one being spoken */
		std::atomic<unsigned int> generation;
		std::string voice;
		int rate;
		bool quit;

		/* main loop only */
		CRemoteUi *last_ui;
		std::string last_title;
		std::string last_focus;
};

#endif
