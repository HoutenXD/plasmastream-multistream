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

#include <QPointer>
#include <QWidget>

#include <string>

class QLabel;
class QPushButton;
class QTimer;
class QCefWidget;
struct QCef;
struct QCefCookieManager;

namespace plasmastream {

/* PlasmaStream YouTube Chat: the backup that keeps YouTube chat reaching the
 * bot when Google's API cannot read it (the day's quota spent, a sign-in gone).
 *
 * It is a browser, OBS's own, open on the streamer's YouTube live chat. A script
 * the dashboard serves runs inside the page and posts the chat it is sent to
 * PlasmaStream, at no quota, from the streamer's own connection. Everything about
 * what that script does is in the dashboard's server/youtube-reader-script.ts and
 * db/src/youtube-page.ts; this dock only hosts it:
 *
 * - every minute it asks PlasmaStream whether to run and for the script,
 * - it opens the page with the script set to run after every load,
 * - it reloads when the script changes, so a fix reaches it without a release.
 *
 * ## The browser is not in the dock
 *
 * obs-browser creates a widget's browser the first time the widget is SHOWN, and
 * not before. OBS starts docks hidden, or restores them hidden, or tabbed behind
 * another, so a browser inside the dock would often never start at all. Once it
 * has started, hidden is fine: on the rig a reader hidden for over six minutes
 * kept posting at about a second behind YouTube, the same as shown.
 *
 * So the browser is a one-pixel child of OBS's main window, which is shown at
 * startup, hidden again once it has started. It runs while OBS is open, whatever
 * the docks are doing, and this dock only says how it is going. Signing in to
 * YouTube, which only a members-only chat needs, opens a window of its own that
 * shares the reader's cookies. */
class YouTubeReaderDock : public QWidget {
	Q_OBJECT

public:
	explicit YouTubeReaderDock(QWidget *parent = nullptr);
	~YouTubeReaderDock() override;

	QSize sizeHint() const override { return {360, 260}; }

private slots:
	void refresh();
	void urlChanged(const QString &url);

private:
	void apply(const HttpResponse &response);
	void setStatus(const QString &text, bool warn);
	void openBrowser(const std::string &url, const std::string &script);
	void closeBrowser();
	void signIn();

	QLabel *status_ = nullptr;
	QLabel *detail_ = nullptr;
	QPushButton *signIn_ = nullptr;
	QTimer *poll_ = nullptr;

	QCef *cef_ = nullptr;
	QCefCookieManager *cookies_ = nullptr;
	/* Guarded: its parent is the main window, which can delete it at exit
	 * before this dock goes. */
	QPointer<QCefWidget> browser_;

	std::string hash_;
	std::string startUrl_;
	bool fetching_ = false;
};

void register_youtube_reader_dock();

} // namespace plasmastream
