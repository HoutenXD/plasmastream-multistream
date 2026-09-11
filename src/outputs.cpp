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

#include "outputs.hpp"

#include "config.hpp"
#include "vertical.hpp"

#include <memory>
#include <mutex>

#include <graphics/vec2.h>
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

namespace plasmastream {

namespace {

/* Output and service are owned together; OBS will keep a service alive that
 * nothing points at. The encoders are owned only when this destination asked for
 * its own; a shared one belongs to the main output. */
struct RunningOutput {
	std::string id;
	std::string name;
	obs_output_t *output = nullptr;
	obs_service_t *service = nullptr;
	obs_encoder_t *video = nullptr;
	obs_encoder_t *audio = nullptr;
	bool owns_encoders = false;
	OutputState state = OutputState::Idle;
	std::string detail;

	/* For working out a real bitrate: bytes and the clock at the last poll. */
	uint64_t last_bytes = 0;
	uint64_t last_sample_ns = 0;
	int bitrate_kbps = 0;
	int reconnects = 0;

	/* Whether this one encodes from the vertical canvas. The canvas itself
	 * belongs to vertical.cpp, since one of it serves every destination that
	 * wants it. */
	bool vertical = false;
};

/* unique_ptr, not by value: the signal handlers below hold a void* into these,
 * and a vector reallocating on the second push_back would leave every handler
 * registered so far pointing at freed memory. */
std::vector<std::unique_ptr<RunningOutput>> g_running;

/* The dock polls status on the Qt thread while libobs fires signals on its own. */
std::mutex g_mutex;

void handle_stop(void *data, calldata_t *params)
{
	auto *running = static_cast<RunningOutput *>(data);

	if (!running) {
		return;
	}

	const int code = static_cast<int>(calldata_int(params, "code"));

	std::lock_guard<std::mutex> lock(g_mutex);

	if (code == OBS_OUTPUT_SUCCESS) {
		running->state = OutputState::Idle;
		running->detail.clear();
		return;
	}

	running->state = OutputState::Failed;

	switch (code) {
	case OBS_OUTPUT_BAD_PATH:
		running->detail = obs_module_text("Error.BadPath");
		break;
	case OBS_OUTPUT_CONNECT_FAILED:
		running->detail = obs_module_text("Error.ConnectFailed");
		break;
	case OBS_OUTPUT_INVALID_STREAM:
		running->detail = obs_module_text("Error.InvalidStream");
		break;
	case OBS_OUTPUT_DISCONNECTED:
		running->detail = obs_module_text("Error.Disconnected");
		break;
	default:
		running->detail = obs_module_text("Error.Unknown");
		break;
	}

	blog(LOG_WARNING, "[plasmastream] '%s' stopped: %s (code %d)", running->name.c_str(),
	     running->detail.c_str(), code);
}

void handle_start(void *data, calldata_t *)
{
	auto *running = static_cast<RunningOutput *>(data);

	if (!running) {
		return;
	}

	std::lock_guard<std::mutex> lock(g_mutex);

	/* A start after we were already up is a reconnect. Counting them is how
	 * somebody tells "it dropped once" from "it has been flapping all night". */
	if (running->state == OutputState::Failed || running->state == OutputState::Live) {
		running->reconnects++;
	}

	running->state = OutputState::Live;
	running->detail.clear();

	blog(LOG_INFO, "[plasmastream] '%s' is live", running->name.c_str());
}

/* Tear one down. Call with no lock held: obs_output_stop blocks on the flush. */
void release(RunningOutput &running)
{
	if (running.output) {
		signal_handler_t *signals = obs_output_get_signal_handler(running.output);
		signal_handler_disconnect(signals, "start", handle_start, &running);
		signal_handler_disconnect(signals, "stop", handle_stop, &running);

		obs_output_stop(running.output);
		obs_output_release(running.output);
		running.output = nullptr;
	}

	if (running.service) {
		obs_service_release(running.service);
		running.service = nullptr;
	}

	/* Only ours. A shared encoder belongs to the main output, and releasing it
	 * would take the main stream down along with this destination. */
	if (running.owns_encoders) {
		if (running.video) {
			obs_encoder_release(running.video);
		}

		if (running.audio) {
			obs_encoder_release(running.audio);
		}
	}

	running.video = nullptr;
	running.audio = nullptr;
}

/* What the main stream encodes with, so a per-destination encoder defaults to
 * the same kind rather than to x264 on a machine set up for NVENC. */
const char *main_encoder_id(obs_output_t *main_output)
{
	obs_encoder_t *video = main_output ? obs_output_get_video_encoder(main_output) : nullptr;

	return video ? obs_encoder_get_id(video) : "obs_x264";
}

/* The largest video encoder on the main output.
 *
 * With Twitch Enhanced Broadcasting the main output is multitrack: OBS builds a
 * ladder of encoders from a configuration Twitch sends back, and which rung
 * lands at index 0 is Twitch's business, not ours. obs_output_get_video_encoder
 * returns that first one, so borrowing it can quietly relay Twitch's 360p rung
 * to a second platform while the streamer watches a clean 1080p go out on the
 * first. Take the biggest instead, which is the one they think they are
 * sending. */
obs_encoder_t *best_shared_video(obs_output_t *main_output, int *rungs)
{
	obs_encoder_t *best = nullptr;
	uint32_t most_pixels = 0;
	int found = 0;

	for (size_t i = 0; i < MAX_OUTPUT_VIDEO_ENCODERS; i++) {
		obs_encoder_t *candidate = obs_output_get_video_encoder2(main_output, i);

		if (!candidate) {
			continue;
		}

		found++;

		const uint32_t pixels =
			obs_encoder_get_width(candidate) * obs_encoder_get_height(candidate);

		if (!best || pixels > most_pixels) {
			best = candidate;
			most_pixels = pixels;
		}
	}

	if (rungs) {
		*rungs = found;
	}

	return best;
}

/* Borrow the main stream's encoders, or build this destination its own.
 *
 * Its own costs CPU, which is why it is not the default. It buys the thing
 * people actually need: a second destination at a bitrate their upload can
 * carry, rather than a second copy of the first one's. */
bool attach_encoders(RunningOutput &running, const Destination &destination,
		     obs_output_t *main_output, obs_encoder_t *shared_video,
		     obs_encoder_t *shared_audio)
{
	/* Sharing is not on offer for a vertical destination: the main stream's
	 * encoder reads the main canvas, and there is no telling it otherwise. */
	if (!destination.own_encoder && !destination.vertical) {
		running.video = shared_video;
		running.audio = shared_audio;
		running.owns_encoders = false;
		return running.video && running.audio;
	}

	const char *encoder_id = destination.encoder_id.empty()
					 ? main_encoder_id(main_output)
					 : destination.encoder_id.c_str();

	obs_data_t *video_settings = obs_data_create();
	obs_data_set_int(video_settings, "bitrate", destination.video_bitrate);
	/* Without a rate control an encoder built from nothing runs at its own
	 * default rather than at the rate that was asked for. */
	obs_data_set_string(video_settings, "rate_control", "CBR");

	running.video = obs_video_encoder_create(encoder_id, (running.name + " video").c_str(),
						 video_settings, nullptr);
	obs_data_release(video_settings);

	obs_data_t *audio_settings = obs_data_create();
	obs_data_set_int(audio_settings, "bitrate", destination.audio_bitrate);

	running.audio = obs_audio_encoder_create("ffmpeg_aac", (running.name + " audio").c_str(),
						 audio_settings, 0, nullptr);
	obs_data_release(audio_settings);

	if (!running.video || !running.audio) {
		blog(LOG_WARNING, "[plasmastream] could not build encoders for '%s' (%s)",
		     running.name.c_str(), encoder_id);

		if (running.video) {
			obs_encoder_release(running.video);
			running.video = nullptr;
		}

		if (running.audio) {
			obs_encoder_release(running.audio);
			running.audio = nullptr;
		}

		return false;
	}

	running.owns_encoders = true;

	/* A vertical destination reads the vertical canvas; everything else reads
	 * the same mix the main stream does and differs only in compression. Audio
	 * is the main mix either way, since the vertical canvas makes none. */
	video_t *mix = obs_get_video();

#if PLASMASTREAM_HAS_CANVAS
	if (running.vertical) {
		obs_canvas_t *canvas = vertical_canvas();

		if (!canvas) {
			blog(LOG_WARNING,
			     "[plasmastream] '%s' wants a vertical frame and there is no canvas",
			     running.name.c_str());
			return false;
		}

		mix = obs_canvas_get_video(canvas);
	}
#endif

	obs_encoder_set_video(running.video, mix);
	obs_encoder_set_audio(running.audio, obs_get_audio());

	return true;
}

/* Everything start_outputs and start_one have in common. Returns the object,
 * already in g_running, or null. */
RunningOutput *spin_up(const Destination &destination, obs_output_t *main_output,
		       obs_encoder_t *shared_video, obs_encoder_t *shared_audio)
{
	auto running = std::make_unique<RunningOutput>();
	running->id = destination.id.empty() ? destination.name : destination.id;
	running->name = destination.name.empty() ? destination.url : destination.name;

	/* rtmp_custom, not a named service: those carry their own ingest list and
	 * would override the address the streamer typed. */
	obs_data_t *service_settings = obs_data_create();
	obs_data_set_string(service_settings, "server", destination.url.c_str());
	obs_data_set_string(service_settings, "key", destination.key.c_str());

	running->service = obs_service_create("rtmp_custom", running->name.c_str(),
					      service_settings, nullptr);
	obs_data_release(service_settings);

	if (!running->service) {
		blog(LOG_WARNING, "[plasmastream] could not create a service for '%s'",
		     running->name.c_str());
		return nullptr;
	}

	obs_data_t *output_settings = obs_data_create();
	running->output =
		obs_output_create("rtmp_output", running->name.c_str(), output_settings, nullptr);
	obs_data_release(output_settings);

	if (!running->output) {
		blog(LOG_WARNING, "[plasmastream] could not create an output for '%s'",
		     running->name.c_str());
		obs_service_release(running->service);
		return nullptr;
	}

	running->vertical = destination.vertical;

	if (!attach_encoders(*running, destination, main_output, shared_video, shared_audio)) {
		release(*running);
		return nullptr;
	}

	obs_output_set_service(running->output, running->service);
	obs_output_set_video_encoder(running->output, running->video);
	obs_output_set_audio_encoder(running->output, running->audio, 0);

	obs_output_set_reconnect_settings(running->output, config().reconnect_retries,
					  config().reconnect_delay_sec);

	running->state = OutputState::Starting;
	running->last_sample_ns = os_gettime_ns();

	RunningOutput *stable = running.get();

	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_running.push_back(std::move(running));
	}

