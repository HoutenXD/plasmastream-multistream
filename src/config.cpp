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

#include "config.hpp"

#include <obs-module.h>
#include <util/config-file.h>
#include <util/platform.h>

namespace plasmastream {

namespace {

Config g_config;

/** OBS's own JSON, rather than a third-party parser: it is already linked, it is
 *  what every other plugin uses, and it never throws. */
obs_data_t *destination_to_data(const Destination &destination)
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "id", destination.id.c_str());
	obs_data_set_string(data, "name", destination.name.c_str());
	obs_data_set_string(data, "platform", destination.platform.c_str());
	obs_data_set_string(data, "url", destination.url.c_str());
	obs_data_set_string(data, "key", destination.key.c_str());
	obs_data_set_bool(data, "enabled", destination.enabled);
	return data;
}

Destination destination_from_data(obs_data_t *data)
{
	Destination destination;
	destination.id = obs_data_get_string(data, "id");
	destination.name = obs_data_get_string(data, "name");
	destination.platform = obs_data_get_string(data, "platform");
	destination.url = obs_data_get_string(data, "url");
	destination.key = obs_data_get_string(data, "key");

	// Defaults to true when the field is absent, which matters for a config
	// written by an older build: a destination that quietly turned itself off
	// after an update would look like the plugin had broken.
	obs_data_set_default_bool(data, "enabled", true);
	destination.enabled = obs_data_get_bool(data, "enabled");

	return destination;
}

} // namespace

Config &config()
{
	return g_config;
}

std::string config_path()
{
	// obs_module_config_path gives the per-plugin directory inside the OBS
	// profile, which is per user and survives an OBS upgrade.
	char *path = obs_module_config_path("config.json");

	if (!path) {
		return {};
	}

	std::string result = path;
	bfree(path);
	return result;
}

void load_config()
{
	const std::string path = config_path();

	if (path.empty()) {
		return;
	}

	obs_data_t *data = obs_data_create_from_json_file_safe(path.c_str(), "bak");

	// No file yet, which is every first run. Defaults, no complaint.
	if (!data) {
		return;
	}

	g_config.token = obs_data_get_string(data, "token");

	obs_data_set_default_bool(data, "sync_on_launch", true);
	g_config.sync_on_launch = obs_data_get_bool(data, "sync_on_launch");

	g_config.destinations.clear();

	obs_data_array_t *array = obs_data_get_array(data, "destinations");

	if (array) {
		const size_t count = obs_data_array_count(array);

		for (size_t i = 0; i < count; i++) {
			obs_data_t *item = obs_data_array_item(array, i);

			if (item) {
				g_config.destinations.push_back(destination_from_data(item));
				obs_data_release(item);
			}
		}

		obs_data_array_release(array);
	}

	obs_data_release(data);
}

void save_config()
{
	const std::string path = config_path();

	if (path.empty()) {
		return;
	}

	// The directory does not exist until something creates it, and on a first
	// run nothing has. os_mkdirs is happy when it already exists.
	char *dir = obs_module_config_path("");

	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}

	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "token", g_config.token.c_str());
	obs_data_set_bool(data, "sync_on_launch", g_config.sync_on_launch);

	obs_data_array_t *array = obs_data_array_create();

	for (const Destination &destination : g_config.destinations) {
		obs_data_t *item = destination_to_data(destination);
		obs_data_array_push_back(array, item);
		obs_data_release(item);
	}

	obs_data_set_array(data, "destinations", array);
	obs_data_array_release(array);

	// _safe writes to a temp file and renames, so a crash mid-write leaves the
	// previous config intact rather than a half-written one holding no keys.
	obs_data_save_json_safe(data, path.c_str(), "tmp", "bak");
	obs_data_release(data);
}

} // namespace plasmastream
