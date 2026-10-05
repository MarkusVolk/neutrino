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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "assistant.h"
#include "assistant_commands.h"
#include "assistant_setup.h"
#include "assistant_tools.h"

#include <global.h>
#include <neutrino.h>
#include <driver/fontrenderer.h>
#include <driver/framebuffer.h>
#include <driver/rcinput.h>
#include <driver/screen_max.h>
#include <gui/components/cc.h>
#include <gui/components/cc_input_dialog.h>
#include <gui/infoviewer.h>
#include <gui/widget/icons.h>
#include <gui/widget/textbox.h>
#include <system/helpers.h>
#ifdef ENABLE_ASSISTANT_VOICE
#include <system/voice_input.h>
#endif

#include <thread>

#define MAX_ROUNDS	16
#define MAX_NOTE	90

static const char system_prompt[] =
	"You are the assistant built into a Neutrino set-top box. Your answers are shown on the TV screen "
	"and the user operates you with a remote control or by voice. Answer briefly, in the language of "
	"the user, in plain text without Markdown. Use the tools to look things up and to operate the box "
	"instead of guessing, and act on a clear request without asking back. All times are local time.";

static const struct button_label CAssistantFooterButtons[] =
{
	{ NEUTRINO_ICON_BUTTON_RED,	LOCALE_ASSISTANT_ASK },
#ifdef ENABLE_ASSISTANT_VOICE
	{ NEUTRINO_ICON_BUTTON_GREEN,	LOCALE_ASSISTANT_SPEAK },
#endif
	{ NEUTRINO_ICON_BUTTON_YELLOW,	LOCALE_ASSISTANT_NEW },
	{ NEUTRINO_ICON_BUTTON_BLUE,	LOCALE_ASSISTANT_SETTINGS }
};
#define CAssistantFooterButtonCount (sizeof(CAssistantFooterButtons)/sizeof(CAssistantFooterButtons[0]))

CAssistant::CAssistant()
{
	frameBuffer = CFrameBuffer::getInstance();
	header = NULL;
	footer = NULL;
	textBox = NULL;
	llm = NULL;
	cancel = false;
	tools_ready = false;
	leave = false;
	x = y = width = height = 0;
	header_height = footer_height = status_height = 0;
}

CAssistant::~CAssistant()
{
	closeConversation();
}

CAssistant *CAssistant::getInstance()
{
	static CAssistant *instance = NULL;
	if (!instance)
		instance = new CAssistant();
	return instance;
}

void CAssistant::closeConversation()
{
	for (size_t i = 0; i < servers.size(); i++)
		delete servers[i];
	servers.clear();
	routes.clear();
	delete llm;
	llm = NULL;
	tools_ready = false;
	transcript.clear();
}

/* a new conversation when there is none or the settings it was started with have changed */
void CAssistant::setup()
{
	llm_config config;
	config.backend = g_settings.assistant_backend;
	if (config.backend == llm_config::BACKEND_OPENAI)
	{
		config.url = g_settings.assistant_openai_url;
		config.key = g_settings.assistant_openai_key;
		config.model = g_settings.assistant_openai_model;
	}
	else
	{
		static const char *const effort[] = { "", "low", "medium", "high" };
		config.key = g_settings.assistant_claude_key;
		config.model = g_settings.assistant_claude_model;
		config.effort = effort[g_settings.assistant_claude_effort & 3];
	}

	std::string now = to_string(config.backend) + "\n" + config.url + "\n" + config.key + "\n" + config.model + "\n" + config.effort;
	for (int i = 0; i < ASSISTANT_MCP_SERVERS; i++)
		now += "\n" + g_settings.assistant_mcp_name[i] + "\n" + g_settings.assistant_mcp_url[i] + "\n" + g_settings.assistant_mcp_token[i];
	if (llm && now == signature)
		return;

	closeConversation();
	signature = now;
	llm = CLlmClient::create(config);
	llm->setSystem(system_prompt);
	for (int i = 0; i < ASSISTANT_MCP_SERVERS; i++)
	{
		if (g_settings.assistant_mcp_url[i].empty())
			continue;
		std::string name = g_settings.assistant_mcp_name[i];
		if (name.empty())
			name = "mcp" + to_string(i + 1);
		servers.push_back(new CMcpClient(name, g_settings.assistant_mcp_url[i], g_settings.assistant_mcp_token[i]));
	}
}

