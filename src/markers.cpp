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

#include "markers.hpp"

#include "config.hpp"
#include "http.hpp"
#include "scenerec.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <obs-module.h>
#include <util/platform.h>

namespace plasmastream {

namespace {

/* How often PlasmaStream is asked for new markers while something records. Each
 * marker comes with its own age, so this decides only how soon a chapter appears
 * in a Hybrid MP4, never where the line in the chapters file goes. */
constexpr int POLL_SECONDS = 3;

/* A marker older than this when it arrives still gets its line in the chapters
 * file, placed where it happened, but no chapter inside the file: OBS can only
 * add one at the current moment, and a chapter a minute away from the thing it
 * names is worse than none. It happens after a plugin lost its connection for a
 * while and is catching up. */
constexpr uint64_t NATIVE_MAX_AGE_MS = 20000;

/* A marker from before a recording started belongs to no part of it. A little
 * slack, because a raid that lands the second somebody presses Record is a raid
 * they meant to have in it. */
constexpr uint64_t EARLY_SLACK_MS = 2000;

/* Ids already placed, so the overlap PlasmaStream deliberately leaves between
 * reads is dropped rather than marked twice. */
constexpr size_t SEEN_MAX = 500;

struct Chapter {
	uint64_t offset_ms = 0;
	std::string label;
};

/* One recording that markers go into. */
struct Target {
	std::string file;
	std::string sidecar;
	uint64_t started_ns = 0;

	/* Time spent paused so far, and when the current pause began. A paused
	 * recording's clock stops, so a chapter after a pause sits that much earlier
	 * in the file than on the wall clock. Only OBS's own recording pauses. */
	uint64_t paused_ns = 0;
	uint64_t paused_at_ns = 0;

