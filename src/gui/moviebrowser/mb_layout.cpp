/*
	Based up Neutrino-GUI - Tuxbox-Project
	Copyright (C) 2001 by Steffen Hehn 'McClean'

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

	***********************************************************

	Module Name: mb_layout.cpp

	Description: Layout and frame helpers for moviebrowser

	Date:	   Nov 2005
	Author: Guenther@tuxbox.berlios.org
		based on code of Steffen Hehn 'McClean'

	(C) 2009-2015 Stefan Seyfried
	(C) 2016      Sven Hoefer

	outsourced:
	(C) 2026 Thilo Graf 'dbt'
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "mb.h"
#include "mb_constants.h"

#include <global.h>

#include <driver/fontrenderer.h>
#include <driver/screen_max.h>

#include <algorithm>

#define TITLE_FONT g_Font[SNeutrinoSettings::FONT_TYPE_MENU_TITLE]
#define FOOT_FONT g_Font[SNeutrinoSettings::FONT_TYPE_MENU_FOOT]

void CMovieBrowser::clearListLines()
{
	if (m_pcBrowser)
	{
		for (int i = 0; i < MB_MAX_ROWS; i++)
			m_pcBrowser->cleanupRow(&m_browserListLines, i);
	}

	m_browserListLines.Icon.clear();
	m_browserListLines.marked.clear();
}

void CMovieBrowser::initFrames(void)
{
	m_pcFontFoot = FOOT_FONT;
	m_pcFontTitle = TITLE_FONT;

	m_cBoxFrame.iWidth = 			framebuffer->getWindowWidth();
	m_cBoxFrame.iHeight = 			framebuffer->getWindowHeight();
	m_cBoxFrame.iX = 			getScreenStartX(m_cBoxFrame.iWidth);
	m_cBoxFrame.iY = 			getScreenStartY(m_cBoxFrame.iHeight);

	m_cBoxFrameTitleRel.iX =		0;
	m_cBoxFrameTitleRel.iY = 		0;
	m_cBoxFrameTitleRel.iWidth = 		m_cBoxFrame.iWidth;
	m_cBoxFrameTitleRel.iHeight = 		m_pcFontTitle->getHeight();

	const int pic_h = 39;
	m_cBoxFrameTitleRel.iHeight = std::max(m_cBoxFrameTitleRel.iHeight, pic_h);

	m_cBoxFrameBrowserList.iX = 		m_cBoxFrame.iX;
	m_cBoxFrameBrowserList.iY = 		m_cBoxFrame.iY + m_cBoxFrameTitleRel.iHeight;
	if (m_settings.browserAdditional)
		m_cBoxFrameBrowserList.iWidth =	m_cBoxFrame.iWidth / 3 * 2;
	else
		m_cBoxFrameBrowserList.iWidth =	m_cBoxFrame.iWidth;

	m_settings.browserFrameHeight = m_settings.browserAdditional ? m_settings.browserFrameHeightAdditional : m_settings.browserFrameHeightGeneral;
	m_cBoxFrameBrowserList.iHeight = 	m_cBoxFrame.iHeight * m_settings.browserFrameHeight / 100;


	m_cBoxFrameFootRel.iX = 		m_cBoxFrameBrowserList.iX;
	m_cBoxFrameFootRel.iHeight = 		refreshFoot(false);
	m_cBoxFrameFootRel.iY = 		m_cBoxFrameBrowserList.iY + m_cBoxFrameBrowserList.iHeight;
	m_cBoxFrameFootRel.iWidth = 		m_cBoxFrame.iWidth;

	/* the wall takes the window below the title, its footer at the bottom */
	m_cBoxFrameWall.iX =			m_cBoxFrame.iX;
	m_cBoxFrameWall.iY =			m_cBoxFrameBrowserList.iY;
	m_cBoxFrameWall.iWidth =		m_cBoxFrame.iWidth;
	m_cBoxFrameWall.iHeight =		m_cBoxFrame.iHeight - m_cBoxFrameTitleRel.iHeight - m_cBoxFrameFootRel.iHeight;
	if (m_settings.gui == MB_GUI_WALL)
		m_cBoxFrameFootRel.iY =		m_cBoxFrameWall.iY + m_cBoxFrameWall.iHeight;

	m_cBoxFrameInfo1.iX = 			m_cBoxFrameBrowserList.iX;
	m_cBoxFrameInfo1.iY = 			m_cBoxFrameFootRel.iY + m_cBoxFrameFootRel.iHeight + OFFSET_INTER;
	m_cBoxFrameInfo1.iWidth = 		m_cBoxFrame.iWidth;
	m_cBoxFrameInfo1.iHeight = 		m_cBoxFrame.iHeight - m_cBoxFrameBrowserList.iHeight - OFFSET_INTER - m_cBoxFrameFootRel.iHeight - m_cBoxFrameTitleRel.iHeight;

	m_cBoxFrameInfo2.iX = 			m_cBoxFrameBrowserList.iX + m_cBoxFrameBrowserList.iWidth;
	m_cBoxFrameInfo2.iY = 			m_cBoxFrameBrowserList.iY;
	m_cBoxFrameInfo2.iWidth = 		m_cBoxFrame.iWidth - m_cBoxFrameBrowserList.iWidth;
	m_cBoxFrameInfo2.iHeight = 		m_cBoxFrameBrowserList.iHeight;

	m_cBoxFrameFilter.iX = 			m_cBoxFrameInfo1.iX;
	m_cBoxFrameFilter.iY = 			m_cBoxFrameInfo1.iY;
	m_cBoxFrameFilter.iWidth = 		m_cBoxFrameInfo1.iWidth;
	m_cBoxFrameFilter.iHeight = 		m_cBoxFrameInfo1.iHeight;
}
