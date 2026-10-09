#ifndef __trayicon_h__
#define __trayicon_h__

/*
 * A tray icon on the desktop
 *
 * A StatusNotifierItem on the session bus with a small menu, so Neutrino
 * stays reachable while its window is away in standby: a click wakes it
 * up or puts it into standby, the menu offers both and the exit.
 *
 * Without a desktop there is nothing to do.
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
struct DBusMessage;

class CTrayIcon
{
	public:
		static CTrayIcon *getInstance();
		void Start();
		void Stop();

		/* what the icon and the menu say and do */
		bool standby();
		void activate();
		void menuEvent(int id);
		void appendItemProperty(void *iter, const char *name);
		void appendMenuItem(void *iter, int id);
		unsigned int layoutRevision() { return revision; }

	private:
		CTrayIcon();

		DBusConnection *conn;
		pthread_t thread;
		bool running;
		bool shown_standby;
		unsigned int revision;

		static void *run(void *arg);
		void registerItem();
		void stateChanged();
};

#endif /* __trayicon_h__ */
