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

#pragma once

#include <string>

namespace plasmastream {

struct HttpResponse {
	/* Zero when the request never reached a server, which is not the same as a
	 * server answering 404. */
	long status = 0;
	std::string body;
	std::string error;

	bool ok() const { return status >= 200 && status < 300; }
};

/* Blocking, so keep it off the UI thread.
 *
 * curl rather than Qt Network: Qt loads its TLS backend from a "tls" plugin
 * directory that OBS does not ship, so every https request fails at handshake.
 * OBS ships and uses libcurl itself. */
HttpResponse http_get(const std::string &url);

/* The same, as a POST with a JSON body. Same threading rule. The timeout is the
 * caller's, because a voice command nobody hears back about in six seconds is
 * better dropped than waited on. */
HttpResponse http_post_json(const std::string &url, const std::string &json, long timeout_sec = 15);

/* A PlasmaStream address, e.g. plasmastream_url("/api/plugin/abc").
 *
 * The site unless the PLASMASTREAM_URL environment variable says otherwise, which
 * is how a development build is pointed at a dashboard running on the same
 * machine. A streamer never sets it. */
std::string plasmastream_url(const std::string &path);

/* Call once from obs_module_load, before anything spawns a thread. */
void http_init();

} // namespace plasmastream
