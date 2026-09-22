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

/* Three things a streamer only ever finds out about afterwards.
 *
 * The microphone was muted the whole segment. Nothing at all was going out. The
 * stream was still running an hour after they raided out and walked away. Each
 * of them is obvious from inside OBS while it is happening and expensive to
 * discover from a clip the next day.
 *
 * ## Only the streamer sees it
 *
 * These appear in this plugin's dock, which is a panel in the streamer's own OBS
 * window. Nothing is drawn on the stream, nothing is sent to PlasmaStream or
 * anywhere else, and nothing is written down.
 *
 * ## Being wrong is the whole risk
 *
 * A warning that fires when nothing is wrong is worse than no warning at all,
 * because somebody who is told once that they are muted when they are not stops
 * reading the panel, including on the night it is right. So every one of these
 * waits for a condition that has held for a while rather than one that is true
 * right now, all three are off until they are asked for, and each has its own
 * switch. The thresholds and the reasoning behind each of them are at the top of
 * warnings.cpp.
 *
 * Called "Stream check" in the dock, because that is what it is from the
 * streamer's side: a thing quietly checking the stream while they get on with
 * it. */

enum class WarningKind {
	/* The chosen microphone has been muted in OBS long enough that it is
	 * probably not deliberate. */
	MicMuted,
	/* Nothing above the floor anywhere in the mix: no voice, no game, no
	 * music. */
	Silent,
	/* Still live, with nothing happening at all, long past the point where a
	 * stream that had ended would have been stopped. */
	Forgotten,
};

struct Warning {
	WarningKind kind = WarningKind::MicMuted;

	/* How long the condition has held. The dock says this out loud, because
	 * "for two minutes" is the part that makes a warning believable rather than
	 * something to wave away. */
	int seconds = 0;

	/* The microphone's name, for MicMuted. Empty for the others. */
	std::string microphone;
};

struct WarningsStatus {
	/* At least one of the three is switched on. */
	bool watching = false;
	bool streaming = false;

	/* The chosen microphone is not in this scene collection, so the mute
	 * warning cannot do anything. Only ever set when that warning is on. */
	bool microphone_missing = false;
	std::string microphone;

	/* Worst first, and empty when there is nothing to say. */
	std::vector<Warning> warnings;
};

/* Starts or stops watching to match the config. UI thread. Call it after
 * changing config().warnings. */
void warnings_apply();

/* Whether OBS is broadcasting, from the frontend STREAMING_STARTED and
 * STREAMING_STOPPING events. None of the clocks run off air: a stream nobody is
 * watching cannot be a stream anybody is missing. */
void warnings_streaming(bool active);

/* Somebody is at the desk. From the frontend SCENE_CHANGED event, which is the
 * clearest sign of a person available from here. */
void warnings_activity();

/* Also where the microphone's mute state is sampled, so call it from the poll
 * that draws the dock rather than only when something needs to be shown. UI
 * thread, once a second; it never blocks. */
WarningsStatus warnings_status();

/* Drops the audio tap and the microphone. Safe to call twice. */
void warnings_shutdown();

} // namespace plasmastream
