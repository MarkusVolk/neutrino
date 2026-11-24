/*
	Neutrino-GUI  -   DBoxII-Project

	Copyright (C) 2001 Steffen Hehn 'McClean'
	Homepage: http://dbox.cyberphoria.org/

	Copyright (C) 2011-2014 Stefan Seyfried

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

#include <sstream>
#include <fstream>
#include <iostream>
#include <algorithm>

#include <dirent.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>
#include <linux/input.h>
#include <memory>
#include <string.h>

#include <global.h>
#include <neutrino.h>

#include <system/helpers.h>

#include <zapit/client/zapittools.h>
#include "widget/shellwindow.h"
#ifdef ENABLE_TERMINAL
#include "terminal.h"
#endif

#include <poll.h>
#include <vector>

#include <hardware/video.h>
extern cVideo *videoDecoder;

#include "plugins.h"

#include <daemonc/remotecontrol.h>
#include <gui/channellist.h>
#include <gui/movieplayer.h>
#include <zapit/zapit.h>
#include <init.h>
#include <gui/lua/luainstance.h>
#include <gui/widget/remote_ui.h>
#include <driver/shutdown_count.h>
#ifdef ENABLE_LCD4LINUX
#include <driver/lcd4l.h>
#endif

extern CPlugins *g_Plugins;
extern CRemoteControl *g_RemoteControl;

CPlugins::CPlugins()
{
	frameBuffer = NULL;
	number_of_plugins = 0;
	running = 0;
}

bool CPlugins::plugin_exists(const std::string &filename)
{
	return (find_plugin(filename) >= 0);
}

int CPlugins::find_plugin(const std::string &filename)
{
	for (int i = 0; i < (int) plugin_list.size(); i++)
	{
		if ((filename.compare(plugin_list[i].filename) == 0) || (filename.compare(plugin_list[i].filename + ".cfg") == 0))
			return i;
	}
	return -1;
}

void CPlugins::scanDir(const char *dir)
{
	struct dirent **namelist;
	std::string fname;

	int number_of_files = scandir(dir, &namelist, 0, alphasort);

	if (number_of_files < 0)
		return;

	for (int i = 0; i < number_of_files; i++)
	{
		std::string filename(namelist[i]->d_name);
		free(namelist[i]);

		int pos = filename.find(".cfg");
		if (pos > -1)
		{
			plugin new_plugin;
			new_plugin.filename = filename.substr(0, pos);
			fname = dir;
			fname += '/';
			new_plugin.cfgfile = fname.append(new_plugin.filename);
			new_plugin.cfgfile.append(".cfg");
			new_plugin.plugindir = dir;
			bool plugin_ok = parseCfg(&new_plugin);
			if (plugin_ok)
			{
				new_plugin.pluginfile = fname;
				if (new_plugin.type == CPlugins::P_TYPE_SCRIPT || new_plugin.terminal || new_plugin.fullscreen)
					new_plugin.pluginfile.append(".sh");
				else if (new_plugin.type == CPlugins::P_TYPE_LUA)
					new_plugin.pluginfile.append(".lua");
				else // CPlugins::P_TYPE_GAME or CPlugins::P_TYPE_TOOL
					new_plugin.pluginfile.append(".so");
				// We do not check if new_plugin.pluginfile exists since .cfg in
				// PLUGINDIR_VAR can overwrite settings in read only dir
				// PLUGINDIR. This needs PLUGINDIR_VAR to be scanned at
				// first -> .cfg in PLUGINDIR will be skipped since plugin
				// already exists in the list.
				// This behavior is used to make sure plugins can be disabled
				// by creating a .cfg in PLUGINDIR_VAR (PLUGINDIR often is read only).
				if (!plugin_exists(new_plugin.filename))
				{
					plugin_list.push_back(new_plugin);
					number_of_plugins++;
				}
			}
		}
	}
	free(namelist);
}

void CPlugins::loadPlugins()
{
	frameBuffer = CFrameBuffer::getInstance();

	/* Held from the clear to the sort, because in between the list is not one
	   a reader could be told anything true out of: empty, then growing with
	   every directory, then in an order that is not the one it ends in. A
	   reader on another thread waits that long, which is the directories being
	   read; the thread that rebuilds it waits for nothing. */
	PluginGuard guard(this);

	number_of_plugins = 0;
	plugin_list.clear();
	sindex = 100;
	scanDir(GAMESDIR);
	/* Copied under the lock: a request for a plugin list is answered on one of
	   the web server's threads and has this rebuilt from there. */
	scanDir(settingsText(g_settings.plugin_hdd_dir).c_str());
	scanDir(PLUGINDIR_MNT);
	scanDir(PLUGINDIR_VAR);
	scanDir(PLUGINDIR);

	sort(plugin_list.begin(), plugin_list.end());
}

