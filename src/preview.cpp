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

#include "preview.hpp"

#include "appearance.hpp"
#include "vertical.hpp"

#include <QMouseEvent>
#include <QResizeEvent>
#include <QWindow>

#include <graphics/matrix4.h>

#include <obs-module.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace plasmastream {

namespace {

/* The widget's native handle in the shape libobs wants.
 *
 * OBS's own frontend has a helper for this and does not export it, so every
 * plugin that draws a preview writes this function again. */
gs_window native_window(QWidget *widget)
{
	gs_window window = {};

#ifdef _WIN32
	window.hwnd = reinterpret_cast<HWND>(widget->winId());
#elif defined(__APPLE__)
	window.view = reinterpret_cast<id>(widget->winId());
#else
	window.id = widget->winId();
	window.display = nullptr;
#endif

	return window;
}

/* Fit a source of one shape inside a widget of another, centered, without
 * stretching it. The letterbox is the point: a vertical canvas shown in a wide
 * box has to look vertical, or the preview is lying about the framing it exists
 * to show. */
void centered_fit(int boxW, int boxH, uint32_t srcW, uint32_t srcH, int &x, int &y, int &w, int &h)
{
	if (srcW == 0 || srcH == 0) {
		x = y = 0;
		w = boxW;
		h = boxH;
		return;
	}

	const double scale =
		qMin(static_cast<double>(boxW) / srcW, static_cast<double>(boxH) / srcH);

	w = static_cast<int>(srcW * scale);
	h = static_cast<int>(srcH * scale);
	x = (boxW - w) / 2;
	y = (boxH - h) / 2;
}

#if PLASMASTREAM_HAS_CANVAS

/* A filled rectangle in canvas coordinates, using OBS's own solid effect. There
 * is no line primitive worth the trouble here: four thin rectangles are an
 * outline, and they scale with the preview without going sub-pixel. */
void fill(float x, float y, float w, float h)
{
	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0.0f);
	gs_matrix_scale3f(w, h, 1.0f);
	gs_draw_sprite(nullptr, 0, 1, 1);
	gs_matrix_pop();
}

/* The selection: an outline, and a square at each corner big enough to aim at. */
void outline(float x, float y, float w, float h, float thickness)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);

	if (!solid) {
		return;
	}

	gs_eparam_t *param = gs_effect_get_param_by_name(solid, "color");
	gs_technique_t *technique = gs_effect_get_technique(solid, "Solid");

	const QColor accent = color::accent();

	vec4 tint;
	vec4_set(&tint, static_cast<float>(accent.redF()), static_cast<float>(accent.greenF()),
		 static_cast<float>(accent.blueF()), 1.0f);
	gs_effect_set_vec4(param, &tint);

	gs_technique_begin(technique);
	gs_technique_begin_pass(technique, 0);

	fill(x, y, w, thickness);
	fill(x, y + h - thickness, w, thickness);
	fill(x, y, thickness, h);
	fill(x + w - thickness, y, thickness, h);

	const float handle = thickness * 5.0f;

	fill(x - handle / 2.0f, y - handle / 2.0f, handle, handle);
	fill(x + w - handle / 2.0f, y - handle / 2.0f, handle, handle);
	fill(x - handle / 2.0f, y + h - handle / 2.0f, handle, handle);
	fill(x + w - handle / 2.0f, y + h - handle / 2.0f, handle, handle);

	gs_technique_end_pass(technique);
	gs_technique_end(technique);
}

#endif

} // namespace

Preview::Preview(QWidget *parent) : QWidget(parent)
{
	/* Native, because OBS draws into a window handle and not into Qt. */
	setAttribute(Qt::WA_NativeWindow);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_DontCreateNativeAncestors);

	setMinimumSize(160, 90);
}

Preview::~Preview()
{
	destroy();

#if PLASMASTREAM_HAS_CANVAS
	if (canvas_) {
		obs_weak_canvas_release(canvas_);
		canvas_ = nullptr;
	}
#endif
}

#if PLASMASTREAM_HAS_CANVAS
void Preview::watch(obs_canvas_t *canvas)
{
	if (canvas_) {
		obs_weak_canvas_release(canvas_);
		canvas_ = nullptr;
	}

	if (canvas) {
		canvas_ = obs_canvas_get_weak_canvas(canvas);
		source_ = Source::Canvas;
	} else {
		source_ = Source::Main;
	}
}
#endif

void Preview::create()
{
	if (display_) {
		return;
	}

	/* winId() first: the handle has to exist before it can be handed over. */
	const gs_window window = native_window(this);

	gs_init_data info = {};
	info.cx = static_cast<uint32_t>(qMax(1, width()));
	info.cy = static_cast<uint32_t>(qMax(1, height()));
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;
	info.window = window;

	display_ = obs_display_create(&info, 0);

	if (!display_) {
		blog(LOG_WARNING, "[plasmastream] could not create a preview display");
		return;
	}

	obs_display_add_draw_callback(display_, render, this);
}

