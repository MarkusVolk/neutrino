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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "http_request.h"

#include <string.h>
#include <strings.h>
#include <curl/curl.h>

struct http_transfer
{
	http_reply *reply;
	const volatile bool *cancel;
	const char *want_header;
};

static size_t write_cb(char *data, size_t size, size_t nmemb, void *user)
{
	http_transfer *t = (http_transfer *)user;
	t->reply->body.append(data, size * nmemb);
	return size * nmemb;
}

static size_t header_cb(char *data, size_t size, size_t nmemb, void *user)
{
	http_transfer *t = (http_transfer *)user;
	size_t len = size * nmemb;
	size_t name = t->want_header ? strlen(t->want_header) : 0;
	if (name && len > name && data[name] == ':' && !strncasecmp(data, t->want_header, name))
	{
		std::string value(data + name + 1, len - name - 1);
		size_t first = value.find_first_not_of(" \t");
		size_t last = value.find_last_not_of(" \t\r\n");
		t->reply->header = first == std::string::npos ? "" : value.substr(first, last - first + 1);
	}
	return len;
}

static int progress_cb(void *user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
	http_transfer *t = (http_transfer *)user;
	return (t->cancel && *t->cancel) ? 1 : 0;
}

static bool perform(CURL *curl, const std::string &url, const std::vector<std::string> &headers,
		    http_reply &reply, const volatile bool *cancel, const char *want_header, long timeout)
{
	char error[CURL_ERROR_SIZE] = "";
	http_transfer transfer = { &reply, cancel, want_header };
	struct curl_slist *list = NULL;
	for (size_t i = 0; i < headers.size(); i++)
		list = curl_slist_append(list, headers[i].c_str());

	reply = http_reply();
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, &transfer);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "neutrino/" PACKAGE_VERSION);

	CURLcode res = curl_easy_perform(curl);
	if (res == CURLE_OK)
	{
		char *type = NULL;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &reply.status);
		if (curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &type) == CURLE_OK && type)
			reply.content_type = type;
	}
	else if (res == CURLE_ABORTED_BY_CALLBACK)
		reply.error = "cancelled";
	else
		reply.error = error[0] ? error : curl_easy_strerror(res);
	curl_slist_free_all(list);
	return res == CURLE_OK;
}

bool http_post(const std::string &url, const std::vector<std::string> &headers, const std::string &body,
	       http_reply &reply, const volatile bool *cancel, const char *want_header, long timeout)
{
	CURL *curl = curl_easy_init();
	if (!curl)
	{
		reply.error = "curl_easy_init failed";
		return false;
	}
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body.size());
	bool ok = perform(curl, url, headers, reply, cancel, want_header, timeout);
	curl_easy_cleanup(curl);
	return ok;
}

bool http_post_form(const std::string &url, const std::vector<std::string> &headers,
		    const std::vector<http_form_field> &fields, http_reply &reply, const volatile bool *cancel,
		    long timeout)
{
	CURL *curl = curl_easy_init();
	if (!curl)
	{
		reply.error = "curl_easy_init failed";
		return false;
	}
	curl_mime *mime = curl_mime_init(curl);
	for (size_t i = 0; i < fields.size(); i++)
	{
		curl_mimepart *part = curl_mime_addpart(mime);
		curl_mime_name(part, fields[i].name.c_str());
		curl_mime_data(part, fields[i].value.data(), fields[i].value.size());
		if (!fields[i].filename.empty())
		{
			curl_mime_filename(part, fields[i].filename.c_str());
			curl_mime_type(part, fields[i].content_type.c_str());
		}
	}
	curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
	bool ok = perform(curl, url, headers, reply, cancel, NULL, timeout);
	curl_easy_cleanup(curl);
	curl_mime_free(mime);
	return ok;
}

bool http_delete(const std::string &url, const std::vector<std::string> &headers)
{
	CURL *curl = curl_easy_init();
	if (!curl)
		return false;
	http_reply reply;
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
	bool ok = perform(curl, url, headers, reply, NULL, NULL, 10);
	curl_easy_cleanup(curl);
	return ok;
}
