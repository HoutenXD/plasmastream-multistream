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

/* Keeping the plugin current, without asking anybody to find a download page.
 *
 * ## Why it cannot simply overwrite itself
 *
 * OBS loads the plugin at startup and holds the DLL open for as long as it runs,
 * so nothing can replace it while OBS is up. Every step here is arranged around
 * that one fact: the new version is unpacked into a folder of its own while OBS
 * runs, and a small script copies it into place after OBS exits. Nothing is ever
 * half replaced, and a machine that loses power midway still has the install it
 * started with.
 *
 * ## What it will not do
 *
 * It will not install anything that needs a password. On macOS and Linux the
 * plugin arrives as a signed package and a .deb, which are the system's business
 * rather than ours, so there an update is an offer to open the download page.
 *
 * ## Nothing is sent
 *
 * The check is a plain GET of a public endpoint carrying no token, no channel and
 * no machine identifier. Everyone who asks gets the same answer.
 */

enum class UpdateStage {
	/* Nothing asked yet, or the answer was that this is the current version. */
	idle,
	checking,
	/* Newer version, and this install can replace itself with it. */
	found,
	/* Newer version, but putting it in place is the system's job. */
	manual,
	downloading,
	/* Unpacked, waiting for OBS to close. */
	ready,
	failed,
};

/* Why an update stopped. The words belong to the dock, which is where the rest of
 * this plugin's sentences live and the only place they can be translated. */
enum class UpdateProblem {
	none,
	/* The site could not be reached, or said something unreadable. */
	no_answer,
	download_failed,
	unpack_failed,
	/* An archive that does not contain this plugin. */
	not_an_update,
	/* The files could not be copied over the install, which on Windows almost
	 * always means the plugin lives somewhere this user cannot write. */
	copy_failed,
};

struct UpdateOffer {
	std::string version;
	std::vector<std::string> notes;
	/* The download page, for the platforms that install through a package and
	 * for anybody who would rather do it by hand. */
	std::string page;
	/* How far the download has got, 0 to 100. */
	int percent = 0;
};

/* All three are safe to call from the UI thread; the work happens elsewhere. */
UpdateStage update_stage();
UpdateProblem update_problem();
UpdateOffer update_offer();

/* Ask the site whether there is anything newer.
 *
 * Returns immediately; the answer lands on a background thread. `manual` is a
 * person pressing a button, so it runs even when automatic checks are switched
 * off, and it reports being up to date rather than staying quiet.
 */
void update_check(bool manual);

/* Download the new version and unpack it beside the old one. Returns
 * immediately. Only meaningful in the `found` stage. */
void update_download();

/* Hand the swap to a script that waits for this OBS to exit.
 *
 * Only meaningful in the `ready` stage, and only ever called once. `relaunch`
 * starts OBS again once the files are in place, which is what the dock's button
 * does: from the streamer's side OBS closes, blinks, and comes back new. Closing
 * OBS is the caller's job, because only the UI can ask it nicely.
 */
void update_install(bool relaunch);

/* Called once at module load and once at unload.
 *
 * Load reports the result of any swap that ran while OBS was closed, so an update
 * that could not be copied says so rather than silently not happening, and starts
 * the timer for the first check. Unload hands over an update that was downloaded
 * but never installed, so closing OBS normally is enough to finish one.
 */
void update_init();
void update_shutdown();

/* Compares two dotted versions. Exposed for the check that proves 0.10.0 reads as
 * newer than 0.9.0, which comparing the strings gets backwards. */
bool update_is_newer(const std::string &candidate, const std::string &current);

} // namespace plasmastream
