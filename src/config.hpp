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

	/* Off means share the main stream's encoder, which costs no CPU but sends
	 * everyone the same bitrate. On makes a second encoder for this
	 * destination: the fix for an upload that cannot carry two full streams. */
	bool own_encoder = false;
	/* kbps. Only read when own_encoder is set. */
	int video_bitrate = 2500;
	int audio_bitrate = 160;
	/* Empty means whatever OBS is already using for the main stream. */
	std::string encoder_id;

	/* Renders the program a second time into a 9:16 frame, for the platforms
	 * that only take portrait. It has to encode separately whatever else is set,
	 * because the main stream's encoder is tied to the main canvas. */
	bool vertical = false;
	int vertical_width = 1080;
	int vertical_height = 1920;
	/* Fill the frame and lose the sides, or fit the whole thing between bars. */
	bool vertical_crop = true;

	bool usable() const { return enabled && !url.empty() && !key.empty(); }
};

struct Config {
	std::vector<Destination> destinations;
	/* PlasmaStream account token. Optional: the plugin works without one. */
	std::string token;
	bool sync_on_launch = true;

	/* Applied to every output. OBS's own default is 20 retries, 10s apart. */
	int reconnect_retries = 20;
	int reconnect_delay_sec = 5;
};

Config &config();

/* Both run during module load, so neither throws. A missing or unreadable file
 * just means defaults. */
void load_config();
void save_config();

std::string config_path();

} // namespace plasmastream
