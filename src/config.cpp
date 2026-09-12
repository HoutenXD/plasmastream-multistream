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

obs_data_t *destination_to_data(const Destination &destination)
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "id", destination.id.c_str());
	obs_data_set_string(data, "name", destination.name.c_str());
	obs_data_set_string(data, "platform", destination.platform.c_str());
	obs_data_set_string(data, "url", destination.url.c_str());
	obs_data_set_string(data, "key", destination.key.c_str());
	obs_data_set_bool(data, "enabled", destination.enabled);
	obs_data_set_bool(data, "own_encoder", destination.own_encoder);
	obs_data_set_int(data, "video_bitrate", destination.video_bitrate);
	obs_data_set_int(data, "audio_bitrate", destination.audio_bitrate);
	obs_data_set_string(data, "encoder_id", destination.encoder_id.c_str());
	obs_data_set_bool(data, "vertical", destination.vertical);
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

	/* Absent in configs written before this field existed; those destinations
	 * were on. */
	obs_data_set_default_bool(data, "enabled", true);
	destination.enabled = obs_data_get_bool(data, "enabled");

	obs_data_set_default_int(data, "video_bitrate", 2500);
	obs_data_set_default_int(data, "audio_bitrate", 160);

	destination.own_encoder = obs_data_get_bool(data, "own_encoder");
	destination.video_bitrate = static_cast<int>(obs_data_get_int(data, "video_bitrate"));
	destination.audio_bitrate = static_cast<int>(obs_data_get_int(data, "audio_bitrate"));
	destination.encoder_id = obs_data_get_string(data, "encoder_id");
	destination.vertical = obs_data_get_bool(data, "vertical");

	/* Carried over from when the frame's shape lived on each destination rather
	 * than on the canvas they now share. The first one that has it wins, which
	 * is the only sensible answer when two disagree and there is now only one
	 * canvas to satisfy. Dropped from the file on the next save. */
	if (destination.vertical && obs_data_has_user_value(data, "vertical_width")) {
		g_config.vertical_width = static_cast<int>(obs_data_get_int(data, "vertical_width"));
		g_config.vertical_height =
			static_cast<int>(obs_data_get_int(data, "vertical_height"));
		g_config.vertical_crop = obs_data_get_bool(data, "vertical_crop");
	}

	return destination;
}

} // namespace

Config &config()
{
	return g_config;
}

Framing framing_for(const std::string &scene)
{
	for (const Framing &framing : g_config.framings) {
		if (framing.scene == scene) {
			return framing;
		}
	}

	/* Whole frame, cropped or fitted per the old single setting, which is what
	 * every scene did before framing existed. */
	Framing fallback;
	fallback.scene = scene;
	fallback.crop = g_config.vertical_crop;
	return fallback;
}

void set_framing(const Framing &framing)
{
	for (Framing &existing : g_config.framings) {
		if (existing.scene == framing.scene) {
			existing = framing;
			return;
		}
	}

	g_config.framings.push_back(framing);
}

