/*
	Model Context Protocol client: the tools of a server, over the
	Streamable HTTP transport

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

#include "mcp_client.h"
#include "http_request.h"
#include "llm_client.h"

#include <stdio.h>

#define MCP_PROTOCOL_VERSION	"2025-06-18"
#define MCP_SESSION_HEADER	"Mcp-Session-Id"

CMcpClient::CMcpClient(const std::string &Name, const std::string &Url, const std::string &Token)
{
	name = Name;
	url = Url;
	token = Token;
	next_id = 1;
	connected = false;
}

CMcpClient::~CMcpClient()
{
	disconnect();
}

std::vector<std::string> CMcpClient::headers()
{
	std::vector<std::string> list;
	list.push_back("Content-Type: application/json");
	list.push_back("Accept: application/json, text/event-stream");
	if (!version.empty())
		list.push_back("MCP-Protocol-Version: " + version);
	if (!session.empty())
		list.push_back(MCP_SESSION_HEADER ": " + session);
	if (!token.empty())
		list.push_back("Authorization: Bearer " + token);
	return list;
}

static bool is_response(const std::string &data, int id, Json::Value &response)
{
	Json::Value message;
	if (data.empty() || !json_parse(data, message) || !message.isObject() || !message["id"].isInt() ||
	    message["id"].asInt() != id || !(message.isMember("result") || message.isMember("error")))
		return false;
	response = message;
	return true;
}

/* the message with our id out of an event stream: the data lines of an event make one message */
static bool find_response(const std::string &stream, int id, Json::Value &response)
{
	std::string data;
	size_t pos = 0;
	while (pos < stream.size())
	{
		size_t end = stream.find('\n', pos);
		if (end == std::string::npos)
			end = stream.size();
		std::string line = stream.substr(pos, end - pos);
		pos = end + 1;
		if (!line.empty() && line[line.size() - 1] == '\r')
			line.erase(line.size() - 1);

		if (line.empty())
		{
			if (is_response(data, id, response))
				return true;
			data.clear();
		}
		else if (!line.compare(0, 5, "data:"))
		{
			if (!data.empty())
				data += '\n';
			data += line.substr((line.size() > 5 && line[5] == ' ') ? 6 : 5);
		}
	}
	return is_response(data, id, response);
}

/* a request when result is set, a notification otherwise */
bool CMcpClient::post(const Json::Value &message, Json::Value *result, std::string &error,
		      const volatile bool *cancel)
{
	http_reply http;
	if (!http_post(url, headers(), json_compact(message), http, cancel, MCP_SESSION_HEADER, 120))
	{
		error = http.error;
		return false;
	}
	if (http.status < 200 || http.status > 299)
	{
		char status[32];
		snprintf(status, sizeof(status), "HTTP %ld", http.status);
		error = status;
		/* the server has forgotten the session */
		if (http.status == 404 && !session.empty())
		{
			session.clear();
			connected = false;
		}
		return false;
	}
	if (!http.header.empty())
		session = http.header;
	if (!result)
		return true;

	Json::Value response;
	bool found;
	if (!http.content_type.compare(0, 17, "text/event-stream"))
		found = find_response(http.body, message["id"].asInt(), response);
	else
		found = json_parse(http.body, response) && response.isObject();
	if (!found)
	{
		error = "no response";
		return false;
	}
	if (response.isMember("error"))
	{
		error = response["error"]["message"].isString() ? response["error"]["message"].asString() : "error";
		return false;
	}
	*result = response["result"];
	return true;
}

bool CMcpClient::rpc(const std::string &method, const Json::Value &params, Json::Value &result,
		     std::string &error, const volatile bool *cancel)
{
	Json::Value message;
	message["jsonrpc"] = "2.0";
	message["id"] = next_id++;
	message["method"] = method;
	if (!params.isNull())
		message["params"] = params;
	return post(message, &result, error, cancel);
}

void CMcpClient::notify(const std::string &method, const volatile bool *cancel)
{
	std::string error;
	Json::Value message;
	message["jsonrpc"] = "2.0";
	message["method"] = method;
	post(message, NULL, error, cancel);
}

bool CMcpClient::connect(std::string &error, const volatile bool *cancel)
{
	if (connected)
		return true;
	session.clear();
	version.clear();

	Json::Value params, result;
	params["protocolVersion"] = MCP_PROTOCOL_VERSION;
	params["capabilities"] = Json::Value(Json::objectValue);
	params["clientInfo"]["name"] = "neutrino";
	params["clientInfo"]["version"] = PACKAGE_VERSION;
	if (!rpc("initialize", params, result, error, cancel))
		return false;
	/* the server answers with the version it wants to speak */
	version = result["protocolVersion"].isString() ? result["protocolVersion"].asString() : MCP_PROTOCOL_VERSION;
	notify("notifications/initialized", cancel);
	connected = true;
	return true;
}

void CMcpClient::disconnect()
{
	if (connected && !session.empty())
		http_delete(url, headers());
	connected = false;
	session.clear();
}

bool CMcpClient::listTools(std::vector<mcp_tool> &tools, std::string &error, const volatile bool *cancel)
{
	std::string cursor;
	tools.clear();
	if (!connect(error, cancel))
		return false;
	/* a server may hand its list out in pages */
	for (int page = 0; page < 20; page++)
	{
		Json::Value params, result;
		if (!cursor.empty())
			params["cursor"] = cursor;
		if (!rpc("tools/list", params, result, error, cancel))
			return false;
		const Json::Value &list = result["tools"];
		for (Json::ArrayIndex i = 0; list.isArray() && i < list.size(); i++)
		{
			mcp_tool tool;
			tool.name = list[i]["name"].asString();
			tool.description = list[i]["description"].isString() ? list[i]["description"].asString() : "";
			tool.schema = list[i]["inputSchema"];
			if (!tool.schema.isObject())
			{
				tool.schema = Json::Value(Json::objectValue);
				tool.schema["type"] = "object";
			}
			if (!tool.name.empty())
				tools.push_back(tool);
		}
		if (!result["nextCursor"].isString() || result["nextCursor"].asString().empty())
			break;
		cursor = result["nextCursor"].asString();
	}
	return true;
}

bool CMcpClient::callTool(const std::string &tool, const Json::Value &arguments, std::string &result,
			  bool &is_error, std::string &error, const volatile bool *cancel)
{
	Json::Value params, reply;
	params["name"] = tool;
	params["arguments"] = arguments.isObject() ? arguments : Json::Value(Json::objectValue);

	bool had_session = connected;
	if (!connect(error, cancel))
		return false;
	if (!rpc("tools/call", params, reply, error, cancel))
	{
		/* once more with a new session when the old one was gone */
		if (!had_session || connected || !connect(error, cancel) ||
		    !rpc("tools/call", params, reply, error, cancel))
			return false;
	}

	result.clear();
	const Json::Value &content = reply["content"];
	for (Json::ArrayIndex i = 0; content.isArray() && i < content.size(); i++)
	{
		std::string type = content[i]["type"].asString();
		if (!result.empty())
			result += '\n';
		if (type == "text")
			result += content[i]["text"].asString();
		else if (type == "resource" && content[i]["resource"]["text"].isString())
			result += content[i]["resource"]["text"].asString();
		else
			result += "[" + type + "]";
	}
	if (result.empty() && reply.isMember("structuredContent"))
		result = json_compact(reply["structuredContent"]);
	is_error = reply["isError"].isBool() && reply["isError"].asBool();
	return true;
}
