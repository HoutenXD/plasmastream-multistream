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

#include "dock.hpp"

#include "config.hpp"
#include "http.hpp"
#include "outputs.hpp"

#include <thread>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QApplication>
#include <QJsonParseError>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

namespace plasmastream {

namespace {

/* Starting addresses only. Platforms run regional ingests and hand some
 * streamers a different one, so the field stays editable.
 *
 * Mirrors DESTINATION_PLATFORMS on the website; duplicated so the plugin can
 * name a platform without talking to a server it is meant to work without. */
struct PlatformOption {
	const char *key;
	const char *label;
	const char *url;
};

const PlatformOption PLATFORMS[] = {
	{"twitch", "Twitch", "rtmp://live.twitch.tv/app"},
	{"youtube", "YouTube", "rtmp://a.rtmp.youtube.com/live2"},
	{"kick", "Kick", "rtmps://fa723fc1b171.global-contribute.live-video.net"},
	{"tiktok", "TikTok", ""},
	{"trovo", "Trovo", "rtmp://livepush.trovo.live/live"},
	{"rumble", "Rumble", ""},
	{"facebook", "Facebook", "rtmps://live-api-s.facebook.com:443/rtmp"},
	{"custom", "Something else", ""},
};

QString platform_label(const std::string &key)
{
	for (const PlatformOption &option : PLATFORMS) {
		if (key == option.key) {
			return QString::fromUtf8(option.label);
		}
	}

	return QObject::tr("Something else");
}

/* Takes either a bare token or the whole URL the dashboard's copy button hands
 * over, since pasting that used to send the prefix twice and 404. */
std::string read_token(const QString &pasted)
{
	QString token = pasted.trimmed();

	const QString marker = QStringLiteral("/api/plugin/");
	const int at = token.lastIndexOf(marker);

	if (at >= 0) {
		token = token.mid(at + marker.length());
	}

	// A copied link can carry a query string or trailing slash.
	token = token.section('?', 0, 0).section('#', 0, 0);

	while (token.endsWith('/')) {
		token.chop(1);
	}

	return token.trimmed().toStdString();
}

/** A short, stable id for a destination somebody typed in by hand. */
std::string make_local_id()
{
	static int counter = 0;
	return "local-" + std::to_string(++counter) + "-" +
	       std::to_string(static_cast<long long>(os_gettime_ns() / 1000000));
}

/* The stream key is a password field on purpose: people configure OBS while
 * screen sharing. */
bool edit_destination(QWidget *parent, Destination &destination, bool creating)
{
	QDialog dialog(parent);
	dialog.setWindowTitle(creating ? QObject::tr("Add a destination")
				       : QObject::tr("Edit destination"));
	dialog.setMinimumWidth(420);

	auto *platform = new QComboBox(&dialog);
	for (const PlatformOption &option : PLATFORMS) {
		platform->addItem(QString::fromUtf8(option.label), QString::fromUtf8(option.key));
	}

	auto *name = new QLineEdit(QString::fromStdString(destination.name), &dialog);
	name->setPlaceholderText(QObject::tr("Main YouTube"));

	auto *url = new QLineEdit(QString::fromStdString(destination.url), &dialog);
	url->setPlaceholderText(QStringLiteral("rtmp://..."));

	auto *key = new QLineEdit(QString::fromStdString(destination.key), &dialog);
	key->setEchoMode(QLineEdit::Password);
	key->setPlaceholderText(QObject::tr("Stays on this computer"));

	const int index = platform->findData(QString::fromStdString(destination.platform));
	if (index >= 0) {
		platform->setCurrentIndex(index);
	}

	// Prefill only while the box still holds the last prefill; once edited it is
	// theirs.
	QObject::connect(platform, &QComboBox::currentIndexChanged, [platform, url]() {
		const QString chosen = platform->currentData().toString();

		for (const PlatformOption &option : PLATFORMS) {
			if (chosen != QString::fromUtf8(option.key)) {
				continue;
			}

			const bool untouched =
				url->text().isEmpty() || url->property("prefilled").toBool();

			if (untouched && *option.url) {
				url->setText(QString::fromUtf8(option.url));
				url->setProperty("prefilled", true);
			}

			break;
		}
	});

	QObject::connect(url, &QLineEdit::textEdited,
			 [url]() { url->setProperty("prefilled", false); });

	auto *form = new QFormLayout;
	form->addRow(QObject::tr("Platform"), platform);
	form->addRow(QObject::tr("Name"), name);
	form->addRow(QObject::tr("Server URL"), url);
	form->addRow(QObject::tr("Stream key"), key);

	auto *hint = new QLabel(
		QObject::tr("Your stream key is saved on this computer only. It is never sent to "
			    "PlasmaStream, and there is nowhere on the website to put one."),
		&dialog);
	hint->setWordWrap(true);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
					     &dialog);
	QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

