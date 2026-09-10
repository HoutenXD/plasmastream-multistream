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

#include <memory>
#include <mutex>

#include <obs-frontend-api.h>
#include <obs-module.h>

namespace plasmastream {

namespace {

/**
 * One extra destination, live.
 *
 * The service and the output are both owned here and released together. OBS will
 * happily keep a service alive that nothing points at, so forgetting one is a
 * leak that only shows up after somebody has streamed twenty times in one
 * session, which is exactly the person least likely to report it.
 */
struct RunningOutput {
	std::string id;
	std::string name;
	obs_output_t *output = nullptr;
	obs_service_t *service = nullptr;
	OutputState state = OutputState::Idle;
	std::string detail;
};

/**
 * Held by pointer, and that is not a style choice.
 *
 * libobs signal handlers take a void* that they hand back on every callback, and
 * the obvious thing to pass is a pointer into this container. Store the objects
 * by value in a vector and the second push_back reallocates, so every handler
 * registered for an earlier destination is left pointing at freed memory. It
 * would work perfectly with one destination and corrupt memory with two, during
 * a live stream, which is the worst possible way to find out.
 *
 * unique_ptr keeps each object's address fixed for its whole life no matter what
 * the container does.
 */
std::vector<std::unique_ptr<RunningOutput>> g_running;

/**
 * Guards g_running.
 *
 * The reads and writes genuinely happen on different threads: the dock polls for
 * status on the Qt thread while libobs fires start and stop signals on its own.
 * Without this, a destination dropping mid-stream races the table repaint.
 */
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

	// Translated into something a streamer can act on. "Error -4" tells them
	// nothing; "the stream key was rejected" tells them where to look.
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
	running->state = OutputState::Live;
	running->detail.clear();

	blog(LOG_INFO, "[plasmastream] '%s' is live", running->name.c_str());
}

} // namespace

void start_outputs()
{
	stop_outputs();

	// The main stream is what everything here attaches to. No main stream means
	// no encoders to borrow, so there is nothing this plugin can do.
	obs_output_t *main_output = obs_frontend_get_streaming_output();

	if (!main_output) {
		blog(LOG_WARNING, "[plasmastream] streaming started but there is no main output");
		return;
	}

	// Borrowed pointers, owned by the main output, so not released here.
	obs_encoder_t *video = obs_output_get_video_encoder(main_output);
	obs_encoder_t *audio = obs_output_get_audio_encoder(main_output, 0);

	if (!video || !audio) {
		blog(LOG_WARNING, "[plasmastream] the main stream has no encoders to share");
		obs_output_release(main_output);
		return;
	}

	int started = 0;

	for (const Destination &destination : config().destinations) {
		if (!destination.usable()) {
			continue;
		}

		auto running = std::make_unique<RunningOutput>();
		running->id = destination.id.empty() ? destination.name : destination.id;
		running->name = destination.name.empty() ? destination.url : destination.name;

		// rtmp_custom rather than one of the named services, because a named
		// service carries its own ingest list and would override the address
		// the streamer typed. They picked it; it is not ours to improve on.
		obs_data_t *service_settings = obs_data_create();
		obs_data_set_string(service_settings, "server", destination.url.c_str());
		obs_data_set_string(service_settings, "key", destination.key.c_str());

		running->service = obs_service_create("rtmp_custom", running->name.c_str(),
						     service_settings, nullptr);
		obs_data_release(service_settings);

		if (!running->service) {
			blog(LOG_WARNING, "[plasmastream] could not create a service for '%s'",
			     running->name.c_str());
			continue;
		}

		obs_data_t *output_settings = obs_data_create();
		running->output = obs_output_create("rtmp_output", running->name.c_str(),
						    output_settings, nullptr);
		obs_data_release(output_settings);

		if (!running->output) {
			blog(LOG_WARNING, "[plasmastream] could not create an output for '%s'",
			     running->name.c_str());
			obs_service_release(running->service);
			continue;
		}

		obs_output_set_service(running->output, running->service);

		// The shared encoders. This is the line that makes a second destination
		// nearly free: the frames are already compressed for the first one.
		obs_output_set_video_encoder(running->output, video);
		obs_output_set_audio_encoder(running->output, audio, 0);

		// Reconnect on its own, the way the main output does. A destination
		// that drops for ten seconds should come back rather than stay dead for
		// the rest of the stream.
		obs_output_set_reconnect_settings(running->output, 20, 5);

		running->state = OutputState::Starting;

		RunningOutput *stable = running.get();

		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_running.push_back(std::move(running));
		}

		// Connected before the start attempt, so a failure fast enough to fire
		// synchronously still finds a handler.
		signal_handler_t *signals = obs_output_get_signal_handler(stable->output);
		signal_handler_connect(signals, "start", handle_start, stable);
		signal_handler_connect(signals, "stop", handle_stop, stable);

		if (obs_output_start(stable->output)) {
			started++;
			blog(LOG_INFO, "[plasmastream] starting '%s'", stable->name.c_str());
		} else {
			const char *error = obs_output_get_last_error(stable->output);
			blog(LOG_WARNING, "[plasmastream] '%s' refused to start: %s",
			     stable->name.c_str(), error ? error : "no reason given");

			std::lock_guard<std::mutex> lock(g_mutex);
			stable->state = OutputState::Failed;
			stable->detail = error ? error : obs_module_text("Error.Unknown");
		}
	}

	obs_output_release(main_output);

	blog(LOG_INFO, "[plasmastream] %d extra destination(s) starting", started);
}

void stop_outputs()
{
	std::vector<std::unique_ptr<RunningOutput>> to_stop;

	{
		std::lock_guard<std::mutex> lock(g_mutex);
		to_stop.swap(g_running);
	}

	// Stopped outside the lock. obs_output_stop can block while the output
	// flushes, and holding the mutex through that would stall the dock's status
	// poll on the Qt thread: the UI freezing at exactly the moment somebody is
	// watching to see whether their stream ended cleanly.
	//
	// The handlers are disconnected FIRST. They capture the RunningOutput
	// address, and the objects here are destroyed when this function returns, so
	// a signal arriving after that would be handed freed memory.
	for (std::unique_ptr<RunningOutput> &running : to_stop) {
		if (running->output) {
			signal_handler_t *signals = obs_output_get_signal_handler(running->output);
			signal_handler_disconnect(signals, "start", handle_start, running.get());
			signal_handler_disconnect(signals, "stop", handle_stop, running.get());

			obs_output_stop(running->output);
			obs_output_release(running->output);
		}

		if (running->service) {
			obs_service_release(running->service);
		}
	}
}

std::vector<OutputStatus> output_statuses()
{
	std::lock_guard<std::mutex> lock(g_mutex);

	std::vector<OutputStatus> statuses;
	statuses.reserve(g_running.size());

	for (const std::unique_ptr<RunningOutput> &running : g_running) {
		OutputStatus status;
		status.id = running->id;
		status.name = running->name;
		status.state = running->state;
		status.detail = running->detail;
		statuses.push_back(status);
	}

	return statuses;
}

} // namespace plasmastream
