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

/* A dock rather than a settings dialog: which destinations are connected, and
 * which just dropped, is worth having on screen beside OBS's own stream status.
 * Nobody opens a dialog mid-stream. */
class MultistreamDock : public QWidget {
	Q_OBJECT

public:
	explicit MultistreamDock(QWidget *parent = nullptr);

	/* QDockWidget takes its opening size from this, and a QTableWidget asks for
	 * very little: without it the dock opens too narrow to read its own headers. */
	QSize sizeHint() const override;

private slots:
	void addDestination();
	void editSelected();
	void removeSelected();
	void fetchFromPlasmaStream();
	void changeToken();
	void showOptions();
	void refreshStatuses();

private:
	void rebuildTable();
	void updateEmptyState();
	void updateButtons();
	void setNotice(const QString &text, bool warning);
	void applyFetch(const HttpResponse &response);

	QTableWidget *table_ = nullptr;
	QLabel *empty_ = nullptr;
	QPushButton *add_ = nullptr;
	QPushButton *edit_ = nullptr;
	QPushButton *remove_ = nullptr;
	QPushButton *fetch_ = nullptr;
	QPushButton *keyButton_ = nullptr;
	QPushButton *options_ = nullptr;
	QLabel *notice_ = nullptr;
	QTimer *poll_ = nullptr;
};

void register_dock();

} // namespace plasmastream
