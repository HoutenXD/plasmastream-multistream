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

struct Destination {
	std::string id;
	std::string name;
	std::string platform;
	std::string url;
	/* Never leaves this machine. The website has no field for one. */
	std::string key;
	bool enabled = true;

	bool usable() const { return enabled && !url.empty() && !key.empty(); }
};

struct Config {
	std::vector<Destination> destinations;
	/* PlasmaStream account token. Optional: the plugin works without one. */
	std::string token;
	bool sync_on_launch = true;
};

Config &config();

/* Both run during module load, so neither throws. A missing or unreadable file
 * just means defaults. */
void load_config();
void save_config();

std::string config_path();

} // namespace plasmastream