	auto *layout = new QVBoxLayout(&dialog);
	layout->addLayout(form);
	layout->addWidget(hint);
	layout->addWidget(buttons);

	if (dialog.exec() != QDialog::Accepted) {
		return false;
	}

	destination.platform = platform->currentData().toString().toStdString();
	destination.name = name->text().trimmed().toStdString();
	destination.url = url->text().trimmed().toStdString();
	destination.key = key->text().trimmed().toStdString();

	if (destination.name.empty()) {
		destination.name = platform_label(destination.platform).toStdString();
	}

	if (destination.id.empty()) {
		destination.id = make_local_id();
	}

	return true;
}

} // namespace

MultistreamDock::MultistreamDock(QWidget *parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("PlasmaStreamMultistreamDock"));

	table_ = new QTableWidget(0, 4, this);
	table_->setHorizontalHeaderLabels({tr("On"), tr("Destination"), tr("Where"), tr("Status")});

	// Every column needs a mode. Leave one out and it keeps Qt's 100px default,
	// which in a narrow dock squeezes the columns that matter until their own
	// headers truncate.
	QHeaderView *header = table_->horizontalHeader();
	header->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	header->setSectionResizeMode(1, QHeaderView::Stretch);
	header->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	header->setSectionResizeMode(3, QHeaderView::ResizeToContents);
	header->setStretchLastSection(false);
	header->setHighlightSections(false);

	table_->verticalHeader()->setVisible(false);
	table_->verticalHeader()->setDefaultSectionSize(24);
	table_->setSelectionBehavior(QAbstractItemView::SelectRows);
	table_->setSelectionMode(QAbstractItemView::SingleSelection);
	table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table_->setWordWrap(false);
	table_->setAlternatingRowColors(true);

	// Elide rather than scroll sideways.
	table_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	table_->setTextElideMode(Qt::ElideRight);

	// The first thing everyone sees, so it should say what to do.
	empty_ = new QLabel(tr("No destinations yet.\n\nPress Add to send this stream somewhere "
			       "as well as wherever OBS is already sending it."),
			    this);
	empty_->setAlignment(Qt::AlignCenter);
	empty_->setWordWrap(true);
	empty_->setEnabled(false);

	add_ = new QPushButton(tr("Add"), this);
	edit_ = new QPushButton(tr("Edit"), this);
	remove_ = new QPushButton(tr("Remove"), this);
	fetch_ = new QPushButton(tr("Sync from PlasmaStream"), this);
	keyButton_ = new QPushButton(tr("Plugin key..."), this);

	notice_ = new QLabel(this);
	notice_->setWordWrap(true);
	notice_->setVisible(false);

	auto *buttons = new QHBoxLayout;
	buttons->addWidget(add_);
	buttons->addWidget(edit_);
	buttons->addWidget(remove_);
	buttons->addStretch();

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(table_);
	layout->addWidget(empty_);
	layout->addLayout(buttons);
	auto *syncRow = new QHBoxLayout;
	syncRow->addWidget(fetch_, 1);
	syncRow->addWidget(keyButton_);
	layout->addLayout(syncRow);
	layout->addWidget(notice_);

	connect(add_, &QPushButton::clicked, this, &MultistreamDock::addDestination);
	connect(edit_, &QPushButton::clicked, this, &MultistreamDock::editSelected);
	connect(remove_, &QPushButton::clicked, this, &MultistreamDock::removeSelected);
	connect(fetch_, &QPushButton::clicked, this, &MultistreamDock::fetchFromPlasmaStream);
	connect(keyButton_, &QPushButton::clicked, this, &MultistreamDock::changeToken);
	connect(table_, &QTableWidget::itemSelectionChanged, this,
		&MultistreamDock::updateButtons);

	// Polled, not pushed: libobs signals arrive on its own thread and the table
	// belongs to Qt's. A second of lag on a status light costs nothing.
	poll_ = new QTimer(this);
	poll_->setInterval(1000);
	connect(poll_, &QTimer::timeout, this, &MultistreamDock::refreshStatuses);
	poll_->start();

	// A floor, not a fixed size: it can still be dragged narrower.
	setMinimumWidth(360);

	rebuildTable();
	updateButtons();
}

QSize MultistreamDock::sizeHint() const
{
	return {420, 460};
}

void MultistreamDock::updateEmptyState()
{
	const bool any = !config().destinations.empty();
	table_->setVisible(any);
	empty_->setVisible(!any);
}

void MultistreamDock::setNotice(const QString &text, bool warning)
{
	notice_->setText(text);
	notice_->setVisible(!text.isEmpty());
	notice_->setStyleSheet(warning ? QStringLiteral("color: #f0a500;") : QString());
}

