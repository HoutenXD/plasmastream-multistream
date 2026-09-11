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

#include "vertical.hpp"

#include "config.hpp"

#include <mutex>
#include <utility>
#include <vector>

#include <QtGlobal>

#include <graphics/vec2.h>
#include <obs-frontend-api.h>
#include <obs-module.h>

#if PLASMASTREAM_HAS_CANVAS

namespace plasmastream {

namespace {

/* Everything the vertical frame is made of. One of these, for the life of the
 * module. */
struct Vertical {
	obs_canvas_t *canvas = nullptr;
	obs_scene_t *scene = nullptr;

	/* The program, as one item on that scene. In this model the whole wide
	 * composition is a single picture that can be placed and sized, which is
	 * what separates it from a second set of scenes: you arrange where your
	 * stream sits in the tall frame, not what is in it. */
	obs_sceneitem_t *program = nullptr;

	/* Which programme scene the item above is showing, so the framing control
	 * knows whose arrangement it is editing. */
	std::string scene_name;

	/* Your own sources, over the top of it, keyed by source name. Held active
	 * for as long as they are here, which is what lets a camera that appears in
	 * none of your wide scenes still light up for the vertical one. */
	std::vector<std::pair<std::string, obs_sceneitem_t *>> extras;

	uint32_t width = 0;
	uint32_t height = 0;
};

Vertical g_vertical;

/* Guards the struct above. The dock asks for the canvas on Qt's thread while the
 * frontend fires scene changes on its own. */
std::mutex g_mutex;

void drop_program()
{
	if (!g_vertical.program) {
		return;
	}

	obs_sceneitem_remove(g_vertical.program);
	obs_sceneitem_release(g_vertical.program);
	g_vertical.program = nullptr;
}

/* Defined below, beside the rest of the extras handling. */
void drop_extras();

void release_all()
{
	drop_extras();
	drop_program();

	if (g_vertical.canvas) {
		obs_canvas_remove(g_vertical.canvas);
		obs_canvas_release(g_vertical.canvas);
		g_vertical.canvas = nullptr;
		g_vertical.scene = nullptr;
	}

	g_vertical.width = 0;
	g_vertical.height = 0;
}

/* Put the programme where this scene's framing says, as a box inside the tall
 * frame.
 *
 * The framing is fractions of the canvas, so it survives the canvas being
 * resized; this is where those fractions become pixels. OUTER fills that box and
 * loses whatever overflows it, INNER fits inside and accepts bars within it.
 * Assumes the lock. */
void apply_framing(obs_sceneitem_t *item, const Framing &framing)
{
	const auto width = static_cast<float>(g_vertical.width);
	const auto height = static_cast<float>(g_vertical.height);

	/* Never zero, whatever the file said: a box with no pixels in it reads as
	 * a broken preview rather than as a bad setting. */
	const float boxW = qMax(1.0f, static_cast<float>(framing.width) * width);
	const float boxH = qMax(1.0f, static_cast<float>(framing.height) * height);

	vec2 bounds;
	vec2_set(&bounds, boxW, boxH);

	obs_sceneitem_set_bounds_type(item, framing.crop ? OBS_BOUNDS_SCALE_OUTER
							 : OBS_BOUNDS_SCALE_INNER);
	obs_sceneitem_set_bounds(item, &bounds);
	obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);

	/* Top left, like every other item on this canvas. Dragging works on the
	 * rectangle an item occupies, and a rectangle is far easier to reason about
	 * when its position is a corner rather than a centre that has to have half
	 * the size added back before it means anything. */
	obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);

	vec2 corner;
	vec2_set(&corner, static_cast<float>(framing.x) * width,
		 static_cast<float>(framing.y) * height);
	obs_sceneitem_set_pos(item, &corner);
}

/* Put the program on the scene and shape it. Assumes the lock. */
void aim_at_program()
{
	if (!g_vertical.scene || !g_vertical.canvas) {
		return;
	}

	obs_source_t *program = obs_frontend_get_current_scene();

	if (!program) {
		return;
	}

	drop_program();

	obs_sceneitem_t *item = obs_scene_add(g_vertical.scene, program);

	/* The name before the release, not after: the pointer is only ours until
	 * then. */
	const char *name = obs_source_get_name(program);
	const std::string scene = name ? name : "";

	obs_source_release(program);

	if (!item) {
		return;
	}

	/* obs_scene_add hands back the scene's own reference rather than a new
	 * one, so keeping it past this function means taking one. */
	obs_sceneitem_addref(item);
	g_vertical.program = item;
	g_vertical.scene_name = scene;

	/* Under everything. obs_scene_add puts a new item on top, and on a scene
	 * change the programme is the one being re-added, so without this it would
	 * climb over the camera every time somebody switched scenes. */
	obs_sceneitem_set_order(item, OBS_ORDER_MOVE_BOTTOM);

	apply_framing(item, framing_for(scene));
}

