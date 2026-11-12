/*
	HbbTV: runs neutrino-hbbtv with the application of the running channel

	The browser draws on the display itself and reads the remote control
	while it runs. Neutrino keeps live TV, timers and its interfaces going
	and only stops painting; it takes the display back when the application
	ends, the channel changes or the box leaves the mode it started in. Keys of remote
	clients go to the browser as "key <code>" (codes of the input device);
	for tv_screen it answers "describe" with "screen <text>", where the text
	has its newlines and backslashes escaped.

	The application's videos are played by the box: the browser sends lines
	such as "play <url>", "pause", "resume", "seek <ms>", "stop" and
	"window <x> <y> <w> <h>" (in the application's 1280x720) or "window full"
	over a socket on its fd 3, and gets "position <ms> <ms> playing|paused",
	"ended" and "error" back. A page's script may ask for "resolve <arg>" and
	"prepare" instead, see CHbbtvPage. "input" says a key was pressed, for
	the inactivity timer.

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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <vector>

#include <algorithm>

#include <global.h>
#include <gui/audiomute.h>
#include <gui/widget/menue.h>
#include <gui/widget/msgbox.h>
#include <driver/volume.h>
#include <neutrino.h>
#include <gui/osd_helpers.h>
#include <driver/abstime.h>
#include <driver/framebuffer.h>
#include <driver/rcinput.h>
#include <driver/record.h>
#include <driver/shutdown_count.h>
#include <gui/hbbtv.h>
#include <gui/infoviewer.h>
#include <gui/movieplayer.h>
#include <gui/widget/remote_ui.h>
#include <hardware/video.h>
#include <system/helpers.h>
#include <zapit/getservices.h>
#include <zapit/zapit.h>

extern cVideo *videoDecoder;
extern char **environ;

#define CONTROL_FD 3
#define DISPLAY_FD 4

static std::string browser()
{
	static std::string path = find_executable("neutrino-hbbtv");
	return path;
}

bool CHbbtv::available()
{
	return !browser().empty();
}

static bool shown;
static CAitApp shown_app;
static bool close_wanted;

bool CHbbtv::open(CAitApp *app)
{
	if (shown && app)
		*app = shown_app;
	return shown;
}

void CHbbtv::close()
{
	close_wanted = shown;
}

/* the running channel with its applications, when one can start */
static bool current_apps(t_channel_id &id, std::string &channel, std::vector<CAitApp> &apps)
{
	if (!CHbbtv::available() || shown || CNeutrinoApp::getInstance()->getMode() != NeutrinoModes::mode_tv)
		return false;
	{
		CServiceManager::ChannelGuard guard;
		CZapitChannel *ch = CZapit::getInstance()->GetCurrentChannel();
		if (!ch)
			return false;
		id = ch->getChannelID();
		channel = to_string(ch->getOriginalNetworkId()) + "." + to_string(ch->getTransportStreamId()) + "." +
			  to_string(ch->getServiceId()) + ":" + ch->getName();
	}
	return CZapit::getInstance()->GetAitApps(id, apps);
}

bool CHbbtv::startAutostart()
{
	t_channel_id id;
	std::string channel;
	std::vector<CAitApp> apps;
	if (!current_apps(id, channel, apps))
		return false;
	for (size_t i = 0; i < apps.size(); i++)
	{
		if (apps[i].control_code == CAitApp::AUTOSTART)
		{
			/* on a TV set the application runs already and gets this red */
			run(apps[i], id, channel, 403);
			return true;
		}
	}
	return false;
}

bool CHbbtv::startPage(const CHbbtvPage &page)
{
	if (!available() || shown)
		return false;
	/* a page needs no broadcast application: it starts in any mode, with
	   the running channel as context when there is one */
	t_channel_id id = 0;
	std::string channel;
	{
		CServiceManager::ChannelGuard guard;
		CZapitChannel *ch = CZapit::getInstance()->GetCurrentChannel();
		if (ch)
		{
			id = ch->getChannelID();
			channel = to_string(ch->getOriginalNetworkId()) + "." + to_string(ch->getTransportStreamId()) + "." +
				  to_string(ch->getServiceId()) + ":" + ch->getName();
		}
	}
	CAitApp ait_app;
	ait_app.url = page.url;
	ait_app.name = page.name;
	run(ait_app, id, channel, 0, &page);
	return true;
}

