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

	/* The user's own scene, held while it is on the canvas. */
	obs_source_t *source = nullptr;

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

/* The codec an encoder makes, like "h264", or empty for an id OBS does not know. */
std::string codec_of(const char *id)
{
	const char *codec = id ? obs_get_encoder_codec(id) : nullptr;
	return codec ? codec : "";
}

/* Which hardware an encoder id belongs to, so an H.264 encoder can be found on
 * the same one. Null for software encoders. */
const char *hardware_of(const char *id)
{
	static const char *const families[] = {"nvenc", "amf", "qsv", "videotoolbox"};

	for (const char *family : families) {
		if (strstr(id, family)) {
			return family;
		}
	}

	return nullptr;
}

/* What the recording encodes with when nobody picked an encoder.
 *
 * The stream's own kind of encoder, so it lands on the same hardware rather than
 * on x264 on a machine set up for NVENC. But always H.264. It used to copy the
 * stream's CODEC too, and a stream in HEVC or AV1, which YouTube and Twitch's
 * Enhanced Broadcasting both take, made a recording that Windows' own players
 * refuse to open without an extension from the Microsoft Store: the file had the
 * right length and size and would not play. The dialog calls mp4 the one that
 * "opens anywhere", and only H.264 does.
 *
 * Somebody who picks HEVC or AV1 by name still gets it. */
