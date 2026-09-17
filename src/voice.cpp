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

#include "voice.hpp"

#include "config.hpp"
#include "http.hpp"

#include <obs-module.h>
#include <media-io/audio-io.h>
#include <util/platform.h>
#include <util/threading.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>

#ifdef PLASMASTREAM_VOICE
#include <sherpa-onnx/c-api/c-api.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#endif

namespace plasmastream {

/* ---- Text: the two functions that must agree with the website ------------- */

namespace {

bool is_space(unsigned char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

} // namespace

std::string voice_normalize(const std::string &text)
{
	std::string spaced;
	spaced.reserve(text.size());

	for (size_t i = 0; i < text.size(); i++) {
		const unsigned char c = static_cast<unsigned char>(text[i]);

		/* The three-byte punctuation a transcript actually contains: curly
		 * apostrophes become straight ones, as on the website, and dashes,
		 * curly quotes and an ellipsis become spaces, as \p{L}\p{N} would make
		 * them there. */
		if (c == 0xE2 && i + 2 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0x80) {
			const unsigned char third = static_cast<unsigned char>(text[i + 2]);

			if (third == 0x98 || third == 0x99) {
				spaced.push_back('\'');
				i += 2;
				continue;
			}

			if (third == 0x93 || third == 0x94 || third == 0x9C || third == 0x9D || third == 0xA6) {
				spaced.push_back(' ');
				i += 2;
				continue;
			}
		}

		/* Any other non-ASCII byte is kept as part of a word: an accented
		 * letter in a game's name should survive, and the website lowercases
		 * and tidies it again anyway. */
		if (c >= 0x80) {
			spaced.push_back(static_cast<char>(c));
		} else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
			spaced.push_back(static_cast<char>(c));
		} else if (c >= 'A' && c <= 'Z') {
			spaced.push_back(static_cast<char>(c - 'A' + 'a'));
		} else if (c == '\'') {
			spaced.push_back('\'');
		} else {
			spaced.push_back(' ');
		}
	}

	/* Words, with apostrophes stripped from either end of each ("'til" and
	 * "players'" read the way they are written), one space between. */
	std::string result;
	std::string word;

	const auto flush = [&]() {
		size_t from = 0;
		size_t to = word.size();

		while (from < to && word[from] == '\'') {
			from++;
		}

		while (to > from && word[to - 1] == '\'') {
			to--;
		}

		if (to > from) {
			if (!result.empty()) {
				result.push_back(' ');
			}

			result.append(word, from, to - from);
		}

		word.clear();
	};

	for (char c : spaced) {
		if (is_space(static_cast<unsigned char>(c))) {
			flush();
		} else {
			word.push_back(c);
		}
	}

