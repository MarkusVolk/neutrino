/*
	Assistant: ask a language model in writing or by voice; it answers
	and operates the box with tools of our own and of MCP servers

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

#ifndef __assistant__
#define __assistant__

#include <gui/widget/menue.h>
#include <system/llm_client.h>
#include <system/mcp_client.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

class CComponentsFooter;
class CComponentsHeader;
class CFrameBuffer;
class CTextBox;

class CAssistant : public CMenuTarget
{
	private:
		struct tool_route
		{
			CMcpClient *server;
			std::string tool;
		};

		CFrameBuffer *frameBuffer;
		CComponentsHeader *header;
		CComponentsFooter *footer;
		CTextBox *textBox;
		CLlmClient *llm;
		std::vector<CMcpClient *> servers;
		/* the tools of the servers, by the name the model knows them by */
		std::map<std::string, tool_route> routes;
		/* the settings the conversation was started with */
		std::string signature;
		std::string transcript;
		volatile bool cancel;
		bool tools_ready;
		bool leave;
		int x, y, width, height;
		int header_height, footer_height, status_height;

		CAssistant();

		void setup();
		bool haveModel();
		void closeConversation();
		bool prepareTools();
		void runTurn(const std::string &text);
		void ask();
		void speak();
		bool wait(const std::function<void()> &job, const std::string &what);

		void paint();
		void paintText();
		void paintStatus(const std::string &text);
		void say(const std::string &text);

	public:
		~CAssistant();
		static CAssistant *getInstance();

		void hide();
		int exec(CMenuTarget *parent, const std::string &actionKey);
};

#endif
