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

#include "scenerec.hpp"

#include "config.hpp"

#include <mutex>

#include <cstring>

#include <callback/calldata.h>
#include <callback/signal.h>

#include <graphics/vec2.h>
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>
#include <util/platform.h>

#if PLASMASTREAM_HAS_CANVAS

namespace plasmastream {

namespace {

struct Recording {
	obs_canvas_t *canvas = nullptr;
	obs_scene_t *scene = nullptr;
	obs_sceneitem_t *item = nullptr;

	obs_output_t *output = nullptr;
	obs_encoder_t *video = nullptr;
	obs_encoder_t *audio = nullptr;

	std::string file;

	/* The scene the user chose. Not the member above, which is this canvas's
	 * own wrapper scene holding it. */
	std::string scene_name;
	bool running = false;

	/* Wall clock, not frames: this is for a human reading a dock, and a
	 * dropped frame should not make the clock disagree with theirs. */
	uint64_t started_ns = 0;

	/* Raised by the output's own stop signal, on the output's own thread.
	 * Nothing is torn down there; see the handler. */
	bool stopped_itself = false;

	/* Why, when it stopped itself. Cleared on the next start, so a failure
	 * stays on screen until somebody does something about it. */
	std::string error;
};

Recording g_rec;
std::mutex g_mutex;

/* What the main stream encodes with, so this defaults to the same kind rather
 * than to x264 on a machine set up for NVENC. */
const char *main_encoder_kind()
{
	obs_output_t *stream = obs_frontend_get_streaming_output();

	if (!stream) {
		return "obs_x264";
	}

	obs_encoder_t *video = obs_output_get_video_encoder(stream);
	const char *id = video ? obs_encoder_get_id(video) : "obs_x264";

	obs_output_release(stream);
	return id;
}

void on_output_stopped(void *, calldata_t *data);

/* Assumes the lock. */
void release_all()
{
	if (g_rec.output) {
		/* Disconnected first, so stopping it does not come straight back
		 * through the handler and ask for a lock this thread holds. */
		signal_handler_disconnect(obs_output_get_signal_handler(g_rec.output), "stop",
					  on_output_stopped, nullptr);

		obs_output_stop(g_rec.output);
		obs_output_release(g_rec.output);
		g_rec.output = nullptr;
	}

	if (g_rec.video) {
		obs_encoder_release(g_rec.video);
		g_rec.video = nullptr;
	}

	if (g_rec.audio) {
		obs_encoder_release(g_rec.audio);
		g_rec.audio = nullptr;
	}

	if (g_rec.item) {
		obs_sceneitem_remove(g_rec.item);
		obs_sceneitem_release(g_rec.item);
		g_rec.item = nullptr;
	}

	if (g_rec.canvas) {
		obs_canvas_remove(g_rec.canvas);
		obs_canvas_release(g_rec.canvas);
		g_rec.canvas = nullptr;
		g_rec.scene = nullptr;
	}

	g_rec.running = false;
	g_rec.stopped_itself = false;
	g_rec.file.clear();
	g_rec.scene_name.clear();
	g_rec.started_ns = 0;
}

/* The output stopping on its own: a full disk, a folder that went away with the
 * drive it was on, an encoder that gave up.
 *
 * Raised on the output's thread from inside its own stop dispatch, so this only
 * writes down what happened. Releasing the output here would be releasing the
 * thing currently calling us, and disconnecting this handler would be editing
 * the list being walked. The teardown happens on the next call from outside,
 * which is what reap() below is for. OBS's own recording handler defers in
 * exactly the same way. */
void on_output_stopped(void *, calldata_t *data)
{
	const long long code = calldata_int(data, "code");

	std::lock_guard<std::mutex> lock(g_mutex);

	if (!g_rec.running) {
		return;
	}

	g_rec.running = false;
	g_rec.stopped_itself = true;

	if (code == OBS_OUTPUT_SUCCESS) {
		return;
	}

	const char *last = g_rec.output ? obs_output_get_last_error(g_rec.output) : nullptr;

	g_rec.error = last && *last ? last
				    : "The recording stopped on its own. Check there is room "
				      "on the drive it was writing to.";

	blog(LOG_WARNING, "[plasmastream] the scene recording stopped by itself: %s",
	     g_rec.error.c_str());
}

/* Clears up after a recording that stopped itself, keeping the reason. Assumes
 * the lock, and must not be called from the stop handler. */
void reap()
{
	if (!g_rec.stopped_itself) {
		return;
	}

	/* release_all deliberately leaves the reason alone: it is the one thing
	 * worth keeping from a recording that failed. */
	release_all();
}

/* A canvas the same shape as the stream, showing one scene of your choosing.
 * Assumes the lock. */
bool build_canvas(const std::string &scene_name)
{
	obs_source_t *scene = obs_get_source_by_name(scene_name.c_str());

	if (!scene) {
		blog(LOG_WARNING, "[plasmastream] no scene called '%s' to record",
		     scene_name.c_str());
		return false;
	}

	obs_video_info ovi = {};

	if (!obs_get_video_info(&ovi)) {
		obs_source_release(scene);
		return false;
	}

	/* Same size as the stream. This is a different COMPOSITION, not a
	 * different format, so anything else would be a second decision nobody
	 * asked to make.
	 *
	 * ACTIVATE is the whole feature: it makes the scene on this canvas run as
	 * though it were on programme, which is what lets you record the scene you
	 * are NOT showing. libobs balances it when the canvas goes away, which is
	 * why there is no matching call anywhere below.
	 *
	 * Deliberately without MIX_AUDIO. The audio this records is the main mix,
	 * asked for once by the encoder; adding this canvas to the mix as well
	 * would put anything it carries into the output twice. */
	g_rec.canvas = obs_canvas_create_private("PlasmaStream recording", &ovi,
						 ACTIVATE | SCENE_REF);

	if (!g_rec.canvas) {
		obs_source_release(scene);
		return false;
	}

	g_rec.scene = obs_canvas_scene_create(g_rec.canvas, "PlasmaStream recording");

	if (!g_rec.scene) {
		obs_source_release(scene);
		obs_canvas_remove(g_rec.canvas);
		obs_canvas_release(g_rec.canvas);
		g_rec.canvas = nullptr;
		return false;
	}

	/* The scene sits on this canvas as well as wherever else it appears; OBS
	 * has always let a source live in more than one scene, and this is that. */
	obs_sceneitem_t *item = obs_scene_add(g_rec.scene, scene);

	obs_source_release(scene);

	if (!item) {
		release_all();
		return false;
	}

	obs_sceneitem_addref(item);
	g_rec.item = item;

	/* Whole frame, untouched. A recording of a scene should be that scene. */
	vec2 bounds;
	vec2_set(&bounds, static_cast<float>(ovi.base_width),
		 static_cast<float>(ovi.base_height));

	obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
	obs_sceneitem_set_bounds(item, &bounds);
	obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
	obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);