bool CHbbtv::start(const std::string &url)
{
	t_channel_id id;
	std::string channel;
	std::vector<CAitApp> apps;
	if (!current_apps(id, channel, apps))
		return false;
	for (size_t i = 0; i < apps.size(); i++)
	{
		if (apps[i].url == url)
		{
			run(apps[i], id, channel, apps[i].control_code == CAitApp::AUTOSTART ? 403 : 0);
			return true;
		}
	}
	return false;
}

/* the browser's side: the application's video, played by the box instead
 * of the browser, keys of remote clients and the screen text */
class CHbbtvControl
{
	private:
		int fd;
		bool active;
		bool paused;
		int64_t next_report;
		std::string input;
		std::string screen_text;
		bool screen_new;
		bool loaded;
		/* an ordinary page has no TV underneath: the player stays taken */
		bool keep;
		std::string resolver;
		pid_t resolving;
		int resolving_fd;
		bool resolve_plays;
		std::string resolved;
		std::vector<std::pair<int, int> > skip_ranges;
		int skip_last;
		/* "drm <license> [<certificate>]": the next play is a manifest
		 * with Widevine */
		std::string drm_license;
		std::string drm_certificate;
		/* a stream with Widevine came while it is switched off */
		bool widevine_off;

		void send(const std::string &line)
		{
			std::string l = line + "\n";
			if (write(fd, l.data(), l.size()) < 0)
				printf("[hbbtv] control: %m\n");
		}

		void claim()
		{
			if (loaded)
				close();
			else if (!active)
			{
				CRecordManager::getInstance()->StopAutoRecord();
				CNeutrinoApp::getInstance()->lockPlayBack();
				active = true;
			}
		}

		void play(const std::string &url, const std::string &audio = "")
		{
			cPlayback *playback = CMoviePlayerGui::getPlayback();
			claim();
			printf("[hbbtv] play %s\n", url.c_str());
			playback->Open(PLAYMODE_FILE);
			loaded = true;
			if (!drm_license.empty())
			{
				std::string license, certificate;
				license.swap(drm_license);
				certificate.swap(drm_certificate);
#if ENABLE_WIDEVINE && HAVE_GENERIC_HARDWARE
				if (!g_settings.widevine)
				{
					printf("[hbbtv] Widevine is switched off in the media settings\n");
					widevine_off = true;
				}
				if (!g_settings.widevine || !playback->SetDrm(license, "", certificate))
#endif
				{
					send("error");
					stop();
					return;
				}
			}
			if (!playback->Start(url, "", audio))
			{
				send("error");
				stop();
				return;
			}
			playback->SetSpeed(1);
			paused = false;
			next_report = 0;
		}

		void window(const std::string &args)
		{
			int x, y, w, h;
			printf("[hbbtv] window %s\n", args.c_str());
			if (args == "full" || sscanf(args.c_str(), "%d %d %d %d", &x, &y, &w, &h) != 4)
				videoDecoder->Pig(-1, -1, -1, -1);
			else
				videoDecoder->Pig(x, y, w, h, 1280, 720);
		}

		void cancel_resolver()
		{
			if (resolving <= 0)
				return;
			kill(resolving, SIGKILL);
			waitpid(resolving, NULL, 0);
			::close(resolving_fd);
			resolving = 0;
			resolving_fd = -1;
		}

		/* the page's resolver runs beside the main loop: it takes seconds,
		 * and a page gives up on a video that does not start */
		void start_resolver(const std::string &action, const std::string &arg, bool plays)
		{
			int p[2];
			cancel_resolver();
			if (resolver.empty() || pipe2(p, O_CLOEXEC) < 0)
				return;
			resolve_plays = plays;
			resolved.clear();
			std::string cmd = resolver + " \"$0\" \"$1\"";
			resolving = fork();
			if (resolving == 0)
			{
				dup2(p[1], 1);
				execl("/bin/sh", "sh", "-c", cmd.c_str(), action.c_str(), arg.c_str(), (char *)NULL);
				_exit(127);
			}
			::close(p[1]);
			if (resolving < 0)
			{
				::close(p[0]);
				resolving = 0;
				return;
			}
			fcntl(p[0], F_SETFL, O_NONBLOCK);
			resolving_fd = p[0];
		}

