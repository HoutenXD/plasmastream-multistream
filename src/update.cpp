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

#include "update.hpp"

#include "config.hpp"
#include "http.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace plasmastream {

namespace {

#if defined(_WIN32)
constexpr const char *PLATFORM = "windows";
#elif defined(__APPLE__)
constexpr const char *PLATFORM = "macos";
#else
constexpr const char *PLATFORM = "linux";
#endif

/* Long enough that it is irrelevant to anybody who restarts OBS for every
 * stream, short enough that a machine left running for days still notices. */
constexpr auto CHECK_EVERY = std::chrono::hours(6);

/* Not the instant OBS starts: the first seconds after launch are the busiest they
 * get, and nothing here is urgent. */
constexpr auto FIRST_CHECK_AFTER = std::chrono::seconds(20);

struct State {
	std::mutex mutex;
	UpdateStage stage = UpdateStage::idle;
	UpdateProblem problem = UpdateProblem::none;
	UpdateOffer offer;

	/* The archive this platform updates from, and the speech models when the
	 * ones installed are not the ones the release carries. */
	std::string archive_url;
	std::string models_url;

	/* Where the unpacked new version is waiting, once it is. */
	std::string staged;

	/* Set once the swap has been handed to the script, so unload cannot hand it
	 * over a second time. */
	bool handed_over = false;
};

State &state()
{
	static State value;
	return value;
}

std::atomic<bool> g_busy{false};
std::atomic<bool> g_cancel{false};
std::thread g_worker;
std::thread g_timer;
std::atomic<bool> g_stopping{false};
std::mutex g_sleep_mutex;
std::condition_variable g_sleep;

void set_stage(UpdateStage stage, UpdateProblem problem = UpdateProblem::none)
{
	std::lock_guard<std::mutex> guard(state().mutex);
	state().stage = stage;
	state().problem = problem;
}

std::string trimmed(const std::string &text)
{
	const auto first = text.find_first_not_of(" \t\r\n");

	if (first == std::string::npos) {
		return {};
	}

	return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

std::string read_file(const fs::path &path)
{
	std::ifstream file(path, std::ios::binary);

	if (!file) {
		return {};
	}

	std::ostringstream all;
	all << file.rdbuf();
	return trimmed(all.str());
}

/* The numbers in a version, so 0.10.0 can be compared with 0.9.0 as numbers
 * rather than as text. Anything after them is ignored, which is how a 0.5.0-rc1
 * tag never reads as newer than the 0.5.0 it precedes. */
std::vector<int> version_parts(const std::string &version)
{
	std::vector<int> parts;
	std::string number;

	for (const char c : version) {
		if (c >= '0' && c <= '9') {
			number.push_back(c);
			continue;
		}

		if (!number.empty()) {
			parts.push_back(std::atoi(number.c_str()));
			number.clear();
		}

		if (c != '.') {
			break;
		}
	}

	if (!number.empty()) {
		parts.push_back(std::atoi(number.c_str()));
	}

	return parts;
}

/* Which speech models are installed, as the build that installed them wrote it.
 * Empty when there is no such file, which covers every install older than this
 * feature and every build without voice: both mean fetch the models too. */
std::string installed_models()
{
	char *path = obs_module_file("voice/models/models.txt");

	if (!path) {
		return {};
	}

	const std::string version = read_file(path);
	bfree(path);
	return version;
}

/* Where an update is unpacked, and where the script that installs it lives.
 * Beside the user's own application data rather than in TEMP, which gets swept
 * while OBS is closed, which is exactly when the script is waiting to run. */
fs::path work_dir()
{
	/* A test rig points this somewhere it can watch. A streamer never sets it,
	 * the same way they never set PLASMASTREAM_URL. */
	const char *told = std::getenv("PLASMASTREAM_UPDATE_DIR");

	if (told && *told) {
		return fs::path(told);
	}

#ifdef _WIN32
	const char *local = std::getenv("LOCALAPPDATA");

	if (local && *local) {
		return fs::path(local) / "PlasmaStream" / "update";
	}
#endif

	const char *home = std::getenv("HOME");
	return fs::path(home && *home ? home : ".") / ".plasmastream-update";
}

#ifdef _WIN32

/* This DLL's own path, which is the only honest answer to "where am I
 * installed?". A streamer may have put the plugin under ProgramData or under
 * their own AppData, and the update belongs wherever the copy that is running
 * came from. */
fs::path own_dll()
{
	HMODULE self = nullptr;

	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&own_dll), &self)) {
		return {};
	}

	wchar_t buffer[MAX_PATH] = {0};

	if (GetModuleFileNameW(self, buffer, MAX_PATH) == 0) {
		return {};
	}

	return fs::path(buffer);
}

