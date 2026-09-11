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

#include "canvases.hpp"

#include <string>

#include "appearance.hpp"
#include "config.hpp"
#include "outputs.hpp"
#include "vertical.hpp"

#include <QComboBox>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QResizeEvent>
#include <QDockWidget>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <obs-frontend-api.h>
#include <obs-module.h>

namespace plasmastream {

namespace {

/* A heading over each pane, in the site's muted weight rather than a bold label,
 * because the picture underneath is the thing worth looking at. */
QLabel *caption(const QString &text, QWidget *parent)
{
	auto *label = new QLabel(text, parent);

	QFont font = label->font();
	font.setPointSizeF(font.pointSizeF() * 0.92);
	font.setLetterSpacing(QFont::PercentageSpacing, 104);
	label->setFont(font);

	QPalette palette = label->palette();
	palette.setColor(QPalette::WindowText, color::muted());
	label->setPalette(palette);

	return label;
}

/* The arrangements worth having as one click.
 *
 * A band is the programme fitted to the full width, which for a 16:9 picture in
 * a 9:16 frame is a little under a third of the height, leaving the rest for
 * whatever you put around it. The exact height is worked out when it is chosen,
 * because it depends on both aspects and neither is ours to assume. */
enum class Placement {
	Fill,
	BandTop,
	BandMiddle,
	BandBottom,
	Custom,
};

/* How tall a full-width band of the programme is, as a fraction of the canvas. */
double band_height()
{
	obs_video_info ovi = {};

	if (!obs_get_video_info(&ovi) || ovi.base_width == 0 || ovi.base_height == 0) {
		return 0.5;
	}

	const double program = static_cast<double>(ovi.base_height) / ovi.base_width;
	const double canvas = static_cast<double>(config().vertical_height) /
			      qMax(1, config().vertical_width);

	return qBound(0.05, program / canvas, 1.0);
}

Framing placement_framing(Placement placement, const std::string &scene)
{
	Framing framing = framing_for(scene);
	framing.scene = scene;

	if (placement == Placement::Custom) {
		return framing;
	}

	if (placement == Placement::Fill) {
		framing.x = 0.0;
		framing.y = 0.0;
		framing.width = 1.0;
		framing.height = 1.0;
		framing.crop = true;
		return framing;
	}

	const double height = band_height();

	framing.x = 0.0;
	framing.width = 1.0;
	framing.height = height;
	framing.crop = false;

	switch (placement) {
	case Placement::BandTop:
		framing.y = 0.0;
		break;
	case Placement::BandMiddle:
		framing.y = (1.0 - height) / 2.0;
		break;
	default:
		framing.y = 1.0 - height;
		break;
	}

	return framing;
}

/* Which of the presets a stored framing matches, so reopening the dock shows the
 * choice that was made rather than always saying Custom. */
Placement placement_of(const Framing &framing)
{
	const auto near = [](double a, double b) { return qAbs(a - b) < 0.01; };

	if (framing.crop && near(framing.x, 0.0) && near(framing.y, 0.0) &&
	    near(framing.width, 1.0) && near(framing.height, 1.0)) {
		return Placement::Fill;
	}

	if (!framing.crop && near(framing.x, 0.0) && near(framing.width, 1.0) &&
	    near(framing.height, band_height())) {
		if (near(framing.y, 0.0)) {
			return Placement::BandTop;
		}

		if (near(framing.y, (1.0 - framing.height) / 2.0)) {
			return Placement::BandMiddle;
		}

		if (near(framing.y, 1.0 - framing.height)) {
			return Placement::BandBottom;
		}
	}

	return Placement::Custom;
}

/* Every source in the collection that could sensibly go on the tall frame.
 *
 * Scenes are left out. Putting a scene on the vertical canvas is how you get a
 * loop, since the programme scene is already there, and it is never what
 * somebody means when they are looking for their camera. */
bool collect_source(void *param, obs_source_t *source)
{
	auto *names = static_cast<QStringList *>(param);

	if (obs_source_is_scene(source)) {
		return true;
	}

	/* Inputs only: filters and transitions are sources too, and neither is a
	 * thing you place on a frame. */
	if ((obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) == 0) {
		return true;
	}

	const char *name = obs_source_get_name(source);

	if (name && *name) {
		names->append(QString::fromUtf8(name));
	}

	return true;
}

} // namespace

CanvasDock::CanvasDock(QWidget *parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("PlasmaStreamCanvasDock"));

	main_ = new Preview(this);
	vertical_ = new Preview(this);

#if PLASMASTREAM_HAS_CANVAS
	/* Only the tall one. The wide pane shows OBS's own composition, and OBS
	 * already has a preview for editing that. */
	vertical_->setEditable(true);