/* Take one of the extras off the frame. Assumes the lock. */
void drop_extra(size_t index)
{
	auto &[name, item] = g_vertical.extras[index];

	if (item) {
		obs_source_t *source = obs_sceneitem_get_source(item);

		/* Balanced against the inc when it was added. A source nobody else is
		 * using goes back to sleep. */
		if (source) {
			obs_source_dec_active(source);
		}

		obs_sceneitem_remove(item);
		obs_sceneitem_release(item);
	}

	g_vertical.extras.erase(g_vertical.extras.begin() + static_cast<long long>(index));
}

void drop_extras()
{
	while (!g_vertical.extras.empty()) {
		drop_extra(g_vertical.extras.size() - 1);
	}
}

/* Put a source on the frame at the transform the config remembers, or at a
 * sensible default if it has none yet. Assumes the lock. */
obs_sceneitem_t *place_extra(const VerticalSource &wanted)
{
	obs_source_t *source = obs_get_source_by_name(wanted.name.c_str());

	if (!source) {
		/* The scene collection changed under us, or it was renamed. Not an
		 * error worth shouting about: the entry stays in the config so it
		 * comes back if the source does. */
		return nullptr;
	}

	obs_sceneitem_t *item = obs_scene_add(g_vertical.scene, source);

	if (!item) {
		obs_source_release(source);
		return nullptr;
	}

	obs_sceneitem_addref(item);

	/* Active for as long as it is on the frame, so a camera used by no wide
	 * scene still runs. The canvas itself has no ACTIVATE flag, on purpose, so
	 * this is the only thing keeping it awake. */
	obs_source_inc_active(source);
	obs_source_release(source);

	obs_sceneitem_set_visible(item, wanted.visible);

	obs_transform_info info = {};
	obs_sceneitem_get_info2(item, &info);

	if (wanted.width > 0.0 && wanted.height > 0.0) {
		vec2_set(&info.pos, static_cast<float>(wanted.x), static_cast<float>(wanted.y));
		vec2_set(&info.bounds, static_cast<float>(wanted.width),
			 static_cast<float>(wanted.height));
		info.bounds_type = static_cast<obs_bounds_type>(wanted.bounds_type);
	} else {
		/* Never placed: a third of the width, bottom left, which is out of
		 * the way of a band across the top and visible enough to be found
		 * and dragged. */
		const auto width = static_cast<float>(g_vertical.width);
		const auto height = static_cast<float>(g_vertical.height);
		const float box = width / 3.0f;

		vec2_set(&info.pos, width * 0.06f, height - box - height * 0.06f);
		vec2_set(&info.bounds, box, box);
		info.bounds_type = OBS_BOUNDS_SCALE_INNER;
	}

	info.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
	info.bounds_alignment = OBS_ALIGN_CENTER;
	obs_sceneitem_set_info2(item, &info);

	return item;
}

/* Put every configured extra back on a freshly built frame. Assumes the lock. */
void restore_extras()
{
	for (const VerticalSource &wanted : config().vertical_sources) {
		obs_sceneitem_t *item = place_extra(wanted);

		if (item) {
			g_vertical.extras.emplace_back(wanted.name, item);
		}
	}
}

/* Build it, at the size the config asks for. Assumes the lock. */
bool build()
{
	obs_video_info ovi = {};

	if (!obs_get_video_info(&ovi)) {
		/* No video yet, which happens if something asks during module load
		 * before OBS has reset video. Nothing is lost by trying again. */
		return false;
	}

	ovi.base_width = ovi.output_width = static_cast<uint32_t>(config().vertical_width);
	ovi.base_height = ovi.output_height = static_cast<uint32_t>(config().vertical_height);

	/* SCENE_REF and nothing else. ACTIVATE would activate every source a
	 * second time, and a capture device that dislikes being opened twice would
	 * pick the middle of a stream to say so. The program scene is already
	 * active for the main canvas; this one only renders it again. MIX_AUDIO is
	 * off for the same kind of reason: audio comes from the main mix once. */
	g_vertical.canvas = obs_canvas_create_private("PlasmaStream vertical", &ovi, SCENE_REF);

	if (!g_vertical.canvas) {
		blog(LOG_WARNING, "[plasmastream] could not create the vertical canvas");
		return false;
	}

	g_vertical.scene = obs_canvas_scene_create(g_vertical.canvas, "PlasmaStream vertical");

	if (!g_vertical.scene) {
		obs_canvas_remove(g_vertical.canvas);
		obs_canvas_release(g_vertical.canvas);
		g_vertical.canvas = nullptr;
		return false;
	}

	g_vertical.width = ovi.base_width;
	g_vertical.height = ovi.base_height;

	aim_at_program();
	restore_extras();
	obs_canvas_set_channel(g_vertical.canvas, 0, obs_scene_get_source(g_vertical.scene));

	blog(LOG_INFO, "[plasmastream] vertical canvas ready at %ux%u", g_vertical.width,
	     g_vertical.height);

	return true;
}

} // namespace

