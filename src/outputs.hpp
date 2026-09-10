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

namespace plasmastream {

/** What a destination is doing right now, for the dock to show. */
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
	/** Why it failed, in words a streamer can act on. Empty unless Failed. */
	std::string detail;
};

/**
 * Start every usable destination, borrowing the main stream's encoders.
 *
 * Called when OBS reports that streaming has started, and not before: the
 * encoders this attaches to do not exist until then.
 *
 * Sharing the encoder rather than making new ones is the whole reason this is
 * cheap. The frames are already compressed for the main destination, so a second
 * one costs upload bandwidth and almost no CPU. The trade is that every
 * destination receives identical settings, which is the right default and is why
 * the website says so plainly.
 */
void start_outputs();

/** Stop and release everything. Safe to call when nothing is running. */
void stop_outputs();

/** A snapshot for the UI. Copies rather than exposing the outputs themselves,
 *  so the dock cannot release something the streaming thread is using. */
std::vector<OutputStatus> output_statuses();

} // namespace plasmastream
