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

#ifndef __mcp_client_h__
#define __mcp_client_h__

#include <string>
#include <vector>
#include <json/json.h>

struct mcp_tool
{
	std::string name;
	std::string description;
	Json::Value schema;
};

class CMcpClient
{
	private:
		std::string name;
		std::string url;
		std::string token;
		std::string session;
		std::string version;
		int next_id;
		bool connected;

		std::vector<std::string> headers();
		bool post(const Json::Value &message, Json::Value *result, std::string &error,
			  const volatile bool *cancel);
		bool rpc(const std::string &method, const Json::Value &params, Json::Value &result,
			 std::string &error, const volatile bool *cancel);
		void notify(const std::string &method, const volatile bool *cancel);

	public:
		CMcpClient(const std::string &Name, const std::string &Url, const std::string &Token);
		~CMcpClient();

		const std::string &getName() { return name; }
		bool connect(std::string &error, const volatile bool *cancel);
		void disconnect();
		bool listTools(std::vector<mcp_tool> &tools, std::string &error, const volatile bool *cancel);
		/* false when the call did not get through; a tool that fails sets is_error */
		bool callTool(const std::string &tool, const Json::Value &arguments, std::string &result,
			      bool &is_error, std::string &error, const volatile bool *cancel);
};

#endif