/* The folder the archive extracts over: the one holding this plugin's own folder.
 *
 * Empty when the layout is not the one the download produces, which is how a
 * plugin somebody built and placed by hand, or one shipped inside an OBS install,
 * declines to be overwritten by an archive that assumes otherwise.
 */
fs::path install_root()
{
	const fs::path dll = own_dll();

	if (dll.empty()) {
		return {};
	}

	/* ...\plugins\plasmastream-multistream\bin\64bit\plasmastream-multistream.dll */
	const fs::path sixtyfour = dll.parent_path();
	const fs::path bin = sixtyfour.parent_path();
	const fs::path module = bin.parent_path();

	if (sixtyfour.filename() != "64bit" || bin.filename() != "bin") {
		return {};
	}

	if (module.filename() != "plasmastream-multistream") {
		return {};
	}

	return module.parent_path();
}

/* Windows' own tools, by full path.
 *
 * Ends with a backslash, so a name appends straight onto it. */
std::wstring system32()
{
	wchar_t buffer[MAX_PATH] = {0};
	const UINT length = GetSystemDirectoryW(buffer, MAX_PATH);

	if (length == 0 || length >= MAX_PATH) {
		return L"C:\\Windows\\System32\\";
	}

	std::wstring path(buffer, length);

	if (path.back() != L'\\') {
		path.push_back(L'\\');
	}

	return path;
}

/* Runs a program and waits for it, with no console window. For the two things
 * Windows already ships that this would otherwise need a library for: unpacking
 * a zip, and waiting for a process to end. */
bool run_and_wait(const std::wstring &command)
{
	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process = {};

	std::wstring mutable_command = command;

	if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
			    nullptr, &startup, &process)) {
		return false;
	}

	WaitForSingleObject(process.hProcess, INFINITE);

	DWORD code = 1;
	GetExitCodeProcess(process.hProcess, &code);
	CloseHandle(process.hProcess);
	CloseHandle(process.hThread);

	return code == 0;
}

/* tar is part of Windows and reads zips. PowerShell is the fallback for an
 * install old enough not to have it. */
bool unpack(const fs::path &archive, const fs::path &into)
{
	std::error_code ignored;
	fs::create_directories(into, ignored);

	const std::wstring tar = L"\"" + system32() + L"tar.exe\" -xf \"" + archive.wstring() + L"\" -C \"" +
				 into.wstring() + L"\"";

	if (run_and_wait(tar)) {
		return true;
	}

	const std::wstring powershell = L"\"" + system32() +
					L"WindowsPowerShell\\v1.0\\powershell.exe\" -NoProfile -ExecutionPolicy Bypass -Command "
					L"\"Expand-Archive -LiteralPath '" +
					archive.wstring() + L"' -DestinationPath '" + into.wstring() + L"' -Force\"";

	return run_and_wait(powershell);
}

/* The script that does the swap after OBS exits.
 *
 * A batch file rather than anything cleverer, because it has to run when this
 * plugin no longer exists to run anything: OBS is gone and the DLL is unloaded,
 * so whatever finishes the job has to already be on the machine. cmd always is.
 *
 * robocopy rather than copy: it retries a file that is briefly still locked while
 * OBS finishes exiting, and its exit codes tell "copied nothing because nothing
 * changed" apart from "could not copy". Anything under 8 means it worked.
 */