		void collect_resolver()
		{
			if (resolving <= 0)
				return;
			char buf[4096];
			ssize_t n;
			while ((n = ::read(resolving_fd, buf, sizeof(buf))) > 0)
				resolved.append(buf, n);
			if (n != 0)
				return;
			waitpid(resolving, NULL, 0);
			::close(resolving_fd);
			resolving = 0;
			resolving_fd = -1;
			if (!resolve_plays)
				return;
			std::vector<std::string> urls;
			skip_ranges.clear();
			skip_last = -1;
			size_t begin = 0, end;
			while ((end = resolved.find('\n', begin)) != std::string::npos)
			{
				std::string entry = resolved.substr(begin, end - begin);
				begin = end + 1;
				double from, to;
				if (!entry.compare(0, 4, "http"))
					urls.push_back(entry);
				else if (entry.find_first_not_of("0123456789") == std::string::npos)
					send("height " + entry);
				else if (sscanf(entry.c_str(), "%lf %lf", &from, &to) == 2 && to > from)
					skip_ranges.push_back(std::make_pair((int)(from * 1000), (int)(to * 1000)));
			}
			if (urls.empty())
			{
				printf("[hbbtv] no stream\n");
				send("error");
				stop();
				return;
			}
			play(urls[0], urls.size() > 1 ? urls[1] : "");
		}

		/* as in the movie player: playing into a part jumps over it, a jump
		 * into its middle leaves it be */
		void skip(int &position)
		{
			for (size_t i = 0; i < skip_ranges.size(); i++)
			{
				if (skip_last < skip_ranges[i].first && position >= skip_ranges[i].first && position < skip_ranges[i].second)
				{
					printf("[hbbtv] skipping %d..%d ms\n", skip_ranges[i].first, skip_ranges[i].second);
					CMoviePlayerGui::getPlayback()->SetPosition(skip_ranges[i].second, true);
					position = skip_ranges[i].second;
					break;
				}
			}
			skip_last = position;
		}

		/* the display is the browser's, so no volume bar */
		void volume(const std::string &what)
		{
			if (what == "mute")
			{
				CAudioMute::getInstance()->AudioMute(!CNeutrinoApp::getInstance()->isMuted(), true);
				return;
			}
			int vol = g_settings.current_volume + (what == "up" ? 1 : -1) * g_settings.current_volume_step;
			CVolume::getInstance()->setVolumeExt(std::min(100, std::max(0, vol)));
		}

		void command(const std::string &line)
		{
			std::string cmd = line.substr(0, line.find(' '));
			std::string args = line.size() > cmd.size() ? line.substr(cmd.size() + 1) : "";
			cPlayback *playback = CMoviePlayerGui::getPlayback();
			if (cmd == "screen")
			{
				screen_text.clear();
				for (size_t i = 0; i < args.size(); i++)
				{
					if (args[i] == '\\' && i + 1 < args.size())
						screen_text += args[++i] == 'n' ? '\n' : args[i];
					else
						screen_text += args[i];
				}
				screen_new = true;
			}
			else if (cmd == "play" && !args.empty())
				play(args);
			else if (cmd == "drm")
			{
				size_t sp = args.find(' ');
				drm_license = args.substr(0, sp);
				drm_certificate = sp == std::string::npos ? "" : args.substr(sp + 1);
			}
			else if (cmd == "resolve" && !args.empty() && !resolver.empty())
			{
				claim();
				start_resolver("resolve", args, true);
			}
			else if (cmd == "prepare")
				start_resolver("prepare", "", false);
			else if (cmd == "volume")
				volume(args);
			else if (cmd == "input")
				SHTDCNT::getInstance()->resetSleepTimer();
			else if (cmd == "window")
				window(args);
			else if (cmd == "stop")
				stop();
			else if (!active || !loaded)
				return;
			else if (cmd == "pause" || cmd == "resume")
			{
				paused = cmd == "pause";
				playback->SetSpeed(paused ? 0 : 1);
				next_report = 0;
			}
			else if (cmd == "seek")
			{
				playback->SetPosition(atoi(args.c_str()), true);
				next_report = 0;
			}
		}

		/* a paused player still has data on its way to the decoder, which
		 * takes none: Close() would wait forever, as in the movie player */
		void close()
		{
			cPlayback *playback = CMoviePlayerGui::getPlayback();
			if (paused)
				playback->SetSpeed(1);
			playback->Close();
			paused = false;
			loaded = false;
		}

	public:
		CHbbtvControl(int control, const CHbbtvPage *page) : fd(control), active(false), paused(false), next_report(0), screen_new(false),
			loaded(false), keep(page != NULL), resolver(page ? page->resolver : ""), resolving(0), resolving_fd(-1), resolve_plays(false), skip_last(-1),
			widevine_off(false)
		{
			if (keep)
				claim();
		}
		~CHbbtvControl()
		{
			keep = false;
			stop();
		}

