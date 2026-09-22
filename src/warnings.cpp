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

#include "warnings.hpp"

#include "config.hpp"

#include <obs-module.h>
#include <media-io/audio-io.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace plasmastream {

namespace {

/* ---- The three thresholds -------------------------------------------------- */

/* How long the microphone has to stay muted before it is worth saying anything.
 *
 * Mutes people mean are short. A cough, a sip, a sneeze, a word to somebody in
 * the room: seconds each, and the long end of them is still under a minute. A
 * mute nobody meant has no end at all. Ninety seconds clears every deliberate
 * one with room to spare and still catches the accident inside the first two
 * minutes, which is early enough that the segment is worth keeping.
 *
 * Shorter would be better if it were free, and it is not. At thirty seconds this
 * fires on somebody blowing their nose, and the first time it does that the
 * streamer learns the panel is wrong and stops reading it. */
constexpr int kMutedSeconds = 90;

/* Below this, nothing on the stream is audible.
 *
 * -50 dBFS, about a thousandth of full scale, which nobody hears with their
 * volume at any normal setting. Peak rather than average, so a single cough or
 * one game sound anywhere in the window is enough to call the stream not silent:
 * that errs towards saying nothing, which is the cheap mistake here.
 *
 * It also means a room with a noisy fan and no gate sits above the floor and
 * never gets this warning at all. That is the right trade. The case worth
 * catching is the one where the audio is not merely quiet but gone, because the
 * device was unplugged, the wrong one was picked, or the track was never
 * assigned, and all of those produce something far below this. */
constexpr float kSilenceFloor = 0.00316f;

/* How long the whole mix has to stay under the floor.
 *
 * Pauses in speech run a second or two. A streamer reading chat, lining up a
 * shot or just concentrating goes about half a minute without making a sound,
 * and that is the longest silence a stream produces on purpose. Twice it is the
 * smallest window that cannot fire on somebody who is simply busy.
 *
 * Deliberately tens of seconds rather than minutes: past a minute of dead air
 * the segment is already unwatchable, so waiting longer to be even surer buys
 * nothing and costs the streamer the chance to fix it. */
constexpr int kSilentSeconds = 60;

/* Still live, with nothing happening at all, for this long.
 *
 * Uptime by itself is the wrong signal. People stream for twelve hours on
 * purpose, and a warning that fires on every marathon is one that gets ignored
 * on the single night it is right. What actually separates "forgot to stop"
 * from "still streaming quietly" is that somebody who is still there makes a
 * noise or touches OBS, and somebody who raided out and walked away never does
 * either again.
 *
 * So this counts from the last sound above the floor, the last scene change and
 * the last time the mute button moved, and half an hour of none of them means
 * nobody is at the desk. A quiet streamer resets it constantly without trying,
 * and nobody sits in front of a live stream for thirty minutes in total silence
 * without so much as changing scene. */
constexpr int kForgottenSeconds = 30 * 60;

/* ---- State ----------------------------------------------------------------- */

/* Written by OBS's audio thread and read by the UI, which is the whole of the
 * traffic between them. */
std::atomic<uint64_t> g_last_sound_ns{0};

/* UI thread only. */
bool g_tapped = false;
bool g_streaming = false;
obs_weak_source_t *g_weak_mic = nullptr;
uint64_t g_last_look_ns = 0;

/* When the current run of mute started, or zero when it is not muted. */
uint64_t g_muted_since_ns = 0;

/* The last sign of a person: a scene change, the mute button moving, or the
 * settings dialog being used. Sound counts too, but that one is timed
 * separately because the silence warning wants it on its own. */
uint64_t g_activity_ns = 0;

/* Whether the microphone was muted at the previous look, so a change can be
 * spotted. Minus one means there has not been a previous look, which is where
 * every stream starts and is not a change from anything. */
int g_was_muted = -1;

/* Whole seconds since a monotonic timestamp, never negative: the audio thread
 * can stamp a few microseconds ahead of the poll that reads it. */
int elapsed_sec(uint64_t now, uint64_t then)
{
	return then >= now ? 0 : static_cast<int>((now - then) / 1000000000ULL);
}

/* ---- Listening to the mix -------------------------------------------------- */

/* OBS's audio thread, forty-odd times a second.
 *
 * The only question asked here is whether this block had any sound in it, and it
 * stops looking the moment it finds some, so a stream with audio costs a handful
 * of comparisons and a silent one costs a scan of a few thousand floats. One
 * timestamp goes out. Everything that decides what any of it means happens on
 * the UI thread in warnings_status, a second at a time. */
void on_mix(void *, size_t, struct audio_data *data)
{
	if (!data || data->frames == 0) {
		return;
	}

	const audio_t *audio = obs_get_audio();

	if (!audio) {
		return;
	}

	const size_t channels = std::min<size_t>(audio_output_get_channels(audio), MAX_AV_PLANES);

	for (size_t channel = 0; channel < channels; channel++) {
		const float *samples = static_cast<const float *>(static_cast<void *>(data->data[channel]));

		if (!samples) {
			continue;
		}

		/* Every channel, not just the first: a source panned hard to one side
		 * is still the stream making a sound. */
		for (uint32_t i = 0; i < data->frames; i++) {
			if (std::fabs(samples[i]) > kSilenceFloor) {
				g_last_sound_ns.store(os_gettime_ns(), std::memory_order_relaxed);
				return;
			}
		}
	}
}

void tap_mix(bool on)
{
	if (on == g_tapped) {
		return;
	}

	if (on) {
		/* The clock starts now rather than at whenever a sound was last heard.
		 * Switching this on mid-stream must not hand the silence warning a head
		 * start over a stretch it was not watching. */
		g_last_sound_ns.store(os_gettime_ns(), std::memory_order_relaxed);

		/* Mix zero is the one the stream goes out on. No conversion asked for:
		 * the master mix is already float planar at OBS's own rate, and asking
		 * for anything else would put a resampler in the path for a reader that
		 * only wants to know whether it is zero. */
		obs_add_raw_audio_callback(0, nullptr, on_mix, nullptr);
	} else {
		obs_remove_raw_audio_callback(0, on_mix, nullptr);
	}

	g_tapped = on;
}

/* ---- The microphone -------------------------------------------------------- */

void forget_microphone()
{
	if (!g_weak_mic) {
		return;
	}

	obs_weak_source_release(g_weak_mic);
	g_weak_mic = nullptr;
	g_last_look_ns = 0;
}

/* The chosen microphone, or null. The caller releases it.
 *
 * Found by UUID with the name as a fallback, the same way voice finds its own: a
 * rename keeps the UUID, and a source rebuilt in another scene collection keeps
 * the name. Kept as a weak reference between polls so the usual case costs a
 * pointer rather than a search, and a collection that does not have it is only
 * searched every few seconds, since it is not going to grow one in the meantime. */
obs_source_t *microphone()
{
	if (g_weak_mic) {
		obs_source_t *source = obs_weak_source_get_source(g_weak_mic);

		if (source && !obs_source_removed(source)) {
			return source;
		}

		obs_source_release(source);
		obs_weak_source_release(g_weak_mic);
		g_weak_mic = nullptr;
	}

	const uint64_t now = os_gettime_ns();

	if (g_last_look_ns != 0 && now - g_last_look_ns < 3000000000ULL) {
		return nullptr;
	}

	g_last_look_ns = now;

	const StreamWarnings &settings = config().warnings;
	obs_source_t *source = nullptr;

	if (!settings.source_uuid.empty()) {
		source = obs_get_source_by_uuid(settings.source_uuid.c_str());
	}

	if (!source && !settings.source_name.empty()) {
		source = obs_get_source_by_name(settings.source_name.c_str());
	}

	if (source) {
		g_weak_mic = obs_source_get_weak_source(source);
	}

	return source;
}

} // namespace

