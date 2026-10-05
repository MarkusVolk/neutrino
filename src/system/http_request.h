/*
	HTTP requests of the assistant: JSON and form posts that can be cancelled

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

#ifndef __http_request_h__
#define __http_request_h__

#include <string>
#include <vector>

struct http_reply
{
	long status;
	std::string body;
	std::string content_type;
	/* the value of the header asked for with want_header, if it came */
	std::string header;
	/* what went wrong when there is no status */
	std::string error;

	http_reply() : status(0) {}
};

struct http_form_field
{
	std::string name;
	std::string value;
	/* a file part when set: value is its content */
	std::string filename;
	std::string content_type;
};

/*
	Both return false when no reply came: the error says why. A reply with
	an HTTP error status is a reply. A request ends early once *cancel is set.
*/
bool http_post(const std::string &url, const std::vector<std::string> &headers, const std::string &body,
	       http_reply &reply, const volatile bool *cancel = NULL, const char *want_header = NULL, long timeout = 300);
bool http_post_form(const std::string &url, const std::vector<std::string> &headers,
		    const std::vector<http_form_field> &fields, http_reply &reply, const volatile bool *cancel = NULL,
		    long timeout = 120);
bool http_delete(const std::string &url, const std::vector<std::string> &headers);

#endif