CPlugins::~CPlugins()
{
	plugin_list.clear();
}

bool CPlugins::overrideType(plugin *plugin_data, const std::string &setting, p_type type)
{
	if (!setting.empty())
	{
		char s[setting.length() + 1];
		cstrncpy(s, setting, setting.length() + 1);
		char *t, *p = s;
		while ((t = strsep(&p, ",")))
			if (!strcmp(t, plugin_data->filename.c_str()))
			{
				plugin_data->type = type;
				return true;
			}
	}
	return false;
}

bool CPlugins::parseCfg(plugin *plugin_data)
{
	std::ifstream inFile;
	std::string line[20];
	int linecount = 0;
	bool reject = false;

	inFile.open(plugin_data->cfgfile.c_str());

	while (linecount < 20 && getline(inFile, line[linecount++]))
	{};

	plugin_data->index = sindex++;
	plugin_data->key = CRCInput::RC_nokey;
	plugin_data->name = "";
	plugin_data->description = "";
	plugin_data->shellwindow = false;
	plugin_data->terminal = false;
	plugin_data->fullscreen = false;
	plugin_data->hide = false;
	plugin_data->menu_return = menu_return::RETURN_REPAINT;
	plugin_data->type = CPlugins::P_TYPE_DISABLED;
	plugin_data->integration = PLUGIN_INTEGRATION_DISABLED;
	plugin_data->hinticon = NEUTRINO_ICON_HINT_PLUGIN;

	for (int i = 0; i < linecount; i++)
	{
		std::istringstream iss(line[i]);
		std::string cmd;
		std::string parm;

		getline(iss, cmd, '=');
		getline(iss, parm, '=');

		if (cmd == "index")
		{
			plugin_data->index = atoi(parm);
		}
		else if (cmd == "key")
		{
			plugin_data->key = getPluginKey(parm);
		}
		else if (cmd == "name." + settingsText(g_settings.language))
		{
			plugin_data->name = parm;
		}
		else if (cmd == "name")
		{
			if (plugin_data->name.empty())
				plugin_data->name = parm;
		}
		else if (cmd == "desc." + settingsText(g_settings.language))
		{
			plugin_data->description = parm;
		}
		else if (cmd == "desc")
		{
			if (plugin_data->description.empty())
				plugin_data->description = parm;
		}
		else if (cmd == "depend")
		{
			plugin_data->depend = parm;
		}
		else if (cmd == "hinticon")
		{
			plugin_data->hinticon = parm;
		}
		else if (cmd == "type")
		{
			plugin_data->type = getPluginType(atoi(parm));
		}
		else if (cmd == "integration")
		{
			plugin_data->integration = getPluginIntegration(atoi(parm));
		}
		else if (cmd == "shellwindow")
		{
			plugin_data->shellwindow = atoi(parm);
		}
		else if (cmd == "terminal")
		{
			plugin_data->terminal = atoi(parm);
		}
		else if (cmd == "fullscreen")
		{
			plugin_data->fullscreen = atoi(parm);
		}
		else if (cmd == "hide")
		{
			plugin_data->hide = atoi(parm);
		}
		else if (cmd == "menu_return")
		{
			if (parm == "none")
				plugin_data->menu_return = menu_return::RETURN_NONE;
			else if (parm == "exit")
				plugin_data->menu_return = menu_return::RETURN_EXIT;
			else if (parm == "exit_all")
				plugin_data->menu_return = menu_return::RETURN_EXIT_ALL;
			else // (parm == "repaint")
				plugin_data->menu_return = menu_return::RETURN_REPAINT;
		}
		else if (cmd == "needenigma")
		{
			reject = atoi(parm);
		}
	}

	inFile.close();

	if (plugin_data->name.empty())
		plugin_data->name = plugin_data->filename;

	std::string fileType[] = { ".png", ".svg" };
	std::string h_path[] = { plugin_data->hinticon, plugin_data->filename + "_hint" };

	for (size_t i = 0; i < (sizeof(fileType) / sizeof(fileType[0])); i++)
	{
		for (size_t j = 0; j < (sizeof(h_path) / sizeof(h_path[0])); j++)
		{
			std::string _hintIcon = plugin_data->plugindir + "/" + h_path[j] + fileType[i];
			if (access(_hintIcon.c_str(), F_OK) == 0)
				plugin_data->hinticon = _hintIcon;
		}
	}

	overrideType(plugin_data, settingsText(g_settings.plugins_disabled), P_TYPE_DISABLED) ||
	overrideType(plugin_data, settingsText(g_settings.plugins_game), P_TYPE_GAME) ||
	overrideType(plugin_data, settingsText(g_settings.plugins_tool), P_TYPE_TOOL) ||
	overrideType(plugin_data, settingsText(g_settings.plugins_script), P_TYPE_SCRIPT) ||
	overrideType(plugin_data, settingsText(g_settings.plugins_lua), P_TYPE_LUA);

	return !reject;
}

