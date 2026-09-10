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
#include <vector>

namespace plasmastream {

/**
 * Somewhere the stream is sent, besides whatever OBS is already sending to.
 *
 * `key` is the one field that matters for safety. It is the credential that lets
 * anybody broadcast as this person, it is stored only on this machine, and it is
 * deliberately not something the PlasmaStream website can hold or hand back. See
 * fetch_destinations() for what the website does supply.
 */
struct Destination {
	std::string id;
	std::string name;
	std::string platform;
	std::string url;
	std::string key;
	bool enabled = true;

	/** Whether this is worth attempting. A destination missing either half of
	 *  its address cannot connect, and starting it only produces an error in
	 *  the log at the moment the streamer is least able to read one. */
	bool usable() const { return enabled && !url.empty() && !key.empty(); }
};

/**
 * Everything the plugin remembers between runs.
 *
 * Kept in the plugin's own config directory rather than in OBS's global settings
 * store, for two reasons. The API for that store moved between OBS versions, and
 * a file we own can be backed up, inspected, and deleted by somebody who wants
 * their stream keys gone without hunting through OBS's own configuration.
 */
struct Config {
	std::vector<Destination> destinations;

	/** The PlasmaStream account token, if they have one. Optional by design:
	 *  the plugin is fully usable with this empty forever. */
	std::string token;

	/** Whether to ask PlasmaStream for destinations when OBS starts. */
	bool sync_on_launch = true;
};

/** The one instance, loaded at module start. */
Config &config();

/** Read the config file. Missing or unreadable means defaults, never a crash:
 *  this runs during module load, where throwing would take OBS with it. */
void load_config();

/** Write it back. Called after every edit rather than at shutdown, because OBS
 *  crashing should not cost somebody the stream keys they just typed. */
void save_config();

/** Where the config file lives, for the UI to show and for support to ask about. */
std::string config_path();

} // namespace plasmastream
