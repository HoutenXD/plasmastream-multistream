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
#include "dock.hpp"
#include "http.hpp"
#include "outputs.hpp"

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
		plasmastream::start_outputs();
		break;

	/* STOPPING, not STOPPED: by the time OBS says STOPPED the main output has
	 * torn down the encoders these are still holding. */
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
		plasmastream::stop_outputs();
		break;

	/* STOPPING never arrives if OBS is closed mid-stream. */
	case OBS_FRONTEND_EVENT_EXIT:
		plasmastream::stop_outputs();
		break;

	/* A vertical destination renders whatever is on program, so it has to be
	 * told when that changes. Nothing else cares. */
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		plasmastream::program_scene_changed();
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

	obs_frontend_add_event_callback(on_frontend_event, nullptr);
	plasmastream::register_dock();

	obs_log(LOG_INFO, "loaded (version %s), %zu destination(s) configured", PLUGIN_VERSION,
		plasmastream::config().destinations.size());

	return true;
}

void obs_module_unload(void)
{
	/* A module can be unloaded without the frontend exiting, so EXIT is not
	 * enough on its own. */
	plasmastream::stop_outputs();

	obs_frontend_remove_event_callback(on_frontend_event, nullptr);

	obs_log(LOG_INFO, "unloaded");
}
