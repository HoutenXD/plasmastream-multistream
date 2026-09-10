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

/**
 * One HTTP GET, and why it went wrong if it did.
 *
 * `status` is 0 when the request never reached a server at all, which is a
 * different problem from a server answering 404 and has to be told apart.
 */
struct HttpResponse {
	long status = 0;
	std::string body;
	/** curl's own description, when there is one. Shown to the streamer, because
	 *  "could not reach PlasmaStream" alone is not something anybody can act on. */
	std::string error;

	bool ok() const { return status >= 200 && status < 300; }
};

/**
 * Fetch a URL. Blocking, so call it off the UI thread.
 *
 * ## Why curl and not Qt
 *
 * The first version used QNetworkAccessManager, since Qt was already linked, and
 * every HTTPS request failed before leaving the process. Qt does TLS through a
 * backend plugin loaded from a `tls` directory, and OBS ships `platforms`,
 * `styles` and `imageformats` but not that one. Qt Network inside OBS can do
 * plain HTTP and nothing else.
 *
 * libcurl has no such problem, and it is not a new dependency: OBS ships
 * libcurl.dll and uses it itself, and obs-deps provides the headers we build
 * against. Same library, already loaded in the process.
 */
HttpResponse http_get(const std::string &url);

/** Initialise curl once, during module load, before any thread uses it. */
void http_init();

} // namespace plasmastream
