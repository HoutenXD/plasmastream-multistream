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

#include <QColor>
#include <QIcon>
#include <QStyledItemDelegate>

class QTableWidget;

namespace plasmastream {

/* Which colors come from us and which come from OBS.
 *
 * The accent and the three state colors are PlasmaStream's, taken from the
 * site's globals.css so the dock and the dashboard agree; somebody who uses both
 * should not have to learn two meanings for green. Everything else (the row
 * background, the text, the grid) is left to whatever OBS theme is loaded, which
 * is why this has no background or text color in it at all.
 *
 * That split is the whole trick. A plugin that paints its own chrome looks
 * pasted into OBS on the dark themes and unreadable on the light one; a plugin
 * that paints nothing looks like a spreadsheet. Brand the meaning, inherit the
 * furniture. */
namespace color {

QColor accent();
/* Live, and everything is fine. */
QColor ok();
/* Connecting, or dropping enough frames to be worth looking at. */
QColor warn();
/* Stopped, and not on purpose. */
QColor danger();
/* Text that should recede: idle states, units, separators. */
QColor muted();

} // namespace color

/* The Twitch and YouTube marks, drawn from the same path data the website uses.
 * Anything else gets a null icon rather than a generic one, because a made-up
 * mark next to a real one reads as a platform nobody recognizes. */
QIcon platform_icon(const QString &platform);

/* What a row is doing, which is what the dot means. Kept separate from
 * OutputState because a row has states an output does not: a destination with no
 * stream key is a real thing to show and never becomes an output at all. */
enum class RowState {
	Unconfigured,
	Ready,
	Starting,
	Live,
	Dropping,
	Failed,
};

/* Paints the first two columns: the state dot, and the platform mark with the
 * destination's name and its 9:16 chip.
 *
 * A delegate rather than icons and rich text in the items themselves, because
 * the dot has to sit on the text baseline at whatever row height the user's OBS
 * theme decides, and a chip has to be drawn rather than typed. */
class RowDelegate : public QStyledItemDelegate {
	Q_OBJECT

public:
	using QStyledItemDelegate::QStyledItemDelegate;

	/* Roles the table fills in, read back here at paint time. */
	enum Role {
		StateRole = Qt::UserRole + 1,
		PlatformRole,
		VerticalRole,
	};

	void paint(QPainter *painter, const QStyleOptionViewItem &option,
		   const QModelIndex &index) const override;

	QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

/* Set up the destination table's four columns, and share its width between
 * them whenever that width changes.
 *
 * Not ResizeToContents, which is what this used to be. That sizes a column to
 * the widest thing that has ever been in it, and the State column holds a whole
 * sentence when a destination fails ("The stream key was rejected. Copy it
 * again from the platform."). On a dock dragged narrow, Where and State were
 * pushed off the right edge completely, where no amount of resizing brought
 * them back.
 *
 * So the two right-hand columns take a share of whatever room there is, within
 * limits, and Destination takes the rest. Long text elides, which is fine: the
 * whole of it is in the tooltip, and half a reason on screen beats none.
 *
 * Lives here rather than in the dock so the offscreen preview under tools/ can
 * exercise the real thing instead of a copy that might drift from it. */
void set_up_destination_columns(QTableWidget *table);
void lay_out_destination_columns(QTableWidget *table);

} // namespace plasmastream
