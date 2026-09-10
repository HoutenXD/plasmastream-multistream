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

/**
 * Where the plugin hooks into OBS.
 *
 * Everything rides the frontend's streaming events rather than a timer or a
 * button of our own, and that is deliberate: the extra destinations should begin
 * and end with the stream the streamer actually started. A separate control would
 * be one more thing to remember at the moment they have the least attention to
 * spare, and forgetting it means going live to one platform believing you went
 * live to three.
 */
void on_frontend_event(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		plasmastream::start_outputs();
		break;

	// STOPPING rather than STOPPED. By the time OBS reports STOPPED the main
	// output has torn down its encoders, and the extra outputs are still
	// holding them. Stopping first is what keeps that teardown ordered.
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
		plasmastream::stop_outputs();
		break;

	// A safety net rather than a duplicate. If OBS goes down while streaming,
	// STOPPING may never arrive, and an output still holding an encoder during
	// shutdown is a crash on exit.
	case OBS_FRONTEND_EVENT_EXIT:
		plasmastream::stop_outputs();
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

	// The dock needs the main window, which exists by the time modules load.
	plasmastream::register_dock();

	obs_log(LOG_INFO, "plugin loaded successfully (version %s), %zu destination(s) configured",
		PLUGIN_VERSION, plasmastream::config().destinations.size());

	return true;
}

void obs_module_unload(void)
{
	// Not strictly required, since OBS_FRONTEND_EVENT_EXIT already ran, but a
	// module can be unloaded without the frontend exiting and an output left
	// running past its module's lifetime is a crash with no useful stack.
	plasmastream::stop_outputs();

	obs_frontend_remove_event_callback(on_frontend_event, nullptr);

	obs_log(LOG_INFO, "plugin unloaded");
}
