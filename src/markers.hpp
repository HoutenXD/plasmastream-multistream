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

#include <obs-frontend-api.h>

#include <cstdint>
#include <string>

namespace plasmastream {

/* Chapter markers in recordings.
 *
 * A four hour recording is edited by scrubbing for the raid, the gift bomb, the
 * point the game changed. PlasmaStream's bot already sees each of those as it
 * happens and writes it down; while anything is recording, this collects them
 * and marks them in every recording running at the time, along with markers
 * added from the hotkey and the dock.
 *
 * Two places each, because they are read by different things:
 *
 *  - A chapter inside the file, when the file is a Hybrid MP4, the one format
 *    OBS can write chapters into. Players show them, and so do some editors.
 *  - A line in a chapters file beside the recording, whatever its format, as
 *    "12:34 Raid: Mr_Dino_Saur_, 42 viewers". This is the one that works with an
 *    mkv, and the one that pastes straight into a YouTube description.
 *
 * The chapter in the file goes in when the marker arrives, because OBS can only
 * add one at the current moment. The line in the chapters file goes at the moment
 * the thing actually happened, a few seconds earlier for anything from the bot. */

struct MarkerStatus {
	/* Something is recording and markers are being collected for it. */
	bool collecting = false;

	/* Markers added since the recordings running now began. */
	int added = 0;

	/* The last one, for the dock to show. */
	std::string last;

	/* Why markers from PlasmaStream are not arriving, when they are not. Empty
	 * when they are, or when nothing is asking for them. */
	std::string problem;
};

/* Registers the hotkey. Call once from obs_module_load. */
void markers_init();

/* OBS keeps no copy of a plugin's key binding, so it is saved at exit. */
void markers_save_hotkey();

/* Stops the collector. Safe to call more than once. */
void markers_shutdown();

/* OBS's own recording starting, stopping, pausing and resuming. Everything else
 * is ignored, so every frontend event can be handed over. */
void markers_frontend_event(enum obs_frontend_event event);

/* A marker somebody asked for, at this moment, in every recording running. False
 * when nothing is recording to receive it. */
bool markers_add_now();

MarkerStatus markers_status();

/* Settings or the PlasmaStream key changed. */
void markers_apply();

/* "12:34" under an hour and "1:02:34" past it: how YouTube writes a chapter. */
std::string chapter_timestamp(uint64_t offset_ms);

} // namespace plasmastream
