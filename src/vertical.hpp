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

#include <string>
#include <vector>

namespace plasmastream {

/* The vertical canvas, owned here rather than by whichever output happens to be
 * running.
 *
 * It used to be built inside spin_up and thrown away when the stream stopped,
 * which had two problems. Two vertical destinations made two identical canvases
 * and rendered the same scene twice for no reason. And a canvas that only exists
 * while live is a canvas nobody can arrange beforehand, which is the whole point
 * of being able to arrange it.
 *
 * So: one canvas, made the first time anything asks for it, kept until the
 * module unloads. Outputs borrow it, the dock previews it, and neither has to
 * know the other exists. */

#if PLASMASTREAM_HAS_CANVAS

/* Borrowed, not counted: this module owns it for the life of the plugin, so
 * there is nothing for a caller to release. Null if it could not be made, or if
 * there is no video to derive a frame from yet. */
obs_canvas_t *vertical_canvas();

/* Rebuild at the configured size. Cheap when the size has not changed, so it is
 * safe to call whenever the settings dialog closes. */
void vertical_resize();

/* Re-aim the program block at whatever is on program now. Called from the
 * frontend's scene-changed event. */
void vertical_follow_program();

/* The programme scene the vertical frame is currently showing, so a framing
 * control can say whose arrangement it is editing. Empty before anything is on
 * programme. */
std::string vertical_scene();

/* Re-read the framing for the current scene and apply it. Called when the
 * framing control changes something, so the preview moves as you set it. */
void vertical_reframe();

/* Something on the tall frame that can be dragged.
 *
 * The programme block is in here alongside your own sources, because from the
 * point of view of a mouse they are the same thing: a rectangle to be moved and
 * resized. Where the result gets written differs, which is what the flag is for.
 */
struct VerticalItem {
	std::string name;
	obs_sceneitem_t *item = nullptr;
	/* The programme block, remembered as a Framing; everything else is a
	 * VerticalSource. */
	bool is_program = false;
};

/* Everything draggable, programme first so a click prefers what is over it.
 * Borrowed: the items belong to the scene. */
std::vector<VerticalItem> vertical_items();

/* Read one item's current transform back into the config, after a drag. Does not
 * save the file. */
void vertical_commit_item(const VerticalItem &item);

/* Put one of your existing sources on the tall frame, over the programme.
 * Nothing happens if it is already there. */
bool vertical_add_source(const std::string &name);

/* Take it off again, and stop holding it active. */
void vertical_remove_source(const std::string &name);

/* The scene item for a named source, so OBS's own transform dialog can be opened
 * on it. Null if it is not on the frame. Borrowed. */
obs_sceneitem_t *vertical_source_item(const std::string &name);

/* Put the configured sources back on the frame, resolving them by name again.
 *
 * Needed because the canvas can be built before OBS has loaded the scene
 * collection, at which point none of those names exist yet and every lookup
 * fails silently. Called once the frontend says it has finished loading, and
 * again whenever the collection changes underneath. */
void vertical_reload_sources();

/* Copy every item's current transform back into the config, so whatever OBS's
 * transform dialog just did survives a restart. Does not save the file. */
void vertical_capture_layout();

/* Released at module unload, before libobs tears the graphics subsystem down. */
void vertical_shutdown();

#else

/* obs_sceneitem_t exists on every OBS; only the canvas does not. So this one
 * keeps its real signature and simply never finds anything, which lets the dock
 * call it without a guard at every site. */
inline obs_sceneitem_t *vertical_source_item(const std::string &) { return nullptr; }
inline bool vertical_add_source(const std::string &) { return false; }
inline void vertical_remove_source(const std::string &) {}
inline void vertical_capture_layout() {}
inline void vertical_reload_sources() {}
inline std::string vertical_scene() { return {}; }
inline void vertical_reframe() {}
inline void vertical_resize() {}
inline void vertical_follow_program() {}
inline void vertical_shutdown() {}

#endif

} // namespace plasmastream