int CPlugins::startPlugin_by_name(const std::string &name)
{
	for (int i = 0; i < (int) plugin_list.size(); i++)
	{
		if (name.compare(g_Plugins->getName(i)) == 0)
		{
			return startPlugin(i);
		}
	}
	return menu_return::RETURN_REPAINT;
}

int CPlugins::startPlugin(const char *const filename)
{
	int pluginnr = find_plugin(filename);
	if (pluginnr > -1)
		return startPlugin(pluginnr);
	else
		printf("[CPlugins] could not find %s\n", filename);
	return menu_return::RETURN_REPAINT;
}

int CPlugins::popenScriptPlugin(int number, const char *script)
{
	pid_t pid = 0;
	FILE *f = my_popen(pid, script, "r");
	if (f != NULL)
	{
		char *output = NULL;
		size_t len = 0;
		while ((getline(&output, &len, f)) != -1)
			scriptOutput += output;
		pclose(f);
		int s;
		while (waitpid(pid, &s, WNOHANG) > 0);
		kill(pid, SIGTERM);
		if (output)
			free(output);
	}
	else
		printf("[CPlugins] can't execute %s\n", script);

	return plugin_list[number].menu_return;
}

int CPlugins::startScriptPlugin(int number)
{
	const char *script = plugin_list[number].pluginfile.c_str();
	printf("[CPlugins] executing script %s\n", script);
	if (!file_exists(script))
	{
		printf("[CPlugins] could not find %s,\nperhaps wrong plugin type in %s\n",
			script, plugin_list[number].cfgfile.c_str());
		return menu_return::RETURN_REPAINT;
	}

	// workaround for manually messed up permissions
	if (access(script, X_OK))
		chmod(script, 0755);
	if (plugin_list[number].shellwindow)
	{
		int res = 0;
		CShellWindow(script, CShellWindow::VERBOSE | CShellWindow::ACKNOWLEDGE, &res);
		scriptOutput = "";
	}
	else
		return popenScriptPlugin(number, script);

	return plugin_list[number].menu_return;
}