	bool obs_own = false;
	std::vector<Chapter> chapters;
};

std::mutex g_mutex;
std::vector<Target> g_targets;

int g_added = 0;
int g_manual_count = 0;
std::string g_last;
std::string g_problem;

std::string g_cursor;
std::deque<std::string> g_seen_order;
std::set<std::string> g_seen;

std::thread g_worker;
std::condition_variable g_wake;
bool g_stop = false;
bool g_started = false;

obs_hotkey_id g_hotkey = OBS_INVALID_HOTKEY_ID;

/* The file's own name with ".chapters.txt" for its extension. Empty for a
 * recording with no file, which a custom FFmpeg output to a URL can be. */
std::string sidecar_for(const std::string &file)
{
	if (file.empty()) {
		return {};
	}

	const size_t slash = file.find_last_of("/\\");
	const size_t dot = file.find_last_of('.');
	const std::string stem = dot != std::string::npos && (slash == std::string::npos || dot > slash)
					 ? file.substr(0, dot)
					 : file;

	return stem + ".chapters.txt";
}

/* Where a moment on the wall clock falls in a recording, or false when it falls
 * before the recording began. */
bool offset_in(const Target &target, uint64_t at_ns, uint64_t &offset_ms)
{
	/* A moment during the current pause sits where the pause began, which is
	 * where it is in the file. */
	if (target.paused_at_ns != 0 && at_ns > target.paused_at_ns) {
		at_ns = target.paused_at_ns;
	}

	const uint64_t early = EARLY_SLACK_MS * 1000000ULL;

	if (at_ns + early < target.started_ns) {
		return false;
	}

	const uint64_t since_start = at_ns > target.started_ns ? at_ns - target.started_ns : 0;
	const uint64_t playing = since_start > target.paused_ns ? since_start - target.paused_ns : 0;

	offset_ms = playing / 1000000ULL;
	return true;
}

/* Rewritten whole rather than appended to, so chapters stay in order: a hotkey
 * marker can arrive after a raid that happened a few seconds before it, and
 * YouTube only reads a chapter list that runs forwards. Written through a temporary
 * file, so a crash mid-write leaves the last good list rather than half of one. */
void write_sidecar(const Target &target)
{
	if (target.sidecar.empty()) {
		return;
	}

	std::string text = "00:00 Start\n";

	for (const Chapter &chapter : target.chapters) {
		text += chapter_timestamp(chapter.offset_ms) + " " + chapter.label + "\n";
	}

	if (!os_quick_write_utf8_file_safe(target.sidecar.c_str(), text.c_str(), text.size(), false, "tmp",
					   nullptr)) {
		blog(LOG_WARNING, "[plasmastream] could not write the chapters file %s", target.sidecar.c_str());
	}
}

/* Picks up the scene recording starting or stopping. Assumes the lock. */
void refresh_scene_target_locked()
{
	std::string file;
	uint64_t started_ns = 0;
	const bool running = scene_recording_marker_target(file, started_ns);

	auto it = std::find_if(g_targets.begin(), g_targets.end(), [](const Target &t) { return !t.obs_own; });

	if (it != g_targets.end() && (!running || it->file != file)) {
		g_targets.erase(it);
	}

	const bool have = std::any_of(g_targets.begin(), g_targets.end(), [](const Target &t) { return !t.obs_own; });

	if (running && !have) {
		Target target;
		target.file = file;
		target.sidecar = sidecar_for(file);
		target.started_ns = started_ns;
		g_targets.push_back(std::move(target));
	}
}

/* Anything recording at all. Assumes the lock. */
bool recording_locked()
{
	refresh_scene_target_locked();
	return !g_targets.empty();
}

/* Marks one moment in every recording running. `age_ms` is how long ago it
 * happened. Returns how many recordings it went into.
 *
 * The chapters inside the files are added after the lock is let go, because they
 * call into OBS and into the scene recording, which take locks of their own. */
int place(const std::string &label, uint64_t age_ms)
{
	const uint64_t now_ns = os_gettime_ns();
	const uint64_t age_ns = age_ms * 1000000ULL;
	const uint64_t at_ns = now_ns > age_ns ? now_ns - age_ns : 0;

	bool native_obs = false;
	bool native_scene = false;
	int placed = 0;

	{
		std::lock_guard<std::mutex> lock(g_mutex);
		refresh_scene_target_locked();

		for (Target &target : g_targets) {
			uint64_t offset_ms = 0;

			if (!offset_in(target, at_ns, offset_ms)) {
				continue;
			}

			Chapter chapter;
			chapter.offset_ms = offset_ms;
			chapter.label = label;

			auto later = std::upper_bound(
				target.chapters.begin(), target.chapters.end(), offset_ms,
				[](uint64_t value, const Chapter &existing) { return value < existing.offset_ms; });
			target.chapters.insert(later, std::move(chapter));

			write_sidecar(target);
			placed++;

			if (age_ms <= NATIVE_MAX_AGE_MS) {
				(target.obs_own ? native_obs : native_scene) = true;
			}
		}

		if (placed > 0) {
			g_added++;
			g_last = label;
		}
	}

	/* Both of these do nothing for anything but a Hybrid MP4, which is the one
	 * format OBS can write a chapter into. That is the right answer for the
	 * others, not a failure: their chapters are in the file beside them. */
	/* OBS 30.2 is where recordings could first take a chapter. The Linux build
	 * is pinned to the 30.0 Ubuntu ships, and there the chapters file is all
	 * there is, which is still the useful half. */
#if LIBOBS_API_MAJOR_VER > 30 || (LIBOBS_API_MAJOR_VER == 30 && LIBOBS_API_MINOR_VER >= 2)
	if (native_obs) {
		obs_frontend_recording_add_chapter(label.c_str());
	}
#else
	(void)native_obs;
#endif

	if (native_scene) {
		scene_recording_add_chapter(label);
	}

	return placed;
}

bool wanted(const std::string &kind)
{
	const MarkerSettings &settings = config().markers;

	if (kind == "raid") {
		return settings.raids;
	}

	if (kind == "sub") {
		return settings.subs;
	}

	if (kind == "money") {
		return settings.money;
	}

	if (kind == "game") {
		return settings.games;
	}

	/* Somebody typed !marker. Asked for by name, so always placed. */
	return true;
}

/* Asks PlasmaStream for what has happened since the last read. Runs on the
 * worker, never the UI thread: it blocks on the network. */
void collect(const std::string &token)
{
	std::string cursor;

	{
		std::lock_guard<std::mutex> lock(g_mutex);
		cursor = g_cursor;
	}

	const std::string path = "/api/plugin/" + token + "/markers" + (cursor.empty() ? "" : "?cursor=" + cursor);
	const HttpResponse response = http_get(plasmastream_url(path));

	if (response.status == 404) {
		std::lock_guard<std::mutex> lock(g_mutex);
		g_problem = "PlasmaStream does not recognise this key, so only hotkey markers are being added.";
		return;
	}

	if (!response.ok()) {
		/* A rate limit is a pause, not a problem worth showing. Anything else
		 * is said, and fixes itself on the next read that lands: the cursor has
		 * not moved, so nothing in between is lost. */
		if (response.status != 429) {
			std::lock_guard<std::mutex> lock(g_mutex);
			g_problem = "Could not reach PlasmaStream. Markers will catch up when it answers.";
		}

		return;
	}

	obs_data_t *data = obs_data_create_from_json(response.body.c_str());

	if (!data) {
		return;
	}

	struct Arrived {
		std::string label;
		uint64_t age_ms;
	};

	std::vector<Arrived> arrived;

	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_problem.clear();

		const char *next = obs_data_get_string(data, "cursor");

		if (next && *next) {
			g_cursor = next;
		}

		obs_data_array_t *markers = obs_data_get_array(data, "markers");
		const size_t count = markers ? obs_data_array_count(markers) : 0;

		for (size_t i = 0; i < count; i++) {
			obs_data_t *marker = obs_data_array_item(markers, i);
			const std::string id = obs_data_get_string(marker, "id");
			const std::string kind = obs_data_get_string(marker, "kind");
			const std::string label = obs_data_get_string(marker, "label");
			const long long age = obs_data_get_int(marker, "ageMs");
			obs_data_release(marker);

			if (id.empty() || label.empty() || g_seen.count(id)) {
				continue;
			}

			g_seen.insert(id);
			g_seen_order.push_back(id);

			while (g_seen_order.size() > SEEN_MAX) {
				g_seen.erase(g_seen_order.front());
				g_seen_order.pop_front();
			}

			if (wanted(kind)) {
				arrived.push_back({label, static_cast<uint64_t>(age > 0 ? age : 0)});
			}
		}

		obs_data_array_release(markers);
	}

