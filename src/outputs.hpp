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

/* obs.h rather than just obs-config.h, which is all the version macros below
 * need: vertical_canvas() hands back an obs_canvas_t, and that type has to be
 * known to everyone who includes this. */
#include <obs.h>

/* Vertical needs obs_canvas_*, which arrived in libobs 31.1.
 *
 * This is not a courtesy to people on old OBS: on Linux a plugin has to be
 * built against the libobs the user actually runs, and Ubuntu 24.04 still ships
 * 30.0.2. Building against anything newer there produces a .deb that cannot
 * load. So the canvas compiles out rather than the Linux build not existing.
 *
 * Overridable so the other branch can be compiled on a machine that only has
 * the newer headers, which is the only way to find out it still builds. */
#ifndef PLASMASTREAM_HAS_CANVAS
#define PLASMASTREAM_HAS_CANVAS \
	(LIBOBS_API_MAJOR_VER > 31 || (LIBOBS_API_MAJOR_VER == 31 && LIBOBS_API_MINOR_VER >= 1))
#endif

namespace plasmastream {

enum class OutputState {
	Idle,
	Starting,
	Live,
	Failed,
};

struct OutputStatus {
	std::string id;
	std::string name;
	OutputState state = OutputState::Idle;
	/* Empty unless Failed. */
	std::string detail;

	/* Measured from bytes actually sent between polls, not from the configured
	 * bitrate. The configured number is what was asked for, and the gap between
	 * the two is the whole question when frames start dropping. */
	int bitrate_kbps = 0;
	int dropped_frames = 0;
	int total_frames = 0;
	int reconnects = 0;
	int uptime_sec = 0;

	double drop_percent() const
	{
		return total_frames > 0 ? (100.0 * dropped_frames) / total_frames : 0.0;
	}
};

/* Call on OBS_FRONTEND_EVENT_STREAMING_STARTED, not before: an output sharing
 * the main encoder needs it to exist. */
void start_outputs();

void stop_outputs();

/* Start or stop one destination mid-stream, so a platform that is failing can
 * be dropped without ending the broadcast. */
bool start_one(const std::string &id);
void stop_one(const std::string &id);

/* Whether the main stream is up, which decides whether start_one can do
 * anything. */
bool streaming_live();

/* Whether this build can do vertical at all, for the dock to say so rather than
 * offering a tick box that quietly does nothing. */
constexpr bool vertical_supported()
{
	return PLASMASTREAM_HAS_CANVAS;
}

/* A copy, so the dock cannot release something the streaming thread is using. */
std::vector<OutputStatus> output_statuses();

} // namespace plasmastream