/* a script that runs a text user interface, in the terminal on the OSD */
int CPlugins::startTerminalPlugin(int number)
{
	const char *script = plugin_list[number].pluginfile.c_str();
	printf("[CPlugins] executing %s in the terminal\n", script);
	if (!file_exists(script))
	{
		printf("[CPlugins] could not find %s\n", script);
		return menu_return::RETURN_REPAINT;
	}
#ifdef ENABLE_TERMINAL
	CTerminal(script).exec();
#endif
	return plugin_list[number].menu_return;
}

/* SIGTERM to the program and what it started, SIGKILL when it is still
   there after a while: Kodi takes some seconds for its add-on services */
static void endFullscreen(pid_t pid)
{
	kill(-pid, SIGTERM);
	for (int i = 0; i < 150; i++)
	{
		if (waitpid(pid, NULL, WNOHANG) == pid)
			return;
		usleep(100 * 1000);
	}
	printf("[CPlugins] the program did not end, killed\n");
	kill(-pid, SIGKILL);
	waitpid(pid, NULL, 0);
}

/* the exit key of the input devices, which the program reads meanwhile */
class ExitKey
{
	public:
		ExitKey()
		{
			DIR *d = opendir("/dev/input");
			struct dirent *e;
			while (d && (e = readdir(d)))
			{
				if (strncmp(e->d_name, "event", 5))
					continue;
				int fd = open((std::string("/dev/input/") + e->d_name).c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
				if (fd < 0)
					continue;
				unsigned char keys[KEY_MAX / 8 + 1] = {};
				if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) >= 0 && (keys[KEY_EXIT / 8] & (1 << (KEY_EXIT % 8))))
					fds.push_back(fd);
				else
					close(fd);
			}
			if (d)
				closedir(d);
		}
		~ExitKey()
		{
			for (int fd : fds)
				close(fd);
		}
		ExitKey(const ExitKey &) = delete;
		ExitKey &operator=(const ExitKey &) = delete;

		bool pressed()
		{
			bool hit = false;
			struct input_event ev;
			for (int fd : fds)
				while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev))
					if (ev.type == EV_KEY && ev.code == KEY_EXIT && ev.value == 1)
						hit = true;
			return hit;
		}

	private:
		std::vector<int> fds;
};

/*
	While a program has the whole display, Neutrino goes on with its events:
	recordings start in the background. What needs the screen ends the
	program first and comes back as the returned message, to be posted once
	Neutrino has the display again.
*/
neutrino_msg_t CPlugins::runFullscreen(const std::string &name, pid_t pid, neutrino_msg_data_t &data, bool end_on_exit, int &status)
{
	CRemoteView view(name, [&name] {
		return name + " runs on the whole display and has the remote control; "
		       "recordings start in the background. exit ends it.";
	});
	CNeutrinoApp *app = CNeutrinoApp::getInstance();
	neutrino_msg_t msg = 0;
	data = 0;
	std::unique_ptr<ExitKey> exit_key(end_on_exit ? new ExitKey() : NULL);
	int wstatus = 0;
	status = -1;
	while (waitpid(pid, &wstatus, WNOHANG) != pid)
	{
		SHTDCNT::getInstance()->resetSleepTimer();
		g_RCInput->getMsg_ms(&msg, &data, 100);
		if (msg == CRCInput::RC_timeout)
		{
			if (exit_key && exit_key->pressed())
			{
				printf("[CPlugins] %s ends: exit on the remote control\n", name.c_str());
				endFullscreen(pid);
				data = 0;
				return 0;
			}
			continue;
		}
		/* keys here come from remote clients, the remote control is the program's */
		if (msg <= CRCInput::RC_MaxRC)
		{
			if (msg == CRCInput::RC_home || msg == CRCInput::RC_standby)
			{
				printf("[CPlugins] %s ends: a remote client pressed %s\n", name.c_str(),
				       msg == CRCInput::RC_home ? "exit" : "standby");
				endFullscreen(pid);
				data = 0;
				return msg == CRCInput::RC_standby ? msg : 0;
			}
			continue;
		}
		if (msg == NeutrinoMessages::EVT_START_PLUGIN)
		{
			delete[] (unsigned char *)data;
			continue;
		}
		if (msg == NeutrinoMessages::ZAPTO || msg == NeutrinoMessages::STANDBY_ON ||
		    msg == NeutrinoMessages::LEAVE_ALL || msg == NeutrinoMessages::SHUTDOWN ||
		    msg == NeutrinoMessages::REBOOT || (msg == NeutrinoMessages::SLEEPTIMER && !data))
		{
			printf("[CPlugins] %s ends for a timer, standby or shutdown\n", name.c_str());
			endFullscreen(pid);
			return msg;
		}
		app->handleMsg(msg, data);
	}
	if (WIFEXITED(wstatus))
		status = WEXITSTATUS(wstatus);
	data = 0;
	return 0;
}

