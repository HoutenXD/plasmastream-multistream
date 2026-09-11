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

#include <QPointF>
#include <QSizeF>
#include <QRectF>
#include <QWidget>

#include <obs.h>

namespace plasmastream {

/* A live video preview, drawn by OBS into a widget of ours.
 *
 * OBS renders into a native window handle rather than into Qt's paint device,
 * which is why this asks for WA_NativeWindow and why it has no paintEvent: Qt
 * owns the rectangle, the graphics thread owns the pixels inside it. Qt must
 * therefore be told not to paint the background over the top, or the widget
 * flickers between OBS's frame and Qt's grey on every repaint.
 *
 * The display cannot be made in the constructor. winId() has to have been called
 * for the handle to exist, and the graphics subsystem has to be up, so it is
 * created on the first show and destroyed on hide. */
class Preview : public QWidget {
	Q_OBJECT

public:
	/* What to draw. Main is OBS's own program output; a canvas is one of ours,
	 * which is how the vertical frame gets a preview without a second
	 * composition existing anywhere. */
	enum class Source {
		Main,
		Canvas,
	};

	explicit Preview(QWidget *parent = nullptr);
	~Preview() override;

#if PLASMASTREAM_HAS_CANVAS
	/* Null goes back to the main canvas. */
	void watch(obs_canvas_t *canvas);
#endif

	QSize sizeHint() const override { return {320, 180}; }

#if PLASMASTREAM_HAS_CANVAS
	/* Turn on click-to-select and drag-to-place for whatever is on the canvas.
	 * Only meaningful on the vertical preview; the programme one shows OBS's own
	 * composition, which OBS's own preview is for. */
	void setEditable(bool editable) { editable_ = editable; }
#endif

signals:
	/* A drag finished and the config now has the new geometry in it. The dock
	 * saves rather than this, because this has no business knowing there is a
	 * file. */
	void itemMoved();

protected:
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
#if PLASMASTREAM_HAS_CANVAS
	/* Declared with the same guard their definitions carry. Without it a build
	 * against an OBS with no canvas declares three overrides nobody defines,
	 * which compiles all the way to a link error. */
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
#endif

	/* Qt would otherwise erase to the palette between OBS's frames. */
	QPaintEngine *paintEngine() const override { return nullptr; }

private:
	static void render(void *data, uint32_t cx, uint32_t cy);

	void create();
	void destroy();

	obs_display_t *display_ = nullptr;

#if PLASMASTREAM_HAS_CANVAS
	/* Weak, so a canvas released while this is on screen cannot leave a
	 * dangling pointer for the graphics thread to walk into. */
	obs_weak_canvas_t *canvas_ = nullptr;
#endif
	Source source_ = Source::Main;

#if PLASMASTREAM_HAS_CANVAS
	/* Which corner or edge is being pulled, or the whole thing, or nothing.
	 * Ordered so the corners are the four values with both bits set. */
	enum Grab {
		None,
		Move,
		Left = 1 << 2,
		Right = 1 << 3,
		Top = 1 << 4,
		Bottom = 1 << 5,
	};

	QPointF toCanvas(const QPointF &widget) const;
	QRectF itemRect(obs_sceneitem_t *item) const;
	int grabAt(const QRectF &rect, const QPointF &canvas) const;

	bool editable_ = false;

	/* The rectangle the canvas is drawn into, in widget pixels. Written by the
	 * graphics thread and read by Qt's; both only ever see a whole QRectF, and
	 * a frame of staleness after a resize costs nothing. */
	QRectF viewport_;

	/* The canvas size the viewport was fitted to, so the mouse can scale between
	 * the two without asking libobs again on Qt's thread. */
	QSizeF canvasSize_;

	std::string selected_;
	int grab_ = None;
	QPointF grabFrom_;
	QRectF grabRect_;
#endif
};

} // namespace plasmastream
