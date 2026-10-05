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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "llm_client.h"
#include "http_request.h"

#include <stdlib.h>
#include <string.h>

#define CLAUDE_API_URL		"https://api.anthropic.com"
#define CLAUDE_API_VERSION	"2023-06-01"
#define CLAUDE_FALLBACK_BETA	"server-side-fallback-2026-07-01"
#define MAX_TOKENS		16000

std::string json_compact(const Json::Value &value)
{
	Json::StreamWriterBuilder builder;
	builder["indentation"] = "";
	builder["emitUTF8"] = true;
	return Json::writeString(builder, value);
}

bool json_parse(const std::string &text, Json::Value &value)
{
	Json::CharReaderBuilder builder;
	Json::CharReader *reader = builder.newCharReader();
	std::string errors;
	bool ok = reader->parse(text.data(), text.data() + text.size(), &value, &errors);
	delete reader;
	return ok;
}

/* what a server says about a failed request, or the status when it says nothing we know */
static std::string http_error(const http_reply &reply)
{
	Json::Value root;
	if (json_parse(reply.body, root) && root.isObject())
	{
		const Json::Value &error = root["error"];
		if (error.isObject() && error["message"].isString())
			return error["message"].asString();
		if (error.isString())
			return error.asString();
	}
	char status[32];
	snprintf(status, sizeof(status), "HTTP %ld", reply.status);
	return status;
}

CLlmClient::CLlmClient(const llm_config &Config)
{
	config = Config;
	messages = Json::Value(Json::arrayValue);
	turn_start = 0;
}

void CLlmClient::reset()
{
	messages = Json::Value(Json::arrayValue);
	turn_start = 0;
}

void CLlmClient::abortTurn()
{
	messages.resize(turn_start);
}

/* ------------------------------------------------------------------ */

class CClaudeClient : public CLlmClient
{
	private:
		Json::Value results;

		bool hasFallbacks();

	public:
		CClaudeClient(const llm_config &Config) : CLlmClient(Config), results(Json::arrayValue) {}

		void addUser(const std::string &text);
		void addToolResult(const llm_tool_call &call, const std::string &result, bool is_error);
		bool request(llm_reply &reply, const volatile bool *cancel);
};

void CClaudeClient::addUser(const std::string &text)
{
	turn_start = messages.size();
	results = Json::Value(Json::arrayValue);
	Json::Value message;
	message["role"] = "user";
	message["content"] = text;
	messages.append(message);
}

void CClaudeClient::addToolResult(const llm_tool_call &call, const std::string &result, bool is_error)
{
	Json::Value block;
	block["type"] = "tool_result";
	block["tool_use_id"] = call.id;
	block["content"] = result;
	if (is_error)
		block["is_error"] = true;
	results.append(block);
}

/* the models whose safeguards can decline a request, with a server side fallback for it */
bool CClaudeClient::hasFallbacks()
{
	static const char *const prefix[] = { "claude-opus-5", "claude-fable-5", "claude-sonnet-5-5" };
	for (size_t i = 0; i < sizeof(prefix) / sizeof(prefix[0]); i++)
		if (!config.model.compare(0, strlen(prefix[i]), prefix[i]))
			return true;
	return false;
}

bool CClaudeClient::request(llm_reply &reply, const volatile bool *cancel)
{
	reply = llm_reply();

	/* the results of all calls of a reply go back in one message */
	if (!results.empty())
	{
		Json::Value message;
		message["role"] = "user";
		message["content"] = results;
		messages.append(message);
		results = Json::Value(Json::arrayValue);
	}

	const char *base = getenv("ANTHROPIC_BASE_URL");
	std::string key = config.key;
	if (key.empty() && getenv("ANTHROPIC_API_KEY"))
		key = getenv("ANTHROPIC_API_KEY");
	if (key.empty())
	{
		reply.error = "no API key";
		return false;
	}

	Json::Value body;
	body["model"] = config.model;
	body["max_tokens"] = MAX_TOKENS;
	if (!system.empty())
		body["system"] = system;
	if (!config.effort.empty())
		body["output_config"]["effort"] = config.effort;
	body["cache_control"]["type"] = "ephemeral";
	if (!tools.empty())
	{
		Json::Value list(Json::arrayValue);
		for (size_t i = 0; i < tools.size(); i++)
		{
			Json::Value tool;
			tool["name"] = tools[i].name;
			tool["description"] = tools[i].description;
			tool["input_schema"] = tools[i].schema;
			list.append(tool);
		}
		body["tools"] = list;
	}
	body["messages"] = messages;

	std::vector<std::string> headers;
	headers.push_back("Content-Type: application/json");
	headers.push_back("x-api-key: " + key);
	headers.push_back("anthropic-version: " CLAUDE_API_VERSION);
	if (hasFallbacks())
	{
		body["fallbacks"] = "default";
		headers.push_back("anthropic-beta: " CLAUDE_FALLBACK_BETA);
	}

	http_reply http;
	std::string url = std::string((base && *base) ? base : CLAUDE_API_URL) + "/v1/messages";
	if (!http_post(url, headers, json_compact(body), http, cancel))
	{
		reply.error = http.error;
		return false;
	}
	if (http.status != 200)
	{
		reply.error = http_error(http);
		return false;
	}

	Json::Value root;
	if (!json_parse(http.body, root) || !root["content"].isArray())
	{
		reply.error = "unexpected reply";
		return false;
	}
	std::string stop = root["stop_reason"].asString();
	if (stop == "refusal")
	{
		reply.error = "the model declined the request";
		return false;
	}

	const Json::Value &content = root["content"];
	for (Json::ArrayIndex i = 0; i < content.size(); i++)
	{
		std::string type = content[i]["type"].asString();
		if (type == "text")
			reply.text += content[i]["text"].asString();
		else if (type == "tool_use")
		{
			llm_tool_call call;
			call.id = content[i]["id"].asString();
			call.name = content[i]["name"].asString();
			call.input = content[i]["input"];
			reply.calls.push_back(call);
		}
	}
	/* a call that was cut off must not run */
	if (stop == "max_tokens" && !reply.calls.empty())
	{
		reply.calls.clear();
		reply.error = "the reply was cut off";
		return false;
	}

	/* the content goes back as it came, with the blocks we do not read */
	Json::Value message;
	message["role"] = "assistant";
	message["content"] = content;
	messages.append(message);
	return true;
}