/* without one the assistant understands its own commands and nothing else */
bool CAssistant::haveModel()
{
	if (g_settings.assistant_backend == llm_config::BACKEND_OPENAI)
		return !g_settings.assistant_openai_url.empty();
	const char *key = getenv("ANTHROPIC_API_KEY");
	return !g_settings.assistant_claude_key.empty() || (key && *key);
}

/* what a model accepts as the name of a tool */
static std::string tool_name(const std::string &server, const std::string &tool)
{
	std::string name = server + "_" + tool;
	for (size_t i = 0; i < name.size(); i++)
		if (!isalnum((unsigned char)name[i]) && name[i] != '_' && name[i] != '-')
			name[i] = '_';
	return name.substr(0, 64);
}

/* our own tools and those of the servers; a server that does not answer is left out and named */
bool CAssistant::prepareTools()
{
	if (tools_ready)
		return true;

	std::vector<llm_tool> tools;
	CAssistantTools::list(tools);
	routes.clear();
	for (size_t i = 0; i < servers.size(); i++)
	{
		CMcpClient *server = servers[i];
		std::vector<mcp_tool> list;
		std::string error;
		bool ok = false;
		if (!wait([&] { ok = server->listTools(list, error, &cancel); },
			  std::string(g_Locale->getText(LOCALE_ASSISTANT_CONNECTING)) + " " + server->getName()))
			return false;
		if (!ok)
		{
			say("(" + server->getName() + ": " + error + ")");
			continue;
		}
		for (size_t j = 0; j < list.size(); j++)
		{
			llm_tool tool;
			tool.name = tool_name(server->getName(), list[j].name);
			if (routes.count(tool.name) || !tool.name.compare(0, 3, "tv_"))
				continue;
			tool.description = list[j].description;
			tool.schema = list[j].schema;
			tools.push_back(tool);
			tool_route route = { server, list[j].name };
			routes[tool.name] = route;
		}
	}
	llm->setTools(tools);
	tools_ready = true;
	return true;
}

/*
	Runs the job in a thread of its own and serves the remote control
	meanwhile. False when the user gave up: the job has ended by then too,
	as its requests look at cancel.
*/
bool CAssistant::wait(const std::function<void()> &job, const std::string &what)
{
	volatile bool done = false;
	int ticks = 0;
	cancel = false;
	std::thread worker([&] { job(); done = true; });
	while (!done)
	{
		neutrino_msg_t msg;
		neutrino_msg_data_t data;
		g_RCInput->getMsg_ms(&msg, &data, 100);
		if (msg == CRCInput::RC_timeout)
		{
			if (ticks++ % 4 == 0)
				paintStatus(what + " " + std::string(1 + (ticks / 4) % 3, '.'));
		}
		else if (msg == CRCInput::RC_home || msg == CRCInput::RC_stop)
			cancel = true;
		else if (msg > CRCInput::RC_MaxRC)
		{
			if (CNeutrinoApp::getInstance()->handleMsg(msg, data) & messages_return::cancel_all)
			{
				cancel = true;
				leave = true;
			}
		}
	}
	worker.join();
	paintStatus("");
	return !cancel;
}

void CAssistant::say(const std::string &text)
{
	if (!transcript.empty())
		transcript += "\n";
	transcript += text;
	transcript += "\n";
	paintText();
}