	connect(vertical_, &Preview::itemMoved, this, [this]() {
		save_config();

		/* A dragged programme block is no longer any of the presets, and the
		 * combo saying otherwise would be a lie about what is on screen. */
		framedScene_.clear();
		follow();
	});
#endif

	verticalNote_ = new QLabel(tr("Nothing vertical is running.\n\nAdd a destination with a "
				      "9:16 frame and it will appear here when you go live."),
				   this);
	verticalNote_->setAlignment(Qt::AlignCenter);
	verticalNote_->setWordWrap(true);
	verticalNote_->setEnabled(false);

	wideCaption_ = caption(tr("Your stream"), this);
	tallCaption_ = caption(tr("Vertical"), this);

	/* Stacked so the note occupies exactly the space the preview will, and the
	 * pane does not jump when a vertical destination starts. */
	tallPane_ = new QStackedWidget(this);
	tallPane_->addWidget(verticalNote_);
	tallPane_->addWidget(vertical_);

	framingFor_ = caption(QString(), this);

	placement_ = new QComboBox(this);
	placement_->addItem(tr("Fill the frame"), static_cast<int>(Placement::Fill));
	placement_->addItem(tr("Band at the top"), static_cast<int>(Placement::BandTop));
	placement_->addItem(tr("Band in the middle"), static_cast<int>(Placement::BandMiddle));
	placement_->addItem(tr("Band at the bottom"), static_cast<int>(Placement::BandBottom));
	placement_->addItem(tr("Custom"), static_cast<int>(Placement::Custom));

	connect(placement_, &QComboBox::activated, this, &CanvasDock::reframe);

	overlayCaption_ = caption(tr("On the vertical frame"), this);

	sources_ = new QListWidget(this);
	sources_->setSelectionMode(QAbstractItemView::SingleSelection);
	sources_->setAlternatingRowColors(true);

	addSource_ = new QPushButton(tr("Add"), this);
	removeSource_ = new QPushButton(tr("Remove"), this);

	/* OBS's own dialogs, not ours. The transform dialog is the one people
	 * already know, it does numbers better than a dock has room for, and
	 * reimplementing it would be building a worse copy of something already
	 * installed. */
	transform_ = new QPushButton(tr("Transform..."), this);
	properties_ = new QPushButton(tr("Properties..."), this);

	connect(addSource_, &QPushButton::clicked, this, &CanvasDock::addSource);
	connect(removeSource_, &QPushButton::clicked, this, &CanvasDock::removeSource);
	connect(transform_, &QPushButton::clicked, this, &CanvasDock::editTransform);
	connect(properties_, &QPushButton::clicked, this, &CanvasDock::editProperties);
	connect(sources_, &QListWidget::itemSelectionChanged, this,
		&CanvasDock::sourceSelectionChanged);

	rebuildSourceList();
	sourceSelectionChanged();

	poll_ = new QTimer(this);
	poll_->setInterval(1000);
	connect(poll_, &QTimer::timeout, this, &CanvasDock::follow);
	poll_->start();

	/* The stack is reached through the widget the note lives in. */
	follow();
	layOut();
}

void CanvasDock::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	layOut();
}

/* Give each pane the shape of the thing inside it, rather than half the dock
 * each.
 *
 * A QHBoxLayout can only hand out width, and both previews letterbox themselves
 * into whatever they are given, so any stretch factor leaves them the same
 * HEIGHT. At that point a 9:16 frame is always the smaller picture, which is
 * backwards: the vertical frame is the reason to open this dock, since OBS
 * already shows the wide one in its own preview.
 *
 * So the vertical pane is given exactly the width 9:16 needs at full height, and
 * the wide one takes what is left and is then only as tall as 16:9 needs at that
 * width. Both end up with no letterboxing at all, and the vertical one is the
 * taller of the two. */