	obs_data_release(data);

	for (const Arrived &marker : arrived) {
		place(marker.label, marker.age_ms);
	}
}

void worker()
{
	for (;;) {
		std::string token;
		bool collecting = false;

		{
			std::unique_lock<std::mutex> lock(g_mutex);
			g_wake.wait_for(lock, std::chrono::seconds(POLL_SECONDS), [] { return g_stop; });

			if (g_stop) {
				return;
			}

			const bool recording = recording_locked();

			if (!recording) {
				/* The next recording starts from its own beginning, not from
				 * wherever the last one left off. */
				g_cursor.clear();
				g_seen.clear();
				g_seen_order.clear();
				g_problem.clear();
				g_added = 0;
				g_manual_count = 0;
				g_last.clear();
				continue;
			}

			token = config().token;
			collecting = config().markers.enabled;

			if (collecting && token.empty()) {
				g_problem = "Add your PlasmaStream key to get markers for raids, subs and game changes.";
				collecting = false;
			}
		}

		if (collecting) {
			collect(token);
		}
	}
}

void on_hotkey(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed) {
		markers_add_now();
	}
}

} // namespace

std::string chapter_timestamp(uint64_t offset_ms)
{
	const uint64_t total = offset_ms / 1000;
	const uint64_t hours = total / 3600;
	const uint64_t minutes = (total % 3600) / 60;
	const uint64_t seconds = total % 60;

	char text[32];

	if (hours > 0) {
		snprintf(text, sizeof(text), "%llu:%02llu:%02llu", static_cast<unsigned long long>(hours),
			 static_cast<unsigned long long>(minutes), static_cast<unsigned long long>(seconds));
	} else {
		snprintf(text, sizeof(text), "%02llu:%02llu", static_cast<unsigned long long>(minutes),
			 static_cast<unsigned long long>(seconds));
	}

	return text;
}