obs_canvas_t *vertical_canvas()
{
	std::lock_guard<std::mutex> lock(g_mutex);

	if (!g_vertical.canvas && !build()) {
		return nullptr;
	}

	return g_vertical.canvas;
}

void vertical_resize()
{
	std::lock_guard<std::mutex> lock(g_mutex);

	if (!g_vertical.canvas) {
		return;
	}

	const auto wanted_w = static_cast<uint32_t>(config().vertical_width);
	const auto wanted_h = static_cast<uint32_t>(config().vertical_height);

	if (g_vertical.width == wanted_w && g_vertical.height == wanted_h) {
		/* The framing may still have changed, and that is cheap. */
		aim_at_program();
		return;
	}

	/* Rebuilt rather than reset: obs_canvas_reset_video leaves encoders that
	 * are already reading the old mix pointed at it, and the only moment this
	 * is called is when nothing is streaming. */
	release_all();
	build();
}

std::string vertical_scene()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_vertical.scene_name;
}

void vertical_reframe()
{
	std::lock_guard<std::mutex> lock(g_mutex);

	if (g_vertical.program) {
		apply_framing(g_vertical.program, framing_for(g_vertical.scene_name));
	}
}

void vertical_follow_program()
{
	std::lock_guard<std::mutex> lock(g_mutex);

	if (g_vertical.canvas) {
		aim_at_program();
	}
}

bool vertical_add_source(const std::string &name)
{
	std::lock_guard<std::mutex> lock(g_mutex);

	if (!g_vertical.scene) {
		return false;
	}

	for (const auto &[existing, item] : g_vertical.extras) {
		if (existing == name) {
			return false;
		}
	}

	VerticalSource wanted;
	wanted.name = name;

	obs_sceneitem_t *item = place_extra(wanted);

	if (!item) {
		return false;
	}

	g_vertical.extras.emplace_back(name, item);
	return true;
}

void vertical_remove_source(const std::string &name)
{
	std::lock_guard<std::mutex> lock(g_mutex);

	for (size_t i = 0; i < g_vertical.extras.size(); i++) {
		if (g_vertical.extras[i].first == name) {
			drop_extra(i);
			return;
		}
	}
}

obs_sceneitem_t *vertical_source_item(const std::string &name)
{
	std::lock_guard<std::mutex> lock(g_mutex);

	for (const auto &[existing, item] : g_vertical.extras) {
		if (existing == name) {
			return item;
		}
	}

	return nullptr;
}

void vertical_reload_sources()
{
	std::lock_guard<std::mutex> lock(g_mutex);

	if (!g_vertical.scene) {
		return;
	}

	drop_extras();
	restore_extras();
}

void vertical_capture_layout()
{
	std::lock_guard<std::mutex> lock(g_mutex);

	for (const auto &[name, item] : g_vertical.extras) {
		if (!item) {
			continue;
		}

		obs_transform_info info = {};
		obs_sceneitem_get_info2(item, &info);

		for (VerticalSource &stored : config().vertical_sources) {
			if (stored.name != name) {
				continue;
			}

			stored.x = info.pos.x;
			stored.y = info.pos.y;
			stored.width = info.bounds.x;
			stored.height = info.bounds.y;
			stored.bounds_type = static_cast<int>(info.bounds_type);
			stored.visible = obs_sceneitem_visible(item);
			break;
		}
	}
}

std::vector<VerticalItem> vertical_items()
{
	std::lock_guard<std::mutex> lock(g_mutex);

	std::vector<VerticalItem> items;

	/* The programme first, so a click landing on both picks whatever is on top
	 * of it rather than the block underneath everything. */
	if (g_vertical.program) {
		items.push_back({g_vertical.scene_name, g_vertical.program, true});
	}

	for (const auto &[name, item] : g_vertical.extras) {
		items.push_back({name, item, false});
	}

	return items;
}

void vertical_commit_item(const VerticalItem &item)
{
	std::lock_guard<std::mutex> lock(g_mutex);

	if (!item.item) {
		return;
	}

	obs_transform_info info = {};
	obs_sceneitem_get_info2(item.item, &info);

	if (!item.is_program) {
		for (VerticalSource &stored : config().vertical_sources) {
			if (stored.name != item.name) {
				continue;
			}

			stored.x = info.pos.x;
			stored.y = info.pos.y;
			stored.width = info.bounds.x;
			stored.height = info.bounds.y;
			stored.bounds_type = static_cast<int>(info.bounds_type);
			break;
		}

		return;
	}

	/* The programme is remembered as fractions of the canvas, so the pixels a
	 * drag produced have to go back the way they came. */
	const auto width = static_cast<double>(qMax(1u, g_vertical.width));
	const auto height = static_cast<double>(qMax(1u, g_vertical.height));

	Framing framing = framing_for(item.name);
	framing.scene = item.name;
	framing.x = info.pos.x / width;
	framing.y = info.pos.y / height;
	framing.width = info.bounds.x / width;
	framing.height = info.bounds.y / height;

	set_framing(framing);
}

void vertical_shutdown()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	release_all();
}

} // namespace plasmastream

#endif
