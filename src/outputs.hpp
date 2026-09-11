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
};

/* Call on OBS_FRONTEND_EVENT_STREAMING_STARTED, not before: the encoders these
 * borrow do not exist until the main stream is running. Sharing them is what
 * makes an extra destination cost upload and almost no CPU, at the price of
 * every destination getting identical settings. */
void start_outputs();

void stop_outputs();

/* A copy, so the dock cannot release something the streaming thread is using. */
std::vector<OutputStatus> output_statuses();

} // namespace plasmastream
