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

#include "trackmix.hpp"

#include <algorithm>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>

#include <media-io/audio-io.h>

namespace plasmastream {

namespace {

/* One track's mix for one moment, as OBS made it: every channel's samples, one
 * plane after another. */
struct Chunk {
	uint64_t timestamp = 0;
	uint32_t frames = 0;
	std::vector<float> samples;
};

struct Mixer;

/* What a raw audio callback is handed: which mixer, and which of its queues. */
struct Tap {
	Mixer *mixer = nullptr;
	size_t slot = 0;
	size_t mix_idx = 0;
};

/* About a second of sound at OBS's usual rates. The queues only grow past a
 * chunk or two if this module's audio thread stops being scheduled; past this
 * the oldest goes, so a stall costs a skip rather than memory without end. */
constexpr size_t MAX_QUEUED = 48;

struct Mixer {
	uint32_t mask = 0;
	int refs = 0;
	audio_t *audio = nullptr;
	size_t planes = 2;

	/* Guards the queues. Never held while calling into libobs: the raw audio
	 * callbacks run under libobs' own input lock and take this one. */
	std::mutex lock;
	std::vector<std::deque<Chunk>> queues;
	std::vector<std::unique_ptr<Tap>> taps;
	bool listening = false;
	bool warned_overflow = false;
};

/* Guards the list. The audio threads never take it. */
std::mutex g_lock;
std::vector<std::unique_ptr<Mixer>> g_mixers;

/* A track's finished mix, from OBS's audio thread. Copied and queued; the sum
 * is made on the mixer's own thread. */
void on_track_audio(void *param, size_t, struct audio_data *data)
{
	auto *tap = static_cast<Tap *>(param);

	if (!tap || !data || data->frames == 0) {
		return;
	}

	Mixer &mixer = *tap->mixer;

	Chunk chunk;
	chunk.timestamp = data->timestamp;
	chunk.frames = data->frames;
	chunk.samples.assign(mixer.planes * data->frames, 0.0f);

	for (size_t plane = 0; plane < mixer.planes; plane++) {
		if (data->data[plane]) {
			std::memcpy(&chunk.samples[plane * data->frames], data->data[plane],
				    data->frames * sizeof(float));
		}
	}

	std::lock_guard<std::mutex> hold(mixer.lock);
	std::deque<Chunk> &queue = mixer.queues[tap->slot];
	queue.push_back(std::move(chunk));

	if (queue.size() > MAX_QUEUED) {
		queue.pop_front();

		if (!mixer.warned_overflow) {
			mixer.warned_overflow = true;
			blog(LOG_WARNING, "[plasmastream] track mix %u fell behind; dropped audio to catch up",
			     mixer.mask);
		}
	}
}

/* The mixer's own audio thread asking for its next block.
 *
 * Only answers once every track has the same moment queued, then answers with
 * that moment's timestamp rather than the time asked about, which is what keeps
 * the sum on OBS's timeline. A track whose queue starts earlier than the others
 * (it began listening a block sooner) has its extra blocks dropped once, at the
 * start. Answering false skips the tick and loses nothing: the next one takes
 * the block. */
bool on_mix_input(void *param, uint64_t, uint64_t, uint64_t *new_ts, uint32_t active_mixers,
		  struct audio_output_data *mixes)
{
	auto *mixer = static_cast<Mixer *>(param);

	if (!mixer) {
		return false;
	}

	std::lock_guard<std::mutex> hold(mixer->lock);

	if (mixer->queues.empty()) {
		return false;
	}

	for (;;) {
		uint64_t latest = 0;

		for (const std::deque<Chunk> &queue : mixer->queues) {
			if (queue.empty()) {
				return false;
			}

			latest = std::max(latest, queue.front().timestamp);
		}

		bool aligned = true;

		for (std::deque<Chunk> &queue : mixer->queues) {
			while (!queue.empty() && queue.front().timestamp < latest) {
				queue.pop_front();
				aligned = false;
			}
		}

		if (aligned) {
			break;
		}
	}

	const uint64_t timestamp = mixer->queues.front().front().timestamp;

	for (size_t mix = 0; mix < MAX_AUDIO_MIXES; mix++) {
		if ((active_mixers & (1u << mix)) == 0) {
			continue;
		}

		for (size_t plane = 0; plane < mixer->planes; plane++) {
			float *out = mixes[mix].data[plane];

			if (!out) {
				continue;
			}

			for (const std::deque<Chunk> &queue : mixer->queues) {
				const Chunk &chunk = queue.front();
				const size_t frames = std::min<size_t>(chunk.frames, AUDIO_OUTPUT_FRAMES);
				const float *in = &chunk.samples[plane * chunk.frames];

				for (size_t i = 0; i < frames; i++) {
					out[i] += in[i];
				}
			}
		}
	}

	for (std::deque<Chunk> &queue : mixer->queues) {
		queue.pop_front();
	}