bool write_script(const fs::path &script, const fs::path &staged, const fs::path &root, const fs::path &result,
		  bool relaunch)
{
	wchar_t obs[MAX_PATH] = {0};
	GetModuleFileNameW(nullptr, obs, MAX_PATH);

	std::wofstream file(script, std::ios::binary);

	if (!file) {
		return false;
	}

	/* Full paths, not names. A developer's PATH can put an MSYS or Cygwin bin
	 * directory ahead of System32, and their `find` is a different program that
	 * answers a different question: the loop below would then decide OBS had
	 * already exited and copy over a locked DLL. The plugin inherits whatever
	 * PATH OBS was started with, so this cannot be assumed either way. */
	const std::wstring system = system32();
	const std::wstring log = (fs::path(result).parent_path() / "install.log").wstring();

	file << L"@echo off\r\n";
	file << L"setlocal\r\n";
	file << L"set PID=" << GetCurrentProcessId() << L"\r\n";
	file << L">\"" << log << L"\" echo waiting for %PID%\r\n";
	file << L":wait\r\n";
	file << L"\"" << system << L"tasklist.exe\" /FI \"PID eq %PID%\" /NH 2>nul | \"" << system
	     << L"find.exe\" \"%PID%\" >nul\r\n";
	file << L"if not errorlevel 1 (\r\n";
	/* ping rather than timeout, which wants a console to read a key from and
	 * this script has none. */
	file << L"  \"" << system << L"ping.exe\" -n 2 127.0.0.1 >nul\r\n";
	file << L"  goto wait\r\n";
	file << L")\r\n";
	file << L">>\"" << log << L"\" echo copying\r\n";
	file << L"\"" << system << L"robocopy.exe\" \"" << staged.wstring() << L"\" \"" << root.wstring()
	     << L"\" /E /IS /IT /R:3 /W:2 >>\"" << log << L"\"\r\n";
	file << L"if errorlevel 8 (\r\n";
	file << L"  >\"" << result.wstring() << L"\" echo failed %errorlevel%\r\n";
	file << L") else (\r\n";
	file << L"  >\"" << result.wstring() << L"\" echo ok\r\n";
	file << L"  rmdir /s /q \"" << staged.wstring() << L"\"\r\n";
	file << L")\r\n";

	if (relaunch && obs[0]) {
		file << L"start \"\" \"" << obs << L"\"\r\n";
	}

	return file.good();
}

/* Starts the script and does not wait for it: it outlives this process on
 * purpose, because what it is waiting for is this process ending. */
bool launch_script(const fs::path &script)
{
	std::wstring command = L"\"" + system32() + L"cmd.exe\" /c \"" + script.wstring() + L"\"";

	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process = {};

	/* Out of any job the launcher put OBS in, so closing OBS cannot take the
	 * script with it. Some launchers do this and some do not, and a job that
	 * forbids breakaway refuses the flag outright, so it is tried and then done
	 * without. CREATE_NO_WINDOW rather than DETACHED_PROCESS: the two together
	 * are contradictory, and a console cmd never shows is what this wants. */
	for (const DWORD flags : {CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB, CREATE_NO_WINDOW}) {
		std::wstring attempt = command;

		if (CreateProcessW(nullptr, attempt.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &startup,
				   &process)) {
			CloseHandle(process.hProcess);
			CloseHandle(process.hThread);
			return true;
		}
	}

	return false;
}

#endif /* _WIN32 */

/* What the endpoint said, in the parts this cares about. */
struct Answer {
	bool ok = false;
	std::string version;
	std::vector<std::string> notes;
	std::string page;
	std::string archive;
	std::string kind;
	std::string models_version;
	std::string models_url;
};

Answer ask_the_site()
{
	Answer answer;

	const HttpResponse response = http_get(plasmastream_url("/api/plugin/update"));

	if (!response.ok()) {
		return answer;
	}

	obs_data_t *data = obs_data_create_from_json(response.body.c_str());

	if (!data) {
		return answer;
	}

	answer.version = obs_data_get_string(data, "version");
	answer.page = obs_data_get_string(data, "page");

	if (!answer.page.empty() && answer.page.front() == '/') {
		answer.page = plasmastream_url(answer.page);
	}

	/* Objects with a "line", not bare strings: obs_data's reader is built for
	 * arrays of objects, and an array of strings is the one shape it is not
	 * worth being clever about. */
	obs_data_array_t *notes = obs_data_get_array(data, "notes");

	if (notes) {
		const size_t count = obs_data_array_count(notes);

		for (size_t i = 0; i < count; i++) {
			obs_data_t *item = obs_data_array_item(notes, i);

			if (!item) {
				continue;
			}

			const char *line = obs_data_get_string(item, "line");

			if (line && *line) {
				answer.notes.emplace_back(line);
			}

			obs_data_release(item);
		}

		obs_data_array_release(notes);
	}

	obs_data_t *platform = obs_data_get_obj(data, PLATFORM);

	if (platform) {
		answer.kind = obs_data_get_string(platform, "kind");
		answer.archive = obs_data_get_string(platform, "url");

		/* The smaller archive where the release offers one: the same build
		 * with the speech models left out, for an install that has them
		 * already. */
		obs_data_t *smaller = obs_data_get_obj(platform, "update");

		if (smaller) {
			const char *url = obs_data_get_string(smaller, "url");

			if (url && *url) {
				answer.archive = url;
			}

			obs_data_release(smaller);
		}

		obs_data_release(platform);
	}

	obs_data_t *models = obs_data_get_obj(data, "models");

	if (models) {
		answer.models_version = obs_data_get_string(models, "version");
		answer.models_url = obs_data_get_string(models, "url");
		obs_data_release(models);
	}

	obs_data_release(data);
	answer.ok = !answer.version.empty();
	return answer;
}