/*
	A program that draws on the display itself, a game for instance: live
	TV stops, and on generic hardware the display and the input devices
	are given to it until it ends.
*/
int CPlugins::runProgram(const std::string &name, const char *const argv[], const std::string &picture, bool end_on_exit)
{
	t_channel_id channel = CZapit::getInstance()->GetCurrentChannelID();
	running++;
	g_RCInput->clearRCMsg();
	g_RCInput->stopInput();
	CNeutrinoApp::getInstance()->lockPlayBack();
#ifdef ENABLE_LCD4LINUX
	/* a game should not share the processor with the display */
	CLCD4l::getInstance()->ShowStill(picture);
#else
	(void)picture;
#endif
	frameBuffer->Lock();
	frameBuffer->setDisplayHandedOver(true);
	hal_display_suspend();

	int status = -1;
	pid_t pid = my_spawn(argv);
	neutrino_msg_t repost = 0;
	neutrino_msg_data_t repost_data = 0;
	if (pid > 0)
		repost = runFullscreen(name, pid, repost_data, end_on_exit, status);

	hal_display_resume();
#ifdef ENABLE_LCD4LINUX
	CLCD4l::getInstance()->ClearStill();
#endif
	frameBuffer->setDisplayHandedOver(false);
	frameBuffer->Unlock();
	frameBuffer->paintBackground();
	frameBuffer->blit();
	CNeutrinoApp::getInstance()->unlockPlayBack();
	/* a recording may have moved the live channel meanwhile: it stays */
	if (channel && CNeutrinoApp::getInstance()->channelList &&
	    CZapit::getInstance()->GetCurrentChannelID() == channel)
		CNeutrinoApp::getInstance()->channelList->zapTo_ChannelID(channel, true);
	g_RCInput->restartInput();
	g_RCInput->clearRCMsg();
	if (repost)
		g_RCInput->postMsg(repost, repost_data);
	running--;
	return status;
}

int CPlugins::startFullscreenPlugin(int number)
{
	const char *script = plugin_list[number].pluginfile.c_str();
	printf("[CPlugins] executing %s on the whole display\n", script);
	if (!file_exists(script))
	{
		printf("[CPlugins] could not find %s\n", script);
		return menu_return::RETURN_REPAINT;
	}
	/* the program's own icon (kodi.svg for kodi.sh) where the system has
	   one, else the hint icon of the plugin */
	std::string base = plugin_list[number].pluginfile;
	base = base.substr(base.rfind('/') + 1);
	base = base.substr(0, base.rfind('.'));
	std::string picture = "/usr/share/icons/hicolor/scalable/apps/" + base + ".svg";
	if (access(picture.c_str(), R_OK) != 0)
		picture = "/usr/share/icons/hicolor/256x256/apps/" + base + ".png";
	if (access(picture.c_str(), R_OK) != 0) {
		picture = plugin_list[number].hinticon;
		if (picture.find('/') == std::string::npos)
			picture = frameBuffer->getIconPath(picture);
	}
	const char *argv[] = { "/bin/sh", script, NULL };
	std::string name = plugin_list[number].name;
	int menu_ret = plugin_list[number].menu_return;
	runProgram(name, argv, picture);
	return menu_ret;
}