void warnings_apply()
{
	const StreamWarnings &settings = config().warnings;

	/* Whoever called this either just came out of the settings dialog or just
	 * went live, and both of those are a person at the desk. */
	g_activity_ns = os_gettime_ns();

	/* The tap costs a scan of the mix on OBS's audio thread, so it exists only
	 * while a warning that reads it is switched on and OBS is actually live. */
	tap_mix(g_streaming && (settings.silence || settings.forgotten));

	if (!settings.mic_muted) {
		forget_microphone();
	}
}

void warnings_streaming(bool active)
{
	if (active == g_streaming) {
		return;
	}

	g_streaming = active;

	/* Nothing survives the edge of a stream. A microphone muted before going
	 * live has been muted for zero seconds as far as this is concerned, and a
	 * warning left over from last night should not be the first thing on screen
	 * tonight. */
	g_muted_since_ns = 0;
	g_was_muted = -1;

	warnings_apply();
}

void warnings_activity()
{
	/* Nobody changes scene by accident, which is what makes it worth as much as
	 * a sound for telling whether anyone is still there. */
	g_activity_ns = os_gettime_ns();
}

WarningsStatus warnings_status()
{
	const StreamWarnings &settings = config().warnings;

	WarningsStatus status;
	status.watching = settings.mic_muted || settings.silence || settings.forgotten;
	status.streaming = g_streaming;

	if (!status.watching) {
		g_muted_since_ns = 0;
		g_was_muted = -1;
		return status;
	}

	const uint64_t now = os_gettime_ns();
	bool muted = false;

	if (settings.mic_muted) {
		obs_source_t *source = microphone();

		if (!source) {
			/* Reported off air as well as on, because the point of knowing
			 * that the microphone has gone is to fix it before the stream
			 * rather than to read about it afterwards. */
			status.microphone_missing = true;
			status.microphone = settings.source_name;
		} else {
			const char *name = obs_source_get_name(source);

			status.microphone = name ? name : settings.source_name;
			muted = obs_source_muted(source);
			obs_source_release(source);
		}
	}

	if (!g_streaming) {
		g_muted_since_ns = 0;
		g_was_muted = -1;
		return status;
	}

	if (!muted) {
		g_muted_since_ns = 0;
	} else if (g_muted_since_ns == 0) {
		g_muted_since_ns = now;
	}

	/* Reaching for the mute button is somebody at the desk whichever way they
	 * pressed it, so muting counts as a sign of life even while it is the thing
	 * being warned about. */
	const int state = muted ? 1 : 0;

	if (g_was_muted >= 0 && state != g_was_muted) {
		g_activity_ns = now;
	}

	g_was_muted = state;

	const uint64_t sound = g_last_sound_ns.load(std::memory_order_relaxed);
	const int quiet = elapsed_sec(now, sound);
	const int idle = elapsed_sec(now, std::max(sound, g_activity_ns));

	if (settings.forgotten && idle >= kForgottenSeconds) {
		Warning warning;
		warning.kind = WarningKind::Forgotten;
		warning.seconds = idle;
		status.warnings.push_back(warning);
	}

	if (settings.mic_muted && g_muted_since_ns != 0) {
		const int held = elapsed_sec(now, g_muted_since_ns);

		if (held >= kMutedSeconds) {
			Warning warning;
			warning.kind = WarningKind::MicMuted;
			warning.seconds = held;
			warning.microphone = status.microphone;
			status.warnings.push_back(warning);
		}
	}

	/* Last, and only when neither of the others has already said it. A muted
	 * microphone on a stream with no other sound IS silence, and a stream
	 * nobody has touched for half an hour is silence with a better explanation
	 * attached. Two lines saying one thing is how a panel stops being read. */
	if (settings.silence && status.warnings.empty() && quiet >= kSilentSeconds) {
		Warning warning;
		warning.kind = WarningKind::Silent;
		warning.seconds = quiet;
		status.warnings.push_back(warning);
	}

	return status;
}

void warnings_shutdown()
{
	tap_mix(false);
	forget_microphone();
	g_streaming = false;
}

} // namespace plasmastream
