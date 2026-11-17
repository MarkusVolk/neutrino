/*
	lcd4l - Neutrino-GUI

	Copyright (C) 2012 'defans'
	Homepage: http://www.bluepeercrew.us/

	Copyright (C) 2012-2018 'vanhofen'
	Homepage: http://www.neutrino-images.de/

	Copyright (C) 2016-2019 'TangoCash'
	Copyright (C) 2021, Thilo Graf 'dbt'

	License: GPL

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program. If not, see <http://www.gnu.org/licenses/>.


*/

#ifndef __lcd4l__
#define __lcd4l__

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sigc++/signal.h>

class CLCD4l
{
	public:
		CLCD4l();
		~CLCD4l();
		static CLCD4l *getInstance();

		// Displays
		enum
		{
			PEARL320x240	= 0,
			SAMSUNG800x480	= 1,
			SAMSUNG800x600	= 2,
			SAMSUNG1024x600	= 3,
			VUSOLO4K480x320	= 4,
			PNG800x480	= 5,
			PNG800x600	= 6,
			PNG1024x600	= 7
		};

		// Functions
		void	InitLCD4l();
		void	StartLCD4l();
		void	StopLCD4l();
		void	SwitchLCD4l();
		void	RestartLCD4lScript();
		void	ForceRun() { wait4daemon = false; }
		void	setActionKey(const std::string ActionKey) { m_ActionKey = ActionKey; }
		void    clearActionKey(void) { m_ActionKey.clear(); }

		int	CreateFile(const char *file, std::string content = "", bool convert = false);
		int	RemoveFile(const char *file);

		int	CreateEventFile(std::string content = "", bool convert = false);
		int	CreateMenuFile(std::string content = "", bool convert = false);
		int	RemoveMenuFile();

		int	GetMaxBrightness();

		void	ResetParseID() { m_ParseID = 0; }

		/* what an HbbTV application or a media library shows while it has the
		   screen, in place of the channel: its name, the highlighted entry or
		   the video that plays, and where that video is (ms, 0 if unknown) */
		void	SetExternal(const std::string &service, const std::string &event, bool playing, bool paused, int position, int duration);
		void	ClearExternal();

		/* the open menu as a picture for the display: the menu hands over
		   where it is drawn each time it waits for a key, a thread of its
		   own writes the picture once the keys rest */
		void	MenuShown(int x, int y, int w, int h);
		void	MenuHidden();

		/* a program has the whole display: one still picture, drawn once,
		   until it ends - nothing else is read or drawn meanwhile */
		void	ShowStill(const std::string &picture);
		void	ClearStill();

		static void lcd4linux(int run);
		enum
		{
			STOP_LCD4L 	= 0,
			START_LCD4L 	= 1,
			RELOAD_LCD4L	= 2
		};

		// use signal/slot handlers
		// That is helping to keep the GUI code away from code inside ./src/driver.
		sigc::signal<void>
					OnBeforeStart,
					OnAfterStart,
					OnBeforeStop,
					OnAfterStop,
					OnBeforeRestart,
					OnAfterRestart,
					OnError;

	private:
		bool		StillChanged();
		bool		HandleStill();
		std::string	m_StillPicture;
		bool		m_StillWanted;
		bool		m_StillShown;

		std::thread	*thrMenu;
		void		MenuProc();
		bool		MenuWrite(std::vector<uint32_t> &pixels, int w, int h);
		std::mutex	m_MenuLock;
		std::condition_variable m_MenuWake;
		std::vector<uint32_t> m_MenuPixels;
		int		m_MenuW;
		int		m_MenuH;
		bool		m_MenuNew;
		bool		m_MenuHide;
		bool		m_MenuQuit;
		uint64_t	m_MenuDue;

		std::thread	*thrLCD4l;
		static void	*LCD4lProc(void *arg);
		bool		exit_proc;

		struct tm	*tm_struct;
		bool		wait4daemon;

		// Functions
		void		Init();
		void		ParseInfo(uint64_t parseID, bool newID, bool firstRun = false);

		uint64_t	GetParseID();
		bool		CompareParseID(uint64_t &i_ParseID);

		std::string	hexStr(unsigned char data);
		std::string	hexStrA2A(unsigned char data);
		void		strReplace(std::string &orig, const std::string &fstr, const std::string &rstr);
		bool		WriteFile(const char *file, std::string content = "", bool convert = false);

		void		SetWaitStatus(bool wait) { wait4daemon = wait; }
		bool		GetWaitStatus() { return wait4daemon; }

		struct external_info
		{
			bool active;
			std::string service;
			std::string event;
			bool playing;
			bool paused;
			int position;
			int duration;
		};
		std::mutex	m_ExternalLock;
		external_info	m_External;

		// Variables
		std::string m_ActionKey;
		uint64_t	m_ParseID;
		int		m_Mode;
		int		m_ModeChannel;

		int		m_Brightness;
		int		m_Brightness_standby;
		std::string	m_Resolution;
		std::string	m_AspectRatio;
		int		m_Videotext;
		int		m_Radiotext;
		std::string	m_DolbyDigital;
		int		m_Tuner;
		int		m_Tuner_sig;
		int		m_Tuner_snr;
		int		m_Tuner_ber;
		int		m_Volume;
		int		m_ModeRec;
		int		m_RecordCount;
		int		m_ModeTshift;
		int		m_ModeTimer;
// 		int		m_ModeEcm;
		bool		m_ModeCamPresent;
		int		m_ModeCam;

		std::string	m_Service;
		int		m_ChannelNr;
		std::string	m_Logo;
		int		m_ModeLogo;

		std::string	m_Layout;

		std::string	m_Event;
		std::string	m_Info1;
		std::string	m_Info2;
		int		m_Progress;
		char		m_Duration[128];
		std::string	m_Start;
		std::string	m_End;

		std::string	m_font;
		std::string	m_fgcolor;
		std::string	m_bgcolor;
		std::string	m_fcolor1;
		std::string	m_fcolor2;
		std::string	m_pbcolor;

		std::string	m_wcity;
		std::string	m_wtimestamp;
		std::string	m_wtemp;
		std::string	m_wwind;
		std::string	m_wicon;
};

#endif