	vec2 origin;
	vec2_set(&origin, 0.0f, 0.0f);
	obs_sceneitem_set_pos(item, &origin);

	obs_canvas_set_channel(g_rec.canvas, 0, obs_scene_get_source(g_rec.scene));
	return true;
}

/* Anything Windows will not accept in a filename, plus the percent sign, which
 * OBS's filename generator would read as the start of a date field. */
std::string clean_for_filename(const std::string &text)
{
	std::string out;

	for (const char c : text) {
		if (strchr("\\/:*?\"<>|%", c) || static_cast<unsigned char>(c) < 0x20) {
			continue;
		}

		out.push_back(c);
	}

	/* Trailing dots and spaces are legal to write and impossible to open. */
	while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
		out.pop_back();
	}

	return out;
}

/* Where the file goes. Assumes the lock. */
std::string next_file(const SceneRecording &settings)
{
	std::string folder = settings.folder;

	if (folder.empty()) {
		/* Wherever OBS already records, so the files land beside the ones the
		 * Start Recording button makes rather than somewhere of our choosing. */
		char *configured = obs_frontend_get_current_record_output_path();

		if (configured) {
			folder = configured;
			bfree(configured);
		}
	}

	if (folder.empty()) {
		return {};
	}

	os_mkdirs(folder.c_str());

	/* The user's own filename format, so the timestamp matches the recordings
	 * beside it. The scene name goes on the end because OBS's recording can be
	 * running at the same time as this one and two files started in the same
	 * second would otherwise collide. */
	std::string pattern = "%CCYY-%MM-%DD %hh-%mm-%ss";
	config_t *profile = obs_frontend_get_profile_config();

	if (profile) {
		const char *configured = config_get_string(profile, "Output", "FilenameFormatting");

		if (configured && *configured) {
			pattern = configured;
		}
	}

	const std::string scene = clean_for_filename(settings.scene);

	if (!scene.empty()) {
		pattern += " " + scene;
	}

	const std::string extension = settings.format.empty() ? "mkv" : settings.format;

	char *name = os_generate_formatted_filename(extension.c_str(), true, pattern.c_str());

	if (!name) {
		return {};
	}

	std::string path = folder + "/" + name;
	bfree(name);
	return path;
}

} // namespace

