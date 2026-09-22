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

/* Where the wide programme sits inside the tall frame, for one programme scene.
 *
 * A fraction of the canvas rather than pixels, so changing the vertical size
 * from 1080x1920 to 720x1280 does not throw the arrangement away.
 *
 * Per scene because that is how people actually stream: a full-bleed crop is
 * right for gameplay and wrong for a talking head, and nobody wants to redo it
 * every time they switch. A scene with no entry gets the default. */
struct Framing {
	/* The programme scene this describes. Empty is the fallback for any scene
	 * nobody has arranged yet. */
	std::string scene;

	double x = 0.0;
	double y = 0.0;
	double width = 1.0;
	double height = 1.0;

	/* Fill the box and lose what overflows, or fit inside it and accept bars. */
	bool crop = true;
};

/* One of your own sources, placed on the vertical canvas.
 *
 * Named rather than owned: these are the sources already in your scene
 * collection, your camera and your chat overlay, borrowed onto the tall frame so
 * they can sit where a phone wants them instead of where a monitor does. OBS
 * lets a source live in several scenes at once, and this is that.
 *
 * The transform is in canvas pixels, not fractions, because it is edited through
 * OBS's own transform dialog and that dialog speaks pixels. Scaled if the canvas
 * size changes, which is the only time it would otherwise drift off the frame. */
struct VerticalSource {
	std::string name;

	double x = 0.0;
	double y = 0.0;
	double width = 0.0;
	double height = 0.0;

	/* Mirrors obs_bounds_type. Kept as an int so config.hpp does not have to
	 * pull in all of libobs for one enum. */
	int bounds_type = 0;
	bool visible = true;
};

/* Record one scene while streaming another.
 *
 * A stream scene carries the chat box and the alerts; a recording scene carries
 * none of it, because none of that belongs in a video somebody watches next
 * week. OBS records the programme, so without this you choose one. */
struct SceneRecording {
	bool enabled = false;

	/* The scene to record. Empty means nothing has been picked yet. */
	std::string scene;

	/* Where the files go. Empty means the user's Videos folder. */
	std::string folder;

	/* Start and stop it with the stream, which is what somebody wants who set
	 * this up to get a clean copy of a broadcast. */
	bool with_stream = true;

	int video_bitrate = 8000;
	/* Empty means the same kind of encoder the main stream is using. */
	std::string encoder_id;

	/* Container. mkv survives a crash with the footage intact, which is why OBS
	 * recommends it; mp4 goes straight into an editor. */
	std::string format = "mkv";

	/* Which of OBS's six audio tracks to record, 1 based. Track 1 is what the
	 * stream hears; a different one is how people keep music off the recording. */
	int audio_track = 1;
};

/* Voice commands: the streamer talks, and the words after the wake phrase go to
 * PlasmaStream, which runs the command they match.
 *
 * Everything that decides WHAT runs lives on the website: the wake phrase, the
 * phrases and the commands behind them. Only what belongs to this computer is
 * kept here: which microphone, how to listen, and how hard to work at it. */
struct VoiceConfig {
	bool enabled = false;

	/* The microphone, as an OBS audio source. The UUID is what finds it, so a
	 * rename does not lose it; the name is kept for showing and as a fallback
	 * for a source recreated with the same name in another collection. */
	std::string source_uuid;
	std::string source_name;

	/* Listen for the wake phrase all the time. Off leaves push-to-talk as the
	 * only way in, which costs nothing until the key is held. */
	bool wake = true;

	/* "fast" or "accurate": which speech model. Fast is smaller and was the
	 * better of the two on short commands in testing. */
	std::string model = "fast";

	/* Keep what the microphone heard, not only the commands in it.
	 *
	 * Off, and it stays off until it is switched on in two places: here, and on
	 * the PlasmaStream dashboard. Either one off and nothing leaves this
	 * computer, and nothing at all is sent while OBS is not streaming.
	 *
	 * With both on, the lines this plugin already decodes to find the wake
	 * phrase in are sent to PlasmaStream while live, where they are kept for
	 * fourteen days so the streamer's co-host can answer questions about what
	 * was said. See db/src/transcript.ts. */
	bool transcript = false;

	/* OBS's own saved form of the push-to-talk key, as JSON. OBS does not save
	 * a frontend hotkey's binding for a plugin, so this does. */
	std::string hotkey;
};

