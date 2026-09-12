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

/*
 * Draws the destination table to a PNG, without OBS.
 *
 * The dock is the part of this plugin nobody can review by reading, and the
 * obvious way to look at it (start OBS, find the dock, take a screenshot) fights
 * the window manager, needs a real display, and cannot be done twice the same
 * way. This links the same delegate against Qt alone and renders offscreen, so
 * "what does a dropping row look like" is a question with a repeatable answer.
 *
 * It builds only when asked for:
 *
 *   cmake --preset windows-x64 -DBUILD_DOCK_PREVIEW=ON
 *   cmake --build build_x64 --config RelWithDebInfo --target dock-preview
 *   build_x64/RelWithDebInfo/dock-preview.exe out.png
 *
 * The rows below are the states worth looking at together, including the ugly
 * ones: a long name that has to elide, a failure whose reason is a sentence, and
 * a destination nobody has finished setting up.
 */

#include "../../src/appearance.hpp"

#include <cstdio>

#include <QApplication>
#include <QHeaderView>
#include <QPixmap>
#include <QTableWidget>

using namespace plasmastream;

namespace {

struct Row {
	const char *name;
	const char *platform;
	bool vertical;
	const char *where;
	const char *state;
	RowState dot;
};

const Row ROWS[] = {
	{"Twitch", "twitch", false, "Twitch", "2500 kbps  1h 04m", RowState::Live},
	{"YouTube", "youtube", false, "YouTube", "4500 kbps  1h 04m", RowState::Live},
	{"Shorts", "youtube", true, "YouTube", "1200 kbps  1h 04m  0.8% dropped",
	 RowState::Dropping},
	{"Kick", "kick", false, "kick.com", "Connecting", RowState::Starting},
	{"A rather long destination name that will not fit", "twitch", false, "Twitch",
	 "The stream key was rejected. Copy it again from the platform.", RowState::Failed},
	{"Old Kick account", "kick", false, "fa723fc1b171.global-contribute.live-video.net",
	 "No stream key yet", RowState::Unconfigured},
};

} // namespace

int main(int argc, char **argv)
{
	QApplication app(argc, argv);

	/* Pass --dark for something close to what OBS's own themes look like. The
	 * colors in appearance.cpp choose a light or a dark variant from the window
	 * color, so rendering both is the only way to see that they do. */
	for (int i = 1; i < argc; i++) {
		if (QString::fromUtf8(argv[i]) != QLatin1String("--dark")) {
			continue;
		}

		QPalette dark;
		dark.setColor(QPalette::Window, QColor("#18181b"));
		dark.setColor(QPalette::Base, QColor("#18181b"));
		dark.setColor(QPalette::AlternateBase, QColor("#1f1f23"));
		dark.setColor(QPalette::Text, QColor("#efeff1"));
		dark.setColor(QPalette::WindowText, QColor("#efeff1"));
		dark.setColor(QPalette::ButtonText, QColor("#efeff1"));
		dark.setColor(QPalette::Highlight, QColor("#9147ff"));
		dark.setColor(QPalette::HighlightedText, QColor("#ffffff"));
		QApplication::setPalette(dark);
	}

	auto *table = new QTableWidget(static_cast<int>(std::size(ROWS)), 4);
	table->setHorizontalHeaderLabels({"On", "Destination", "Where", "State"});
	table->setItemDelegate(new RowDelegate(table));

	/* The dock's own setup, not a copy of it, so this can actually catch the
	 * columns going wrong. */
	set_up_destination_columns(table);

	QHeaderView *header = table->horizontalHeader();

	table->verticalHeader()->setVisible(false);
	table->setSelectionBehavior(QAbstractItemView::SelectRows);
	table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table->setWordWrap(false);
	table->setAlternatingRowColors(true);

	for (int row = 0; row < static_cast<int>(std::size(ROWS)); row++) {
		const Row &r = ROWS[static_cast<size_t>(row)];

		auto *on = new QTableWidgetItem();
		on->setCheckState(r.dot == RowState::Unconfigured ? Qt::Unchecked : Qt::Checked);
		table->setItem(row, 0, on);

		auto *name = new QTableWidgetItem(QString::fromUtf8(r.name));
		name->setData(RowDelegate::PlatformRole, QString::fromUtf8(r.platform));
		name->setData(RowDelegate::VerticalRole, r.vertical);
		table->setItem(row, 1, name);

		table->setItem(row, 2, new QTableWidgetItem(QString::fromUtf8(r.where)));

		auto *state = new QTableWidgetItem(QString::fromUtf8(r.state));
		state->setData(RowDelegate::StateRole, static_cast<int>(r.dot));
		table->setItem(row, 3, state);
	}

	int width = 880;

	for (int i = 1; i < argc - 1; i++) {
		if (QString::fromUtf8(argv[i]) == QLatin1String("--width")) {
			width = QString::fromUtf8(argv[i + 1]).toInt();
		}
	}

	table->resize(width, 40 + 27 * static_cast<int>(std::size(ROWS)));

	/* Laid out but never put on screen. The obvious alternative, the offscreen
	 * platform plugin, is not in the Qt that obs-deps ships (it offers direct2d,
	 * minimal and windows), and minimal draws every glyph as a box because it
	 * has no fonts. This keeps real text and still never steals focus. */
	table->setAttribute(Qt::WA_DontShowOnScreen, true);
	table->show();

	/* One turn of the loop so the layout settles before the grab; without it
	 * the columns are still at their construction widths. */
	QApplication::processEvents();

	lay_out_destination_columns(table);
	QApplication::processEvents();

	/* What each column actually got, against what it asked for. A column
	 * narrower than its hint is a column that is eliding, which is the whole
	 * question when somebody says the dock truncates. */
	const char *names[] = {"On", "Destination", "Where", "State"};
	std::printf("table %d wide, viewport %d\n", table->width(), table->viewport()->width());

	for (int column = 0; column < 4; column++) {
		const int got = table->columnWidth(column);
		const int wants = header->sectionSizeHint(column);

		std::printf("  %-12s got %4d  wants %4d  %s\n", names[column], got, wants,
			    got < wants ? "ELIDING" : "ok");
	}

	const QString out = argc > 1 ? QString::fromUtf8(argv[1]) : QStringLiteral("dock.png");
	table->grab().save(out);

	return 0;
}
