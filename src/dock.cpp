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

#include "appearance.hpp"

#include "config.hpp"
#include "http.hpp"
#include "outputs.hpp"
#include "scenerec.hpp"
#include "vertical.hpp"

#include <cstring>
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
#include <QFileDialog>
#include <QFrame>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
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

/* Every scene in the collection, in the order OBS lists them. */
QStringList scene_names()
{
	QStringList names;

	obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);

	for (size_t i = 0; i < scenes.sources.num; i++) {
		const char *name = obs_source_get_name(scenes.sources.array[i]);

		if (name && *name) {
			names.append(QString::fromUtf8(name));
		}
	}

	obs_frontend_source_list_free(&scenes);
	return names;
}

/* Bytes as something that fits on one line beside a clock. */
QString short_size(uint64_t bytes)
{
	if (bytes < 1024ULL * 1024ULL) {
		return QObject::tr("%1 KB").arg(bytes / 1024ULL);
	}

	if (bytes < 1024ULL * 1024ULL * 1024ULL) {
		return QObject::tr("%1 MB").arg(bytes / (1024ULL * 1024ULL));
	}

	const uint64_t gb = 1024ULL * 1024ULL * 1024ULL;

	return QObject::tr("%1.%2 GB").arg(bytes / gb).arg((bytes % gb) * 10ULL / gb);
}

/* A hairline above a group, so the recording row reads as its own thing rather
 * than as one more button under the destination list. */
QFrame *divider(QWidget *parent)
{
	auto *line = new QFrame(parent);
	line->setFrameShape(QFrame::HLine);
	line->setFrameShadow(QFrame::Plain);

	/* Stylesheet, not palette: an OBS theme is a stylesheet and would win. */
	QColor rule = color::muted();
	rule.setAlpha(70);
	line->setStyleSheet(QStringLiteral("color: rgba(%1,%2,%3,%4);")
				    .arg(rule.red())
				    .arg(rule.green())
				    .arg(rule.blue())
				    .arg(rule.alpha()));

	return line;
}

/* Where this actually goes.
 *
 * "Something else" is the right label in the dropdown and the wrong one in a
 * column headed Where, because it answers a question nobody asked. For anything
 * custom the host is what the streamer recognizes: they typed it. */
QString where_label(const Destination &destination);

/* Seconds as something short enough to sit inline: 45s, 12m, 2h 04m. The full
 * clock is in the tooltip, where there is room for it. */