void CAssistant::runTurn(const std::string &text)
{
	std::string answer;
	say("> " + text);
	if (!haveModel())
	{
		say(CAssistantCommands::run(text, answer) ? answer : std::string(g_Locale->getText(LOCALE_ASSISTANT_NOT_UNDERSTOOD)));
		return;
	}
	if (!prepareTools())
		return;

	/* the model is told what time it is and what is on, the user does not have to read that */
	llm->addUser(text + "\n\n[" + CAssistantTools::context() + "]");
	const std::string thinking = g_Locale->getText(LOCALE_ASSISTANT_THINKING);
	for (int round = 0; round < MAX_ROUNDS; round++)
	{
		llm_reply reply;
		bool ok = false;
		if (!wait([&] { ok = llm->request(reply, &cancel); }, thinking))
		{
			llm->abortTurn();
			say(std::string("(") + g_Locale->getText(LOCALE_ASSISTANT_CANCELLED) + ")");
			return;
		}
		if (!ok)
		{
			llm->abortTurn();
			/* when the model cannot be reached, a command we know still works */
			if (round == 0 && CAssistantCommands::run(text, answer))
				say(answer);
			else
				say("(" + reply.error + ")");
			return;
		}
		if (!reply.text.empty())
			say(reply.text);
		if (reply.calls.empty())
			return;

		for (size_t i = 0; i < reply.calls.size(); i++)
		{
			const llm_tool_call &call = reply.calls[i];
			std::string note = call.name + " " + json_compact(call.input);
			if (note.size() > MAX_NOTE)
				note = note.substr(0, MAX_NOTE) + " ...";
			transcript += "  - " + note + "\n";
			paintText();
			printf("[assistant] %s %s\n", call.name.c_str(), json_compact(call.input).c_str());

			std::string result;
			bool failed;
			std::map<std::string, tool_route>::iterator route = routes.find(call.name);
			if (route == routes.end())
				failed = !CAssistantTools::call(call.name, call.input, result);
			else
			{
				std::string error;
				bool got = false;
				failed = false;
				CMcpClient *server = route->second.server;
				const std::string &tool = route->second.tool;
				if (!wait([&] { got = server->callTool(tool, call.input, result, failed, error, &cancel); },
					  server->getName()))
				{
					llm->abortTurn();
					say(std::string("(") + g_Locale->getText(LOCALE_ASSISTANT_CANCELLED) + ")");
					return;
				}
				if (!got)
				{
					result = error;
					failed = true;
				}
			}
			llm->addToolResult(call, result, failed);
		}
	}
	llm->abortTurn();
	say(std::string("(") + g_Locale->getText(LOCALE_ASSISTANT_TOO_MANY_STEPS) + ")");
}

void CAssistant::ask()
{
	std::string text;
	CCTextInputDialog input(g_Locale->getText(LOCALE_ASSISTANT_ASK), &text);
	input.enableOnScreenKeyboard(true);
	input.exec(NULL, "");
	g_RCInput->clearRCMsg();
	paint();
	if (!trim(text).empty())
		runTurn(text);
}

/* records until the speaker pauses or green or OK is pressed, and asks what was understood */
void CAssistant::speak()
{
#ifdef ENABLE_ASSISTANT_VOICE
	stt_config config;
	config.url = g_settings.assistant_stt_url;
	config.key = g_settings.assistant_stt_key;
	config.model = g_settings.assistant_stt_model;
	config.language = g_settings.assistant_stt_language;
	if (config.url.empty())
	{
		say(std::string("(") + g_Locale->getText(LOCALE_ASSISTANT_NO_STT) + ")");
		return;
	}

	CVoiceInput voice;
	std::string error;
	if (!voice.start(g_settings.assistant_stt_device, error))
	{
		say("(" + error + ")");
		return;
	}
	const std::string listening = g_Locale->getText(LOCALE_ASSISTANT_LISTENING);
	bool given_up = false;
	while (voice.isRunning())
	{
		neutrino_msg_t msg;
		neutrino_msg_data_t data;
		g_RCInput->getMsg_ms(&msg, &data, 100);
		if (msg == CRCInput::RC_timeout)
			paintStatus(listening + "  " + std::string(voice.getLevel() / 4, '|'));
		else if (msg == CRCInput::RC_green || msg == CRCInput::RC_ok)
			break;
		else if (msg == CRCInput::RC_home)
		{
			given_up = true;
			break;
		}
		else if (msg > CRCInput::RC_MaxRC)
			CNeutrinoApp::getInstance()->handleMsg(msg, data);
	}
	voice.stop();
	paintStatus("");
	if (given_up)
		return;
	if (!voice.heardSpeech())
	{
		say(std::string("(") + g_Locale->getText(LOCALE_ASSISTANT_NO_SPEECH) + ")");
		return;
	}

	std::string wav = voice.getWav();
	std::string text;
	bool ok = false;
	if (!wait([&] { ok = transcribe(config, wav, text, error, &cancel); }, g_Locale->getText(LOCALE_ASSISTANT_TRANSCRIBING)))
		return;
	if (!ok)
		say("(" + error + ")");
	else if (text.empty())
		say(std::string("(") + g_Locale->getText(LOCALE_ASSISTANT_NO_SPEECH) + ")");
	else
		runTurn(text);
#endif
}