void CanvasDock::layOut()
{
	const int pad = 8;
	const int gap = 12;
	const int capGap = 6;

	const int capH = qMax(wideCaption_->sizeHint().height(),
			      tallCaption_->sizeHint().height());

	const int availW = width() - pad * 2 - gap;
	const int availH = height() - pad * 2 - capH - capGap;

	if (availW <= 0 || availH <= 0) {
		return;
	}

	/* Never more than half, or a short wide dock leaves nothing for the
	 * programme. */
	int tallW = qMin(availH * 9 / 16, availW / 2);
	int tallH = qMin(availH, tallW * 16 / 9);
	tallW = tallH * 9 / 16;

	int wideW = availW - tallW;
	int wideH = wideW * 9 / 16;

	if (wideH > availH) {
		wideH = availH;
		wideW = wideH * 16 / 9;
	}

	const int top = pad + capH + capGap;

	wideCaption_->setGeometry(pad, pad, wideW, capH);
	main_->setGeometry(pad, top + (availH - wideH) / 2, wideW, wideH);

	const int tallX = pad + wideW + gap;
	tallCaption_->setGeometry(tallX, pad, tallW, capH);
	tallPane_->setGeometry(tallX, top, tallW, tallH);

	/* Under the wide pane, which is the shorter of the two and therefore the
	 * one with room beneath it. */
	const int controlsY = top + wideH + (availH - wideH) / 2 + 8;
	const int rowH = placement_->sizeHint().height();

	framingFor_->setGeometry(pad, controlsY, wideW, capH);
	placement_->setGeometry(pad, controlsY + capH + 2, qMin(wideW, 240), rowH);

	/* The overlay list fills whatever is left under the framing row. A dock
	 * dragged short enough loses it rather than squashing everything, since a
	 * two-pixel list is worse than none. */
	const int listY = controlsY + capH + 2 + rowH + 10;
	const int buttonsH = addSource_->sizeHint().height();

	/* Two rows of two. Four across a pane this narrow clipped the longer labels
	 * to "ransform" and "roperties", which reads as a broken widget rather
	 * than a narrow one. */
	const int listH = height() - pad - listY - buttonsH * 2 - 12;

	const bool room = listH >= 30;

	overlayCaption_->setVisible(room);
	sources_->setVisible(room);
	addSource_->setVisible(room);
	removeSource_->setVisible(room);
	transform_->setVisible(room);
	properties_->setVisible(room);

	if (!room) {
		return;
	}

	overlayCaption_->setGeometry(pad, listY - capH - 2, wideW, capH);
	sources_->setGeometry(pad, listY, wideW, listH);

	const int buttonW = (wideW - 6) / 2;
	const int buttonsY = listY + listH + 6;
	const int secondRow = buttonsY + buttonsH + 6;

	addSource_->setGeometry(pad, buttonsY, buttonW, buttonsH);
	removeSource_->setGeometry(pad + buttonW + 6, buttonsY, buttonW, buttonsH);
	transform_->setGeometry(pad, secondRow, buttonW, buttonsH);
	properties_->setGeometry(pad + buttonW + 6, secondRow, buttonW, buttonsH);
}

void CanvasDock::rebuildSourceList()
{
	const std::string chosen = selectedSource();

	sources_->clear();

	for (const VerticalSource &source : config().vertical_sources) {
		sources_->addItem(QString::fromStdString(source.name));

		if (source.name == chosen) {
			sources_->setCurrentRow(sources_->count() - 1);
		}
	}
}

std::string CanvasDock::selectedSource() const
{
	QListWidgetItem *item = sources_->currentItem();
	return item ? item->text().toStdString() : std::string();
}

void CanvasDock::sourceSelectionChanged()
{
	const bool any = sources_->currentItem() != nullptr;

	removeSource_->setEnabled(any);
	transform_->setEnabled(any);
	properties_->setEnabled(any);
}

void CanvasDock::addSource()
{
	QStringList names;
	obs_enum_sources(collect_source, &names);

	/* Already on the frame is already on the frame. */
	for (const VerticalSource &source : config().vertical_sources) {
		names.removeAll(QString::fromStdString(source.name));
	}

	if (names.isEmpty()) {
		QMessageBox::information(
			this, tr("Nothing to add"),
			tr("Every source in this scene collection is already on the vertical "
			   "frame. Make a new one in OBS first, in any scene, and it will turn "
			   "up here."));
		return;
	}

	names.sort(Qt::CaseInsensitive);

	bool chose = false;
	const QString name =
		QInputDialog::getItem(this, tr("Put a source on the vertical frame"),
				      tr("It stays where it is in your scenes as well. This "
					 "only adds it to the tall frame."),
				      names, 0, false, &chose);

	if (!chose || name.isEmpty() || !vertical_add_source(name.toStdString())) {
		return;
	}

	VerticalSource added;
	added.name = name.toStdString();
	config().vertical_sources.push_back(added);

	/* Written back straight away so the placement it just landed at is the one
	 * it comes back to. */
	vertical_capture_layout();
	save_config();
	rebuildSourceList();
	sourceSelectionChanged();
}

