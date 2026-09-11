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

/* Output and service are owned together; OBS will keep a service alive that
 * nothing points at. */
struct RunningOutput {
	std::string id;
	std::string name;
	obs_output_t *output = nullptr;
	obs_service_t *service = nullptr;
	OutputState state = OutputState::Idle;
	std::string detail;
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
	running->state = OutputState::Live;
	running->detail.clear();

	blog(LOG_INFO, "[plasmastream] '%s' is live", running->name.c_str());
}

} // namespace

void start_outputs()
{
	stop_outputs();

	obs_output_t *main_output = obs_frontend_get_streaming_output();

	if (!main_output) {
		blog(LOG_WARNING, "[plasmastream] streaming started but there is no main output");
		return;
	}

	/* Borrowed from the main output, so not released here. */
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

		/* rtmp_custom, not a named service: those carry their own ingest list
		 * and would override the address the streamer typed. */
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

		/* Shared, so the frames are compressed once for all destinations. */
		obs_output_set_video_encoder(running->output, video);
		obs_output_set_audio_encoder(running->output, audio, 0);

		obs_output_set_reconnect_settings(running->output, 20, 5);

		running->state = OutputState::Starting;

		RunningOutput *stable = running.get();

		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_running.push_back(std::move(running));
		}

		/* Before the start attempt, so a synchronous failure still finds them. */
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

	/* Outside the lock: obs_output_stop blocks while the output flushes, and the
	 * dock polls status on the Qt thread. Handlers come off first, since they
	 * hold addresses of objects destroyed when this returns. */
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