void MultistreamDock::rebuildTable()
{
	const std::vector<Destination> &destinations = config().destinations;

	updateEmptyState();

	table_->setRowCount(static_cast<int>(destinations.size()));

	for (int row = 0; row < static_cast<int>(destinations.size()); row++) {
		const Destination &destination = destinations[static_cast<size_t>(row)];

		auto *toggle = new QCheckBox(table_);
		toggle->setChecked(destination.enabled);

		// Index, not a pointer: removing a destination reallocates.
		connect(toggle, &QCheckBox::toggled, this, [row](bool on) {
			if (row < static_cast<int>(config().destinations.size())) {
				config().destinations[static_cast<size_t>(row)].enabled = on;
				save_config();
			}
		});

		table_->setCellWidget(row, 0, toggle);
		table_->setItem(row, 1,
				new QTableWidgetItem(QString::fromStdString(destination.name)));
		table_->setItem(row, 2,
				new QTableWidgetItem(platform_label(destination.platform)));

		// Never show the key, not even masked: the length is itself a hint.
		const QString status = destination.key.empty() ? tr("No stream key yet")
							       : tr("Ready");
		table_->setItem(row, 3, new QTableWidgetItem(status));
	}
}

void MultistreamDock::updateButtons()
{
	const bool selected = table_->currentRow() >= 0;
	edit_->setEnabled(selected);
	remove_->setEnabled(selected);
}

void MultistreamDock::addDestination()
{
	Destination destination;

	if (!edit_destination(this, destination, true)) {
		return;
	}

	config().destinations.push_back(destination);
	save_config();
	rebuildTable();
	updateButtons();
}

void MultistreamDock::editSelected()
{
	const int row = table_->currentRow();

	if (row < 0 || row >= static_cast<int>(config().destinations.size())) {
		return;
	}

	Destination destination = config().destinations[static_cast<size_t>(row)];

	if (!edit_destination(this, destination, false)) {
		return;
	}

	config().destinations[static_cast<size_t>(row)] = destination;
	save_config();
	rebuildTable();
}

void MultistreamDock::removeSelected()
{
	const int row = table_->currentRow();

	if (row < 0 || row >= static_cast<int>(config().destinations.size())) {
		return;
	}

	config().destinations.erase(config().destinations.begin() + row);
	save_config();
	rebuildTable();
	updateButtons();
}

/* Optional: the plugin works with this never pressed.
 *
 * Merges rather than replaces. The website holds server URLs and names and no
 * stream keys, so replacing the local list would wipe every key and leave
 * destinations that cannot connect.
 *
 * Runs on a worker because curl blocks. The QPointer matters: the dock can be
 * closed while a slow request is in flight. */
/* Reachable at any time, not only when nothing is stored: a key the server
 * rejects has to be replaceable. Empty disconnects the account and leaves the
 * destinations alone. */
void MultistreamDock::changeToken()
{
	bool ok = false;

	const QString entered = QInputDialog::getText(
		this, tr("Plugin key"),
		tr("Paste the link from your PlasmaStream dashboard, under Multistream. The "
		   "whole link is fine, or just the key at the end of it.\n\nLeave it empty to "
		   "disconnect from your account. Your destinations here stay as they are."),
		QLineEdit::Normal, QString::fromStdString(config().token), &ok);

	if (!ok) {
		return;
	}

	config().token = read_token(entered);
	save_config();

	setNotice(config().token.empty() ? tr("Plugin key cleared.") : tr("Plugin key saved."),
		  false);
}

void MultistreamDock::fetchFromPlasmaStream()
{
	if (config().token.empty()) {
		changeToken();

		// Cancelled.
		if (config().token.empty()) {
			return;
		}
	}

	setNotice(tr("Checking with PlasmaStream..."), false);
	fetch_->setEnabled(false);

	const std::string url = "https://plasmastream.live/api/plugin/" + config().token;

	QPointer<MultistreamDock> alive(this);

	std::thread([alive, url]() {
		const HttpResponse response = http_get(url);

		// Widgets belong to the Qt thread.
		QMetaObject::invokeMethod(
			qApp,
			[alive, response]() {
				if (!alive) {
					return;
				}

				alive->applyFetch(response);
			},
			Qt::QueuedConnection);
	}).detach();
}

