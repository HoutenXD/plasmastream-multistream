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
#include <vector>

#include <QBrush>
#include <QCheckBox>
#include <QColor>
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
#include <QSize>
#include <QSpinBox>
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

/* Every video encoder OBS has registered on this machine. Asked at the moment
 * the dialog opens rather than cached: plugins register encoders at load, and a
 * machine gains and loses them with its hardware. */
std::vector<const char *> available_encoders()
{
	std::vector<const char *> ids;

	const char *id = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); i++) {
		if (obs_get_encoder_type(id) == OBS_ENCODER_VIDEO) {
			ids.push_back(id);
		}
	}

	return ids;
}

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

	// The platforms that only take portrait are the reason anybody runs two
	// destinations in the first place, so this sits above the encoder settings
	// it quietly forces on.
	auto *vertical = new QCheckBox(QObject::tr("Send this one a vertical 9:16 frame"),
				       &dialog);
	vertical->setChecked(destination.vertical);

	auto *verticalSize = new QComboBox(&dialog);
	verticalSize->addItem(QObject::tr("1080 x 1920"), QSize(1080, 1920));
	verticalSize->addItem(QObject::tr("720 x 1280"), QSize(720, 1280));
	verticalSize->addItem(QObject::tr("1440 x 2560"), QSize(1440, 2560));

	const int sizeIndex = verticalSize->findData(
		QSize(destination.vertical_width, destination.vertical_height));

	if (sizeIndex >= 0) {
		verticalSize->setCurrentIndex(sizeIndex);
	}

	auto *verticalFit = new QComboBox(&dialog);
	verticalFit->addItem(QObject::tr("Fill the frame, crop the sides"), true);
	verticalFit->addItem(QObject::tr("Fit it all in, bars top and bottom"), false);
	verticalFit->setCurrentIndex(destination.vertical_crop ? 0 : 1);

	// Off by default because sharing costs nothing. On is for the case people
	// actually hit: an upload that cannot carry two copies of the main stream.
	auto *own = new QCheckBox(QObject::tr("Use its own bitrate for this destination"), &dialog);
	own->setChecked(destination.own_encoder);

	auto *videoBitrate = new QSpinBox(&dialog);
	videoBitrate->setRange(200, 51000);
	videoBitrate->setSingleStep(250);
	videoBitrate->setSuffix(QObject::tr(" kbps"));
	videoBitrate->setValue(destination.video_bitrate);

	auto *audioBitrate = new QSpinBox(&dialog);
	audioBitrate->setRange(32, 320);
	audioBitrate->setSingleStep(32);
	audioBitrate->setSuffix(QObject::tr(" kbps"));
	audioBitrate->setValue(destination.audio_bitrate);

	auto *encoder = new QComboBox(&dialog);
	encoder->addItem(QObject::tr("Same as main stream"), QString());

	// Whatever this machine actually has. Listing encoders it does not have
	// would offer a choice that fails at the moment somebody goes live.
	for (const char *id : available_encoders()) {
		encoder->addItem(QString::fromUtf8(obs_encoder_get_display_name(id)),
				 QString::fromUtf8(id));
	}

	const int encoderIndex = encoder->findData(QString::fromStdString(destination.encoder_id));
	if (encoderIndex >= 0) {
		encoder->setCurrentIndex(encoderIndex);
	}

	auto *ownNote = new QLabel(
		QObject::tr("Off, this destination shares the encoder OBS is already using: no "
			    "extra CPU, but the same bitrate as your main stream. On, it gets its "
			    "own, which costs CPU and lets you send less to a second platform. "
			    "Vertical always needs its own, because it is a different picture."),
		&dialog);
	ownNote->setWordWrap(true);

	// Vertical has no choice about its own encoder: the main stream's is bound
	// to the main canvas. Showing the box ticked and greyed says that, where a
	// box that silently disagrees with what happens does not.
	const auto syncEncoderRow = [own, vertical, verticalSize, verticalFit, videoBitrate,
				     audioBitrate, encoder]() {
		const bool portrait = vertical->isChecked();
		const bool separate = portrait || own->isChecked();

		own->setEnabled(!portrait);

		if (portrait) {
			own->setChecked(true);
		}

		verticalSize->setEnabled(portrait);
		verticalFit->setEnabled(portrait);
		videoBitrate->setEnabled(separate);
		audioBitrate->setEnabled(separate);
		encoder->setEnabled(separate);
	};

	QObject::connect(own, &QCheckBox::toggled, syncEncoderRow);
	QObject::connect(vertical, &QCheckBox::toggled, syncEncoderRow);
	syncEncoderRow();

	auto *form = new QFormLayout;
	form->addRow(QObject::tr("Platform"), platform);
	form->addRow(QObject::tr("Name"), name);
	form->addRow(QObject::tr("Server URL"), url);
	form->addRow(QObject::tr("Stream key"), key);
	form->addRow(QString(), vertical);
	form->addRow(QObject::tr("Vertical size"), verticalSize);
	form->addRow(QObject::tr("Vertical framing"), verticalFit);
	form->addRow(QString(), own);
	form->addRow(QObject::tr("Video bitrate"), videoBitrate);
	form->addRow(QObject::tr("Audio bitrate"), audioBitrate);
	form->addRow(QObject::tr("Encoder"), encoder);

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
	layout->addWidget(ownNote);
	layout->addWidget(hint);
	layout->addWidget(buttons);

	if (dialog.exec() != QDialog::Accepted) {
		return false;
	}

	destination.platform = platform->currentData().toString().toStdString();
	destination.name = name->text().trimmed().toStdString();
	destination.url = url->text().trimmed().toStdString();
	destination.key = key->text().trimmed().toStdString();
	destination.own_encoder = own->isChecked();
	destination.video_bitrate = videoBitrate->value();
	destination.audio_bitrate = audioBitrate->value();
	destination.encoder_id = encoder->currentData().toString().toStdString();
	destination.vertical = vertical->isChecked();
	destination.vertical_crop = verticalFit->currentData().toBool();

	const QSize portrait = verticalSize->currentData().toSize();
	destination.vertical_width = portrait.width();
	destination.vertical_height = portrait.height();

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

	table_ = new QTableWidget(0, 5, this);
	table_->setHorizontalHeaderLabels(
		{tr("On"), tr("Destination"), tr("Where"), tr("Status"), tr("Health")});

	// Every column needs a mode. Leave one out and it keeps Qt's 100px default,
	// which in a narrow dock squeezes the columns that matter until their own
	// headers truncate.
	QHeaderView *header = table_->horizontalHeader();
	header->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	header->setSectionResizeMode(1, QHeaderView::Stretch);
	header->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	header->setSectionResizeMode(3, QHeaderView::ResizeToContents);
	header->setSectionResizeMode(4, QHeaderView::ResizeToContents);
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
	options_ = new QPushButton(tr("Options..."), this);

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
	syncRow->addWidget(options_);
	layout->addLayout(syncRow);
	layout->addWidget(notice_);

	connect(add_, &QPushButton::clicked, this, &MultistreamDock::addDestination);
	connect(edit_, &QPushButton::clicked, this, &MultistreamDock::editSelected);
	connect(remove_, &QPushButton::clicked, this, &MultistreamDock::removeSelected);
	connect(fetch_, &QPushButton::clicked, this, &MultistreamDock::fetchFromPlasmaStream);
	connect(keyButton_, &QPushButton::clicked, this, &MultistreamDock::changeToken);
	connect(options_, &QPushButton::clicked, this, &MultistreamDock::showOptions);
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
			if (row >= static_cast<int>(config().destinations.size())) {
				return;
			}

			Destination &destination = config().destinations[static_cast<size_t>(row)];
			destination.enabled = on;
			save_config();

			// Takes effect now, not at the next stream. Dropping a platform
			// that is failing should not mean ending the broadcast.
			if (!streaming_live()) {
				return;
			}

			const std::string id =
				destination.id.empty() ? destination.name : destination.id;

			if (on) {
				start_one(id);
			} else {
				stop_one(id);
			}
		});

		table_->setCellWidget(row, 0, toggle);
		table_->setItem(row, 1,
				new QTableWidgetItem(QString::fromStdString(destination.name)));
		const QString where = destination.vertical
					      ? tr("%1, 9:16").arg(platform_label(destination.platform))
					      : platform_label(destination.platform);

		table_->setItem(row, 2, new QTableWidgetItem(where));

		// Never show the key, not even masked: the length is itself a hint.
		const QString status = destination.key.empty() ? tr("No stream key yet")
							       : tr("Ready");
		table_->setItem(row, 3, new QTableWidgetItem(status));
		table_->setItem(row, 4, new QTableWidgetItem(QString()));
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