		bool widevineOff() const
		{
			return widevine_off;
		}

		/* the browser takes the codes of the input devices; value as they
		   give it (1 down, 2 held, 0 up), or -1 for a whole press */
		void key(neutrino_msg_t msg, int value = -1)
		{
			std::string line = "key " + to_string(g_RCInput->translateRevert((int)msg));
			if (value >= 0)
				line += " " + to_string(value);
			send(line);
		}

		/* what the application shows; the browser gets a second for it */
		std::string screen()
		{
			screen_new = false;
			send("describe");
			int64_t until = time_monotonic_ms() + 1000;
			while (!screen_new && time_monotonic_ms() < until)
			{
				struct pollfd p = { fd, POLLIN, 0 };
				poll(&p, 1, 50);
				if (!read())
					break;
			}
			return screen_new ? screen_text : "The application does not answer.";
		}

		/* restart false: standby or shutdown follows, nothing starts again */
		void stop(bool restart = true)
		{
			cancel_resolver();
			if (!active)
				return;
			if (loaded)
				close();
			videoDecoder->Pig(-1, -1, -1, -1);
			if (keep)
				return;
			CNeutrinoApp::getInstance()->unlockPlayBack(restart);
			active = false;
			printf("[hbbtv] video stopped, live TV again\n");
		}

		/* commands from the browser; false once it closed its side */
		bool read()
		{
			char buf[4096];
			ssize_t n;
			while ((n = ::read(fd, buf, sizeof(buf))) > 0)
				input.append(buf, n);
			if (n == 0)
				return false;
			size_t end;
			while ((end = input.find('\n')) != std::string::npos)
			{
				std::string line = input.substr(0, end);
				input.erase(0, end + 1);
				/* a seek may restart the stream and take seconds; of the
				 * seeks that came meanwhile only the last one counts */
				if (line.compare(0, 5, "seek ") == 0 && seekFollows())
					continue;
				command(line);
			}
			return true;
		}

		bool seekFollows() const
		{
			size_t pos = 0, end;
			while ((end = input.find('\n', pos)) != std::string::npos)
			{
				if (input.compare(pos, 5, "seek ") == 0)
					return true;
				pos = end + 1;
			}
			return false;
		}

		/* position twice a second while a video plays */
		void report()
		{
			collect_resolver();
			int64_t now = time_monotonic_ms();
			if ((!loaded && !(resolving > 0 && resolve_plays)) || now < next_report)
				return;
			next_report = now + 500;
			if (!loaded)
			{
				send("position 0 0 playing");
				return;
			}
			int position = 0, duration = 0;
			if (!CMoviePlayerGui::getPlayback()->GetPosition(position, duration))
			{
				send("ended");
				stop();
				return;
			}
			skip(position);
			send("position " + to_string(position) + " " + to_string(duration) + (paused ? " paused" : " playing"));
		}
};

