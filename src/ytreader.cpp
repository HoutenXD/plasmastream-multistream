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

#include "ytreader.hpp"

#include "config.hpp"
#include "obs-browser-panel.hpp"

#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QApplication>
#include <QDialog>
#include <QDockWidget>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <thread>

namespace plasmastream {

namespace {

/* Asked again every minute: whether to run, and the script, whose hash says
 * when the page needs reloading with a new one. */
constexpr int REFRESH_MS = 60'000;

/* Where the reader keeps its YouTube cookies, apart from OBS's own docks, so a
 * sign-in for it (for a members-only chat) is its own and nobody else's. */
constexpr const char *COOKIE_STORE = "plasmastream_youtube_reader";

/* How long the reader's browser stays shown, at one pixel, before it is hidden.
 * It is created when shown (see ytreader.hpp) and only needs to have started. */
constexpr int SHOWN_MS = 5'000;

/* Google's sign-in, coming back to YouTube. Google may refuse to sign in inside
 * an embedded browser; the button says what it is for, and a public chat needs
 * none of it. */
constexpr const char *SIGN_IN_URL =
	"https://accounts.google.com/ServiceLogin?service=youtube&continue=https%3A%2F%2Fwww.youtube.com%2Frobots.txt";

} // namespace

YouTubeReaderDock::YouTubeReaderDock(QWidget *parent) : QWidget(parent)
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(8, 8, 8, 8);
	layout->setSpacing(6);

	status_ = new QLabel(this);
	status_->setWordWrap(true);
	layout->addWidget(status_);

	detail_ = new QLabel(tr("PlasmaStream reads your YouTube chat through Google's API. When Google's daily "
				"limit runs out, this plugin carries on reading it from your live chat page "
				"instead, in the background, for as long as OBS is open. This dock only shows "
				"how that is going, so it can be closed."),
			     this);
	detail_->setWordWrap(true);
	detail_->setStyleSheet("color: palette(mid);");
	layout->addWidget(detail_);

	signIn_ = new QPushButton(tr("Sign in to YouTube (members-only chat only)"), this);
	signIn_->setEnabled(false);
	connect(signIn_, &QPushButton::clicked, this, &YouTubeReaderDock::signIn);
	layout->addWidget(signIn_);
	layout->addStretch(1);

	cef_ = obs_browser_init_panel();

	if (!cef_) {
		setStatus(tr("OBS's built-in browser is not available here, so this dock cannot read YouTube chat."),
			  true);
		return;
	}

	cef_->init_browser();

	setStatus(tr("Starting..."), false);

	poll_ = new QTimer(this);
	poll_->setInterval(REFRESH_MS);
	connect(poll_, &QTimer::timeout, this, &YouTubeReaderDock::refresh);
	poll_->start();

	// Soon after OBS has loaded, not inside module load.
	QTimer::singleShot(3000, this, &YouTubeReaderDock::refresh);
}

YouTubeReaderDock::~YouTubeReaderDock()
{
	closeBrowser();
	delete cookies_;
	delete cef_;
}

void YouTubeReaderDock::setStatus(const QString &text, bool warn)
{
	status_->setText(warn ? QStringLiteral("<b>%1</b>").arg(text.toHtmlEscaped()) : text.toHtmlEscaped());
}

void YouTubeReaderDock::refresh()
{
	if (fetching_) {
		return;
	}

	if (config().token.empty()) {
		closeBrowser();
		setStatus(tr("Link this plugin to PlasmaStream first: press Sync in the PlasmaStream Multistream dock "
			     "and paste the link from your dashboard's Multistream page."),
			  true);
		return;
	}

	fetching_ = true;

	const std::string url = plasmastream_url("/api/plugin/" + config().token + "/youtube-reader");
	QPointer<YouTubeReaderDock> alive(this);

	std::thread([alive, url]() {
		const HttpResponse response = http_get(url);

		// Widgets belong to the Qt thread.
		QMetaObject::invokeMethod(
			qApp,
			[alive, response]() {
				if (!alive) {
					return;
				}

				alive->fetching_ = false;
				alive->apply(response);
			},
			Qt::QueuedConnection);
	}).detach();
}

void YouTubeReaderDock::apply(const HttpResponse &response)
{
	if (response.status == 404) {
		closeBrowser();
		setStatus(tr("PlasmaStream did not recognize this plugin's link. Press Sync in the PlasmaStream "
			     "Multistream dock and paste the link from your dashboard again."),
			  true);
		return;
	}

	if (!response.ok()) {
		// Whatever the page is doing, it keeps doing: one failed check is not a
		// reason to stop reading chat.
		if (!browser_) {
			setStatus(tr("Could not reach PlasmaStream. Trying again in a minute."), true);
		}
		return;
	}

	const QJsonObject body = QJsonDocument::fromJson(QByteArray::fromStdString(response.body)).object();

	if (!body.value("run").toBool()) {
		closeBrowser();
		const QString reason = body.value("reason").toString();
		setStatus(reason.isEmpty() ? tr("YouTube chat reading is off for this channel.") : reason, true);
		return;
	}

	const std::string startUrl = body.value("startUrl").toString().toStdString();
	const std::string script = body.value("script").toString().toStdString();
	const std::string hash = body.value("hash").toString().toStdString();

	if (startUrl.empty() || script.empty()) {
		return;
	}

	if (browser_ && hash == hash_) {
		return;
	}

	// A new script, or none running yet: start the page again with it.
	hash_ = hash;
	startUrl_ = startUrl;
	openBrowser(startUrl, script);
}