std::string config_path()
{
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

	/* No file yet: first run. */
	if (!data) {
		return;
	}

	g_config.token = obs_data_get_string(data, "token");

	obs_data_set_default_bool(data, "sync_on_launch", true);
	g_config.sync_on_launch = obs_data_get_bool(data, "sync_on_launch");

	obs_data_set_default_int(data, "reconnect_retries", 20);
	obs_data_set_default_int(data, "reconnect_delay_sec", 5);
	g_config.reconnect_retries = static_cast<int>(obs_data_get_int(data, "reconnect_retries"));
	g_config.reconnect_delay_sec = static_cast<int>(obs_data_get_int(data, "reconnect_delay_sec"));

	obs_data_set_default_int(data, "vertical_width", 1080);
	obs_data_set_default_int(data, "vertical_height", 1920);
	obs_data_set_default_bool(data, "vertical_crop", true);
	g_config.vertical_width = static_cast<int>(obs_data_get_int(data, "vertical_width"));
	g_config.vertical_height = static_cast<int>(obs_data_get_int(data, "vertical_height"));
	g_config.vertical_crop = obs_data_get_bool(data, "vertical_crop");

	g_config.framings.clear();
	obs_data_array_t *framings = obs_data_get_array(data, "framings");

	if (framings) {
		const size_t count = obs_data_array_count(framings);

		for (size_t i = 0; i < count; i++) {
			obs_data_t *entry = obs_data_array_item(framings, i);

			if (!entry) {
				continue;
			}

			/* A width or height of zero would put the programme in a box
			 * with no pixels in it, which reads as the preview being
			 * broken rather than as a bad setting. */
			obs_data_set_default_double(entry, "width", 1.0);
			obs_data_set_default_double(entry, "height", 1.0);

			Framing framing;
			framing.scene = obs_data_get_string(entry, "scene");
			framing.x = obs_data_get_double(entry, "x");
			framing.y = obs_data_get_double(entry, "y");
			framing.width = obs_data_get_double(entry, "width");
			framing.height = obs_data_get_double(entry, "height");
			framing.crop = obs_data_get_bool(entry, "crop");

			if (framing.width > 0.0 && framing.height > 0.0) {
				g_config.framings.push_back(framing);
			}

			obs_data_release(entry);
		}

		obs_data_array_release(framings);
	}

	g_config.canvases_introduced = obs_data_get_bool(data, "canvases_introduced");

	g_config.scene_recording = SceneRecording();
	obs_data_t *recording = obs_data_get_obj(data, "scene_recording");

	if (recording) {
		obs_data_set_default_bool(recording, "with_stream", true);
		obs_data_set_default_int(recording, "video_bitrate", 8000);
		obs_data_set_default_string(recording, "format", "mkv");
		obs_data_set_default_int(recording, "audio_track", 1);

		g_config.scene_recording.enabled = obs_data_get_bool(recording, "enabled");
		g_config.scene_recording.scene = obs_data_get_string(recording, "scene");
		g_config.scene_recording.folder = obs_data_get_string(recording, "folder");
		g_config.scene_recording.with_stream = obs_data_get_bool(recording, "with_stream");
		g_config.scene_recording.video_bitrate =
			static_cast<int>(obs_data_get_int(recording, "video_bitrate"));
		g_config.scene_recording.encoder_id = obs_data_get_string(recording, "encoder_id");
		g_config.scene_recording.format = obs_data_get_string(recording, "format");
		g_config.scene_recording.audio_track =
			static_cast<int>(obs_data_get_int(recording, "audio_track"));

		/* A track outside OBS's six would be asked of the audio encoder and
		 * quietly produce a recording with no sound. */
		if (g_config.scene_recording.audio_track < 1 ||
		    g_config.scene_recording.audio_track > 6) {
			g_config.scene_recording.audio_track = 1;
		}

		obs_data_release(recording);
	}

	g_config.vertical_sources.clear();
	obs_data_array_t *sources = obs_data_get_array(data, "vertical_sources");

	if (sources) {
		const size_t count = obs_data_array_count(sources);

		for (size_t i = 0; i < count; i++) {
			obs_data_t *entry = obs_data_array_item(sources, i);

			if (!entry) {
				continue;
			}

			obs_data_set_default_bool(entry, "visible", true);

			VerticalSource source;
			source.name = obs_data_get_string(entry, "name");
			source.x = obs_data_get_double(entry, "x");
			source.y = obs_data_get_double(entry, "y");
			source.width = obs_data_get_double(entry, "width");
			source.height = obs_data_get_double(entry, "height");
			source.bounds_type = static_cast<int>(obs_data_get_int(entry, "bounds_type"));
			source.visible = obs_data_get_bool(entry, "visible");

			/* A nameless entry cannot be looked up, so it would sit in the
			 * list doing nothing but confusing whoever reads it. */
			if (!source.name.empty()) {
				g_config.vertical_sources.push_back(source);
			}

			obs_data_release(entry);
		}

		obs_data_array_release(sources);
	}

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

	char *dir = obs_module_config_path("");

	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}

	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "token", g_config.token.c_str());
	obs_data_set_bool(data, "sync_on_launch", g_config.sync_on_launch);
	obs_data_set_int(data, "reconnect_retries", g_config.reconnect_retries);
	obs_data_set_int(data, "reconnect_delay_sec", g_config.reconnect_delay_sec);
	obs_data_set_int(data, "vertical_width", g_config.vertical_width);
	obs_data_set_int(data, "vertical_height", g_config.vertical_height);
	obs_data_set_bool(data, "vertical_crop", g_config.vertical_crop);

	obs_data_array_t *framings = obs_data_array_create();

	for (const Framing &framing : g_config.framings) {
		obs_data_t *entry = obs_data_create();
		obs_data_set_string(entry, "scene", framing.scene.c_str());
		obs_data_set_double(entry, "x", framing.x);
		obs_data_set_double(entry, "y", framing.y);
		obs_data_set_double(entry, "width", framing.width);
		obs_data_set_double(entry, "height", framing.height);
		obs_data_set_bool(entry, "crop", framing.crop);
		obs_data_array_push_back(framings, entry);
		obs_data_release(entry);
	}

	obs_data_set_array(data, "framings", framings);
	obs_data_array_release(framings);

	obs_data_array_t *sources = obs_data_array_create();

	for (const VerticalSource &source : g_config.vertical_sources) {
		obs_data_t *entry = obs_data_create();
		obs_data_set_string(entry, "name", source.name.c_str());
		obs_data_set_double(entry, "x", source.x);
		obs_data_set_double(entry, "y", source.y);
		obs_data_set_double(entry, "width", source.width);
		obs_data_set_double(entry, "height", source.height);
		obs_data_set_int(entry, "bounds_type", source.bounds_type);
		obs_data_set_bool(entry, "visible", source.visible);
		obs_data_array_push_back(sources, entry);
		obs_data_release(entry);
	}

	obs_data_set_array(data, "vertical_sources", sources);
	obs_data_array_release(sources);

	obs_data_set_bool(data, "canvases_introduced", g_config.canvases_introduced);

	obs_data_t *recording = obs_data_create();
	obs_data_set_bool(recording, "enabled", g_config.scene_recording.enabled);
	obs_data_set_string(recording, "scene", g_config.scene_recording.scene.c_str());
	obs_data_set_string(recording, "folder", g_config.scene_recording.folder.c_str());
	obs_data_set_bool(recording, "with_stream", g_config.scene_recording.with_stream);
	obs_data_set_int(recording, "video_bitrate", g_config.scene_recording.video_bitrate);
	obs_data_set_string(recording, "encoder_id", g_config.scene_recording.encoder_id.c_str());
	obs_data_set_string(recording, "format", g_config.scene_recording.format.c_str());
	obs_data_set_int(recording, "audio_track", g_config.scene_recording.audio_track);
	obs_data_set_obj(data, "scene_recording", recording);
	obs_data_release(recording);

	obs_data_array_t *array = obs_data_array_create();

	for (const Destination &destination : g_config.destinations) {
		obs_data_t *item = destination_to_data(destination);
		obs_data_array_push_back(array, item);
		obs_data_release(item);
	}

	obs_data_set_array(data, "destinations", array);
	obs_data_array_release(array);

	/* Temp file and rename, so a crash mid-write cannot leave a config holding
	 * no stream keys. */
	obs_data_save_json_safe(data, path.c_str(), "tmp", "bak");
	obs_data_release(data);
}

} // namespace plasmastream