static pid_t spawn(const std::string &url, const std::string &channel, const std::string &apps, const CHbbtvPage *page, int control, int display, int start_key)
{
	/* built before the fork, the child may not allocate */
	std::vector<std::string> env;
	for (char **e = environ; *e; e++)
		if (strncmp(*e, "NEUTRINO_HBBTV_FD=", 18) && strncmp(*e, "NEUTRINO_HBBTV_START_KEY=", 25) &&
		    strncmp(*e, "NEUTRINO_HBBTV_DISPLAY=", 23) && strncmp(*e, "NEUTRINO_HBBTV_APPS=", 20) &&
		    strncmp(*e, "NEUTRINO_HBBTV_PAGE=", 20) && strncmp(*e, "NEUTRINO_HBBTV_PAGE_KEYS=", 25) &&
		    strncmp(*e, "NEUTRINO_HBBTV_KEYS=", 20) &&
		    strncmp(*e, "XDG_DATA_HOME=", 14))
			env.push_back(*e);
	if (page)
		env.push_back("NEUTRINO_HBBTV_PAGE=" + page->script);
	if (page && page->hbbtv)
		env.push_back("NEUTRINO_HBBTV_PAGE_KEYS=hbbtv");
	/* cookies, logins and consents outlast an update of the image */
	env.push_back("XDG_DATA_HOME=" + (page && !page->data.empty() ? page->data : std::string(CONFIGDIR "/hbbtv")));
	if (!apps.empty())
		env.push_back("NEUTRINO_HBBTV_APPS=" + apps);
	env.push_back("NEUTRINO_HBBTV_FD=" + to_string(CONTROL_FD));
	if (display >= 0)
		env.push_back("NEUTRINO_HBBTV_DISPLAY=" + to_string(DISPLAY_FD));
	/* in a window the keys come from the compositor through Neutrino */
	if (g_info.hw_caps->windowed)
		env.push_back("NEUTRINO_HBBTV_KEYS=forward");
	if (start_key)
		env.push_back("NEUTRINO_HBBTV_START_KEY=" + to_string(start_key));
	std::vector<char *> envp;
	for (size_t i = 0; i < env.size(); i++)
		envp.push_back((char *)env[i].c_str());
	envp.push_back(NULL);
	std::string bin = browser();
	const char *argv[] = { bin.c_str(), url.c_str(), channel.c_str(), NULL };

	pid_t pid = fork();
	if (pid < 0)
	{
		printf("[hbbtv] fork: %m\n");
		return -1;
	}
	if (pid == 0)
	{
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = SIG_DFL;
		for (int i = 1; i < NSIG; i++)
			sigaction(i, &sa, NULL);
		/* short of memory the kernel ends the browser, never Neutrino */
		int oom = open("/proc/self/oom_score_adj", O_WRONLY);
		if (oom >= 0)
		{
			(void)!write(oom, "1000", 4);
			::close(oom);
		}
		/* out of the way first, either may sit on the other's number */
		control = fcntl(control, F_DUPFD, 10);
		if (display >= 0)
			display = fcntl(display, F_DUPFD, 10);
		dup2(control, CONTROL_FD);
		if (display >= 0)
			dup2(display, DISPLAY_FD);
		close_range((display >= 0 ? DISPLAY_FD : CONTROL_FD) + 1, ~0U, 0);
		execve(argv[0], (char * const *)argv, envp.data());
		_exit(127);
	}
	return pid;
}

/* ok goes to the application, back is its back key, exit ends it */
class CHbbtvView : public CRemoteView
{
	public:
		CHbbtvView(const std::string &title, text_fn text) : CRemoteView(title, text) {}
		bool remoteSelect(const std::string &what, std::string &error)
		{
			if (what == "back" || what == "Back")
			{
				g_RCInput->postMsg(CRCInput::RC_back, 0);
				return true;
			}
			return CRemoteView::remoteSelect(what, error);
		}
};

/* the browser gets two seconds to end before it is killed */
static void stop_browser(pid_t pid)
{
	kill(pid, SIGTERM);
	for (int i = 0; i < 40; i++)
	{
		pid_t r = waitpid(pid, NULL, WNOHANG);
		if (r == pid || (r < 0 && errno != EINTR))
			return;
		usleep(50000);
	}
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
}

