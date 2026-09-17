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

#include <cstdint>
#include <string>

namespace plasmastream {

/* Voice commands.
 *
 * The streamer says their wake phrase and a phrase ("Jarvis, set my game to
 * Hades"), and PlasmaStream runs the command that phrase stands for, exactly as
 * the dashboard's voice page does. The difference is where the listening
 * happens: here, in OBS, on the microphone OBS already has, so there is no tab
 * to keep open and nothing a browser can throttle.
 *
 * ## What leaves this computer
 *
 * Speech becomes text HERE, with an open model running on this machine. The wake
 * phrase is matched here too, and only the words after it are sent. Everything
 * else the microphone hears is turned into text, shown in the dock for setting
 * up, and dropped. Holding the push-to-talk key sends what is said while it is
 * held, because holding it is the streamer saying "this is a command".
 *
 * ## It works offline
 *
 * Nothing here waits for a stream to start. A streamer can set it up, say a
 * phrase and watch it run long before going live. */

enum class VoiceState {
	/* Switched off in the plugin. */
	Off,
	/* This build, or this computer, cannot run it. detail says why. */
	Unavailable,
	/* Loading the speech model, which takes about a second. */
	Loading,
	/* On, but the chosen microphone is not in this scene collection. */
	NoMicrophone,
	Listening,
	/* Something went wrong loading or running. detail says what. */
	Error,
};

struct VoiceStatus {
	VoiceState state = VoiceState::Off;
	std::string detail;

	/* The microphone being listened to, or the one that could not be found. */
	std::string microphone;

	/* A plugin key is set, so commands have somewhere to go. */
	bool linked = false;
	/* What the website said: voice switched on there, and the wake phrase.
	 * phrase is empty until the website has answered. */
	bool website_on = true;
	std::string phrase;
	/* Why the website could not be asked, when it could not. */
	std::string website_problem;

	/* Listening for the wake phrase all the time, as opposed to push-to-talk
	 * only. */
	bool wake = true;
	bool push_to_talk_held = false;

	/* The last thing the model heard, for setting up. Shown in the dock and
	 * never sent unless it carried the wake phrase. */
	std::string heard;
	/* What became of the last line sent, or why it was not sent. */
	std::string outcome;
};

/* Registers the push-to-talk hotkey. From obs_module_load. */
void voice_init();

/* Starts, stops or restarts listening to match the config. UI thread. Call it
 * after changing config().voice or the plugin key. */
void voice_apply();

/* Finds the microphone again after the scene collection changed. Cheap to call
 * often: it only looks when the source it had is gone, and not more than every
 * few seconds. UI thread. */
void voice_reattach();

/* Asks the website for the wake phrase again, e.g. after the key changed. */
void voice_refresh_website();

/* OBS does not save a plugin's frontend hotkey, so this writes the binding into
 * the plugin's config. From the EXIT event. */
void voice_save_hotkey();

/* Stops listening and unloads the model. Safe to call twice. */
void voice_shutdown();

VoiceStatus voice_status();

/* normalizeSpoken and voiceAfterWake from db/src/voice.ts, in C++. The website
 * normalizes again, so these only have to agree with it about WHERE the wake
 * phrase is, which is what decides what gets sent. */
std::string voice_normalize(const std::string &text);
bool voice_after_wake(const std::string &phrase, const std::string &heard, std::string &after);

} // namespace plasmastream