std::string default_encoder_kind()
{
	std::string stream_id = "obs_x264";
	obs_output_t *stream = obs_frontend_get_streaming_output();

	if (stream) {
		obs_encoder_t *video = obs_output_get_video_encoder(stream);

		if (video) {
			stream_id = obs_encoder_get_id(video);
		}

		obs_output_release(stream);
	}

	if (codec_of(stream_id.c_str()) == "h264") {
		return stream_id;
	}

	const char *hardware = hardware_of(stream_id.c_str());

	if (hardware) {
		const char *candidate = nullptr;

		for (size_t i = 0; obs_enum_encoder_types(i, &candidate); i++) {
			/* Deprecated ids linger for old profiles, and internal ones are the
			 * fallbacks OBS switches to by itself; neither is one to choose. */
			const uint32_t unwanted = OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL;

			if (obs_get_encoder_type(candidate) == OBS_ENCODER_VIDEO &&
			    codec_of(candidate) == "h264" && strstr(candidate, hardware) &&
			    (obs_get_encoder_caps(candidate) & unwanted) == 0) {
				return candidate;
			}
		}
	}

	return "obs_x264";
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

	if (g_rec.canvas) {
		/* Clearing the channel is what gives the scene its activation back;
		 * destroying the canvas does it too, but doing it here means the
		 * order is ours rather than the refcount's. */
		obs_canvas_set_channel(g_rec.canvas, 0, nullptr);
		obs_canvas_remove(g_rec.canvas);
		obs_canvas_release(g_rec.canvas);
		g_rec.canvas = nullptr;
	}

	if (g_rec.source) {
		obs_source_release(g_rec.source);
		g_rec.source = nullptr;
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

	/* Eight bit 4:2:0, whatever OBS itself is set to. OBS set to I444 or to ten
	 * bit makes H.264 in High 4:4:4 or High 10, which is a real format and one
	 * Windows' own players cannot decode, so the file would not open. This canvas
	 * is ours and has its own format, so the main output is left as it is. An HDR
	 * setup records in SDR here for the same reason. */
	if (ovi.output_format != VIDEO_FORMAT_NV12 && ovi.output_format != VIDEO_FORMAT_I420) {
		ovi.output_format = VIDEO_FORMAT_NV12;
	}

	if (ovi.colorspace == VIDEO_CS_2100_PQ || ovi.colorspace == VIDEO_CS_2100_HLG) {
		ovi.colorspace = VIDEO_CS_709;
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

	/* Straight onto the canvas, with no wrapper scene holding it as an item.
	 *
	 * There used to be one, with bounds set to the frame. It bought nothing:
	 * this canvas is the same size as the main one, so there is no scaling to
	 * do and the whole transform was an identity. What it cost was a level of
	 * nesting, and a scene rendered as an item inside another scene does not
	 * go through the same path as a scene rendered as a canvas channel. Capture
	 * sources came out black through the nested path. */
	obs_canvas_set_channel(g_rec.canvas, 0, scene);

	g_rec.source = scene;
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

	const std::string kind = settings.encoder_id.empty() ? default_encoder_kind()
							     : settings.encoder_id;

	g_rec.video = obs_video_encoder_create(kind.c_str(), "PlasmaStream recording video",
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

	/* The encoder goes in the log as well, because "the file will not open" is
	 * almost always a question about the codec, and this line answers it. */
	blog(LOG_INFO, "[plasmastream] recording scene '%s' to %s with %s (%s)", settings.scene.c_str(),
	     g_rec.file.c_str(), kind.c_str(), codec_of(kind.c_str()).c_str());


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

namespace {

/* Which of OBS's six tracks the stream is sending. Simple output mode is always
 * the first; advanced mode is wherever it was pointed. */
int streaming_track()
{
	config_t *profile = obs_frontend_get_profile_config();

	if (!profile) {
		return 1;
	}

	const char *mode = config_get_string(profile, "Output", "Mode");

	if (!mode || strcmp(mode, "Advanced") != 0) {
		return 1;
	}

	const int track = static_cast<int>(config_get_int(profile, "AdvOut", "TrackIndex"));
	return track >= 1 && track <= 6 ? track : 1;
}

struct AudioHunt {
	/* Bit of the track the stream is on. A source not on it cannot be heard
	 * there however loud it is. */
	uint32_t stream_track_bit = 1;

	/* The microphone and desktop audio, which are on the stream already and so
	 * are not news. */
	std::vector<obs_source_t *> globals;

	std::vector<std::string> found;
};

bool hunt_item(obs_scene_t *, obs_sceneitem_t *item, void *param);

void hunt_source(obs_source_t *source, AudioHunt &hunt)
{
	if (!source) {
		return;
	}

	/* A scene inside a scene is a real thing people build, and its audio
	 * reaches the stream by the same route. */
	obs_scene_t *nested = obs_scene_from_source(source);

	if (nested) {
		obs_scene_enum_items(nested, hunt_item, &hunt);
		return;
	}

	if ((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) == 0) {
		return;
	}

	if (obs_source_muted(source)) {
		return;
	}

	if ((obs_source_get_audio_mixers(source) & hunt.stream_track_bit) == 0) {
		return;
	}

	for (obs_source_t *global : hunt.globals) {
		if (global == source) {
			return;
		}
	}

	const char *name = obs_source_get_name(source);

	if (!name || !*name) {
		return;
	}

	/* One source can sit in a scene more than once, and naming it twice reads
	 * as two problems. */
	for (const std::string &already : hunt.found) {
		if (already == name) {
			return;
		}
	}

	hunt.found.push_back(name);
}

bool hunt_item(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto &hunt = *static_cast<AudioHunt *>(param);

	/* A hidden source is not rendered and not heard. */
	if (!obs_sceneitem_visible(item)) {
		return true;
	}

	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, hunt_item, &hunt);
		return true;
	}

	hunt_source(obs_sceneitem_get_source(item), hunt);
	return true;
}

} // namespace

std::vector<std::string> scene_audio_reaching_stream(const std::string &scene_name)
{
	obs_source_t *source = obs_get_source_by_name(scene_name.c_str());

	if (!source) {
		return {};
	}

	obs_scene_t *scene = obs_scene_from_source(source);

	if (!scene) {
		obs_source_release(source);
		return {};
	}

	AudioHunt hunt;
	hunt.stream_track_bit = 1u << (streaming_track() - 1);

	/* Channel 0 is whatever is on programme; 1 to 5 are the audio devices OBS
	 * mixes in on their own, which is why they are not a surprise. */
	for (uint32_t channel = 1; channel <= 5; channel++) {
		obs_source_t *global = obs_get_output_source(channel);

		if (global) {
			hunt.globals.push_back(global);
		}
	}

	obs_scene_enum_items(scene, hunt_item, &hunt);

	for (obs_source_t *global : hunt.globals) {
		obs_source_release(global);
	}

	obs_source_release(source);
	return hunt.found;
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