void CanvasDock::removeSource()
{
	const std::string name = selectedSource();

	if (name.empty()) {
		return;
	}

	vertical_remove_source(name);

	auto &sources = config().vertical_sources;

	for (size_t i = 0; i < sources.size(); i++) {
		if (sources[i].name == name) {
			sources.erase(sources.begin() + static_cast<long long>(i));
			break;
		}
	}

	save_config();
	rebuildSourceList();
	sourceSelectionChanged();
}

void CanvasDock::editTransform()
{
	obs_sceneitem_t *item = vertical_source_item(selectedSource());

	if (!item) {
		return;
	}

	obs_frontend_open_sceneitem_edit_transform(item);

	/* That dialog is modeless, so there is no moment afterwards to read from.
	 * The poll below captures whatever it did, once a second, which is often
	 * enough for something being dragged by hand. */
}

void CanvasDock::editProperties()
{
	obs_sceneitem_t *item = vertical_source_item(selectedSource());

	if (!item) {
		return;
	}

	obs_source_t *source = obs_sceneitem_get_source(item);

	if (source) {
		obs_frontend_open_source_properties(source);
	}
}

void CanvasDock::reframe()
{
	const std::string scene = vertical_scene();

	if (scene.empty()) {
		return;
	}

	const auto placement = static_cast<Placement>(placement_->currentData().toInt());

	set_framing(placement_framing(placement, scene));
	save_config();
	vertical_reframe();
}

void CanvasDock::follow()
{
	QStackedWidget *stack = tallPane_;

	if (!stack) {
		return;
	}

#if PLASMASTREAM_HAS_CANVAS
	/* Only asked for once something is configured to want it, so opening this
	 * dock on a channel that never streams vertical does not build a canvas
	 * nobody looks at. */
	bool wanted = false;

	for (const Destination &destination : config().destinations) {
		if (destination.vertical) {
			wanted = true;
			break;
		}
	}

	/* Borrowed: vertical.cpp owns it for the life of the module, so there is
	 * nothing here to release. */
	obs_canvas_t *canvas = wanted ? vertical_canvas() : nullptr;

	if (canvas && !watching_) {
		vertical_->watch(canvas);
		stack->setCurrentWidget(vertical_);
		watching_ = true;
	} else if (!canvas && watching_) {
		vertical_->watch(nullptr);
		stack->setCurrentWidget(verticalNote_);
		watching_ = false;
	}

	/* OBS's transform dialog has no callback and no modal moment to read after,
	 * so whatever it did is picked up here. Cheap: a handful of items, and the
	 * write only lands in memory unless something actually moved. */
	if (canvas) {
		vertical_capture_layout();
	}

	placement_->setEnabled(canvas != nullptr);
	addSource_->setEnabled(canvas != nullptr);
	framingFor_->setVisible(canvas != nullptr);

	/* Only rewritten when programme moves under us, so a choice being made does
	 * not fight the poll that is about to run. */
	const std::string scene = canvas ? vertical_scene() : std::string();

	if (scene != framedScene_) {
		framedScene_ = scene;

		framingFor_->setText(scene.empty()
					     ? QString()
					     : tr("Framing for %1")
						       .arg(QString::fromStdString(scene)));

		const int index = placement_->findData(
			static_cast<int>(placement_of(framing_for(scene))));

		if (index >= 0) {
			placement_->setCurrentIndex(index);
		}
	}
#else
	stack->setCurrentWidget(verticalNote_);
#endif
}

void register_canvas_dock()
{
	auto *main_window = static_cast<QWidget *>(obs_frontend_get_main_window());

	if (!main_window) {
		return;
	}

	auto *dock = new CanvasDock(main_window);
	dock->setWindowTitle(QObject::tr("PlasmaStream Canvases"));

	obs_frontend_add_dock_by_id("plasmastream_canvases", "PlasmaStream Canvases", dock);

	if (config().canvases_introduced) {
		return;
	}

	/* Shown once, the first time this plugin ever runs, because OBS starts every
	 * dock hidden and the Docks menu is its own top-level menu that people look
	 * for inside View. A panel nobody can find is a panel nobody has.
	 *
	 * Once only, and nothing else touched: no floating, no size, no position.
	 * Whatever is done with it after this is remembered by OBS and never
	 * overridden again.
	 *
	 * Deferred because OBS restores its saved dock layout after modules load,
	 * and anything set before that is simply undone. */
	QTimer::singleShot(3000, dock, [dock]() {
		for (QWidget *w = dock->parentWidget(); w; w = w->parentWidget()) {
			auto *docked = qobject_cast<QDockWidget *>(w);

			if (!docked) {
				continue;
			}

			docked->setVisible(true);
			config().canvases_introduced = true;
			save_config();
			break;
		}
	});
}

} // namespace plasmastream