	/* Before the start attempt, so a synchronous failure still finds them. */
	signal_handler_t *signals = obs_output_get_signal_handler(stable->output);
	signal_handler_connect(signals, "start", handle_start, stable);
	signal_handler_connect(signals, "stop", handle_stop, stable);

	if (!obs_output_start(stable->output)) {
		const char *error = obs_output_get_last_error(stable->output);
		blog(LOG_WARNING, "[plasmastream] '%s' refused to start: %s", stable->name.c_str(),
		     error ? error : "no reason given");

		std::lock_guard<std::mutex> lock(g_mutex);
		stable->state = OutputState::Failed;
		stable->detail = error ? error : obs_module_text("Error.Unknown");
	}

	return stable;
}

} // namespace

bool streaming_live()
{
	obs_output_t *main_output = obs_frontend_get_streaming_output();

	if (!main_output) {
		return false;
	}

	const bool active = obs_output_active(main_output);
	obs_output_release(main_output);

	return active;
}

void start_outputs()
{
	stop_outputs();

	obs_output_t *main_output = obs_frontend_get_streaming_output();

	if (!main_output) {
		blog(LOG_WARNING, "[plasmastream] streaming started but there is no main output");
		return;
	}

	/* Borrowed from the main output, so not released here. */
	int rungs = 0;
	obs_encoder_t *video = best_shared_video(main_output, &rungs);
	obs_encoder_t *audio = obs_output_get_audio_encoder(main_output, 0);

	if (rungs > 1) {
		blog(LOG_INFO,
		     "[plasmastream] the main stream is multitrack (%d encoders); sharing the "
		     "largest, %ux%u",
		     rungs, obs_encoder_get_width(video), obs_encoder_get_height(video));
	}

	int started = 0;

	for (const Destination &destination : config().destinations) {
		if (destination.usable() && spin_up(destination, main_output, video, audio)) {
			started++;
		}
	}

	obs_output_release(main_output);

	blog(LOG_INFO, "[plasmastream] %d extra destination(s) starting", started);
}

