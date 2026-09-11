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

#include "preview.hpp"

#include <QWidget>

class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QTimer;

namespace plasmastream {

/* Both frames at once: what goes out wide, and what goes out vertical.
 *
 * Its own dock rather than a tab on the destinations one, because the two
 * answer different questions and somebody mid-stream wants the health of their
 * destinations without losing sight of the framing. A dock so it can be dragged
 * onto a second monitor, which is where this is actually useful.
 *
 * Deliberately read-only for now. The vertical frame is derived from whatever is
 * on programme, so there is nothing here to compose: the thing worth adding next
 * is the framing control, not a second scene list. */
class CanvasDock : public QWidget {
	Q_OBJECT

public:
	explicit CanvasDock(QWidget *parent = nullptr);

	/* Taller than it is obvious it needs to be: the vertical preview is bounded
	 * by height, so height is the only thing that makes it bigger. */
	QSize sizeHint() const override { return {640, 380}; }

protected:
	/* Laid out by hand rather than by a QHBoxLayout. See the comment on the
	 * implementation: a stretch factor cannot give two panes different heights,
	 * and different heights is the whole requirement. */
	void resizeEvent(QResizeEvent *event) override;

private slots:
	/* The vertical canvas exists only while a vertical destination is running,
	 * so the right-hand pane has to notice it appearing and going away. */
	void follow();

	/* A framing choice was made. Writes it against the scene currently on
	 * programme and moves the preview to match. */
	void reframe();

	void addSource();
	void removeSource();
	void editTransform();
	void editProperties();
	void sourceSelectionChanged();

private:
	void layOut();

	QLabel *wideCaption_ = nullptr;
	QLabel *tallCaption_ = nullptr;
	QStackedWidget *tallPane_ = nullptr;
	Preview *main_ = nullptr;
	Preview *vertical_ = nullptr;
	QLabel *verticalNote_ = nullptr;
	QComboBox *placement_ = nullptr;
	QLabel *framingFor_ = nullptr;
	QLabel *overlayCaption_ = nullptr;
	QListWidget *sources_ = nullptr;
	QPushButton *addSource_ = nullptr;
	QPushButton *removeSource_ = nullptr;
	QPushButton *transform_ = nullptr;
	QPushButton *properties_ = nullptr;

	void rebuildSourceList();
	std::string selectedSource() const;
	QTimer *poll_ = nullptr;
	bool watching_ = false;

	/* The scene the controls were last filled in for, so the poll only rewrites
	 * them when programme actually changes under it. */
	std::string framedScene_;
};

void register_canvas_dock();

} // namespace plasmastream