/* ------------------------------------------------------------------ */

class COpenAIClient : public CLlmClient
{
	public:
		COpenAIClient(const llm_config &Config) : CLlmClient(Config) {}

		void addUser(const std::string &text);
		void addToolResult(const llm_tool_call &call, const std::string &result, bool is_error);
		bool request(llm_reply &reply, const volatile bool *cancel);
};

void COpenAIClient::addUser(const std::string &text)
{
	turn_start = messages.size();
	Json::Value message;
	message["role"] = "user";
	message["content"] = text;
	messages.append(message);
}

void COpenAIClient::addToolResult(const llm_tool_call &call, const std::string &result, bool is_error)
{
	Json::Value message;
	message["role"] = "tool";
	message["tool_call_id"] = call.id;
	message["content"] = is_error ? "Error: " + result : result;
	messages.append(message);
}

bool COpenAIClient::request(llm_reply &reply, const volatile bool *cancel)
{
	reply = llm_reply();
	if (config.url.empty())
	{
		reply.error = "no server";
		return false;
	}

	Json::Value body;
	body["model"] = config.model;
	Json::Value list(Json::arrayValue);
	if (!system.empty())
	{
		Json::Value message;
		message["role"] = "system";
		message["content"] = system;
		list.append(message);
	}
	for (Json::ArrayIndex i = 0; i < messages.size(); i++)
		list.append(messages[i]);
	body["messages"] = list;
	if (!tools.empty())
	{
		Json::Value functions(Json::arrayValue);
		for (size_t i = 0; i < tools.size(); i++)
		{
			Json::Value tool;
			tool["type"] = "function";
			tool["function"]["name"] = tools[i].name;
			tool["function"]["description"] = tools[i].description;
			tool["function"]["parameters"] = tools[i].schema;
			functions.append(tool);
		}
		body["tools"] = functions;
	}

	std::vector<std::string> headers;
	headers.push_back("Content-Type: application/json");
	if (!config.key.empty())
		headers.push_back("Authorization: Bearer " + config.key);

	std::string url = config.url;
	while (!url.empty() && url[url.size() - 1] == '/')
		url.erase(url.size() - 1);
	url += "/chat/completions";

	http_reply http;
	if (!http_post(url, headers, json_compact(body), http, cancel))
	{
		reply.error = http.error;
		return false;
	}
	if (http.status != 200)
	{
		reply.error = http_error(http);
		return false;
	}

	Json::Value root;
	if (!json_parse(http.body, root) || !root["choices"].isArray() || root["choices"].empty() ||
	    !root["choices"][0]["message"].isObject())
	{
		reply.error = "unexpected reply";
		return false;
	}
	const Json::Value &message = root["choices"][0]["message"];
	if (message["content"].isString())
		reply.text = message["content"].asString();
	const Json::Value &calls = message["tool_calls"];
	for (Json::ArrayIndex i = 0; calls.isArray() && i < calls.size(); i++)
	{
		llm_tool_call call;
		call.id = calls[i]["id"].asString();
		call.name = calls[i]["function"]["name"].asString();
		const Json::Value &arguments = calls[i]["function"]["arguments"];
		if (arguments.isString())
		{
			if (!json_parse(arguments.asString(), call.input))
				call.input = Json::Value(Json::objectValue);
		}
		else
			call.input = arguments;
		reply.calls.push_back(call);
	}
	if (root["choices"][0]["finish_reason"].asString() == "length" && !reply.calls.empty())
	{
		reply.calls.clear();
		reply.error = "the reply was cut off";
		return false;
	}
	messages.append(message);
	return true;
}

/* ------------------------------------------------------------------ */

CLlmClient *CLlmClient::create(const llm_config &config)
{
	if (config.backend == llm_config::BACKEND_OPENAI)
		return new COpenAIClient(config);
	return new CClaudeClient(config);
}