bool start_one(const std::string &id)
{
	{
		std::lock_guard<std::mutex> lock(g_mutex);

		for (const std::unique_ptr<RunningOutput> &running : g_running) {
			if (running->id == id) {
				return false;
			}
		}
	}

	const Destination *wanted = nullptr;

	for (const Destination &destination : config().destinations) {
		const std::string key = destination.id.empty() ? destination.name : destination.id;

		if (key == id && destination.usable()) {
			wanted = &destination;
			break;
		}
	}

	if (!wanted) {
		return false;
	}

	obs_output_t *main_output = obs_frontend_get_streaming_output();

	if (!main_output) {
		return false;
	}

	obs_encoder_t *video = best_shared_video(main_output, nullptr);
	obs_encoder_t *audio = obs_output_get_audio_encoder(main_output, 0);

	const bool ok = spin_up(*wanted, main_output, video, audio) != nullptr;
	obs_output_release(main_output);

	return ok;
}

void stop_one(const std::string &id)
{
	std::unique_ptr<RunningOutput> taken;

	{
		std::lock_guard<std::mutex> lock(g_mutex);

		for (auto it = g_running.begin(); it != g_running.end(); ++it) {
			if ((*it)->id == id) {
				taken = std::move(*it);
				g_running.erase(it);
				break;
			}
		}
	}

	/* Outside the lock, for the same reason stop_outputs releases outside it. */
	if (taken) {
		release(*taken);
	}
}

