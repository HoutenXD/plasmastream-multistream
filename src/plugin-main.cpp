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

#include "canvases.hpp"
#include "config.hpp"
#include "dock.hpp"
#include "http.hpp"
#include "outputs.hpp"
#include "scenerec.hpp"
#include "update.hpp"
#include "vertical.hpp"
#include "voice.hpp"
#include "warnings.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

void on_frontend_event(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		/* The transcript's gate. Nothing said before this moment was kept, and
		 * nothing said after the stop below will be. */
		plasmastream::voice_streaming(true);

		/* The other gate. None of the private warnings count anything off air,
		 * and this is the moment their clocks start from. */
		plasmastream::warnings_streaming(true);

		plasmastream::start_outputs();

		/* Off unless somebody asked for it. Whoever set up a clean recording
		 * scene wants the clean copy of the broadcast without having to
		 * remember to press a second button. */
		if (plasmastream::config().scene_recording.with_stream) {
			plasmastream::scene_recording_start();
		}

		break;

	/* STOPPING, not STOPPED: by the time OBS says STOPPED the main output has
	 * torn down the encoders these are still holding. */
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
		plasmastream::voice_streaming(false);
		plasmastream::warnings_streaming(false);

		plasmastream::stop_outputs();

		if (plasmastream::config().scene_recording.with_stream) {
			plasmastream::scene_recording_stop();
		}

		break;

	/* STOPPING never arrives if OBS is closed mid-stream. */
	case OBS_FRONTEND_EVENT_EXIT:
		/* The key binding first: OBS keeps no copy of it for a plugin. Then
		 * the microphone is let go before its source is destroyed. */
		plasmastream::voice_save_hotkey();
		plasmastream::voice_shutdown();

		/* The audio tap comes off before libobs tears its mix down, for the
		 * same reason the microphone does. */
		plasmastream::warnings_shutdown();

		plasmastream::stop_outputs();

		/* Unconditionally, unlike above: a recording started by hand is still
		 * an open file, and closing OBS should not be the thing that loses it. */
		plasmastream::scene_recording_stop();
		break;

	/* The scene being recorded is about to stop existing. Better to close the
	 * file here than to keep writing a picture of nothing. */
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		plasmastream::scene_recording_stop();
		break;

	/* The vertical frame renders whatever is on program, so it has to be told
	 * when that changes. A no-op when there is no vertical canvas. */
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		plasmastream::vertical_follow_program();

		/* Somebody pressed something. The only reason this matters is the
		 * warning about a stream nobody is in front of any more. */
		plasmastream::warnings_activity();
		break;

	/* Sources put on the vertical frame are looked up by name, and none of
	 * those names exist until the collection is loaded. The canvas can be built
	 * before that happens, so this is where they actually arrive. A changed
	 * collection is a different set of sources and needs the same treatment. */
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		plasmastream::vertical_follow_program();
		plasmastream::vertical_reload_sources();

		/* The microphone is a source like any other, so voice waits for the
		 * collection too. Starting it needs no stream: a streamer sets voice
		 * commands up long before going live. */
		plasmastream::voice_apply();
		break;

	default:
		break;
	}
}

} // namespace

bool obs_module_load(void)
{
	plasmastream::http_init();
	plasmastream::load_config();
	plasmastream::voice_init();
	plasmastream::update_init();

	obs_frontend_add_event_callback(on_frontend_event, nullptr);
	plasmastream::register_dock();
	plasmastream::register_canvas_dock();

	obs_log(LOG_INFO, "loaded (version %s), %zu destination(s) configured", PLUGIN_VERSION,
		plasmastream::config().destinations.size());

	return true;
}

void obs_module_unload(void)
{
	/* A module can be unloaded without the frontend exiting, so EXIT is not
	 * enough on its own. */
	plasmastream::stop_outputs();
	plasmastream::voice_shutdown();
	plasmastream::warnings_shutdown();

	/* Before the rest of the teardown: an update that was downloaded and never
	 * installed is handed to the script here, and the script is waiting for
	 * this process to end. */
	plasmastream::update_shutdown();

	obs_frontend_remove_event_callback(on_frontend_event, nullptr);

	/* Before libobs tears the graphics subsystem down, since the canvas lives
	 * in it. */
	plasmastream::vertical_shutdown();
	plasmastream::scene_recording_shutdown();

	obs_log(LOG_INFO, "unloaded");
}