void markers_init()
{
	g_hotkey = obs_hotkey_register_frontend("plasmastream_add_marker", "PlasmaStream: add a chapter marker",
						on_hotkey, nullptr);

	const std::string &saved = config().markers.hotkey;

	if (g_hotkey != OBS_INVALID_HOTKEY_ID && !saved.empty()) {
		obs_data_t *data = obs_data_create_from_json(saved.c_str());

		if (data) {
			obs_data_array_t *bindings = obs_data_get_array(data, "bindings");

			if (bindings) {
				obs_hotkey_load(g_hotkey, bindings);
				obs_data_array_release(bindings);
			}

			obs_data_release(data);
		}
	}

	std::lock_guard<std::mutex> lock(g_mutex);

	if (!g_started) {
		g_stop = false;
		g_started = true;
		g_worker = std::thread(worker);
	}
}

void markers_save_hotkey()
{
	if (g_hotkey == OBS_INVALID_HOTKEY_ID) {
		return;
	}

	obs_data_array_t *bindings = obs_hotkey_save(g_hotkey);
	obs_data_t *data = obs_data_create();
	obs_data_set_array(data, "bindings", bindings);
	config().markers.hotkey = obs_data_get_json(data);
	obs_data_release(data);
	obs_data_array_release(bindings);

	save_config();
}

void markers_shutdown()
{
	{
		std::lock_guard<std::mutex> lock(g_mutex);

		if (!g_started) {
			return;
		}

		g_stop = true;
		g_started = false;
	}

	g_wake.notify_all();

	if (g_worker.joinable()) {
		g_worker.join();
	}

	if (g_hotkey != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(g_hotkey);
		g_hotkey = OBS_INVALID_HOTKEY_ID;
	}
}

void markers_frontend_event(enum obs_frontend_event event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_RECORDING_STARTED: {
		Target target;
		target.obs_own = true;
		target.started_ns = os_gettime_ns();

		obs_output_t *output = obs_frontend_get_recording_output();

		if (output) {
			obs_data_t *settings = obs_output_get_settings(output);
			const char *path = obs_data_get_string(settings, "path");

			/* A custom FFmpeg output names its file "url" rather than "path". */
			if (!path || !*path) {
				path = obs_data_get_string(settings, "url");
			}

			target.file = path ? path : "";
			target.sidecar = target.file.find("://") == std::string::npos ? sidecar_for(target.file) : "";

			obs_data_release(settings);
			obs_output_release(output);
		}

		std::lock_guard<std::mutex> lock(g_mutex);
		g_targets.erase(std::remove_if(g_targets.begin(), g_targets.end(), [](const Target &t) { return t.obs_own; }),
				g_targets.end());
		g_targets.push_back(std::move(target));
		break;
	}

	case OBS_FRONTEND_EVENT_RECORDING_STOPPING:
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED: {
		std::lock_guard<std::mutex> lock(g_mutex);
		g_targets.erase(std::remove_if(g_targets.begin(), g_targets.end(), [](const Target &t) { return t.obs_own; }),
				g_targets.end());
		break;
	}

	case OBS_FRONTEND_EVENT_RECORDING_PAUSED: {
		std::lock_guard<std::mutex> lock(g_mutex);

		for (Target &target : g_targets) {
			if (target.obs_own && target.paused_at_ns == 0) {
				target.paused_at_ns = os_gettime_ns();
			}
		}

		break;
	}

	case OBS_FRONTEND_EVENT_RECORDING_UNPAUSED: {
		std::lock_guard<std::mutex> lock(g_mutex);

		for (Target &target : g_targets) {
			if (target.obs_own && target.paused_at_ns != 0) {
				target.paused_ns += os_gettime_ns() - target.paused_at_ns;
				target.paused_at_ns = 0;
			}
		}

		break;
	}

	default:
		break;
	}
}

bool markers_add_now()
{
	std::string label;

	{
		std::lock_guard<std::mutex> lock(g_mutex);

		if (!recording_locked()) {
			return false;
		}

		label = "Marker " + std::to_string(++g_manual_count);
	}

	return place(label, 0) > 0;
}

MarkerStatus markers_status()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	refresh_scene_target_locked();

	MarkerStatus status;
	status.collecting = !g_targets.empty();
	status.added = g_added;
	status.last = g_last;
	status.problem = g_problem;
	return status;
}

void markers_apply()
{
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_problem.clear();
	}

	g_wake.notify_all();
}

} // namespace plasmastream
