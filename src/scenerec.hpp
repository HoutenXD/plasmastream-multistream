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

#include "outputs.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace plasmastream {

/* What the recording is doing, for the dock to show. */
struct SceneRecordingStatus {
	bool running = false;

	/* The scene being recorded, and the file being written. Empty when idle. */
	std::string scene;
	std::string file;

	int elapsed_sec = 0;
	uint64_t bytes = 0;

	/* Why the last recording stopped, when it stopped on its own. Empty
	 * otherwise, including after a normal stop. */
	std::string error;
};

/* Record one scene while you stream another.
 *
 * The usual shape of this is a stream scene carrying a chat box, alerts and
 * whatever else the live audience wants, and a recording scene without any of
 * it, because none of that belongs in a video somebody watches next week. OBS
 * records the programme, so today you have to choose.
 *
 * This is the vertical canvas trick pointed at a different problem: a second
 * canvas at the same size as your stream, showing a scene you pick rather than
 * the one on programme, with a file output reading it. The main stream never
 * knows about it.
 *
 * It has to encode separately, for the same reason vertical does. OBS's own
 * recording encoder is tied to the main canvas and cannot be asked for a
 * different picture. */

#if PLASMASTREAM_HAS_CANVAS

/* Starts recording the configured scene. False if it is off, has no scene, or
 * the scene has since been deleted; the reason is in the log. */
bool scene_recording_start();

void scene_recording_stop();

bool scene_recording_active();

/* The file currently being written, for the dock to show. Empty when idle. */
std::string scene_recording_file();

/* Also where a recording that stopped itself gets cleaned up, so call it from
 * the poll that draws the dock rather than only when something is on screen. */
SceneRecordingStatus scene_recording_status();

/* Whether this build of OBS can do it at all. False everywhere before 31.1,
 * where there are no canvases to record. */
bool scene_recording_supported();

/* Sounds in a scene that its recording will not have.
 *
 * The recording's sound is OBS's own mix, on the track the dialog picks: the
 * same mix the stream sends. OBS mixes only what is on the scene being streamed,
 * plus the microphone and desktop audio. Recording a scene makes its sources run,
 * and running is not being mixed: a sound that sits only in the recorded scene
 * plays into nothing, so it is missing from the recording and from the stream.
 * Measured in OBS 31.1 on 2026-09-15 by recording the mix as PCM while a tone
 * played only in the recorded scene: the tone's meter moved and the mix was
 * silent. It used to be the other way round here, a warning that such a sound
 * would leak onto the stream, and that was never true.
 *
 * So these are the audible sources in the scene that are on no other scene at
 * all, since nothing can put those on the stream and so nothing can put them in
 * the recording. A source that is also on another scene, which is how people
 * build this, is heard whenever that scene is live. A scene nested inside this
 * one counts as another scene too, because it can be put on stream by itself:
 * this would rather miss a warning than give one about a setup that works. */
std::vector<std::string> scene_audio_missing_from_recording(const std::string &scene);

/* Borrowed, for a preview. Null unless a recording is running. */
obs_canvas_t *scene_recording_canvas();

/* Released at module unload, before libobs takes the graphics subsystem down. */
void scene_recording_shutdown();

#else

inline bool scene_recording_start() { return false; }
inline void scene_recording_stop() {}
inline bool scene_recording_active() { return false; }
inline std::string scene_recording_file() { return {}; }
inline SceneRecordingStatus scene_recording_status() { return {}; }
inline bool scene_recording_supported() { return false; }
inline std::vector<std::string> scene_audio_missing_from_recording(const std::string &) { return {}; }
inline void scene_recording_shutdown() {}

#endif

} // namespace plasmastream
