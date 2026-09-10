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

#include "http.hpp"

#include <QWidget>

class QLabel;
class QPushButton;
class QTableWidget;
class QTimer;

namespace plasmastream {

/**
 * The dock: the whole interface for the plugin.
 *
 * A dock rather than a settings dialog because the interesting information is
 * live. Which destinations are connected, and which one just dropped, is worth
 * having on screen next to the stream status OBS already shows, and a dialog you
 * have to open is a dialog nobody opens while streaming.
 */
class MultistreamDock : public QWidget {
	Q_OBJECT

public:
	explicit MultistreamDock(QWidget *parent = nullptr);

	/**
	 * How large this wants to be before anybody drags it.
	 *
	 * A QDockWidget sizes itself from its child's hint, and a QTableWidget asks
	 * for very little, so the dock opened at a width that truncated its own
	 * column headers. This asks for enough to read a destination name and its
	 * status side by side.
	 */
	QSize sizeHint() const override;

private slots:
	void addDestination();
	void editSelected();
	void removeSelected();
	void fetchFromPlasmaStream();
	void refreshStatuses();

private:
	void rebuildTable();
	/** Show the table or the empty-state hint, never both. */
	void updateEmptyState();
	/** Enable or disable the buttons that need a selected row. */
	void updateButtons();
	void setNotice(const QString &text, bool warning);
	/** Handle a finished sync, back on the Qt thread. */
	void applyFetch(const HttpResponse &response);

	QTableWidget *table_ = nullptr;
	QLabel *empty_ = nullptr;
	QPushButton *add_ = nullptr;
	QPushButton *edit_ = nullptr;
	QPushButton *remove_ = nullptr;
	QPushButton *fetch_ = nullptr;
	QLabel *notice_ = nullptr;
	QTimer *poll_ = nullptr;
};

/** Build the dock and hand it to OBS. Call once, during module load. */
void register_dock();

} // namespace plasmastream
