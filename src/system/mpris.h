#ifndef __mpris_h__
#define __mpris_h__

/*
 * MPRIS on the session bus
 *
 * Lets desktop media keys and tools like playerctl pause, play and wind
 * neutrino. Every call becomes the remote control key that is set for it,
 * so it does what that key does: pause and timeshift in live TV, pause
 * and winding in the movie player.
 *
 * Without a session bus, as on a box, there is nothing to do.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <pthread.h>
#include <string>

struct DBusConnection;

class CMprisServer
{
	public:
		static CMprisServer *getInstance();
		void Start();
		void Stop();

		/* a method of org.mpris.MediaPlayer2.Player */
		void act(const char *method);
		std::string playbackStatus();

	private:
		CMprisServer();

		DBusConnection *conn;
		pthread_t thread;
		bool running;
		std::string status;

		static void *run(void *arg);
		void statusChanged();
};

#endif /* __mpris_h__ */