void stop_outputs()
{
	std::vector<std::unique_ptr<RunningOutput>> to_stop;

	{
		std::lock_guard<std::mutex> lock(g_mutex);
		to_stop.swap(g_running);
	}

	/* Outside the lock: obs_output_stop blocks while the output flushes, and the
	 * dock polls status on the Qt thread. Handlers come off first, since they
	 * hold addresses of objects destroyed when this returns. */
	for (std::unique_ptr<RunningOutput> &running : to_stop) {
		release(*running);
	}
}

std::vector<OutputStatus> output_statuses()
{
	const uint64_t now = os_gettime_ns();

	std::lock_guard<std::mutex> lock(g_mutex);

	std::vector<OutputStatus> statuses;
	statuses.reserve(g_running.size());

	for (const std::unique_ptr<RunningOutput> &running : g_running) {
		OutputStatus status;
		status.id = running->id;
		status.name = running->name;
		status.state = running->state;
		status.detail = running->detail;
		status.reconnects = running->reconnects;

		if (running->output) {
			status.dropped_frames = obs_output_get_frames_dropped(running->output);
			status.total_frames = obs_output_get_total_frames(running->output);
			status.uptime_sec = static_cast<int>(
				obs_output_get_connect_time_ms(running->output) / 1000);

			/* Bytes over elapsed time, sampled. Not the total divided by
			 * the whole stream, which would smooth away the dip somebody
			 * opened this panel to look at. */
			const uint64_t bytes = obs_output_get_total_bytes(running->output);
			const uint64_t elapsed = now - running->last_sample_ns;

			if (elapsed > 500000000ULL && bytes >= running->last_bytes) {
				const double seconds =
					static_cast<double>(elapsed) / 1000000000.0;
				const double bits =
					static_cast<double>(bytes - running->last_bytes) * 8.0;

				running->bitrate_kbps = static_cast<int>(bits / seconds / 1000.0);
				running->last_bytes = bytes;
				running->last_sample_ns = now;
			}

			status.bitrate_kbps = running->bitrate_kbps;
		}

		statuses.push_back(status);
	}

	return statuses;
}

} // namespace plasmastream
