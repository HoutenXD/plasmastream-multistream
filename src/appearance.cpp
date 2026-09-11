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

#include "appearance.hpp"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QSvgRenderer>

namespace plasmastream {

namespace {

/* Whether the OBS theme in use is a light one, decided from the window color
 * rather than from a theme name, since users install themes we have never heard
 * of. The site carries two values for each state for the same reason, and these
 * are those values. */
bool light_theme()
{
	return QApplication::palette().color(QPalette::Window).lightness() > 127;
}

QColor pick(const char *dark, const char *lightish)
{
	return QColor(light_theme() ? lightish : dark);
}

/* The marks, straight from the site's PlatformIcon component. Wrapped in the
 * smallest SVG document that will render: a viewBox and one path. */
const char *TWITCH_PATH = "M11.571 4.714h1.715v5.143H11.57zm4.715 0H18v5.143h-1.714zM6 0L1.714 "
			  "4.286v15.428h5.143V24l4.286-4.286h3.428L22.286 12V0zm14.571 "
			  "11.143l-3.428 3.428h-3.429l-3 3v-3H6.857V1.714h13.714z";

const char *YOUTUBE_PATH =
	"M23.498 6.186a3.016 3.016 0 0 0-2.122-2.136C19.505 3.545 12 3.545 12 3.545s-7.505 "
	"0-9.377.505A3.017 3.017 0 0 0 .502 6.186C0 8.07 0 12 0 12s0 3.93.502 5.814a3.016 3.016 0 "
	"0 0 2.122 2.136c1.871.505 9.376.505 9.376.505s7.505 0 9.377-.505a3.015 3.015 0 0 0 "
	"2.122-2.136C24 15.93 24 12 24 12s0-3.93-.502-5.814zM9.545 15.568V8.432L15.818 12l-6.273 "
	"3.568z";

/* Each platform's own color, not the theme's. A mark in the wrong color is worse
 * than no mark, because purple and red are what somebody recognizes before they
 * have read anything. */
QIcon render_mark(const char *path, const char *fill)
{
	const QString document =
		QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'>"
			       "<path fill='%1' d='%2'/></svg>")
			.arg(QString::fromUtf8(fill), QString::fromUtf8(path));

	QSvgRenderer renderer(document.toUtf8());

	if (!renderer.isValid()) {
		return {};
	}

	/* Rendered at 2x the size it is drawn at, so it stays clean on a display
	 * that scales. */
	QPixmap pixmap(32, 32);
	pixmap.fill(Qt::transparent);

	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing, true);
	renderer.render(&painter);
	painter.end();

	return QIcon(pixmap);
}

QColor state_color(RowState state)
{
	switch (state) {
	case RowState::Live:
		return color::ok();
	case RowState::Starting:
	case RowState::Dropping:
		return color::warn();
	case RowState::Failed:
		return color::danger();
	case RowState::Unconfigured:
	case RowState::Ready:
		break;
	}

	return color::muted();
}

} // namespace

namespace color {

/* --accent, both themes. */
QColor accent()
{
	return pick("#9147ff", "#772ce8");
}

QColor ok()
{
	return pick("#00c07f", "#048a5f");
}

/* --premium, which is the site's amber. */
QColor warn()
{
	return pick("#ffcf3f", "#b57e11");
}

QColor danger()
{
	return pick("#f43f5e", "#e11d48");
}

/* The one neutral taken from us rather than OBS, because the theme's disabled
 * text is often too faint to read at a glance from across a desk. */
QColor muted()
{
	return pick("#adadb8", "#57575f");
}

} // namespace color

QIcon platform_icon(const QString &platform)
{
	/* Built once. Qt's SVG renderer is not free, and this is asked for on every
	 * repaint of every row. */
	static const QIcon twitch = render_mark(TWITCH_PATH, "#9147ff");
	static const QIcon youtube = render_mark(YOUTUBE_PATH, "#ff0000");

	if (platform == QLatin1String("twitch")) {
		return twitch;
	}

	if (platform == QLatin1String("youtube")) {
		return youtube;
	}

	return {};
}

void RowDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
			const QModelIndex &index) const
{
	QStyleOptionViewItem opt = option;
	initStyleOption(&opt, index);

	const auto state = static_cast<RowState>(index.data(StateRole).toInt());
	const QString platform = index.data(PlatformRole).toString();
	const bool vertical = index.data(VerticalRole).toBool();

	/* Let the style draw the row itself: selection, hover, alternating
	 * background and focus rectangle all belong to the user's theme. Only the
	 * text is held back, since this paints its own. */
	const QString text = opt.text;
	opt.text.clear();

	QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
	style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

	painter->save();
	painter->setRenderHint(QPainter::Antialiasing, true);

	QRect content = opt.rect.adjusted(6, 0, -6, 0);

	/* Driven by which roles the cell carries rather than by column number, so
	 * moving a column around does not silently move the dot with it. */
	if (index.data(StateRole).isValid()) {
		/* Small on purpose: it is meant to be read as a color at a glance,
		 * not looked at. */
		const int d = 8;
		const QRect dot(content.left(), content.center().y() - d / 2, d, d);

		painter->setPen(Qt::NoPen);
		painter->setBrush(state_color(state));
		painter->drawEllipse(dot);

		content.setLeft(dot.right() + 8);
	}

	const QIcon mark = platform_icon(platform);

	if (!mark.isNull()) {
		const int s = 14;
		const QRect box(content.left(), content.center().y() - s / 2, s, s);
		mark.paint(painter, box);
		content.setLeft(box.right() + 8);
	}

	QRect chip;

	if (vertical) {
		const QString label = QStringLiteral("9:16");
		const QFontMetrics metrics(opt.font);
		const int w = metrics.horizontalAdvance(label) + 12;
		const int h = metrics.height() + 2;

		chip = QRect(content.right() - w, content.center().y() - h / 2, w, h);

		QColor fill = color::accent();
		fill.setAlpha(46);

		painter->setPen(QPen(color::accent(), 1));
		painter->setBrush(fill);
		painter->drawRoundedRect(QRectF(chip).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);

		painter->setPen(color::accent());
		painter->drawText(chip, Qt::AlignCenter, label);

		content.setRight(chip.left() - 8);
	}

	painter->setPen(opt.state & QStyle::State_Selected
				? opt.palette.color(QPalette::HighlightedText)
				: opt.palette.color(QPalette::Text));

	painter->drawText(content, Qt::AlignVCenter | Qt::AlignLeft,
			  opt.fontMetrics.elidedText(text, Qt::ElideRight, content.width()));

	painter->restore();
}

QSize RowDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
	QSize size = QStyledItemDelegate::sizeHint(option, index);

	/* Tall enough that the dot and the chip are not touching the grid line
	 * above them, whatever row height the theme asked for. */
	size.setHeight(qMax(size.height(), 26));

	/* The parts the delegate draws to the left of the text are not in the
	 * string, so the width Qt measured is short by exactly them. */
	if (index.data(StateRole).isValid()) {
		size.setWidth(size.width() + 16);
	}

	if (!platform_icon(index.data(PlatformRole).toString()).isNull()) {
		size.setWidth(size.width() + 22);
	}

	if (index.data(VerticalRole).toBool()) {
		size.setWidth(size.width() + 48);
	}

	return size;
}

} // namespace plasmastream