QString short_uptime(int seconds)
{
	if (seconds < 60) {
		return QStringLiteral("%1s").arg(seconds);
	}

	if (seconds < 3600) {
		return QStringLiteral("%1m").arg(seconds / 60);
	}

	return QStringLiteral("%1h %2m")
		.arg(seconds / 3600)
		.arg((seconds % 3600) / 60, 2, 10, QLatin1Char('0'));
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

QString where_label(const Destination &destination)
{
	for (const PlatformOption &option : PLATFORMS) {
		if (destination.platform == option.key && destination.platform != "custom") {
			return QString::fromUtf8(option.label);
		}
	}

	/* Host only: the scheme is noise in a narrow column, and the path after it
	 * is the same "/live2" on every row of a given platform. */
	QString host = QString::fromStdString(destination.url);
	const int scheme = host.indexOf(QLatin1String("://"));

	if (scheme >= 0) {
		host = host.mid(scheme + 3);
	}

	const int slash = host.indexOf(QLatin1Char('/'));

	if (slash >= 0) {
		host = host.left(slash);
	}

	return host.isEmpty() ? QObject::tr("Not set yet") : host;
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
	vertical->setChecked(destination.vertical && vertical_supported());

	// A build against libobs older than 31.1 has no canvas to render into, so
	// the box is disabled and says why rather than being offered and ignored.
	// That is the Linux distribution packages, not a hypothetical.
	if (!vertical_supported()) {
		vertical->setEnabled(false);
		vertical->setToolTip(
			QObject::tr("This build was made against a version of OBS without the "
				    "canvas support vertical needs. Build against OBS 31.1 or "
				    "newer to use it."));
	}

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
	const auto syncEncoderRow = [own, vertical, videoBitrate, audioBitrate, encoder]() {
		const bool portrait = vertical->isChecked();
		const bool separate = portrait || own->isChecked();

		own->setEnabled(!portrait);

		if (portrait) {
			own->setChecked(true);
		}

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
	table_->setHorizontalHeaderLabels({tr("On"), tr("Destination"), tr("Where"), tr("State")});

	// Status and Health used to be separate, which meant a column that was empty
	// until you went live and another that carried three unrelated meanings:
	// configured, broken, and connected. One column that answers "what is this
	// doing" is the thing somebody actually glances at mid-stream.
	table_->setItemDelegate(new RowDelegate(table_));

	set_up_destination_columns(table_);

	table_->verticalHeader()->setVisible(false);
	table_->verticalHeader()->setDefaultSectionSize(24);
	table_->setSelectionBehavior(QAbstractItemView::SelectRows);
	table_->setSelectionMode(QAbstractItemView::SingleSelection);
	table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table_->setWordWrap(false);
	table_->setAlternatingRowColors(true);

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

	recordingState_ = new QLabel(this);
	recordingState_->setTextFormat(Qt::PlainText);

	recordingSetUp_ = new QPushButton(tr("Record a scene..."), this);
	recordingToggle_ = new QPushButton(tr("Start"), this);

	connect(recordingSetUp_, &QPushButton::clicked, this, &MultistreamDock::setUpRecording);
	connect(recordingToggle_, &QPushButton::clicked, this, &MultistreamDock::toggleRecording);

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

	layout->addWidget(divider(this));

	auto *recordingRow = new QHBoxLayout;
	recordingRow->addWidget(recordingState_, 1);
	recordingRow->addWidget(recordingSetUp_);
	recordingRow->addWidget(recordingToggle_);
	layout->addLayout(recordingRow);

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
	refreshRecording();
	lay_out_destination_columns(table_);
}

void MultistreamDock::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);

	/* The table is inside a layout, so it has not been given its new width yet
	 * when this runs. Asking for the share now would divide up the old one. */
	QTimer::singleShot(0, this, [this]() { lay_out_destination_columns(table_); });
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
		// The mark and the 9:16 chip are painted by the delegate, so the item
		// carries what to draw rather than a name with things typed into it.
		auto *name = new QTableWidgetItem(QString::fromStdString(destination.name));
		name->setData(RowDelegate::PlatformRole,
			      QString::fromStdString(destination.platform));
		name->setData(RowDelegate::VerticalRole, destination.vertical);
		table_->setItem(row, 1, name);

		table_->setItem(row, 2, new QTableWidgetItem(where_label(destination)));

		// Never show the key, not even masked: the length is itself a hint.
		table_->setItem(row, 3, new QTableWidgetItem(QString()));
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

/* Set up recording a scene other than the one going out.
 *
 * The pitch is on the dialog itself, because the whole feature is one somebody
 * has to be told exists: OBS records the programme, so today the choice is a
 * clean recording or a stream with a chat box, and this is how you stop
 * choosing. */
void MultistreamDock::setUpRecording()
{
	if (!scene_recording_supported()) {
		QMessageBox::information(
			this, tr("Not in this version of OBS"),
			tr("Recording a second scene needs OBS 31.1 or newer, which is where "
			   "the second canvas this uses arrived.\n\nEverything else in "
			   "PlasmaStream works as it is."));
		return;
	}

	const QStringList scenes = scene_names();

	if (scenes.isEmpty()) {
		QMessageBox::information(this, tr("No scenes yet"),
					 tr("Make the scene you want recorded first, then come "
					    "back and pick it here."));
		return;
	}

	SceneRecording settings = config().scene_recording;

	QDialog dialog(this);
	dialog.setWindowTitle(tr("Record a scene"));

	auto *enabled = new QCheckBox(tr("Record a scene while I stream"), &dialog);
	enabled->setChecked(settings.enabled);

	auto *scene = new QComboBox(&dialog);
	scene->addItems(scenes);

	const int sceneIndex = scene->findText(QString::fromStdString(settings.scene));

	if (sceneIndex >= 0) {
		scene->setCurrentIndex(sceneIndex);
	}

	auto *withStream = new QCheckBox(tr("Start and stop it with my stream"), &dialog);
	withStream->setChecked(settings.with_stream);

	auto *folder = new QLineEdit(QString::fromStdString(settings.folder), &dialog);

	/* Blank means OBS's own recording folder, which is almost always what
	 * somebody wants and is worth saying rather than leaving them to guess. */
	char *configured = obs_frontend_get_current_record_output_path();

	if (configured) {
		folder->setPlaceholderText(
			tr("Where OBS records (%1)").arg(QString::fromUtf8(configured)));
		bfree(configured);
	} else {
		folder->setPlaceholderText(tr("Where OBS records"));
	}

	auto *browse = new QPushButton(tr("Browse..."), &dialog);

	connect(browse, &QPushButton::clicked, &dialog, [&dialog, folder]() {
		const QString chosen = QFileDialog::getExistingDirectory(
			&dialog, tr("Where should the recordings go?"), folder->text());

		if (!chosen.isEmpty()) {
			folder->setText(chosen);
		}
	});

	auto *folderRow = new QHBoxLayout;
	folderRow->addWidget(folder, 1);
	folderRow->addWidget(browse);

	auto *format = new QComboBox(&dialog);
	format->addItem(tr("mkv, survives a crash"), QStringLiteral("mkv"));
	format->addItem(tr("mp4, opens anywhere"), QStringLiteral("mp4"));

	const int formatIndex = format->findData(QString::fromStdString(settings.format));

	if (formatIndex >= 0) {
		format->setCurrentIndex(formatIndex);
	}

	auto *bitrate = new QSpinBox(&dialog);
	bitrate->setRange(1000, 60000);
	bitrate->setSingleStep(500);
	bitrate->setSuffix(tr(" kbps"));
	bitrate->setValue(settings.video_bitrate);

	auto *encoder = new QComboBox(&dialog);
	encoder->addItem(tr("Same hardware as my stream, in H.264"), QString());

	for (const char *id : available_encoders()) {
		encoder->addItem(QString::fromUtf8(obs_encoder_get_display_name(id)),
				 QString::fromUtf8(id));
	}

	const int encoderIndex = encoder->findData(QString::fromStdString(settings.encoder_id));

	if (encoderIndex >= 0) {
		encoder->setCurrentIndex(encoderIndex);
	}

	/* HEVC and AV1 are real choices with real advantages, so they stay on the
	 * list. What they cost is said beside them, because the way people find out
	 * otherwise is a finished recording that will not open. */
	auto *codecNote = new QLabel(&dialog);
	codecNote->setWordWrap(true);
	codecNote->setStyleSheet(QStringLiteral("color: %1;").arg(color::warn().name()));

	const auto checkCodec = [=]() {
		const QByteArray id = encoder->currentData().toString().toUtf8();
		const char *codec = id.isEmpty() ? nullptr : obs_get_encoder_codec(id.constData());

		if (!codec || strcmp(codec, "h264") == 0) {
			codecNote->clear();
			codecNote->setVisible(false);
			return;
		}

		codecNote->setText(
			tr("This makes %1 files. YouTube takes them, and so do VLC and most "
			   "editors, but Windows' own Media Player and Movies & TV will not open "
			   "them without an extension from the Microsoft Store. H.264 opens "
			   "everywhere.")
				.arg(strcmp(codec, "hevc") == 0 ? tr("HEVC")
				     : strcmp(codec, "av1") == 0 ? tr("AV1")
								 : QString::fromUtf8(codec).toUpper()));
		codecNote->setVisible(true);
	};

	connect(encoder, &QComboBox::currentIndexChanged, &dialog, checkCodec);

	auto *track = new QSpinBox(&dialog);
	track->setRange(1, 6);
	track->setValue(settings.audio_track);
	track->setPrefix(tr("Track "));

	/* Recording a scene runs it, and a running source makes noise. For the
	 * picture that is the whole point; for the audio it is a surprise, and one
	 * people find out about from somebody watching. So it is said here, with
	 * the names, before it can happen. */
	auto *leak = new QLabel(&dialog);
	leak->setWordWrap(true);
	leak->setStyleSheet(QStringLiteral("color: %1;").arg(color::warn().name()));

	const auto checkAudio = [=]() {
		const std::vector<std::string> heard =
			scene_audio_reaching_stream(scene->currentText().toStdString());

		if (heard.empty() || !enabled->isChecked()) {
			leak->clear();
			leak->setVisible(false);
			return;
		}

		QStringList names;

		for (const std::string &name : heard) {
			names.append(QString::fromStdString(name));
		}

		leak->setText(
			tr("%1 has sound in it: %2. Recording this scene makes it play, and it "
			   "will go out on your stream as well. To keep it off the stream, move "
			   "it to an audio track of its own in OBS, then set Audio below to that "
			   "track.")
				.arg(scene->currentText())
				.arg(names.join(tr(", "))));

		leak->setVisible(true);
	};

	connect(scene, &QComboBox::currentTextChanged, &dialog, checkAudio);

	auto *note = new QLabel(
		tr("This records a second picture of its own, so it costs a second encode. "
		   "The scene you pick runs whether or not it is the one on screen, which is "
		   "the point: stream the scene with your chat box and alerts, keep the clean "
		   "one for the video.\n\nIt does not touch OBS's own Start Recording button. "
		   "Both can run at once."),
		&dialog);
	note->setWordWrap(true);

	note->setStyleSheet(QStringLiteral("color: %1;").arg(color::muted().name()));

	auto *form = new QFormLayout;
	form->addRow(QString(), enabled);
	form->addRow(tr("Scene to record"), scene);
	form->addRow(QString(), withStream);
	form->addRow(tr("Save to"), folderRow);
	form->addRow(tr("File type"), format);
	form->addRow(tr("Quality"), bitrate);
	form->addRow(tr("Encoder"), encoder);
	form->addRow(QString(), codecNote);
	form->addRow(tr("Audio"), track);
	form->addRow(QString(), leak);

	/* Everything below the first box only means something once it is ticked. */
	const auto followEnabled = [=]() {
		const bool on = enabled->isChecked();

		scene->setEnabled(on);
		withStream->setEnabled(on);
		folder->setEnabled(on);
		browse->setEnabled(on);
		format->setEnabled(on);
		bitrate->setEnabled(on);
		encoder->setEnabled(on);
		track->setEnabled(on);
	};

	connect(enabled, &QCheckBox::toggled, &dialog, followEnabled);
	connect(enabled, &QCheckBox::toggled, &dialog, checkAudio);
	followEnabled();
	checkAudio();
	checkCodec();

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

	settings.enabled = enabled->isChecked();
	settings.scene = scene->currentText().toStdString();
	settings.with_stream = withStream->isChecked();
	settings.folder = folder->text().trimmed().toStdString();
	settings.format = format->currentData().toString().toStdString();
	settings.video_bitrate = bitrate->value();
	settings.encoder_id = encoder->currentData().toString().toStdString();
	settings.audio_track = track->value();

	config().scene_recording = settings;
	save_config();

	/* Deliberately not applied to a recording already in progress: changing the
	 * bitrate should not cut the file somebody is in the middle of making. */
	if (scene_recording_active()) {
		setNotice(tr("Saved. It applies to the next recording, not the one running."),
			  false);
	}

	refreshRecording();
}

void MultistreamDock::toggleRecording()
{
	if (scene_recording_active()) {
		scene_recording_stop();
		refreshRecording();
		return;
	}

	/* Pressing Start before it is set up is somebody asking for the thing, not
	 * making a mistake, so it opens the dialog rather than refusing. */
	if (!config().scene_recording.enabled || config().scene_recording.scene.empty()) {
		setUpRecording();
		return;
	}

	if (!scene_recording_start()) {
		setNotice(tr("The recording would not start. Check the scene still exists and "
			     "there is room on the drive; the OBS log has the reason."),
			  true);
	}

	refreshRecording();
}

/* The one line of this that anyone reads mid-stream: is it recording, and for
 * how long. */
void MultistreamDock::refreshRecording()
{
	const SceneRecordingStatus status = scene_recording_status();
	const SceneRecording &settings = config().scene_recording;

	QColor ink = color::muted();
	QString text;

	if (status.running) {
		ink = color::ok();
		text = tr("Recording %1 - %2, %3")
			       .arg(QString::fromStdString(status.scene))
			       .arg(short_uptime(status.elapsed_sec))
			       .arg(short_size(status.bytes));
	} else if (!status.error.empty()) {
		ink = color::danger();
		text = tr("Recording stopped: %1").arg(QString::fromStdString(status.error));
	} else if (!scene_recording_supported()) {
		text = tr("Recording a second scene needs OBS 31.1");
	} else if (!settings.enabled || settings.scene.empty()) {
		text = tr("Record one scene while you stream another");
	} else if (settings.with_stream) {
		text = tr("Ready to record %1 when you go live")
			       .arg(QString::fromStdString(settings.scene));
	} else {
		text = tr("Ready to record %1").arg(QString::fromStdString(settings.scene));
	}

	recordingState_->setText(text);
	recordingState_->setToolTip(status.file.empty() ? QString()
						       : QString::fromStdString(status.file));

	/* A stylesheet rather than a palette: OBS themes are stylesheets, and a
	 * stylesheet beats a palette, so setting the palette here changes nothing
	 * on any theme anybody actually uses. */
	recordingState_->setStyleSheet(QStringLiteral("color: %1;").arg(ink.name()));

	recordingToggle_->setText(status.running ? tr("Stop") : tr("Start"));
	recordingToggle_->setEnabled(scene_recording_supported());
	recordingSetUp_->setEnabled(scene_recording_supported());
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

	// There is one vertical canvas, shared by every destination pointed at it,
	// so its shape is a setting here rather than a copy on each of them.
	auto *verticalSize = new QComboBox(&dialog);
	verticalSize->addItem(tr("1080 x 1920"), QSize(1080, 1920));
	verticalSize->addItem(tr("720 x 1280"), QSize(720, 1280));
	verticalSize->addItem(tr("1440 x 2560"), QSize(1440, 2560));

	const int sizeIndex =
		verticalSize->findData(QSize(config().vertical_width, config().vertical_height));

	if (sizeIndex >= 0) {
		verticalSize->setCurrentIndex(sizeIndex);
	}

	auto *verticalFit = new QComboBox(&dialog);
	verticalFit->addItem(tr("Fill the frame, crop the sides"), true);
	verticalFit->addItem(tr("Fit it all in, bars top and bottom"), false);
	verticalFit->setCurrentIndex(config().vertical_crop ? 0 : 1);

	if (!vertical_supported()) {
		verticalSize->setEnabled(false);
		verticalFit->setEnabled(false);
	}

	auto *note = new QLabel(
		tr("Reconnect settings apply to the destinations this plugin sends to. Your main "
		   "stream keeps whatever is set in OBS's own settings."),
		&dialog);
	note->setWordWrap(true);

	auto *form = new QFormLayout;
	form->addRow(tr("Reconnect attempts"), retries);
	form->addRow(tr("Wait between attempts"), delay);
	form->addRow(QString(), sync);
	form->addRow(tr("Vertical size"), verticalSize);
	form->addRow(tr("Vertical framing"), verticalFit);

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
	config().vertical_crop = verticalFit->currentData().toBool();

	const QSize portrait = verticalSize->currentData().toSize();
	config().vertical_width = portrait.width();
	config().vertical_height = portrait.height();

	save_config();

	// Takes effect now, so the preview in the canvas dock reshapes as soon as
	// this closes rather than at the next stream.
	vertical_resize();

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

	// By value: QJsonArray's iterator returns a prvalue, so a const reference
	// here binds to a temporary. Harmless in practice and clang rejects it
	// anyway, which under the CI presets means the build fails on macOS only.
	for (const QJsonValue value : incoming) {
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
	refreshRecording();

	const std::vector<OutputStatus> statuses = output_statuses();

	for (int row = 0; row < table_->rowCount(); row++) {
		if (row >= static_cast<int>(config().destinations.size())) {
			break;
		}

		const Destination &destination = config().destinations[static_cast<size_t>(row)];
		QTableWidgetItem *item = table_->item(row, 3);

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
			const bool ready = !destination.key.empty() && destination.enabled;

			item->setText(destination.key.empty()  ? tr("No stream key yet")
				      : !destination.enabled ? tr("Off")
							     : tr("Ready"));
			item->setData(RowDelegate::StateRole,
				      static_cast<int>(ready ? RowState::Ready
							     : RowState::Unconfigured));
			item->setToolTip(QString());
			continue;
		}

		switch (found->state) {
		case OutputState::Starting:
			item->setText(tr("Connecting"));
			item->setData(RowDelegate::StateRole, static_cast<int>(RowState::Starting));
			item->setToolTip(QString());
			break;

		case OutputState::Live: {
			// The measured rate and the uptime, because "Live" on its own is
			// the one thing a streamer can already see. A destination can be
			// live and still be losing a tenth of its frames to an upload
			// that cannot carry it, and that is the answer to "why does my
			// stream look bad" that a status word never gives.
			const double dropped = found->drop_percent();

			QString text = tr("%1 kbps").arg(found->bitrate_kbps);

			if (found->uptime_sec > 0) {
				text += QStringLiteral("  %1").arg(short_uptime(found->uptime_sec));
			}

			if (dropped >= 0.05) {
				text += tr("  %1% dropped").arg(dropped, 0, 'f', 1);
			}

			item->setText(text);

			// Half a percent is worth seeing; a couple is worth acting on.
			item->setData(RowDelegate::StateRole,
				      static_cast<int>(dropped >= 0.5 ? RowState::Dropping
								      : RowState::Live));

			item->setToolTip(tr("Up %1, %2 of %3 frames dropped, %4 reconnect(s)")
						 .arg(uptimeText(found->uptime_sec))
						 .arg(found->dropped_frames)
						 .arg(found->total_frames)
						 .arg(found->reconnects));
			break;
		}

		case OutputState::Failed:
			// The reason, not the word: the reason is the thing somebody can
			// act on, and there is room for it now the columns are merged.
			item->setText(found->detail.empty()
					      ? tr("Failed")
					      : QString::fromStdString(found->detail));
			item->setData(RowDelegate::StateRole, static_cast<int>(RowState::Failed));
			item->setToolTip(QString::fromStdString(found->detail));
			break;

		default:
			item->setText(tr("Stopped"));
			item->setData(RowDelegate::StateRole, static_cast<int>(RowState::Ready));
			item->setToolTip(QString());
			break;
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