void CAssistant::paintText()
{
	if (!textBox)
		return;
	std::string text = transcript;
	if (text.empty())
	{
		text = g_Locale->getText(LOCALE_ASSISTANT_HINT);
		if (!haveModel())
			text += std::string("\n\n") + g_Locale->getText(LOCALE_ASSISTANT_HELP);
	}
	textBox->setText(&text);
	if (!textBox->isPainted())
		textBox->paint();
	/* the end is what was said last */
	textBox->scrollPageDown(textBox->getPages());
}

void CAssistant::paintStatus(const std::string &text)
{
	int sy = y + height - footer_height - status_height;
	frameBuffer->paintBoxRel(x, sy, width, status_height, COL_MENUCONTENT_PLUS_0);
	if (!text.empty())
		g_Font[SNeutrinoSettings::FONT_TYPE_MENU_INFO]->RenderString(x + OFFSET_INNER_MID, sy + status_height,
				width - 2 * OFFSET_INNER_MID, text, COL_MENUCONTENTINACTIVE_TEXT);
}

void CAssistant::paint()
{
	width = frameBuffer->getWindowWidth();
	height = frameBuffer->getWindowHeight();
	x = getScreenStartX(width);
	y = getScreenStartY(height);

	delete header;
	header = new CComponentsHeader(x, y, width, 0, g_Locale->getText(LOCALE_ASSISTANT_HEAD), NEUTRINO_ICON_INFO);
	header_height = header->getHeight();
	header->paint(CC_SAVE_SCREEN_NO);

	delete footer;
	footer = new CComponentsFooter();
	footer_height = footer->getHeight();
	status_height = g_Font[SNeutrinoSettings::FONT_TYPE_MENU_INFO]->getHeight() + OFFSET_INNER_SMALL;

	CBox position(x, y + header_height, width, height - header_height - footer_height - status_height);
	delete textBox;
	textBox = new CTextBox("", g_Font[SNeutrinoSettings::FONT_TYPE_MENU], CTextBox::SCROLL | CTextBox::TOP, &position);
	textBox->setTextBorderWidth(OFFSET_INNER_MID, OFFSET_INNER_SMALL);
	paintText();
	paintStatus("");
	footer->paintButtons(x, y + height - footer_height, width, footer_height, CAssistantFooterButtonCount, CAssistantFooterButtons);
}

void CAssistant::hide()
{
	delete textBox;
	textBox = NULL;
	delete header;
	header = NULL;
	delete footer;
	footer = NULL;
	frameBuffer->paintBackgroundBoxRel(x, y, width, height);
}

int CAssistant::exec(CMenuTarget *parent, const std::string &)
{
	int res = menu_return::RETURN_REPAINT;
	if (parent)
		parent->hide();
	if (g_InfoViewer)
		g_InfoViewer->killTitle();

	setup();
	leave = false;
	paint();

	while (!leave)
	{
		neutrino_msg_t msg;
		neutrino_msg_data_t data;
		g_RCInput->getMsg(&msg, &data, 100);
		if (msg == CRCInput::RC_timeout)
			continue;
		if (msg == CRCInput::RC_home)
			break;
		else if (msg == CRCInput::RC_red || msg == CRCInput::RC_ok)
			ask();
		else if (msg == CRCInput::RC_green)
			speak();
		else if (msg == CRCInput::RC_yellow)
		{
			closeConversation();
			setup();
			paintText();
		}
		else if (msg == CRCInput::RC_blue)
		{
			CAssistantSetup settings;
			hide();
			settings.exec(NULL, "");
			setup();
			paint();
		}
		else if (msg == CRCInput::RC_up || msg == CRCInput::RC_page_up)
			textBox->scrollPageUp(1);
		else if (msg == CRCInput::RC_down || msg == CRCInput::RC_page_down)
			textBox->scrollPageDown(1);
		else if (msg > CRCInput::RC_MaxRC)
		{
			if (CNeutrinoApp::getInstance()->handleMsg(msg, data) & messages_return::cancel_all)
			{
				res = menu_return::RETURN_EXIT_ALL;
				break;
			}
		}
	}
	if (leave)
		res = menu_return::RETURN_EXIT_ALL;
	hide();
	return res;
}