/* Private warnings in the dock: a muted microphone, dead air, a stream still
 * running after everybody went home.
 *
 * Three switches rather than one, because they are three different risks and
 * wanting one is no reason to want the others. Somebody streaming a silent art
 * timelapse wants to be told when their microphone is muted and would be told
 * about dead air every minute of every stream.
 *
 * All of them off. A warning nobody asked for is one they will learn to scroll
 * past, and these are only worth anything if they are still being read on the
 * night one of them is right. */
struct StreamWarnings {
	/* The chosen microphone is muted in OBS while the stream is live. */
	bool mic_muted = false;
	/* Nothing audible anywhere in the mix for a sustained stretch. */
	bool silence = false;
	/* Still live long after anything last happened. */
	bool forgotten = false;

	/* The microphone to watch, as an OBS audio source. UUID first with the name
	 * as a fallback, for the same reasons voice keeps both.
	 *
	 * Its own pick rather than a borrow of voice.source_uuid, so that watching a
	 * microphone does not quietly mean switching voice commands on as well.
	 * The dialog offers voice's choice as the default, which is where the two
	 * agreeing is worth something. */
	std::string source_uuid;
	std::string source_name;
};

/* Staying current.
 *
 * Checking is on because a plugin that holds stream keys and talks to three
 * platforms is one nobody should be running a year-old copy of without being
 * told. Installing on its own is off, because replacing files under somebody
 * mid-stream, unasked, is not a thing to do by default: the first time, they
 * press the button and see what it does. */
struct UpdateConfig {
	bool check = true;
	bool automatic = false;
};

struct Destination {
	std::string id;
	std::string name;
	std::string platform;
	std::string url;
	/* Never leaves this machine. The website has no field for one. */
	std::string key;
	bool enabled = true;

	/* Off means share the main stream's encoder, which costs no CPU but sends
	 * everyone the same bitrate. On makes a second encoder for this
	 * destination: the fix for an upload that cannot carry two full streams. */
	bool own_encoder = false;
	/* kbps. Only read when own_encoder is set. */
	int video_bitrate = 2500;
	int audio_bitrate = 160;
	/* Empty means whatever OBS is already using for the main stream. */
	std::string encoder_id;

	/* Encode from the vertical canvas rather than the main one, for the
	 * platforms that only take portrait. It has to encode separately whatever
	 * else is set, because the main stream's encoder is tied to the main canvas.
	 *
	 * The SHAPE of that frame is not here: there is one vertical canvas, so its
	 * size and framing belong to the config rather than to each destination
	 * that happens to point at it. */
	bool vertical = false;

	bool usable() const { return enabled && !url.empty() && !key.empty(); }
};

struct Config {
	std::vector<Destination> destinations;
	/* PlasmaStream account token. Optional: the plugin works without one. */
	std::string token;
	bool sync_on_launch = true;

	/* Applied to every output. OBS's own default is 20 retries, 10s apart. */
	int reconnect_retries = 20;
	int reconnect_delay_sec = 5;

	/* The vertical canvas. One of it, so one size. */
	int vertical_width = 1080;
	int vertical_height = 1920;
	/* Fill the frame and lose the sides, or fit the whole thing between bars.
	 * The default for any scene with no framing of its own. */
	bool vertical_crop = true;

	/* Arranged scenes, in no particular order. Looked up by name. */
	std::vector<Framing> framings;

	/* Layered over the programme block, bottom of the list first. */
	std::vector<VerticalSource> vertical_sources;

	/* Record a scene other than the one going out. */
	SceneRecording scene_recording;

	/* Voice commands through PlasmaStream. */
	VoiceConfig voice;

	/* Warnings only the streamer sees. */
	StreamWarnings warnings;

	/* Whether to look for a newer plugin, and whether to fetch it unasked. */
	UpdateConfig update;

	/* Whether the Canvases dock has ever been shown. OBS starts every dock
	 * hidden and most people never find the Docks menu, so it gets opened once
	 * and then never touched again. */
	bool canvases_introduced = false;
};

/* The framing for a scene, or a default one covering the whole frame. Never
 * fails: a scene nobody has touched is a scene framed the old way. */
Framing framing_for(const std::string &scene);

/* Replaces the entry for framing.scene, or adds one. Does not save. */
void set_framing(const Framing &framing);

Config &config();

/* Both run during module load, so neither throws. A missing or unreadable file
 * just means defaults. */
void load_config();
void save_config();

std::string config_path();

} // namespace plasmastream
