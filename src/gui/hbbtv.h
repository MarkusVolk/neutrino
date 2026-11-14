/*
	HbbTV: runs neutrino-hbbtv with the application of the running channel

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
	along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef __gui_hbbtv_h__
#define __gui_hbbtv_h__

#include <functional>
#include <string>
#include <zapit/client/zapittypes.h>
#include <zapit/scanait.h>

/* an ordinary web page in the browser, not an application of the channel.
 * script is a file of JavaScript the page runs after hbbtv.js. resolver is
 * a shell command for it: a page's "resolve <arg>" runs "<resolver> resolve
 * <arg>" beside the main loop, which prints a video and optionally an audio
 * address, one per line, and parts to skip as "start end" in seconds, for the
 * player; "prepare" runs "<resolver> prepare". data, if set, is a directory
 * for the page's cookies and logins that outlasts an update of the image;
 * without it the page shares that of the channels' applications. hbbtv
 * marks an HbbTV application opened from a menu: its keys stay those of the
 * red button, only exit pressed short is its back */
struct CHbbtvPage
{
	std::string url;
	std::string name;
	std::string script;
	std::string resolver;
	std::string data;
	bool hbbtv = false;
};

class CHbbtv
{
	public:
		/* neutrino-hbbtv is installed */
		static bool available();
		/* starts the AUTOSTART application of the running channel; false if
		 * there is none, so the key can do what it does otherwise */
		static bool startAutostart();
		/* starts the application of the running channel with this URL;
		 * false if the channel has none such */
		static bool start(const std::string &url);
		/* shows a web page until it ends; TV stops meanwhile. False if the
		 * box has no browser or is not in TV mode */
		static bool startPage(const CHbbtvPage &page);
		/* true while an application is on screen, which goes to app */
		static bool open(CAitApp *app = NULL);
		/* ends the application on screen */
		static void close();
		/* calls done in the main loop once the page on screen has loaded
		 * and holds still, after timeout_ms at the latest or when it ends;
		 * false while none is on screen */
		static bool whenSettled(const std::function<void()> &done, int timeout_ms);
		/* why the last one ended, if that was within so many seconds */
		static std::string ended(int within);

	private:
		/* shows app until it ends, the channel changes or the box leaves TV
		 * mode; live TV keeps running underneath. channel is
		 * "onid.tsid.sid:name" of channel id; start_key, if set, is the
		 * HbbTV key code the application gets once it is ready; page, if
		 * set, makes it an ordinary web page */
		static void run(const CAitApp &app, t_channel_id id, const std::string &channel, int start_key, const CHbbtvPage *page = NULL);
};

#endif