void Preview::destroy()
{
	if (!display_) {
		return;
	}

	/* The callback holds this pointer, so it comes off before the widget can
	 * go anywhere. */
	obs_display_remove_draw_callback(display_, render, this);
	obs_display_destroy(display_);
	display_ = nullptr;
}

#if PLASMASTREAM_HAS_CANVAS

QPointF Preview::toCanvas(const QPointF &widget) const
{
	if (viewport_.width() <= 0.0 || viewport_.height() <= 0.0) {
		return {};
	}

	return {(widget.x() - viewport_.x()) * canvasSize_.width() / viewport_.width(),
		(widget.y() - viewport_.y()) * canvasSize_.height() / viewport_.height()};
}

QRectF Preview::itemRect(obs_sceneitem_t *item) const
{
	matrix4 box;
	obs_sceneitem_get_box_transform(item, &box);

	vec3 tl;
	vec3 br;
	vec3_set(&tl, 0.0f, 0.0f, 0.0f);
	vec3_set(&br, 1.0f, 1.0f, 0.0f);
	vec3_transform(&tl, &tl, &box);
	vec3_transform(&br, &br, &box);

	return QRectF(tl.x, tl.y, br.x - tl.x, br.y - tl.y).normalized();
}

/* Which part of the rectangle a point is on: a corner, or the middle.
 *
 * The tolerance is in canvas units scaled from the preview, so a handle is the
 * same size under the mouse whatever size the dock is. Without that, a handle on
 * a small preview would be a couple of canvas pixels wide and impossible to
 * hit. */
int Preview::grabAt(const QRectF &rect, const QPointF &canvas) const
{
	if (viewport_.width() <= 0.0) {
		return None;
	}

	const double slack = 10.0 * canvasSize_.width() / viewport_.width();

	if (!rect.adjusted(-slack, -slack, slack, slack).contains(canvas)) {
		return None;
	}

	int grab = None;

	if (qAbs(canvas.x() - rect.left()) <= slack) {
		grab |= Left;
	} else if (qAbs(canvas.x() - rect.right()) <= slack) {
		grab |= Right;
	}

	if (qAbs(canvas.y() - rect.top()) <= slack) {
		grab |= Top;
	} else if (qAbs(canvas.y() - rect.bottom()) <= slack) {
		grab |= Bottom;
	}

	/* Edges on their own are not offered: with bounds in play an edge drag
	 * changes the aspect of the box, and that is a thing people do by accident
	 * far more often than on purpose. Corners and the middle only. */
	const bool corner = (grab & (Left | Right)) && (grab & (Top | Bottom));

	return corner ? grab : (rect.contains(canvas) ? Move : None);
}

void Preview::mousePressEvent(QMouseEvent *event)
{
	if (!editable_ || event->button() != Qt::LeftButton) {
		QWidget::mousePressEvent(event);
		return;
	}

	const QPointF canvas = toCanvas(event->position());

	/* Topmost first. vertical_items puts the programme at the front because it
	 * sits underneath everything, so this walks it backwards. */
	const std::vector<VerticalItem> items = vertical_items();

	for (auto entry = items.rbegin(); entry != items.rend(); ++entry) {
		if (!entry->item) {
			continue;
		}

		const QRectF rect = itemRect(entry->item);
		const int grab = grabAt(rect, canvas);

		if (grab == None) {
			continue;
		}

		selected_ = entry->name;
		grab_ = grab;
		grabFrom_ = canvas;
		grabRect_ = rect;
		return;
	}

	/* A click on nothing clears the selection, which is how somebody gets the
	 * outline out of the way to look at the picture. */
	selected_.clear();
	grab_ = None;
}

void Preview::mouseMoveEvent(QMouseEvent *event)
{
	if (!editable_ || grab_ == None || selected_.empty()) {
		QWidget::mouseMoveEvent(event);
		return;
	}

	const QPointF canvas = toCanvas(event->position());
	const QPointF delta = canvas - grabFrom_;


	QRectF rect = grabRect_;

	if (grab_ == Move) {
		rect.translate(delta);
	} else {
		if (grab_ & Left) {
			rect.setLeft(rect.left() + delta.x());
		}

		if (grab_ & Right) {
			rect.setRight(rect.right() + delta.x());
		}

		if (grab_ & Top) {
			rect.setTop(rect.top() + delta.y());
		}

		if (grab_ & Bottom) {
			rect.setBottom(rect.bottom() + delta.y());
		}

		/* A box dragged through itself would invert, and an inverted box is a
		 * source that vanishes. */
		if (rect.width() < 16.0 || rect.height() < 16.0) {
			return;
		}
	}

	for (const VerticalItem &entry : vertical_items()) {
		if (entry.name != selected_ || !entry.item) {
			continue;
		}

		obs_transform_info info = {};
		obs_sceneitem_get_info2(entry.item, &info);

		vec2_set(&info.pos, static_cast<float>(rect.x()), static_cast<float>(rect.y()));
		vec2_set(&info.bounds, static_cast<float>(rect.width()),
			 static_cast<float>(rect.height()));

		/* Whatever it was set to stays: a programme block set to fill keeps
		 * filling as it is resized, and one set to fit keeps fitting. */
		if (info.bounds_type == OBS_BOUNDS_NONE) {
			info.bounds_type = OBS_BOUNDS_SCALE_INNER;
		}

		info.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
		obs_sceneitem_set_info2(entry.item, &info);
		break;
	}
}

