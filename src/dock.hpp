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
class QResizeEvent;
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

protected:
	/* Re-shares the table's width between its columns. Dragging the dock
	 * narrow used to push the two right-hand columns off the edge entirely. */
	void resizeEvent(QResizeEvent *event) override;

private slots:
	void addDestination();
	void editSelected();
	void removeSelected();
	void fetchFromPlasmaStream();
	void changeToken();
	void showOptions();
	void refreshStatuses();

	/* Record one scene while a different one goes out. Its own settings dialog
	 * rather than a page of the options one, because it is a feature somebody
	 * either uses or has never heard of, and burying it under Options is how
	 * the second group stays in it. */
	void setUpRecording();
	void toggleRecording();

	/* Voice commands: pick the microphone and how to listen. The phrases
	 * themselves are set on the website, so this stays short. */
	void setUpVoice();

	/* The three private warnings, one switch each. Its own dialog rather than a
	 * row of Options, because the thresholds need a sentence apiece and nobody
	 * switches on a warning they have not been told the rules for. */
	void setUpWarnings();

	/* The one button in the update banner. What it does depends on where the
	 * update has got to: fetch it, finish it, or open the page for the
	 * platforms that install through a package. */
	void updatePressed();

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

	void refreshRecording();

	QLabel *recordingState_ = nullptr;
	QPushButton *recordingSetUp_ = nullptr;
	QPushButton *recordingToggle_ = nullptr;

	void refreshVoice();
	void refreshUpdate();

	/* Hidden unless there is something to say, and the first thing in the dock
	 * when there is: an out of date plugin is worth more of the eye than a
	 * destination list that is working. */
	QWidget *updateRow_ = nullptr;
	QLabel *updateText_ = nullptr;
	QLabel *updateNotes_ = nullptr;
	QPushButton *updateButton_ = nullptr;


	QLabel *voiceState_ = nullptr;
	/* What it last heard and what became of it, under the row, so a long line
	 * does not push the button around. */
	QLabel *voiceDetail_ = nullptr;
	QPushButton *voiceSetUp_ = nullptr;

	void refreshWarnings();

	QLabel *warningsState_ = nullptr;
	/* The second and third warning, when more than one is true at once. Same
	 * shape as the voice row for the same reason. */
	QLabel *warningsDetail_ = nullptr;
	QPushButton *warningsSetUp_ = nullptr;

	QTimer *poll_ = nullptr;
};

void register_dock();

} // namespace plasmastream