bool scene_recording_start()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	reap();

	if (g_rec.running) {
		return true;
	}

	const SceneRecording settings = config().scene_recording;

	if (!settings.enabled || settings.scene.empty()) {
		return false;
	}

	if (!build_canvas(settings.scene)) {
		return false;
	}

	g_rec.file = next_file(settings);

	if (g_rec.file.empty()) {
		release_all();
		return false;
	}

	obs_data_t *encoder_settings = obs_data_create();
	obs_data_set_int(encoder_settings, "bitrate", settings.video_bitrate);
	/* CBR, so a recording of a talking head does not quietly become a
	 * different size from one of a game. */
	obs_data_set_string(encoder_settings, "rate_control", "CBR");

	const char *kind = settings.encoder_id.empty() ? main_encoder_kind()
						       : settings.encoder_id.c_str();

	g_rec.video = obs_video_encoder_create(kind, "PlasmaStream recording video",
					       encoder_settings, nullptr);
	obs_data_release(encoder_settings);

	obs_data_t *audio_settings = obs_data_create();
	obs_data_set_int(audio_settings, "bitrate", 192);

	/* OBS numbers tracks from one in the interface and from zero in the API. */
	const size_t track = static_cast<size_t>(settings.audio_track > 0 ? settings.audio_track - 1 : 0);

	g_rec.audio = obs_audio_encoder_create("ffmpeg_aac", "PlasmaStream recording audio",
					       audio_settings, track, nullptr);
	obs_data_release(audio_settings);

	if (!g_rec.video || !g_rec.audio) {
		blog(LOG_WARNING, "[plasmastream] could not build encoders for the scene recording");
		release_all();
		return false;
	}

	/* The canvas is what makes this a different picture; the audio is the same
	 * mix the stream hears, because nobody asked for a different one. */
	obs_encoder_set_video(g_rec.video, obs_canvas_get_video(g_rec.canvas));
	obs_encoder_set_audio(g_rec.audio, obs_get_audio());

	obs_data_t *output_settings = obs_data_create();
	obs_data_set_string(output_settings, "path", g_rec.file.c_str());

	g_rec.output = obs_output_create("ffmpeg_muxer", "PlasmaStream recording",
					 output_settings, nullptr);
	obs_data_release(output_settings);

	if (!g_rec.output) {
		release_all();
		return false;
	}

	obs_output_set_video_encoder(g_rec.output, g_rec.video);
	obs_output_set_audio_encoder(g_rec.output, g_rec.audio, 0);

	if (!obs_output_start(g_rec.output)) {
		const char *error = obs_output_get_last_error(g_rec.output);
		blog(LOG_WARNING, "[plasmastream] the scene recording would not start: %s",
		     error ? error : "no reason given");
		release_all();
		return false;
	}

	signal_handler_connect(obs_output_get_signal_handler(g_rec.output), "stop",
			       on_output_stopped, nullptr);

	g_rec.running = true;
	g_rec.scene_name = settings.scene;
	g_rec.started_ns = os_gettime_ns();
	g_rec.error.clear();

	blog(LOG_INFO, "[plasmastream] recording scene '%s' to %s", settings.scene.c_str(),
	     g_rec.file.c_str());

	return true;
}

void scene_recording_stop()
{
	std::lock_guard<std::mutex> lock(g_mutex);

	if (!g_rec.running) {
		/* Stopped itself a moment ago and nobody has looked since. Pressing
		 * Stop on that should clear the failure, not preserve it. */
		g_rec.stopped_itself = false;
		release_all();
		return;
	}

	const std::string file = g_rec.file;

	g_rec.error.clear();
	release_all();

	blog(LOG_INFO, "[plasmastream] scene recording saved to %s", file.c_str());
}

bool scene_recording_active()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_rec.running;
}

std::string scene_recording_file()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_rec.file;
}

SceneRecordingStatus scene_recording_status()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	reap();

	SceneRecordingStatus status;
	status.running = g_rec.running;
	status.scene = g_rec.scene_name;
	status.file = g_rec.file;
	status.error = g_rec.error;

	if (g_rec.running && g_rec.started_ns) {
		status.elapsed_sec =
			static_cast<int>((os_gettime_ns() - g_rec.started_ns) / 1000000000ULL);
	}

	if (g_rec.output) {
		status.bytes = obs_output_get_total_bytes(g_rec.output);
	}

	return status;
}

bool scene_recording_supported()
{
	return true;
}

obs_canvas_t *scene_recording_canvas()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_rec.canvas;
}

void scene_recording_shutdown()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	release_all();
}

} // namespace plasmastream

#endif