void set_percent(int percent)
{
	std::lock_guard<std::mutex> guard(state().mutex);
	state().offer.percent = percent;
}

void download_and_stage()
{
	std::string version;
	std::string archive;
	std::string models;

	{
		std::lock_guard<std::mutex> guard(state().mutex);
		version = state().offer.version;
		archive = state().archive_url;
		models = state().models_url;
	}

#ifdef _WIN32
	const fs::path work = work_dir();
	const fs::path staged = work / version;
	std::error_code ignored;

	fs::remove_all(staged, ignored);
	fs::create_directories(staged, ignored);

	const fs::path archive_file = work / "download.zip";

	set_percent(0);
	set_stage(UpdateStage::downloading);

	/* Two downloads share one bar when the models come too, which is the only
	 * time the second one exists. The models are most of the bytes, so the bar
	 * would otherwise sit at 100 percent for several minutes. */
	const bool two = !models.empty();

	const auto part = [two](bool second) {
		return [two, second](double fraction) {
			const double whole = two ? (second ? 0.2 + fraction * 0.8 : fraction * 0.2) : fraction;
			set_percent(static_cast<int>(whole * 100.0));
			return !g_cancel.load();
		};
	};

	if (!http_download(archive, archive_file.string(), part(false))) {
		set_stage(UpdateStage::failed, UpdateProblem::download_failed);
		return;
	}

	if (!unpack(archive_file, staged)) {
		set_stage(UpdateStage::failed, UpdateProblem::unpack_failed);
		return;
	}

	if (two) {
		if (!http_download(models, archive_file.string(), part(true))) {
			set_stage(UpdateStage::failed, UpdateProblem::download_failed);
			return;
		}

		if (!unpack(archive_file, staged)) {
			set_stage(UpdateStage::failed, UpdateProblem::unpack_failed);
			return;
		}
	}

	fs::remove(archive_file, ignored);

	/* An archive with no plugin in it is not an update, whatever its name says.
	 * Better to stop here than to copy half a tree over a working install. */
	const fs::path dll = staged / "plasmastream-multistream" / "bin" / "64bit" / "plasmastream-multistream.dll";

	if (!fs::exists(dll)) {
		fs::remove_all(staged, ignored);
		set_stage(UpdateStage::failed, UpdateProblem::not_an_update);
		return;
	}

	{
		std::lock_guard<std::mutex> guard(state().mutex);
		state().staged = staged.string();
	}

	set_percent(100);
	set_stage(UpdateStage::ready);
	obs_log(LOG_INFO, "update %s is unpacked and waiting for OBS to close", version.c_str());
#else
	(void)archive;
	(void)models;
	set_stage(UpdateStage::manual);
#endif
}

void finish_check(const Answer &answer, bool manual)
{
	if (!answer.ok) {
		set_stage(manual ? UpdateStage::failed : UpdateStage::idle,
			  manual ? UpdateProblem::no_answer : UpdateProblem::none);
		return;
	}

	if (!update_is_newer(answer.version, PLUGIN_VERSION)) {
		set_stage(UpdateStage::idle);
		return;
	}

	const std::string models = installed_models();
	const bool needs_models = !answer.models_url.empty() && models != answer.models_version;

	{
		std::lock_guard<std::mutex> guard(state().mutex);
		state().offer = UpdateOffer{answer.version, answer.notes, answer.page, 0};
		state().archive_url = answer.archive;
		state().models_url = needs_models ? answer.models_url : std::string();
		state().staged.clear();
	}

	/* A package this cannot install without a password, or a layout the archive
	 * does not fit: say there is an update and point at the page. */
#ifdef _WIN32
	const bool installable = answer.kind == "folder" && !answer.archive.empty() && !install_root().empty();
#else
	const bool installable = false;
#endif

	set_stage(installable ? UpdateStage::found : UpdateStage::manual);
	obs_log(LOG_INFO, "version %s is available (this is %s)", answer.version.c_str(), PLUGIN_VERSION);

	/* Already on the worker, so this runs here rather than asking for a second
	 * one that would be refused while this one is still going. */
	if (installable && config().update.automatic) {
		download_and_stage();
	}
}

