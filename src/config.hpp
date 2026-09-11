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

/* Where the wide programme sits inside the tall frame, for one programme scene.
 *
 * A fraction of the canvas rather than pixels, so changing the vertical size
 * from 1080x1920 to 720x1280 does not throw the arrangement away.
 *
 * Per scene because that is how people actually stream: a full-bleed crop is
 * right for gameplay and wrong for a talking head, and nobody wants to redo it
 * every time they switch. A scene with no entry gets the default. */
struct Framing {
	/* The programme scene this describes. Empty is the fallback for any scene
	 * nobody has arranged yet. */
	std::string scene;

	double x = 0.0;
	double y = 0.0;
	double width = 1.0;
	double height = 1.0;

	/* Fill the box and lose what overflows, or fit inside it and accept bars. */
	bool crop = true;
};

/* One of your own sources, placed on the vertical canvas.
 *
 * Named rather than owned: these are the sources already in your scene
 * collection, your camera and your chat overlay, borrowed onto the tall frame so
 * they can sit where a phone wants them instead of where a monitor does. OBS
 * lets a source live in several scenes at once, and this is that.
 *
 * The transform is in canvas pixels, not fractions, because it is edited through
 * OBS's own transform dialog and that dialog speaks pixels. Scaled if the canvas
 * size changes, which is the only time it would otherwise drift off the frame. */
struct VerticalSource {
	std::string name;

	double x = 0.0;
	double y = 0.0;
	double width = 0.0;
	double height = 0.0;

	/* Mirrors obs_bounds_type. Kept as an int so config.hpp does not have to
	 * pull in all of libobs for one enum. */
	int bounds_type = 0;
	bool visible = true;
};

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

	/* Encode from the vertical canvas rather than the main one, for the
	 * platforms that only take portrait. It has to encode separately whatever
	 * else is set, because the main stream's encoder is tied to the main canvas.
	 *
	 * The SHAPE of that frame is not here: there is one vertical canvas, so its
	 * size and framing belong to the config rather than to each destination
	 * that happens to point at it. */
	bool vertical = false;

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

	/* The vertical canvas. One of it, so one size. */
	int vertical_width = 1080;
	int vertical_height = 1920;
	/* Fill the frame and lose the sides, or fit the whole thing between bars.
	 * The default for any scene with no framing of its own. */
	bool vertical_crop = true;

	/* Arranged scenes, in no particular order. Looked up by name. */
	std::vector<Framing> framings;

	/* Layered over the programme block, bottom of the list first. */
	std::vector<VerticalSource> vertical_sources;

	/* Whether the Canvases dock has ever been shown. OBS starts every dock
	 * hidden and most people never find the Docks menu, so it gets opened once
	 * and then never touched again. */
	bool canvases_introduced = false;
};

/* The framing for a scene, or a default one covering the whole frame. Never
 * fails: a scene nobody has touched is a scene framed the old way. */
Framing framing_for(const std::string &scene);

/* Replaces the entry for framing.scene, or adds one. Does not save. */
void set_framing(const Framing &framing);

Config &config();

/* Both run during module load, so neither throws. A missing or unreadable file
 * just means defaults. */
void load_config();
void save_config();

std::string config_path();

} // namespace plasmastream
