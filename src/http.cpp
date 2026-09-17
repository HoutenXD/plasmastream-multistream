/*
PlasmaStream Multistream
Copyright (C) 2026 PlasmaStream

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "http.hpp"

#include <curl/curl.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <cstdlib>

namespace plasmastream {

namespace {

size_t collect(char *data, size_t size, size_t count, void *user)
{
	const size_t total = size * count;
	static_cast<std::string *>(user)->append(data, total);
	return total;
}

} // namespace

void http_init()
{
	/* Not thread-safe, hence the main thread before any worker exists. Never
	 * paired with curl_global_cleanup: OBS uses curl too, and tearing down its
	 * global state on module unload would pull it out from under OBS. */
	curl_global_init(CURL_GLOBAL_DEFAULT);
}

/* "PlasmaStream-Multistream-OBS/1.2.3": the version lets the site's operator
 * console count which plugin versions are still in use. */
static const std::string &user_agent()
{
	static const std::string agent = std::string("PlasmaStream-Multistream-OBS/") + PLUGIN_VERSION;
	return agent;
}

HttpResponse http_get(const std::string &url)
{
	HttpResponse response;

	CURL *curl = curl_easy_init();

	if (!curl) {
		response.error = "could not start a request";
		return response;
	}

	char error_buffer[CURL_ERROR_SIZE] = {0};

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent().c_str());

	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);

	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

	const CURLcode result = curl_easy_perform(curl);

	if (result == CURLE_OK) {
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
	} else {
		response.error = error_buffer[0] ? error_buffer : curl_easy_strerror(result);
		blog(LOG_WARNING, "[plasmastream] request to %s failed: %s", url.c_str(),
		     response.error.c_str());
	}

	curl_easy_cleanup(curl);

	return response;
}

HttpResponse http_post_json(const std::string &url, const std::string &json, long timeout_sec)
{
	HttpResponse response;

	CURL *curl = curl_easy_init();

	if (!curl) {
		response.error = "could not start a request";
		return response;
	}

	char error_buffer[CURL_ERROR_SIZE] = {0};
	struct curl_slist *headers = curl_slist_append(nullptr, "Content-Type: application/json");

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json.c_str());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(json.size()));
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent().c_str());

	curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout_sec < 8 ? timeout_sec : 8L);

	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

	const CURLcode result = curl_easy_perform(curl);

	if (result == CURLE_OK) {
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
	} else {
		response.error = error_buffer[0] ? error_buffer : curl_easy_strerror(result);
		// The path only: the token in it is a credential and logs get pasted.
		blog(LOG_WARNING, "[plasmastream] POST failed: %s", response.error.c_str());
	}

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	return response;
}

std::string plasmastream_url(const std::string &path)
{
	const char *override_url = std::getenv("PLASMASTREAM_URL");
	std::string base = override_url && *override_url ? override_url : "https://plasmastream.live";

	while (!base.empty() && base.back() == '/') {
		base.pop_back();
	}

	return base + path;
}

} // namespace plasmastream
