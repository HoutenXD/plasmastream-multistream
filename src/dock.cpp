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
#include "outputs.hpp"

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
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

namespace plasmastream {

namespace {

/**
 * The platforms offered in the dropdown, with the ingest address each one
 * publishes.
 *
 * A starting value, never a lock. Every platform runs regional ingests and hands
 * some streamers a different address in their own dashboard, so the field stays
 * editable and what somebody types wins. Silently replacing an address they
 * pasted would be the kind of bug that only shows up live.
 *
 * Kept in step with DESTINATION_PLATFORMS in the website's db package. They are
 * two copies of one list, which is a real cost, but the alternative is a plugin
 * that cannot show a platform name until it has talked to a server it is
 * supposed to work without.
 */
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

/** A short, stable id for a destination somebody typed in by hand. */
std::string make_local_id()
{
	static int counter = 0;
	return "local-" + std::to_string(++counter) + "-" +
	       std::to_string(static_cast<long long>(os_gettime_ns() / 1000000));
}

/**
 * The add and edit dialog.
 *
 * The stream key is a password field, and that is the one detail here that is
 * not cosmetic: people configure OBS while screen sharing, and a plain field
 * would put a credential that lets anybody broadcast as them onto a recording.
 */
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

	// Choosing a platform fills the address in, but only while the box still
	// holds whatever the last choice put there. Once somebody edits it, it is
	// theirs and nothing overwrites it.
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

	// Every column gets a mode, which the first version did not do. Setting only
	// the middle two to Stretch left the checkbox column on Qt's 100px default,
	// so a third of a narrow dock went to a tick box and the two columns anybody
	// actually reads were squeezed until their own headers truncated.
	//
	// The checkbox is as wide as a checkbox. The name takes whatever is left,
	// because it is the one field with no natural length. The platform and the
	// status size to their own text, which is short and known.
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

	// Nothing to scroll sideways to once the columns fit the dock, and a
	// scrollbar that appears for one pixel of overflow is worse than a name
	// elided with an ellipsis.
	table_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	table_->setTextElideMode(Qt::ElideRight);

	// An empty table is a large blank rectangle with no hint of what to do, and
	// that is the state every single person sees first.
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
	layout->addWidget(fetch_);
	layout->addWidget(notice_);

	connect(add_, &QPushButton::clicked, this, &MultistreamDock::addDestination);
	connect(edit_, &QPushButton::clicked, this, &MultistreamDock::editSelected);
	connect(remove_, &QPushButton::clicked, this, &MultistreamDock::removeSelected);
	connect(fetch_, &QPushButton::clicked, this, &MultistreamDock::fetchFromPlasmaStream);
	connect(table_, &QTableWidget::itemSelectionChanged, this,
		&MultistreamDock::updateButtons);

	network_ = new QNetworkAccessManager(this);

	// Polled rather than pushed. libobs fires its signals on its own thread and
	// the table has to be touched from the Qt thread, so the choice is a queued
	// connection per output or one timer that reads a snapshot. The timer is far
	// less to get wrong, and a second of lag on a status light costs nothing.
	poll_ = new QTimer(this);
	poll_->setInterval(1000);
	connect(poll_, &QTimer::timeout, this, &MultistreamDock::refreshStatuses);
	poll_->start();

	// A floor rather than a fixed size, so it can still be dragged narrower or
	// docked into a thin column. It just cannot open there by default.
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

		// The row index is captured, not a pointer into the vector, because
		// removing a destination reallocates it.
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

		// The key is never shown, not even masked with its own length, which
		// would leak how long it is. What matters to somebody looking at this
		// row is whether one is set at all.
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

/**
 * Pull the streamer's destinations from their PlasmaStream account.
 *
 * Entirely optional. The plugin works with this never pressed, which is the
 * whole design: somebody can install it, type their destinations in, and never
 * have an account.
 *
 * ## Merging, rather than replacing
 *
 * The website holds server URLs and names, and deliberately holds no stream
 * keys. So a sync that replaced the local list would wipe every key the streamer
 * had entered and leave them with destinations that cannot connect. Existing
 * entries keep their key and take the server's name and address; new ones arrive
 * without a key and the table says so.
 */
void MultistreamDock::fetchFromPlasmaStream()
{
	const QString token = QString::fromStdString(config().token).trimmed();

	if (token.isEmpty()) {
		bool ok = false;
		const QString entered = QInputDialog::getText(
			this, tr("Sync from PlasmaStream"),
			tr("Paste the plugin key from your PlasmaStream dashboard, under "
			   "Multistream."),
			QLineEdit::Normal, QString(), &ok);

		if (!ok || entered.trimmed().isEmpty()) {
			return;
		}

		config().token = entered.trimmed().toStdString();
		save_config();
	}

	setNotice(tr("Checking with PlasmaStream..."), false);
	fetch_->setEnabled(false);

	const QString url = QStringLiteral("https://plasmastream.live/api/plugin/") +
			    QString::fromStdString(config().token);

	QNetworkRequest request{QUrl(url)};
	request.setHeader(QNetworkRequest::UserAgentHeader,
			  QStringLiteral("PlasmaStream-Multistream-OBS"));

	QNetworkReply *reply = network_->get(request);

	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();
		fetch_->setEnabled(true);

		if (reply->error() != QNetworkReply::NoError) {
			const int status =
				reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

			// A 404 is the one worth naming, because it is the only one the
			// streamer can fix and it has exactly one cause.
			setNotice(status == 404
					  ? tr("That plugin key was not recognised. Copy it "
					       "again from your dashboard.")
					  : tr("Could not reach PlasmaStream. Your destinations "
					       "here are unchanged."),
				  true);
			return;
		}

		const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());

		if (!document.isObject()) {
			setNotice(tr("PlasmaStream sent something unexpected. Nothing changed."),
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
				// The key is pointedly not touched. It is the one field the
				// server does not have, and overwriting it with nothing is
				// how a sync silently breaks a working setup.
				existing->name = object.value("name").toString().toStdString();
				existing->platform =
					object.value("platform").toString().toStdString();
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
			setNotice(tr("Added %1 and updated %2. The new ones need their stream "
				     "keys before they will go anywhere.")
					  .arg(added)
					  .arg(updated),
				  true);
		} else {
			setNotice(tr("Up to date. %1 destination(s) checked.").arg(updated), false);
		}
	});
}

void MultistreamDock::refreshStatuses()
{
	const std::vector<OutputStatus> statuses = output_statuses();

	if (statuses.empty()) {
		// Not streaming. Fall back to whether each row could stream if asked,
		// which is the useful thing to know while setting up.
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

	// _by_id rather than the older obs_frontend_add_dock, which is deprecated
	// and leaves OBS unable to remember where the dock was put.
	obs_frontend_add_dock_by_id("plasmastream_multistream", "PlasmaStream Multistream", dock);
}

} // namespace plasmastream