void CHbbtv::run(const CAitApp &ait_app, t_channel_id id, const std::string &channel, int start_key, const CHbbtvPage *page)
{
	CFrameBuffer *fb = CFrameBuffer::getInstance();
	CNeutrinoApp *app = CNeutrinoApp::getInstance();
	const std::string &url = ait_app.url;
	const int mode = app->getMode();

	printf("[hbbtv] start %s for %s\n", url.c_str(), channel.c_str());
	g_InfoViewer->killTitle();
	fb->paintBackground();
	fb->blit();
	g_RCInput->clearRCMsg();
	/* on a box the browser reads the remote control itself; in a window
	   Neutrino keeps the keys and passes them on */
	const bool windowed = g_info.hw_caps->windowed;
	if (!windowed)
		g_RCInput->stopInput();
	fb->setDisplayHandedOver(true);
	/* applications lay out for 1280x720: the browser draws that many pixels
	   and the display layer scales them, less than half the work for the GPU */
	unsigned int xres = fb->getScreenWidth(true), yres = fb->getScreenHeight(true);
	fb->setMode(1280, 720, 32);

	int control[2];
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, control) < 0)
		control[0] = control[1] = -1;
	/* a display that is Neutrino's own stays with it: the browser sends
	   its pictures, and they are shown in place of the OSD */
	int display[2] = { -1, -1 };
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, display) == 0 && !fb->setSource(display[0]))
	{
		::close(display[0]);
		::close(display[1]);
		display[0] = display[1] = -1;
	}
	/* what else the channel signals: an application may start another
	   by its number, dvb://current.ait/<organisation>.<application> */
	std::string others;
	std::vector<CAitApp> apps;
	if (CZapit::getInstance()->GetAitApps(id, apps))
		for (size_t i = 0; i < apps.size(); i++)
			if (!apps[i].url.empty() && apps[i].url.find(' ') == std::string::npos)
				others += (others.empty() ? "" : " ") + to_string(apps[i].org_id) + "." + to_string(apps[i].app_id) + "=" + apps[i].url;
	pid_t pid = spawn(url, channel, others, page, control[1], display[1], start_key);
	if (control[1] >= 0)
		::close(control[1]);
	if (display[1] >= 0)
		::close(display[1]);
	fcntl(control[0], F_SETFL, fcntl(control[0], F_GETFL) | O_NONBLOCK);
	CHbbtvControl link(control[0], page);
	shown = true;
	shown_app = ait_app;
	close_wanted = false;
	neutrino_msg_t msg = 0;
	neutrino_msg_data_t data = 0;
	bool pending = false;
	bool power_off = false;
	neutrino_msg_t repost = 0;
	CHbbtvView *view = new CHbbtvView("HbbTV " + (ait_app.name.empty() ? url : ait_app.name), [&link] {
		return link.screen() + "\nKeys go to the application with tv_key; back is its back key, exit "
		       "or tv_hbbtv with close ends it.";
	});
	/* in a window the keys of the input devices go to the browser as it
	   would read them itself: down, held and up */
	if (windowed)
		g_RCInput->setDeviceKeyFilter([&link](neutrino_msg_t key, neutrino_msg_data_t value) {
			link.key(key, (int)value);
			return true;
		});
	while (pid > 0 && !close_wanted)
	{
		if (waitpid(pid, NULL, WNOHANG) == pid)
		{
			pid = -1;
			break;
		}
		if (control[0] >= 0 && !link.read())
			break;
		link.report();
		g_RCInput->getMsg_ms(&msg, &data, 50);
		if (msg == CRCInput::RC_timeout)
			continue;
		if (msg <= CRCInput::RC_MaxRC)
		{
			/* the remote control is the browser's; these keys come from
			 * remote clients and do what the remote control does */
			if (msg == CRCInput::RC_home)
				break;
			if (msg == CRCInput::RC_standby)
			{
				repost = msg;
				break;
			}
			if (!(msg & (CRCInput::RC_Release | CRCInput::RC_Repeat)))
			{
				SHTDCNT::getInstance()->resetSleepTimer();
				link.key(msg);
			}
			continue;
		}
		if (msg == NeutrinoMessages::STANDBY_ON || msg == NeutrinoMessages::SHUTDOWN ||
		    msg == NeutrinoMessages::REBOOT)
		{
			/* handled once the display is Neutrino's again */
			pending = true;
			power_off = true;
			break;
		}
		if (msg == NeutrinoMessages::EVT_START_PLUGIN)
		{
			delete[] (unsigned char *)data;
			continue;
		}
		if ((app->handleMsg(msg, data) & messages_return::cancel_all) ||
		    app->getMode() != mode ||
		    CZapit::getInstance()->GetCurrentChannelID() != id)
			break;
	}
	delete view;
	if (windowed)
		g_RCInput->setDeviceKeyFilter(NULL);
	if (pid > 0)
		stop_browser(pid);
	shown = false;
	close_wanted = false;
	link.stop(!power_off);
	if (control[0] >= 0)
		::close(control[0]);
	if (display[0] >= 0)
		fb->setSource(-1);
	/* the application may have scaled the live picture */
	videoDecoder->Pig(-1, -1, -1, -1);

	fb->setDisplayHandedOver(false);
	fb->setMode(xres, yres, 32);
	/* a reload of the settings meanwhile took the screen area and the fonts
	   from the browser's size */
	if (g_settings.screen_width != (int) xres)
		COsdHelpers::getInstance()->applyOsdSize(true);
	fb->paintBackground();
	fb->blit();
	if (!windowed)
		g_RCInput->restartInput();
	g_RCInput->clearRCMsg();
	printf("[hbbtv] end\n");
	/* the browser had the display meanwhile */
	if (link.widevineOff() && !power_off)
		DisplayInfoMessage(g_Locale->getText(LOCALE_WIDEVINE_OFF), LOCALE_WIDEVINE_HEAD);

	if (pending)
		app->handleMsg(msg, data);
	if (repost)
		g_RCInput->postMsg(repost, 0);
}