int CPlugins::startLuaPlugin(int number)
{
	const char *script = plugin_list[number].pluginfile.c_str();
	printf("[CPlugins] executing lua script %s\n", script);
	if (!file_exists(script))
	{
		printf("[CPlugins] could not find %s,\nperhaps wrong plugin type in %s\n",
			script, plugin_list[number].cfgfile.c_str());
		return menu_return::RETURN_REPAINT;
	}
#ifdef ENABLE_LUA
	CLuaInstance *lua = new CLuaInstance();
	// FIXME: runScript() should return the exit-code from script
	lua->runScript(script);
	delete lua;
#endif
#if 0
	frameBuffer->ClearFB();
#endif
	std::string keep_bg_flag = "/tmp/.keep_background";
	if (!file_exists(keep_bg_flag))
	{
		videoDecoder->Pig(-1, -1, -1, -1);
		frameBuffer->paintBackground();
	}
	else
		remove(keep_bg_flag.c_str());

	return plugin_list[number].menu_return;
}

int CPlugins::startPlugin(int number)
{
	running++;
	int ret = runPlugin(number);
	running--;
	return ret;
}

bool CPlugins::startPluginIfIdle(const char *const filename)
{
	/* Checked and taken in one step: besides the main loop, a Lua
	 * thread waiting for input can pass a request on to here. */
	int idle = 0;
	if (!running.compare_exchange_strong(idle, 1))
		return false;
	int pluginnr = find_plugin(filename);
	if (pluginnr > -1)
		runPlugin(pluginnr);
	else
		printf("[CPlugins] could not find %s\n", filename);
	running--;
	return true;
}

int CPlugins::runPlugin(int number)
{
	// always delete old output
	delScriptOutput();
	/* export neutrino settings to the environment */
	char tmp[32];
#if 0
	sprintf(tmp, "%d", g_settings.screen_StartX_int);
#else
	sprintf(tmp, "%d", g_settings.screen_StartX);
#endif
	setenv("SCREEN_OFF_X", tmp, 1);
#if 0
	sprintf(tmp, "%d", g_settings.screen_StartY_int);
#else
	sprintf(tmp, "%d", g_settings.screen_StartY);
#endif
	setenv("SCREEN_OFF_Y", tmp, 1);
#if 0
	sprintf(tmp, "%d", g_settings.screen_EndX_int);
#else
	sprintf(tmp, "%d", g_settings.screen_EndX);
#endif
	setenv("SCREEN_END_X", tmp, 1);
#if 0
	sprintf(tmp, "%d", g_settings.screen_EndY_int);
#else
	sprintf(tmp, "%d", g_settings.screen_EndY);
#endif
	setenv("SCREEN_END_Y", tmp, 1);

	bool ispip = strstr(plugin_list[number].pluginfile.c_str(), "pip") != 0;
	//printf("exec: %s pip: %d\n", plugin_list[number].pluginfile.c_str(), ispip);
	if (ispip && !g_RemoteControl->is_video_started)
		return menu_return::RETURN_REPAINT;

	if (plugin_list[number].terminal)
	{
		return startTerminalPlugin(number);
	}
	if (plugin_list[number].fullscreen)
	{
		return startFullscreenPlugin(number);
	}
	if (plugin_list[number].type == CPlugins::P_TYPE_SCRIPT)
	{
		return startScriptPlugin(number);
	}
	if (plugin_list[number].type == CPlugins::P_TYPE_LUA)
	{
		return startLuaPlugin(number);
	}
	if (!file_exists(plugin_list[number].pluginfile.c_str()))
	{
		printf("[CPlugins] could not find %s,\nperhaps wrong plugin type in %s\n",
			plugin_list[number].pluginfile.c_str(), plugin_list[number].cfgfile.c_str());
		return menu_return::RETURN_REPAINT;
	}

	g_RCInput->clearRCMsg();
	g_RCInput->stopInput();
	/* stop automatic updates etc. */
	frameBuffer->Lock();
	//frameBuffer->setMode(720, 576, 8 * sizeof(fb_pixel_t));
	printf("Starting %s\n", plugin_list[number].pluginfile.c_str());

	// workaround for manually messed up permissions
	if (access(plugin_list[number].pluginfile, X_OK))
		chmod(plugin_list[number].pluginfile.c_str(), 0755);

	my_system(2, plugin_list[number].pluginfile.c_str(), NULL);
	//frameBuffer->setMode(720, 576, 8 * sizeof(fb_pixel_t));
	frameBuffer->Unlock();
#if 0
	frameBuffer->ClearFB();
#endif
	videoDecoder->Pig(-1, -1, -1, -1);
	frameBuffer->paintBackground();
	g_RCInput->restartInput();
	g_RCInput->clearRCMsg();

	return plugin_list[number].menu_return;
}