void YouTubeReaderDock::openBrowser(const std::string &url, const std::string &script)
{
	if (!cef_) {
		return;
	}

	if (browser_) {
		browser_->setStartupScript(script);
		browser_->setURL(url);
		return;
	}

	// OBS starts its browser at launch; this only waits if it has not finished.
	cef_->wait_for_browser_init();

	if (!cookies_) {
		cookies_ = cef_->create_cookie_manager(COOKIE_STORE, true);
	}

	auto *host = static_cast<QWidget *>(obs_frontend_get_main_window());

	browser_ = cef_->create_widget(host ? host : this, url, cookies_);

	if (!browser_) {
		setStatus(tr("OBS's browser would not open a page for the reader."), true);
		return;
	}

	/* Set before it is shown: the browser is created on show, and runs the
	 * script after every page load from then on. */
	browser_->setStartupScript(script);
	browser_->setGeometry(0, 0, 1, 1);
	browser_->show();

	QPointer<QCefWidget> started(browser_);
	QTimer::singleShot(SHOWN_MS, this, [started]() {
		if (started) {
			started->hide();
		}
	});

	/* By name, through the widget's own meta-object: QCefWidget lives in
	 * obs-browser, and its header is not run through moc in this plugin. */
	connect(browser_, SIGNAL(urlChanged(const QString &)), this, SLOT(urlChanged(const QString &)));

	signIn_->setEnabled(true);
	setStatus(tr("Waiting for you to go live on YouTube."), false);
}

void YouTubeReaderDock::closeBrowser()
{
	if (!browser_) {
		return;
	}

	browser_->closeBrowser();
	browser_->deleteLater();
	browser_ = nullptr;
	hash_.clear();
	signIn_->setEnabled(false);
}

void YouTubeReaderDock::signIn()
{
	if (!cef_ || !cookies_) {
		return;
	}

	/* Its own window and its own browser, with no script: only the cookies are
	 * shared, and the reader reloads with them once this closes. */
	auto *dialog = new QDialog(this);
	dialog->setWindowTitle(tr("Sign in to YouTube"));
	dialog->resize(520, 680);
	dialog->setAttribute(Qt::WA_DeleteOnClose);

	auto *layout = new QVBoxLayout(dialog);
	layout->setContentsMargins(0, 0, 0, 0);

	QCefWidget *page = cef_->create_widget(dialog, SIGN_IN_URL, cookies_);

	if (page) {
		layout->addWidget(page);
	}

	connect(dialog, &QDialog::finished, this, [this, page]() {
		if (page) {
			page->closeBrowser();
		}

		if (browser_ && !startUrl_.empty()) {
			browser_->setURL(startUrl_);
		}
	});

	dialog->show();
}

void YouTubeReaderDock::urlChanged(const QString &url)
{
	if (url.contains("youtube.com/live_chat")) {
		setStatus(tr("Reading your YouTube live chat. Keep OBS open while you stream."), false);
	} else {
		setStatus(tr("Waiting for you to go live on YouTube."), false);
	}
}

void register_youtube_reader_dock()
{
	auto *main_window = static_cast<QWidget *>(obs_frontend_get_main_window());

	if (!main_window) {
		return;
	}

	auto *dock = new YouTubeReaderDock(main_window);
	dock->setWindowTitle(QObject::tr("PlasmaStream YouTube Chat"));

	// _by_id, not the deprecated obs_frontend_add_dock: OBS remembers placement.
	obs_frontend_add_dock_by_id("plasmastream_youtube_chat", "PlasmaStream YouTube Chat", dock);

	/* Shown once, the first time this version runs, for the same reason as the
	 * Canvases dock: OBS starts every dock hidden, and this one does nothing at
	 * all until it is open. Deferred past OBS restoring its saved layout, which
	 * would otherwise undo it. */
	if (config().youtube_reader_introduced) {
		return;
	}

	QTimer::singleShot(3000, dock, [dock]() {
		for (QWidget *w = dock->parentWidget(); w; w = w->parentWidget()) {
			auto *docked = qobject_cast<QDockWidget *>(w);

			if (!docked) {
				continue;
			}

			docked->setVisible(true);
			config().youtube_reader_introduced = true;
			save_config();
			break;
		}
	});
}

} // namespace plasmastream