void MultistreamDock::showOptions()
{
	QDialog dialog(this);
	dialog.setWindowTitle(tr("Multistream options"));

	auto *retries = new QSpinBox(&dialog);
	retries->setRange(0, 100);
	retries->setValue(config().reconnect_retries);
	retries->setSpecialValueText(tr("Do not reconnect"));

	// Five seconds, against OBS's ten for the main stream. A second destination
	// coming back sooner costs nothing, and the stream people are actually
	// watching is still up while it tries.
	auto *delay = new QSpinBox(&dialog);
	delay->setRange(1, 120);
	delay->setSuffix(tr(" seconds"));
	delay->setValue(config().reconnect_delay_sec);

	auto *sync = new QCheckBox(tr("Sync destinations from PlasmaStream when OBS starts"),
				   &dialog);
	sync->setChecked(config().sync_on_launch);

	auto *note = new QLabel(
		tr("Reconnect settings apply to the destinations this plugin sends to. Your main "
		   "stream keeps whatever is set in OBS's own settings."),
		&dialog);
	note->setWordWrap(true);

	auto *form = new QFormLayout;
	form->addRow(tr("Reconnect attempts"), retries);
	form->addRow(tr("Wait between attempts"), delay);
	form->addRow(QString(), sync);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
					     &dialog);
	connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

	auto *layout = new QVBoxLayout(&dialog);
	layout->addLayout(form);
	layout->addWidget(note);
	layout->addWidget(buttons);

	if (dialog.exec() != QDialog::Accepted) {
		return;
	}

	config().reconnect_retries = retries->value();
	config().reconnect_delay_sec = delay->value();
	config().sync_on_launch = sync->isChecked();
	save_config();

	// Deliberately not applied to anything already running: changing the retry
	// count mid-stream should not restart a destination that is up.
	setNotice(tr("Saved. New settings apply the next time a destination starts."), false);
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

		setNotice(tr("That plugin key was not recognized, so it has been cleared. Press "
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


/* Seconds as h:mm:ss, for uptime. */
static QString uptimeText(int seconds)
{
	const int hours = seconds / 3600;
	const int minutes = (seconds % 3600) / 60;

	return QStringLiteral("%1:%2:%3")
		.arg(hours)
		.arg(minutes, 2, 10, QLatin1Char('0'))
		.arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

void MultistreamDock::refreshStatuses()
{
	const std::vector<OutputStatus> statuses = output_statuses();

	for (int row = 0; row < table_->rowCount(); row++) {
		if (row >= static_cast<int>(config().destinations.size())) {
			break;
		}

		const Destination &destination = config().destinations[static_cast<size_t>(row)];
		QTableWidgetItem *item = table_->item(row, 3);
		QTableWidgetItem *health = table_->item(row, 4);

		if (!item) {
			continue;
		}

		const std::string id = destination.id.empty() ? destination.name : destination.id;
		const OutputStatus *found = nullptr;

		for (const OutputStatus &status : statuses) {
			if (status.id == id) {
				found = &status;
				break;
			}
		}

		if (!found) {
			// Not running: say whether it could, which is what matters while
			// setting up.
			item->setText(destination.key.empty()  ? tr("No stream key yet")
				      : !destination.enabled ? tr("Off")
							     : tr("Ready"));
			item->setToolTip(QString());

			if (health) {
				health->setText(QString());
				health->setToolTip(QString());
			}

			continue;
		}

		switch (found->state) {
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
			item->setToolTip(QString::fromStdString(found->detail));
			break;
		default:
			item->setText(tr("Stopped"));
			item->setToolTip(QString());
			break;
		}

		if (!health) {
			continue;
		}

		// The measured rate, and the drop rate when there is one. This is the
		// answer to "why does my stream look bad", which a status word alone
		// never gives: a destination can be Live and still be losing a tenth of
		// its frames because the upload cannot carry it.
		const double dropped = found->drop_percent();

		QString text = QStringLiteral("%1 kbps").arg(found->bitrate_kbps);

		if (dropped >= 0.05) {
			text += QStringLiteral("  %1% dropped").arg(dropped, 0, 'f', 1);
		}

		health->setText(text);

		// Anything above a fraction of a percent is worth seeing, and above a
		// couple of percent is worth worrying about.
		health->setForeground(dropped >= 2.0   ? QBrush(QColor(0xf4, 0x3f, 0x5e))
				      : dropped >= 0.5 ? QBrush(QColor(0xf0, 0xa5, 0x00))
						       : QBrush());

		health->setToolTip(tr("Up %1, %2 of %3 frames dropped, %4 reconnect(s)")
					   .arg(uptimeText(found->uptime_sec))
					   .arg(found->dropped_frames)
					   .arg(found->total_frames)
					   .arg(found->reconnects));
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