void MultistreamDock::applyFetch(const HttpResponse &response)
{
	fetch_->setEnabled(true);

	if (response.status == 404) {
		// Forget it, or the next Sync skips the prompt and fails the same way.
		config().token.clear();
		save_config();

		setNotice(tr("That plugin key was not recognised, so it has been cleared. Press "
			     "Sync again and paste the link from your dashboard's Multistream "
			     "page."),
			  true);
		return;
	}

	if (!response.ok()) {
		// Include curl's reason: "could not reach PlasmaStream" alone is not
		// something anybody can act on.
		const QString detail =
			!response.error.empty()
				? QString::fromStdString(response.error)
				: tr("the server answered %1").arg(response.status);

		setNotice(tr("Could not reach PlasmaStream (%1). Your destinations here are "
			     "unchanged.")
				  .arg(detail),
			  true);
		return;
	}

	QJsonParseError parse{};
	const QJsonDocument document =
		QJsonDocument::fromJson(QByteArray::fromStdString(response.body), &parse);

	if (!document.isObject()) {
		setNotice(tr("PlasmaStream sent something unexpected (%1). Nothing changed.")
				  .arg(parse.errorString()),
			  true);
		return;
	}

	const QJsonArray incoming = document.object().value("destinations").toArray();
	int added = 0;
	int updated = 0;

	for (const QJsonValue &value : incoming) {
		const QJsonObject object = value.toObject();
		const std::string id = object.value("id").toString().toStdString();

		if (id.empty()) {
			continue;
		}

		Destination *existing = nullptr;

		for (Destination &candidate : config().destinations) {
			if (candidate.id == id) {
				existing = &candidate;
				break;
			}
		}

		if (existing) {
			// Not the key: the server does not have one to give.
			existing->name = object.value("name").toString().toStdString();
			existing->platform = object.value("platform").toString().toStdString();
			existing->url = object.value("ingestUrl").toString().toStdString();
			updated++;
			continue;
		}

		Destination destination;
		destination.id = id;
		destination.name = object.value("name").toString().toStdString();
		destination.platform = object.value("platform").toString().toStdString();
		destination.url = object.value("ingestUrl").toString().toStdString();
		config().destinations.push_back(destination);
		added++;
	}

	save_config();
	rebuildTable();
	updateButtons();

	if (added > 0) {
		setNotice(tr("Added %1 and updated %2. The new ones need their stream keys "
			     "before they will go anywhere.")
				  .arg(added)
				  .arg(updated),
			  true);
	} else if (updated > 0) {
		setNotice(tr("Up to date. %1 destination(s) checked.").arg(updated), false);
	} else {
		setNotice(tr("Your PlasmaStream account has no destinations saved yet. Add them "
			     "on the Multistream page, or just press Add here."),
			  false);
	}
}


void MultistreamDock::refreshStatuses()
{
	const std::vector<OutputStatus> statuses = output_statuses();

	if (statuses.empty()) {
		// Not streaming: show whether each row could, which is what matters while
		// setting up.
		for (int row = 0; row < table_->rowCount(); row++) {
			if (row >= static_cast<int>(config().destinations.size())) {
				break;
			}

			const Destination &destination = config().destinations[static_cast<size_t>(row)];
			QTableWidgetItem *item = table_->item(row, 3);

			if (item) {
				item->setText(destination.key.empty() ? tr("No stream key yet")
								      : tr("Ready"));
				item->setToolTip(QString());
			}
		}

		return;
	}

	for (int row = 0; row < table_->rowCount(); row++) {
		if (row >= static_cast<int>(config().destinations.size())) {
			break;
		}

		const Destination &destination = config().destinations[static_cast<size_t>(row)];
		QTableWidgetItem *item = table_->item(row, 3);

		if (!item) {
			continue;
		}

		bool found = false;

		for (const OutputStatus &status : statuses) {
			if (status.id != destination.id && status.id != destination.name) {
				continue;
			}

			found = true;

			switch (status.state) {
			case OutputState::Live:
				item->setText(tr("Live"));
				item->setToolTip(QString());
				break;
			case OutputState::Starting:
				item->setText(tr("Connecting"));
				item->setToolTip(QString());
				break;
			case OutputState::Failed:
				item->setText(tr("Failed"));
				item->setToolTip(QString::fromStdString(status.detail));
				break;
			default:
				item->setText(tr("Stopped"));
				item->setToolTip(QString());
				break;
			}

			break;
		}

		if (!found) {
			item->setText(destination.enabled ? tr("Not started") : tr("Off"));
			item->setToolTip(QString());
		}
	}
}

void register_dock()
{
	auto *main_window = static_cast<QWidget *>(obs_frontend_get_main_window());

	if (!main_window) {
		blog(LOG_WARNING, "[plasmastream] no main window, so no dock");
		return;
	}

	auto *dock = new MultistreamDock(main_window);
	dock->setWindowTitle(QObject::tr("PlasmaStream Multistream"));

	// _by_id, not the deprecated obs_frontend_add_dock: OBS remembers placement.
	obs_frontend_add_dock_by_id("plasmastream_multistream", "PlasmaStream Multistream", dock);
}

} // namespace plasmastream