	flush();
	return result;
}

bool voice_after_wake(const std::string &phrase, const std::string &heard, std::string &after)
{
	const std::string wanted = voice_normalize(phrase);

	/* A phrase with no letters or numbers in it would be found in every line. */
	if (wanted.empty()) {
		return false;
	}

	const std::string said = voice_normalize(heard);
	size_t from = 0;

	for (;;) {
		const size_t at = said.find(wanted, from);

		if (at == std::string::npos) {
			return false;
		}

		const size_t end = at + wanted.size();
		const bool starts = at == 0 || said[at - 1] == ' ';
		const bool ends = end == said.size() || said[end] == ' ';

		/* Whole words only, so "jarvis" does not fire on "jarvisual". */
		if (starts && ends) {
			after = end < said.size() ? said.substr(end + 1) : std::string();
			return true;
		}

		from = at + 1;
	}
}

#ifndef PLASMASTREAM_VOICE

/* ---- A build without the speech runtime ----------------------------------- */

void voice_init() {}
void voice_apply() {}
void voice_reattach() {}
void voice_refresh_website() {}
void voice_save_hotkey() {}
void voice_shutdown() {}

VoiceStatus voice_status()
{
	VoiceStatus status;
	status.state = config().voice.enabled ? VoiceState::Unavailable : VoiceState::Off;
	status.detail = "Voice commands are not in this build of the plugin yet.";
	return status;
}

#else

namespace {

using Clock = std::chrono::steady_clock;

/* Sixteen kilohertz mono is what the voice detector and the models take. OBS
 * mixes at 48 kHz, usually in stereo. */
constexpr int kModelRate = 16000;
constexpr int kVadWindow = 512;

/* A phrase is a few seconds. Push-to-talk held longer than this is somebody
 * leaning on a key, and decoding it all would be the one expensive thing here. */
constexpr size_t kMaxPushToTalkSamples = kModelRate * 20;

/* Audio arriving faster than the worker takes it is kept for this long and no
 * longer, so a stalled worker cannot grow memory without bound. */
constexpr int kMaxQueuedSeconds = 10;

/* "Jarvis." and then a breath before the command is how people talk. */
constexpr auto kCommandAfterWake = std::chrono::seconds(6);

/* How often the wake phrase is fetched again, for a streamer who changes it on
 * the website while OBS is open. */
constexpr auto kWebsiteRefresh = std::chrono::seconds(60);

/* The last word on a push-to-talk command arrives after the key is released,
 * because OBS hands audio over in batches. */
constexpr auto kPushToTalkTail = std::chrono::milliseconds(300);

struct Paths {
	std::string runtime_dir;
	std::string vad;
	std::string models_dir;
};

Paths g_paths;

/* Everything the dock reads and the worker writes. */
std::mutex g_status_mutex;
VoiceStatus g_status;
std::string g_token;
std::string g_model = "fast";
bool g_wake = true;
enum class EngineState { Idle, Loading, Ready, Unavailable, Failed };
EngineState g_engine = EngineState::Idle;
std::string g_engine_problem;
bool g_mic_attached = false;

/* Audio from OBS's thread to the worker. */
std::mutex g_audio_mutex;
std::condition_variable g_audio_cv;
std::vector<float> g_audio;
uint32_t g_audio_rate = 0;
bool g_stop = false;
bool g_reload = false;

std::atomic<bool> g_push_to_talk{false};
std::atomic<bool> g_refresh_website{true};

/* UI thread only. */
std::thread g_worker;
bool g_running = false;
obs_weak_source_t *g_weak_source = nullptr;
Clock::time_point g_last_attach_try;

obs_hotkey_id g_hotkey = OBS_INVALID_HOTKEY_ID;

void set_outcome(const std::string &outcome)
{
	std::lock_guard<std::mutex> lock(g_status_mutex);
	g_status.outcome = outcome;
}

void set_engine(EngineState state, const std::string &problem = std::string())
{
	std::lock_guard<std::mutex> lock(g_status_mutex);
	g_engine = state;
	g_engine_problem = problem;
}

std::string module_path(const char *relative)
{
	char *path = obs_module_file(relative);

	if (!path) {
		return {};
	}

	std::string result = path;
	bfree(path);
	return result;
}

/* ---- The speech runtime ---------------------------------------------------- */

#ifdef _WIN32
std::wstring widen(const std::string &text)
{
	if (text.empty()) {
		return {};
	}

	const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
	std::wstring wide(static_cast<size_t>(size), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), size);
	wide.resize(wcslen(wide.c_str()));
	return wide;
}

/* True when a DLL of this name is already in OBS and is not the one we ship.
 *
 * Windows hands a DLL's imports whatever module of that name is loaded first, so
 * a second plugin that brought its own ONNX Runtime would quietly have ours
 * running on its version. That is a crash while live. Refusing is the safe
 * answer, and saying so is the useful one. */
bool someone_else_loaded(const wchar_t *name, const std::wstring &ours)
{
	HMODULE existing = GetModuleHandleW(name);

	if (!existing) {
		return false;
	}

	wchar_t loaded[MAX_PATH * 4] = {0};
	GetModuleFileNameW(existing, loaded, static_cast<DWORD>(std::size(loaded)));
	return _wcsicmp(loaded, ours.c_str()) != 0;
}
#endif

/* Loads ONNX Runtime and sherpa-onnx from the plugin's own folder.
 *
 * They live under data/voice/bin, not beside the plugin, for two reasons. OBS
 * tries to load every DLL in its plugin folder as a plugin. And another plugin
 * shipping its own onnxruntime.dll into a shared folder would overwrite ours, or
 * we theirs. The plugin links sherpa-onnx delay-loaded, so nothing is looked for
 * until this has found both files by full path. */
bool load_runtime(std::string &problem)
{
#ifdef _WIN32
	static bool loaded = false;

	if (loaded) {
		return true;
	}

	if (g_paths.runtime_dir.empty()) {
		problem = "The speech runtime is missing from the plugin's folder. Reinstall the plugin.";
		return false;
	}

	const std::wstring dir = widen(g_paths.runtime_dir);
	const std::wstring ort = dir + L"\\onnxruntime.dll";
	const std::wstring api = dir + L"\\sherpa-onnx-c-api.dll";

	if (someone_else_loaded(L"onnxruntime.dll", ort) || someone_else_loaded(L"sherpa-onnx-c-api.dll", api)) {
		problem = "Another OBS plugin has loaded its own copy of the speech runtime, so voice commands "
			  "stay off rather than risk crashing OBS.";
		blog(LOG_WARNING, "[plasmastream] voice: another plugin loaded onnxruntime.dll first");
		return false;
	}

	if (!LoadLibraryExW(ort.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
		problem = "Could not load the speech runtime (error " + std::to_string(GetLastError()) + ").";
		return false;
	}

	if (!LoadLibraryExW(api.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
		problem = "Could not load sherpa-onnx (error " + std::to_string(GetLastError()) + ").";
		return false;
	}

	/* Never unloaded: the delay-load thunks point into these for as long as
	 * the plugin is loaded. */
	loaded = true;
	return true;
#else
	(void)problem;
	return true;
#endif
}

struct Engine {
	const SherpaOnnxOfflineRecognizer *recognizer = nullptr;
	const SherpaOnnxVoiceActivityDetector *vad = nullptr;
	const SherpaOnnxLinearResampler *resampler = nullptr;
	uint32_t resample_from = 0;
	std::string model;
};

void destroy_engine(Engine &engine)
{
	if (engine.resampler) {
		SherpaOnnxDestroyLinearResampler(engine.resampler);
	}

	if (engine.vad) {
		SherpaOnnxDestroyVoiceActivityDetector(engine.vad);
	}

	if (engine.recognizer) {
		SherpaOnnxDestroyOfflineRecognizer(engine.recognizer);
	}

	engine = Engine();
}

bool create_engine(Engine &engine, const std::string &model, std::string &problem)
{
	const std::string dir = g_paths.models_dir + "/" + model;
	const std::string encoder = dir + "/encoder_model.ort";
	const std::string decoder = dir + "/decoder_model_merged.ort";
	const std::string tokens = dir + "/tokens.txt";

	/* Checked here because a missing file is a clear sentence for the dock,
	 * where the library would only return nothing. */
	const std::initializer_list<const std::string *> files = {&encoder, &decoder, &tokens, &g_paths.vad};

	for (const std::string *file : files) {
		if (!os_file_exists(file->c_str())) {
			problem = "A speech model file is missing from the plugin's folder. Reinstall the plugin.";
			blog(LOG_WARNING, "[plasmastream] voice: missing %s", file->c_str());
			return false;
		}
	}

	SherpaOnnxOfflineRecognizerConfig recognizer;
	std::memset(&recognizer, 0, sizeof(recognizer));
	recognizer.feat_config.sample_rate = kModelRate;
	recognizer.feat_config.feature_dim = 80;
	recognizer.model_config.moonshine.encoder = encoder.c_str();
	recognizer.model_config.moonshine.merged_decoder = decoder.c_str();
	recognizer.model_config.tokens = tokens.c_str();
	/* Two threads: a command decodes in well under a tenth of a second on two,
	 * and OBS, the game and the encoder want the rest of the machine. */
	recognizer.model_config.num_threads = 2;
	recognizer.model_config.provider = "cpu";
	recognizer.decoding_method = "greedy_search";

	engine.recognizer = SherpaOnnxCreateOfflineRecognizer(&recognizer);

	if (!engine.recognizer) {
		problem = "The speech model would not load.";
		return false;
	}

	SherpaOnnxVadModelConfig vad;
	std::memset(&vad, 0, sizeof(vad));
	vad.silero_vad.model = g_paths.vad.c_str();
	vad.silero_vad.threshold = 0.5f;
	/* Half a second of quiet ends a phrase. Shorter splits "set my game to...
	 * Hades" at the pause people leave before a name. */
	vad.silero_vad.min_silence_duration = 0.5f;
	vad.silero_vad.min_speech_duration = 0.25f;
	vad.silero_vad.window_size = kVadWindow;
	vad.silero_vad.max_speech_duration = 12.0f;
	vad.sample_rate = kModelRate;
	vad.num_threads = 1;
	vad.provider = "cpu";

	engine.vad = SherpaOnnxCreateVoiceActivityDetector(&vad, 30.0f);

	if (!engine.vad) {
		problem = "The voice detector would not load.";
		destroy_engine(engine);
		return false;
	}

	engine.model = model;
	return true;
}

std::string decode(const Engine &engine, const float *samples, size_t count)
{
	const SherpaOnnxOfflineStream *stream = SherpaOnnxCreateOfflineStream(engine.recognizer);
	SherpaOnnxAcceptWaveformOffline(stream, kModelRate, samples, static_cast<int32_t>(count));
	SherpaOnnxDecodeOfflineStream(engine.recognizer, stream);

	const SherpaOnnxOfflineRecognizerResult *result = SherpaOnnxGetOfflineStreamResult(stream);
	std::string text = result && result->text ? result->text : "";

	SherpaOnnxDestroyOfflineRecognizerResult(result);
	SherpaOnnxDestroyOfflineStream(stream);

	const auto first = text.find_first_not_of(" \t\r\n");
	const auto last = text.find_last_not_of(" \t\r\n");
	return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
}

/* ---- Talking to PlasmaStream --------------------------------------------- */

std::string json_string(const std::string &text)
{
	std::string out = "\"";

	for (unsigned char c : text) {
		if (c == '"' || c == '\\') {
			out.push_back('\\');
			out.push_back(static_cast<char>(c));
		} else if (c < 0x20) {
			char escaped[8];
			snprintf(escaped, sizeof(escaped), "\\u%04x", c);
			out += escaped;
		} else {
			out.push_back(static_cast<char>(c));
		}
	}

	out.push_back('"');
	return out;
}

std::string current_token()
{
	std::lock_guard<std::mutex> lock(g_status_mutex);
	return g_token;
}

/* The wake phrase, and whether voice is switched on, from the website. */
void fetch_website()
{
	const std::string token = current_token();

	if (token.empty()) {
		std::lock_guard<std::mutex> lock(g_status_mutex);
		g_status.website_problem.clear();
		return;
	}

	const HttpResponse response = http_get(plasmastream_url("/api/plugin/" + token + "/voice"));

	std::lock_guard<std::mutex> lock(g_status_mutex);

	if (response.status == 404) {
		g_status.website_problem = "PlasmaStream did not recognize the plugin key. Paste it again under "
					   "Plugin key.";
		return;
	}

	if (!response.ok()) {
		g_status.website_problem = "Could not reach PlasmaStream to read your wake phrase.";
		return;
	}

	obs_data_t *data = obs_data_create_from_json(response.body.c_str());

	if (!data) {
		g_status.website_problem = "PlasmaStream answered with something unreadable.";
		return;
	}

	g_status.website_problem.clear();
	g_status.website_on = obs_data_get_bool(data, "on");
	g_status.phrase = obs_data_get_string(data, "phrase");
	obs_data_release(data);
}

/* Sends one command's words, and says in the dock what became of them. */
void send_command(const std::string &words)
{
	const std::string token = current_token();

	if (token.empty()) {
		set_outcome("Heard a command. Add your plugin key (Plugin key...) so PlasmaStream can run it.");
		return;
	}

	{
		std::lock_guard<std::mutex> lock(g_status_mutex);

		if (!g_status.website_on) {
			g_status.outcome = "Voice commands are switched off on your PlasmaStream dashboard.";
			return;
		}
	}

	set_outcome("Sending \"" + words + "\"...");

	const HttpResponse response =
		http_post_json(plasmastream_url("/api/plugin/" + token + "/voice"), "{\"heard\":" + json_string(words) + "}", 6);

	if (response.status == 0) {
		set_outcome("Could not reach PlasmaStream: " + response.error);
		return;
	}

	if (response.status == 404) {
		set_outcome("PlasmaStream did not recognize the plugin key.");
		return;
	}

	if (response.status == 429) {
		set_outcome("Too many voice commands in a minute. Try again in a moment.");
		return;
	}

	obs_data_t *data = response.ok() ? obs_data_create_from_json(response.body.c_str()) : nullptr;

	if (!data) {
		set_outcome("PlasmaStream could not take that command (" + std::to_string(response.status) + ").");
		return;
	}

	const std::string status = obs_data_get_string(data, "status");

	if (status == "queued") {
		set_outcome(std::string("Ran ") + obs_data_get_string(data, "line") + " (see what it did in chat).");
	} else if (status == "unmatched") {
		set_outcome("\"" + words + "\" did not match any of your phrases.");
	} else if (status == "off") {
		std::lock_guard<std::mutex> lock(g_status_mutex);
		g_status.website_on = false;
		g_status.outcome = "Voice commands are switched off on your PlasmaStream dashboard.";
	} else if (status == "refused") {
		set_outcome(obs_data_get_string(data, "why"));
	} else {
		set_outcome("PlasmaStream answered, but not in a way this plugin understands. Update the plugin.");
	}

	obs_data_release(data);
}

/* ---- Listening ------------------------------------------------------------- */

void on_audio(void *, obs_source_t *, const struct audio_data *audio, bool muted)
{
	/* Muted in OBS means not listening. A streamer who mutes their mic has
	 * said everything they want heard. */
	if (muted || !audio || audio->frames == 0) {
		return;
	}

	const audio_t *output = obs_get_audio();

	if (!output) {
		return;
	}

	const size_t channels = std::min<size_t>(audio_output_get_channels(output), MAX_AV_PLANES);
	const uint32_t rate = audio_output_get_sample_rate(output);

	std::lock_guard<std::mutex> lock(g_audio_mutex);

	if (g_audio_rate != rate) {
		g_audio.clear();
		g_audio_rate = rate;
	}

	const size_t cap = static_cast<size_t>(rate) * kMaxQueuedSeconds;

	if (g_audio.size() > cap) {
		g_audio.erase(g_audio.begin(), g_audio.begin() + static_cast<std::ptrdiff_t>(g_audio.size() - cap / 2));
	}

	const size_t start = g_audio.size();
	g_audio.resize(start + audio->frames);

	for (uint32_t i = 0; i < audio->frames; i++) {
		float sum = 0.0f;
		int used = 0;

		for (size_t channel = 0; channel < channels; channel++) {
			if (audio->data[channel]) {
				sum += static_cast<const float *>(static_cast<void *>(audio->data[channel]))[i];
				used++;
			}
		}

		g_audio[start + i] = used ? sum / static_cast<float>(used) : 0.0f;
	}

	/* A tenth of a second at a time is plenty for the detector, and wakes the
	 * worker ten times a second rather than fifty. */
	if (g_audio.size() >= rate / 10) {
		g_audio_cv.notify_one();
	}
}

void on_push_to_talk(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	g_push_to_talk = pressed;

	{
		std::lock_guard<std::mutex> lock(g_status_mutex);
		g_status.push_to_talk_held = pressed;
	}

	g_audio_cv.notify_one();
}

/* What to do with one decoded line. */
struct Listener {
	Clock::time_point awaiting_command_until{};

	void heard(const std::string &text, bool push_to_talk)
	{
		if (text.empty()) {
			return;
		}

		std::string phrase;

		{
			std::lock_guard<std::mutex> lock(g_status_mutex);
			g_status.heard = text;
			phrase = g_status.phrase;
		}

		std::string words;
		const bool addressed = voice_after_wake(phrase, text, words);

		if (push_to_talk) {
			/* Holding the key is the address. A wake phrase said anyway is
			 * dropped from the front. */
			if (!addressed) {
				words = voice_normalize(text);
			}
		} else if (!addressed) {
			/* Not a command, unless it came right after the wake phrase on its
			 * own. Nothing is sent, and the dock says so: the line above it is
			 * this, not the last command, and a misheard wake phrase is the
			 * thing somebody setting up needs to spot. */
			if (Clock::now() > awaiting_command_until) {
				set_outcome(phrase.empty() ? "Nothing was sent: your wake phrase has not arrived from PlasmaStream yet."
							   : "No \"" + phrase + "\" in that, so nothing was sent.");
				return;
			}

			words = voice_normalize(text);
			awaiting_command_until = {};
		} else if (words.empty()) {
			awaiting_command_until = Clock::now() + kCommandAfterWake;
			set_outcome("Heard \"" + phrase + "\". Say the command.");
			return;
		}

		awaiting_command_until = {};

		if (!words.empty()) {
			send_command(words);
		}
	}
};

void worker_main()
{
	os_set_thread_name("plasmastream-voice");

	std::string problem;

	if (!load_runtime(problem)) {
		set_engine(EngineState::Unavailable, problem);

		std::unique_lock<std::mutex> lock(g_audio_mutex);
		g_audio_cv.wait(lock, [] { return g_stop; });
		return;
	}

	Engine engine;
	Listener listener;
	std::vector<float> chunk;
	std::vector<float> vad_carry;
	std::vector<float> push_audio;
	bool push_active = false;
	Clock::time_point push_released{};
	Clock::time_point website_fetched{};

	for (;;) {
		std::string wanted_model;
		bool wake = true;

		{
			std::lock_guard<std::mutex> lock(g_status_mutex);
			wanted_model = g_model;
			wake = g_wake;
		}

		if (!engine.recognizer || engine.model != wanted_model) {
			destroy_engine(engine);
			set_engine(EngineState::Loading);

			if (!create_engine(engine, wanted_model, problem)) {
				set_engine(EngineState::Failed, problem);
				blog(LOG_WARNING, "[plasmastream] voice: %s", problem.c_str());

				// Wait for a different setting, or for the end.
				std::unique_lock<std::mutex> lock(g_audio_mutex);
				g_audio_cv.wait(lock, [] { return g_stop || g_reload; });

				if (g_stop) {
					break;
				}

				g_reload = false;
				continue;
			}

			set_engine(EngineState::Ready);
			blog(LOG_INFO, "[plasmastream] voice: listening with the %s model", wanted_model.c_str());
		}

		if (g_refresh_website.exchange(false) || Clock::now() - website_fetched > kWebsiteRefresh) {
			fetch_website();
			website_fetched = Clock::now();
		}

		uint32_t rate = 0;

		{
			std::unique_lock<std::mutex> lock(g_audio_mutex);
			g_audio_cv.wait_for(lock, std::chrono::milliseconds(200), [&] {
				return g_stop || g_reload || g_push_to_talk.load() != push_active ||
				       (g_audio_rate > 0 && g_audio.size() >= g_audio_rate / 10);
			});

			if (g_stop) {
				break;
			}

			g_reload = false;
			chunk.swap(g_audio);
			g_audio.clear();
			rate = g_audio_rate;
		}

		/* Down to the models' rate. The resampler keeps its state between
		 * chunks, so it is only rebuilt when OBS's own rate changes. */
		const float *samples = chunk.data();
		size_t count = chunk.size();
		const SherpaOnnxResampleOut *resampled = nullptr;

		if (count > 0 && rate != 0 && rate != kModelRate) {
			if (!engine.resampler || engine.resample_from != rate) {
				if (engine.resampler) {
					SherpaOnnxDestroyLinearResampler(engine.resampler);
				}

				const float cutoff = 0.99f * 0.5f * static_cast<float>(kModelRate);
				engine.resampler = SherpaOnnxCreateLinearResampler(static_cast<int32_t>(rate), kModelRate, cutoff, 6);
				engine.resample_from = rate;
			}

			resampled = SherpaOnnxLinearResamplerResample(engine.resampler, samples, static_cast<int32_t>(count), 0);
			samples = resampled->samples;
			count = static_cast<size_t>(resampled->n);
		}

		const bool held = g_push_to_talk.load();

		if (held && !push_active) {
			push_active = true;
			push_audio.clear();
			/* Whatever the detector was half way through belongs to before the
			 * key, not to the command. */
			SherpaOnnxVoiceActivityDetectorReset(engine.vad);
			vad_carry.clear();
			listener.awaiting_command_until = {};
		}

		if (push_active) {
			if (push_audio.size() + count <= kMaxPushToTalkSamples) {
				push_audio.insert(push_audio.end(), samples, samples + count);
			}

			if (!held) {
				if (push_released == Clock::time_point{}) {
					push_released = Clock::now();
				} else if (Clock::now() - push_released >= kPushToTalkTail) {
					push_active = false;
					push_released = {};

					/* Under a third of a second is a key tapped, not a
					 * command said. */
					if (push_audio.size() > kModelRate / 3) {
						listener.heard(decode(engine, push_audio.data(), push_audio.size()), true);
					}

					push_audio.clear();
				}
			} else {
				push_released = {};
			}
		} else if (wake && count > 0) {
			vad_carry.insert(vad_carry.end(), samples, samples + count);

			size_t used = 0;

			while (vad_carry.size() - used >= kVadWindow) {
				SherpaOnnxVoiceActivityDetectorAcceptWaveform(engine.vad, vad_carry.data() + used, kVadWindow);
				used += kVadWindow;

				while (!SherpaOnnxVoiceActivityDetectorEmpty(engine.vad)) {
					const SherpaOnnxSpeechSegment *segment = SherpaOnnxVoiceActivityDetectorFront(engine.vad);
					std::string text = decode(engine, segment->samples, static_cast<size_t>(segment->n));
					SherpaOnnxDestroySpeechSegment(segment);
					SherpaOnnxVoiceActivityDetectorPop(engine.vad);
					listener.heard(text, false);
				}
			}

			vad_carry.erase(vad_carry.begin(), vad_carry.begin() + static_cast<std::ptrdiff_t>(used));
		}

		if (resampled) {
			SherpaOnnxLinearResamplerResampleFree(resampled);
		}

		chunk.clear();
	}

	destroy_engine(engine);
	set_engine(EngineState::Idle);
}

/* ---- The microphone -------------------------------------------------------- */

void detach_source()
{
	if (!g_weak_source) {
		return;
	}

	obs_source_t *source = obs_weak_source_get_source(g_weak_source);

	if (source) {
		obs_source_remove_audio_capture_callback(source, on_audio, nullptr);
		obs_source_release(source);
	}

	obs_weak_source_release(g_weak_source);
	g_weak_source = nullptr;

	std::lock_guard<std::mutex> lock(g_status_mutex);
	g_mic_attached = false;
}

bool attached_source_alive()
{
	if (!g_weak_source) {
		return false;
	}

	obs_source_t *source = obs_weak_source_get_source(g_weak_source);

	if (!source) {
		return false;
	}

	const bool removed = obs_source_removed(source);
	obs_source_release(source);
	return !removed;
}

void attach_source()
{
	detach_source();
	g_last_attach_try = Clock::now();

	const VoiceConfig &voice = config().voice;
	obs_source_t *source = nullptr;

	if (!voice.source_uuid.empty()) {
		source = obs_get_source_by_uuid(voice.source_uuid.c_str());
	}

	if (!source && !voice.source_name.empty()) {
		source = obs_get_source_by_name(voice.source_name.c_str());
	}

	std::lock_guard<std::mutex> lock(g_status_mutex);
	g_status.microphone = voice.source_name;

	if (!source) {
		g_mic_attached = false;
		return;
	}

	obs_source_add_audio_capture_callback(source, on_audio, nullptr);
	g_weak_source = obs_source_get_weak_source(source);
	g_mic_attached = true;
	g_status.microphone = obs_source_get_name(source);
	obs_source_release(source);
}

void stop_worker()
{
	if (!g_running) {
		return;
	}

	{
		std::lock_guard<std::mutex> lock(g_audio_mutex);
		g_stop = true;
	}

	g_audio_cv.notify_all();

	if (g_worker.joinable()) {
		g_worker.join();
	}

	std::lock_guard<std::mutex> lock(g_audio_mutex);
	g_stop = false;
	g_reload = false;
	g_audio.clear();
	g_running = false;
}

} // namespace

void voice_init()
{
	g_paths.runtime_dir = module_path("voice/bin");
	g_paths.vad = module_path("voice/silero_vad.onnx");
	g_paths.models_dir = module_path("voice/models");

	g_hotkey = obs_hotkey_register_frontend("plasmastream_voice_push_to_talk",
						"PlasmaStream: hold to give a voice command", on_push_to_talk,
						nullptr);

	const std::string &saved = config().voice.hotkey;

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
}

void voice_apply()
{
	const VoiceConfig &voice = config().voice;

	{
		std::lock_guard<std::mutex> lock(g_status_mutex);

		/* A different key is a different channel's wake phrase. */
		if (g_token != config().token) {
			g_status.phrase.clear();
			g_status.website_on = true;
		}

		g_token = config().token;
		g_model = voice.model;
		g_wake = voice.wake;
		g_status.outcome.clear();
	}

	if (!voice.enabled) {
		stop_worker();
		detach_source();
		return;
	}

	g_refresh_website = true;

	if (!g_running) {
		g_running = true;
		g_worker = std::thread(worker_main);
	} else {
		{
			std::lock_guard<std::mutex> lock(g_audio_mutex);
			g_reload = true;
		}

		g_audio_cv.notify_all();
	}

	attach_source();
}

void voice_reattach()
{
	if (!config().voice.enabled || !g_running) {
		return;
	}

	if (attached_source_alive()) {
		return;
	}

	if (Clock::now() - g_last_attach_try < std::chrono::seconds(3)) {
		return;
	}

	attach_source();
}

void voice_refresh_website()
{
	{
		std::lock_guard<std::mutex> lock(g_status_mutex);
		g_token = config().token;
	}

	g_refresh_website = true;
	g_audio_cv.notify_all();
}

void voice_save_hotkey()
{
	if (g_hotkey == OBS_INVALID_HOTKEY_ID) {
		return;
	}

	obs_data_array_t *bindings = obs_hotkey_save(g_hotkey);
	obs_data_t *data = obs_data_create();
	obs_data_set_array(data, "bindings", bindings);

	const char *json = obs_data_get_json(data);
	const std::string saved = json ? json : "";

	obs_data_release(data);
	obs_data_array_release(bindings);

	if (saved != config().voice.hotkey) {
		config().voice.hotkey = saved;
		save_config();
	}
}

void voice_shutdown()
{
	stop_worker();
	detach_source();

	if (g_hotkey != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(g_hotkey);
		g_hotkey = OBS_INVALID_HOTKEY_ID;
	}
}

VoiceStatus voice_status()
{
	std::lock_guard<std::mutex> lock(g_status_mutex);

	VoiceStatus status = g_status;
	status.linked = !g_token.empty();
	status.wake = g_wake;

	if (!config().voice.enabled) {
		status.state = VoiceState::Off;
	} else if (g_engine == EngineState::Unavailable) {
		status.state = VoiceState::Unavailable;
		status.detail = g_engine_problem;
	} else if (g_engine == EngineState::Failed) {
		status.state = VoiceState::Error;
		status.detail = g_engine_problem;
	} else if (g_engine != EngineState::Ready) {
		status.state = VoiceState::Loading;
	} else if (!g_mic_attached) {
		status.state = VoiceState::NoMicrophone;
	} else {
		status.state = VoiceState::Listening;
	}

	return status;
}

#endif

} // namespace plasmastream