void Preview::mouseReleaseEvent(QMouseEvent *event)
{
	if (!editable_ || grab_ == None) {
		QWidget::mouseReleaseEvent(event);
		return;
	}

	grab_ = None;

	for (const VerticalItem &entry : vertical_items()) {
		if (entry.name == selected_) {
			vertical_commit_item(entry);
			emit itemMoved();
			break;
		}
	}
}

#endif

void Preview::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	create();
}

void Preview::hideEvent(QHideEvent *event)
{
	/* Torn down rather than left running: a hidden dock should not be costing
	 * anybody a render pass per frame. */
	destroy();
	QWidget::hideEvent(event);
}

void Preview::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);

	if (display_) {
		obs_display_resize(display_, static_cast<uint32_t>(qMax(1, event->size().width())),
				   static_cast<uint32_t>(qMax(1, event->size().height())));
	}
}

/* Runs on the graphics thread, not Qt's. Touch nothing here that is not libobs. */
void Preview::render(void *data, uint32_t cx, uint32_t cy)
{
	auto *preview = static_cast<Preview *>(data);

	uint32_t sourceW = 0;
	uint32_t sourceH = 0;
	obs_canvas_t *canvas = nullptr;

#if PLASMASTREAM_HAS_CANVAS
	if (preview->source_ == Source::Canvas && preview->canvas_) {
		canvas = obs_weak_canvas_get_canvas(preview->canvas_);

		if (!canvas) {
			return;
		}

		obs_video_info ovi = {};

		if (obs_canvas_get_video_info(canvas, &ovi)) {
			sourceW = ovi.base_width;
			sourceH = ovi.base_height;
		}
	}
#endif

	if (!canvas) {
		obs_video_info ovi = {};

		if (obs_get_video_info(&ovi)) {
			sourceW = ovi.base_width;
			sourceH = ovi.base_height;
		}
	}

	int x = 0;
	int y = 0;
	int w = 0;
	int h = 0;
	centered_fit(static_cast<int>(cx), static_cast<int>(cy), sourceW, sourceH, x, y, w, h);

#if PLASMASTREAM_HAS_CANVAS
	/* Kept for the mouse, which has to run the same fit backwards to turn a
	 * click into a point on the canvas. */
	preview->viewport_ = QRectF(x, y, w, h);
	preview->canvasSize_ = QSizeF(sourceW, sourceH);
#endif

	gs_projection_push();
	gs_viewport_push();

	gs_set_viewport(x, y, w, h);
	gs_ortho(0.0f, static_cast<float>(sourceW), 0.0f, static_cast<float>(sourceH), -100.0f,
		 100.0f);

#if PLASMASTREAM_HAS_CANVAS
	if (canvas) {
		obs_canvas_render(canvas);
		obs_canvas_release(canvas);
	} else
#endif
	{
		obs_render_main_texture();
	}

#if PLASMASTREAM_HAS_CANVAS
	if (preview->editable_ && !preview->selected_.empty()) {
		/* Thin enough to be a line at any preview size: the outline is drawn
		 * in canvas units, so at a quarter scale a one-pixel border would be
		 * a quarter of a pixel and disappear. */
		const float thickness = qMax(1.0f, static_cast<float>(sourceW) / 220.0f);

		for (const VerticalItem &entry : vertical_items()) {
			if (entry.name != preview->selected_ || !entry.item) {
				continue;
			}

			matrix4 box;
			obs_sceneitem_get_box_transform(entry.item, &box);

			vec3 tl;
			vec3 br;
			vec3_set(&tl, 0.0f, 0.0f, 0.0f);
			vec3_set(&br, 1.0f, 1.0f, 0.0f);
			vec3_transform(&tl, &tl, &box);
			vec3_transform(&br, &br, &box);

			outline(tl.x, tl.y, br.x - tl.x, br.y - tl.y, thickness);
			break;
		}
	}
#endif

	gs_viewport_pop();
	gs_projection_pop();
}

} // namespace plasmastream