bool CPlugins::hasPlugin(CPlugins::p_type_t type)
{
	for (std::vector<plugin>::iterator it = plugin_list.begin(); it != plugin_list.end(); ++it)
	{
		/* as CPluginList shows them: what is integrated into another menu
		   is not in the list, and an empty list is not worth an entry */
		if ((it->type & type) && !it->hide && it->integration == PLUGIN_INTEGRATION_DISABLED)
			return true;
	}
	return false;
}

const std::string &CPlugins::getScriptOutput() const
{
	return scriptOutput;
}

void CPlugins::delScriptOutput()
{
	scriptOutput.clear();
}

CPlugins::p_type_t CPlugins::getPluginType(int type)
{
	switch (type)
	{
		case PLUGIN_TYPE_DISABLED:
			return P_TYPE_DISABLED;
			break;
		case PLUGIN_TYPE_GAME:
			return P_TYPE_GAME;
			break;
		case PLUGIN_TYPE_TOOL:
			return P_TYPE_TOOL;
			break;
		case PLUGIN_TYPE_SCRIPT:
			return P_TYPE_SCRIPT;
			break;
		case PLUGIN_TYPE_LUA:
			return P_TYPE_LUA;
		default:
			return P_TYPE_DISABLED;
	}
}

/*
 * An integration value this build does not know would make the plugin
 * invisible: integratePlugins() only matches exact values and the plugin
 * list only shows PLUGIN_INTEGRATION_DISABLED. Fall back to the list so a
 * plugin built for a newer menu slot stays reachable.
 */
int CPlugins::getPluginIntegration(int integration)
{
	switch (integration)
	{
		case PLUGIN_INTEGRATION_MAIN:
		case PLUGIN_INTEGRATION_MULTIMEDIA:
		case PLUGIN_INTEGRATION_SETTING:
		case PLUGIN_INTEGRATION_SERVICE:
		case PLUGIN_INTEGRATION_INFORMATION:
		case PLUGIN_INTEGRATION_SOFTWARE_MANAGE:
		case PLUGIN_INTEGRATION_POWER:
		case PLUGIN_INTEGRATION_NETWORK:
			return integration;
		default:
			return PLUGIN_INTEGRATION_DISABLED;
	}
}

neutrino_msg_t CPlugins::getPluginKey(std::string key)
{
	if (key == "red")
		return CRCInput::RC_red;
	else if (key == "green")
		return CRCInput::RC_green;
	else if (key == "yellow")
		return CRCInput::RC_yellow;
	else if (key == "blue")
		return CRCInput::RC_blue;
	else /* (key == "auto") */
		return CRCInput::RC_nokey;
}
