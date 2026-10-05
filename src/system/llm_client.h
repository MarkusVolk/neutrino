/*
	Language models for the assistant: the Claude API and servers that
	speak the OpenAI chat completions protocol

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

#ifndef __llm_client_h__
#define __llm_client_h__

#include <string>
#include <vector>
#include <json/json.h>

struct llm_tool
{
	std::string name;
	std::string description;
	/* JSON schema of the arguments */
	Json::Value schema;
};

struct llm_tool_call
{
	std::string id;
	std::string name;
	Json::Value input;
};

struct llm_reply
{
	std::string text;
	std::vector<llm_tool_call> calls;
	std::string error;
};

struct llm_config
{
	enum
	{
		BACKEND_CLAUDE,
		BACKEND_OPENAI
	};
	int backend;
	/* OpenAI protocol only, up to and including /v1 */
	std::string url;
	std::string key;
	std::string model;
	/* Claude only: "low", "medium", "high", or empty for the model's default */
	std::string effort;
};

/*
	One conversation. A turn starts with addUser(); request() is repeated,
	with the results of the tools the model called added in between, until
	a reply comes without calls. A turn that fails or is given up is taken
	back with abortTurn(), so the conversation stays one the server accepts.
*/
class CLlmClient
{
	protected:
		llm_config config;
		std::string system;
		std::vector<llm_tool> tools;
		Json::Value messages;
		Json::ArrayIndex turn_start;

		CLlmClient(const llm_config &Config);

	public:
		static CLlmClient *create(const llm_config &config);
		virtual ~CLlmClient() {}

		void setSystem(const std::string &text) { system = text; }
		void setTools(const std::vector<llm_tool> &list) { tools = list; }
		void reset();
		void abortTurn();

		virtual void addUser(const std::string &text) = 0;
		virtual void addToolResult(const llm_tool_call &call, const std::string &result, bool is_error) = 0;
		virtual bool request(llm_reply &reply, const volatile bool *cancel) = 0;
};

std::string json_compact(const Json::Value &value);
bool json_parse(const std::string &text, Json::Value &value);

#endif