/* Runs one job on a worker, refusing a second while one is going. */
void run_in_background(void (*job)())
{
	if (g_busy.exchange(true)) {
		return;
	}

	if (g_worker.joinable()) {
		g_worker.join();
	}

	g_worker = std::thread([job]() {
		job();
		g_busy.store(false);
	});
}

void check_job_quiet()
{
	set_stage(UpdateStage::checking);
	finish_check(ask_the_site(), false);
}

void check_job_manual()
{
	set_stage(UpdateStage::checking);
	finish_check(ask_the_site(), true);
}

/* Reads what the swap script left behind, and says so once.
 *
 * An update that worked needs no announcement: the version in the dock and the
 * log is the new one. An update that could not be copied does need one, because
 * otherwise OBS just starts up old again and nothing ever explains why.
 */
void read_last_result()
{
	const fs::path result = work_dir() / "result.txt";
	std::error_code ignored;

	if (!fs::exists(result, ignored)) {
		return;
	}

	const std::string outcome = read_file(result);
	fs::remove(result, ignored);

	if (outcome.rfind("failed", 0) == 0) {
		set_stage(UpdateStage::failed, UpdateProblem::copy_failed);
		obs_log(LOG_WARNING, "the last update could not be copied into place (%s)", outcome.c_str());
		return;
	}

	obs_log(LOG_INFO, "an update was installed, now running %s", PLUGIN_VERSION);
}

} // namespace

bool update_is_newer(const std::string &candidate, const std::string &current)
{
	const std::vector<int> theirs = version_parts(candidate);
	const std::vector<int> ours = version_parts(current);

	for (size_t i = 0; i < theirs.size() || i < ours.size(); i++) {
		const int a = i < theirs.size() ? theirs[i] : 0;
		const int b = i < ours.size() ? ours[i] : 0;

		if (a != b) {
			return a > b;
		}
	}

	return false;
}

UpdateStage update_stage()
{
	std::lock_guard<std::mutex> guard(state().mutex);
	return state().stage;
}

UpdateProblem update_problem()
{
	std::lock_guard<std::mutex> guard(state().mutex);
	return state().problem;
}

UpdateOffer update_offer()
{
	std::lock_guard<std::mutex> guard(state().mutex);
	return state().offer;
}

void update_check(bool manual)
{
	if (!manual && !config().update.check) {
		return;
	}

	run_in_background(manual ? check_job_manual : check_job_quiet);
}

void update_download()
{
	run_in_background(download_and_stage);
}

void update_install(bool relaunch)
{
#ifdef _WIN32
	std::string staged;

	{
		std::lock_guard<std::mutex> guard(state().mutex);
		staged = state().staged;

		if (staged.empty() || state().handed_over) {
			return;
		}

		state().handed_over = true;
	}

	const fs::path root = install_root();

	if (root.empty()) {
		set_stage(UpdateStage::manual, UpdateProblem::copy_failed);
		return;
	}

	const fs::path work = work_dir();
	const fs::path script = work / "install.cmd";
	const fs::path result = work / "result.txt";

	std::error_code ignored;
	fs::remove(result, ignored);

	if (!write_script(script, staged, root, result, relaunch) || !launch_script(script)) {
		{
			std::lock_guard<std::mutex> guard(state().mutex);
			state().handed_over = false;
		}

		set_stage(UpdateStage::failed, UpdateProblem::copy_failed);
		return;
	}

	obs_log(LOG_INFO, "the update goes into %s once OBS exits", root.string().c_str());
#else
	(void)relaunch;
#endif
}

void update_init()
{
	read_last_result();

	g_timer = std::thread([]() {
		auto wait = FIRST_CHECK_AFTER;

		while (true) {
			std::unique_lock<std::mutex> lock(g_sleep_mutex);

			if (g_sleep.wait_for(lock, wait, []() { return g_stopping.load(); })) {
				return;
			}

			lock.unlock();
			update_check(false);
			wait = std::chrono::duration_cast<std::chrono::seconds>(CHECK_EVERY);
		}
	});
}

void update_shutdown()
{
	/* An update downloaded but never installed goes in now: closing OBS is
	 * exactly the moment it becomes possible, and somebody who pressed Update
	 * should not have to press anything else. No relaunch, since they are
	 * closing OBS on purpose. */
	if (update_stage() == UpdateStage::ready) {
		update_install(false);
	}

	g_stopping.store(true);
	g_cancel.store(true);
	g_sleep.notify_all();

	if (g_timer.joinable()) {
		g_timer.join();
	}

	if (g_worker.joinable()) {
		g_worker.join();
	}
}

} // namespace plasmastream