	*new_ts = timestamp;
	return true;
}

void start_listening(Mixer &mixer)
{
	if (mixer.listening) {
		return;
	}

	{
		std::lock_guard<std::mutex> hold(mixer.lock);

		for (std::deque<Chunk> &queue : mixer.queues) {
			queue.clear();
		}
	}

	for (const std::unique_ptr<Tap> &tap : mixer.taps) {
		obs_add_raw_audio_callback(tap->mix_idx, nullptr, on_track_audio, tap.get());
	}

	mixer.listening = true;
}

/* Once this returns, no callback holds a tap: libobs removes the callback
 * under the same lock it calls it with. */
void stop_listening(Mixer &mixer)
{
	if (!mixer.listening) {
		return;
	}

	for (const std::unique_ptr<Tap> &tap : mixer.taps) {
		obs_remove_raw_audio_callback(tap->mix_idx, on_track_audio, tap.get());
	}

	mixer.listening = false;

	std::lock_guard<std::mutex> hold(mixer.lock);

	for (std::deque<Chunk> &queue : mixer.queues) {
		queue.clear();
	}
}

/* Close the mixers nobody wants and no encoder is still reading. An encoder
 * only lets go of its audio when it stops, which can be a moment after its
 * output is released, so a mixer still attached is left for a later sweep. Call
 * with g_lock held. */
void sweep()
{
	for (auto it = g_mixers.begin(); it != g_mixers.end();) {
		Mixer &mixer = **it;

		if (mixer.refs > 0 || (mixer.audio && audio_output_active(mixer.audio))) {
			++it;
			continue;
		}

		stop_listening(mixer);

		if (mixer.audio) {
			audio_output_close(mixer.audio);
			mixer.audio = nullptr;
		}

		it = g_mixers.erase(it);
	}
}

} // namespace

uint32_t tracks_mask(const std::vector<int> &tracks)
{
	uint32_t mask = 0;

	for (const int track : tracks) {
		if (track >= 1 && track <= MAX_AUDIO_MIXES) {
			mask |= 1u << (track - 1);
		}
	}

	return mask;
}

int tracks_in(uint32_t mask)
{
	int count = 0;

	for (; mask; mask &= mask - 1) {
		count++;
	}

	return count;
}

audio_t *track_mix_acquire(uint32_t mask)
{
	if (tracks_in(mask) < 2) {
		return nullptr;
	}

	std::lock_guard<std::mutex> hold(g_lock);
	sweep();

	Mixer *mixer = nullptr;

	for (const std::unique_ptr<Mixer> &candidate : g_mixers) {
		if (candidate->mask == mask) {
			mixer = candidate.get();
			break;
		}
	}

	if (!mixer) {
		struct obs_audio_info oai = {};

		if (!obs_get_audio_info(&oai)) {
			blog(LOG_WARNING, "[plasmastream] no audio running to mix tracks from");
			return nullptr;
		}

		auto created = std::make_unique<Mixer>();
		created->mask = mask;
		created->planes = get_audio_planes(AUDIO_FORMAT_FLOAT_PLANAR, oai.speakers);

		for (size_t track = 0; track < MAX_AUDIO_MIXES; track++) {
			if (mask & (1u << track)) {
				auto tap = std::make_unique<Tap>();
				tap->mixer = created.get();
				tap->slot = created->queues.size();
				tap->mix_idx = track;
				created->queues.emplace_back();
				created->taps.push_back(std::move(tap));
			}
		}

		struct audio_output_info info = {};
		info.name = "PlasmaStream track mix";
		info.samples_per_sec = oai.samples_per_sec;
		info.format = AUDIO_FORMAT_FLOAT_PLANAR;
		info.speakers = oai.speakers;
		info.input_callback = on_mix_input;
		info.input_param = created.get();

		if (audio_output_open(&created->audio, &info) != AUDIO_OUTPUT_SUCCESS) {
			blog(LOG_WARNING, "[plasmastream] could not open an audio output to mix tracks %u", mask);
			return nullptr;
		}

		mixer = created.get();
		g_mixers.push_back(std::move(created));

		blog(LOG_INFO, "[plasmastream] mixing tracks %u for a destination", mask);
	}

	start_listening(*mixer);
	mixer->refs++;

	return mixer->audio;
}

void track_mix_release(uint32_t mask)
{
	std::lock_guard<std::mutex> hold(g_lock);

	for (const std::unique_ptr<Mixer> &mixer : g_mixers) {
		if (mixer->mask != mask || mixer->refs == 0) {
			continue;
		}

		if (--mixer->refs == 0) {
			stop_listening(*mixer);
		}

		break;
	}

	sweep();
}

void track_mix_shutdown()
{
	std::lock_guard<std::mutex> hold(g_lock);

	for (const std::unique_ptr<Mixer> &mixer : g_mixers) {
		if (mixer->refs > 0 || (mixer->audio && audio_output_active(mixer->audio))) {
			blog(LOG_WARNING, "[plasmastream] track mix %u still in use at unload; closing anyway",
			     mixer->mask);
		}

		stop_listening(*mixer);

		if (mixer->audio) {
			audio_output_close(mixer->audio);
			mixer->audio = nullptr;
		}
	}

	g_mixers.clear();
}

} // namespace plasmastream
