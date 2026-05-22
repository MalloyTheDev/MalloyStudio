#include "SourcesPanel.h"
#include "MicrophonePickerDialog.h"
#include "MonitorPickerDialog.h"
#include "WindowPickerDialog.h"
#include "audio/AudioController.h"
#include "model/SceneCollection.h"
#include "model/Scene.h"
#include "model/SceneItem.h"
#include "model/Source.h"
#include "capture/CameraCapture.h"

#include <QAbstractItemModel>
#include <QListWidget>
#include <QMenu>
#include <QShortcut>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QToolButton>
#include <QCheckBox>
#include <QDialog>
#include <QComboBox>
#include <QLineEdit>
#include <QLabel>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QColorDialog>
#include <QMessageBox>
#include <QFileDialog>

SourcesPanel::SourcesPanel(SceneCollection* scenes, AudioController* audio, QWidget* parent)
    : QWidget(parent), m_scenes(scenes), m_audio(audio)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    // Empty-state placeholder (shown only when the current scene has zero
    // layers). Tier 3 polish — hidden during normal operation.
    m_emptyLabel = new QLabel(tr("No sources in this scene — click + to add one."), this);
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setWordWrap(true);
    m_emptyLabel->setStyleSheet(QStringLiteral("color: #70737a; padding: 24px;"));
    m_emptyLabel->setVisible(false);
    layout->addWidget(m_emptyLabel);

    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    // v7: drag-to-reorder. InternalMove uses the existing model for both
    // source and destination so Qt handles the drag visuals; we listen for
    // rowsMoved below and mirror the change in SceneCollection.
    m_list->setDragDropMode(QAbstractItemView::InternalMove);
    m_list->setDefaultDropAction(Qt::MoveAction);
    layout->addWidget(m_list, 1);

    auto* btnRow = new QHBoxLayout();
    auto* addBtn    = new QPushButton(QStringLiteral("+"), this);
    auto* removeBtn = new QPushButton(QString::fromUtf8("\xE2\x88\x92"), this);
    auto* dupBtn    = new QPushButton(tr("Dup"), this);
    auto* upBtn     = new QPushButton(QString::fromUtf8("\xE2\x86\x91"), this);
    auto* downBtn   = new QPushButton(QString::fromUtf8("\xE2\x86\x93"), this);
    addBtn->setToolTip(tr("Add Layer"));
    removeBtn->setToolTip(tr("Remove Layer"));
    dupBtn->setToolTip(tr("Duplicate Layer"));
    upBtn->setToolTip(tr("Move Toward Front"));
    downBtn->setToolTip(tr("Move Toward Back"));
    addBtn->setFixedWidth(28);
    removeBtn->setFixedWidth(28);
    dupBtn->setFixedWidth(44);
    upBtn->setFixedWidth(28);
    downBtn->setFixedWidth(28);
    btnRow->addWidget(addBtn);
    btnRow->addWidget(removeBtn);
    btnRow->addWidget(dupBtn);
    btnRow->addWidget(upBtn);
    btnRow->addWidget(downBtn);
    btnRow->addStretch();
    layout->addLayout(btnRow);

    connect(addBtn,    &QPushButton::clicked, this, &SourcesPanel::onAddClicked);
    connect(removeBtn, &QPushButton::clicked, this, &SourcesPanel::onRemoveClicked);
    connect(dupBtn,    &QPushButton::clicked, this, &SourcesPanel::onDuplicateClicked);
    connect(upBtn,     &QPushButton::clicked, this, &SourcesPanel::onMoveUp);
    connect(downBtn,   &QPushButton::clicked, this, &SourcesPanel::onMoveDown);
    connect(m_list, &QListWidget::itemSelectionChanged, this, &SourcesPanel::onSelectionChanged);
    connect(m_list, &QListWidget::itemDoubleClicked, this, &SourcesPanel::onItemDoubleClicked);

    // Right-click context menu (mirrors the buttons; the design's ⋯ overflow).
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QMenu menu(this);
        menu.addAction(tr("Add Source…"), this, &SourcesPanel::onAddClicked);
        if (QListWidgetItem* item = m_list->itemAt(pos)) {
            m_list->setCurrentItem(item);
            menu.addAction(tr("Rename"), this, [this, item] { onItemDoubleClicked(item); });
            menu.addAction(tr("Duplicate"), this, &SourcesPanel::onDuplicateClicked);
            menu.addSeparator();
            menu.addAction(tr("Move Toward Front"), this, &SourcesPanel::onMoveUp);
            menu.addAction(tr("Move Toward Back"),  this, &SourcesPanel::onMoveDown);
            menu.addSeparator();
            menu.addAction(tr("Remove"), this, &SourcesPanel::onRemoveClicked);
        }
        menu.exec(m_list->viewport()->mapToGlobal(pos));
    });

    // F2: rename the currently selected layer (mirrors ScenesPanel's F2 behavior).
    auto* f2 = new QShortcut(QKeySequence(Qt::Key_F2), this);
    connect(f2, &QShortcut::activated, this, [this] {
        if (auto* item = m_list->currentItem()) onItemDoubleClicked(item);
    });

    // Drag-to-reorder: Qt's InternalMove handles the visuals; we mirror the
    // model change here. Updating during a model-side rebuild would recurse,
    // so we guard with m_updating (same flag the rebuild path uses).
    connect(m_list->model(), &QAbstractItemModel::rowsMoved, this,
            [this](const QModelIndex&, int sourceStart, int /*sourceEnd*/,
                   const QModelIndex&, int destinationRow) {
        if (m_updating) return;
        // Qt's rowsMoved destinationRow is the index where the row will land
        // BEFORE the move is finalized. For an internal move toward a higher
        // index, that means "destinationRow - 1" in our model's terms because
        // the source row will have been removed from its original position.
        int from = sourceStart;
        int to   = destinationRow > sourceStart ? destinationRow - 1
                                                 : destinationRow;
        if (from != to)
            m_scenes->moveCurrentItem(from, to);
        // Always rebuild so the row widgets reattach correctly — Qt's
        // internal-move detaches and reattaches itemWidget() pointers in
        // ways that can leave stale layouts otherwise.
        rebuild();
    });

    connect(m_scenes, &SceneCollection::currentChanged,  this, [this](int){ rebuild(); });
    connect(m_scenes, &SceneCollection::itemsChanged,    this, &SourcesPanel::rebuild);
    connect(m_scenes, &SceneCollection::sourcesChanged,  this, &SourcesPanel::rebuild);
    connect(m_scenes, &SceneCollection::collectionReset, this, &SourcesPanel::rebuild);
    connect(m_scenes, &SceneCollection::itemSelectionChanged, this, &SourcesPanel::onCurrentItemChanged);

    rebuild();
}

void SourcesPanel::rebuild() {
    m_updating = true;
    m_list->clear();
    Scene* current = m_scenes->currentScene();
    if (!current) {
        // No scene at all (truly fresh project). Both the list and the
        // empty-state get hidden — ScenesPanel's empty-state handles this
        // case at a higher level.
        if (m_emptyLabel) m_emptyLabel->setVisible(false);
        m_list->setVisible(false);
        m_updating = false;
        return;
    }
    m_list->setVisible(true);

    for (int i = 0; i < current->itemCount(); ++i) {
        auto* listItem = new QListWidgetItem();
        listItem->setData(Qt::UserRole, i);
        listItem->setSizeHint(QSize(240, 38));
        m_list->addItem(listItem);
        m_list->setItemWidget(listItem, createLayerRow(i));
    }

    // Empty-state polish: show the helpful label only when the current scene
    // has zero items; otherwise hide it and let the list take the full panel.
    const bool empty = current->itemCount() == 0;
    if (m_emptyLabel) m_emptyLabel->setVisible(empty);
    m_list->setVisible(!empty);

    const int selected = current->selectedIndex();
    if (selected >= 0 && selected < m_list->count()) {
        m_list->setCurrentRow(selected);
    }
    m_updating = false;
}

QWidget* SourcesPanel::createLayerRow(int index) {
    auto* row = new QWidget(m_list);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(6);

    SceneItem* item = m_scenes->currentScene()->itemAt(index);
    Source* source = m_scenes->sourceForItem(item);
    if (!source) return row;

    auto* visible = new QCheckBox(row);
    visible->setToolTip(tr("Visible"));
    visible->setChecked(item->isVisible());
    visible->setFixedWidth(22);
    visible->setProperty("layerIndex", index);

    auto* lock = new QToolButton(row);
    lock->setText(QStringLiteral("L"));
    lock->setToolTip(tr("Lock Layer"));
    lock->setCheckable(true);
    lock->setChecked(item->isLocked());
    lock->setFixedWidth(24);
    lock->setProperty("layerIndex", index);

    auto* name = new QLabel(source->name(), row);
    name->setMinimumWidth(80);
    name->setTextInteractionFlags(Qt::NoTextInteraction);

    auto* type = new QLabel(Source::typeToString(source->type()), row);
    type->setStyleSheet(QStringLiteral("color: #70737a;"));

    if (source->type() == Source::Type::DisplayCapture && source->hasMonitorConfig()) {
        type->setText(type->text() + QStringLiteral("  LIVE-ready"));
    }

    layout->addWidget(visible);
    layout->addWidget(lock);
    layout->addWidget(name, 1);
    layout->addWidget(type);

    connect(visible, &QCheckBox::toggled, this, [this, visible](bool checked) {
        m_scenes->setCurrentItemVisible(visible->property("layerIndex").toInt(), checked);
    });
    connect(lock, &QToolButton::toggled, this, [this, lock](bool checked) {
        m_scenes->setCurrentItemLocked(lock->property("layerIndex").toInt(), checked);
    });

    return row;
}

void SourcesPanel::onAddClicked() {
    Scene* current = m_scenes->currentScene();
    if (!current) return;

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Add Source"));

    auto* modeCombo = new QComboBox(&dlg);
    modeCombo->addItem(tr("Create New Source"), 0);
    if (m_scenes->sourceCount() > 0) {
        modeCombo->addItem(tr("Use Existing Source"), 1);
    }

    auto* existingCombo = new QComboBox(&dlg);
    for (Source* source : m_scenes->sources()) {
        existingCombo->addItem(
            tr("%1 (%2, %3 layers)")
                .arg(source->name())
                .arg(Source::typeToString(source->type()))
                .arg(m_scenes->sourceReferenceCount(source->id())),
            source->id());
    }

    auto* typeCombo = new QComboBox(&dlg);
    typeCombo->addItem(tr("Display Capture"), static_cast<int>(Source::Type::DisplayCapture));
    // v7: WindowCapture and Microphone (AudioInput) used to be missing from
    // the Add dialog even though the model layer supported them. Users could
    // only get them by hand-editing project JSON.
    typeCombo->addItem(tr("Window Capture"),  static_cast<int>(Source::Type::WindowCapture));
    typeCombo->addItem(tr("Image"),           static_cast<int>(Source::Type::Image));
    typeCombo->addItem(tr("Text"),            static_cast<int>(Source::Type::Text));
    typeCombo->addItem(tr("Color Block"),     static_cast<int>(Source::Type::ColorBlock));
    typeCombo->addItem(tr("Browser"),         static_cast<int>(Source::Type::Browser));
    typeCombo->addItem(tr("Microphone"),      static_cast<int>(Source::Type::AudioInput));
    typeCombo->addItem(tr("Camera"),          static_cast<int>(Source::Type::Camera));

    auto* nameEdit = new QLineEdit(&dlg);
    auto updateDefaultName = [&] {
        const QString typeText = typeCombo->currentText();
        int n = 1;
        for (int i = 0; i < current->itemCount(); ++i) {
            Source* source = m_scenes->sourceForItem(current->itemAt(i));
            if (source && Source::typeToString(source->type()) == typeText) ++n;
        }
        nameEdit->setText(QString("%1 %2").arg(typeText).arg(n));
    };
    updateDefaultName();
    QObject::connect(typeCombo, &QComboBox::currentIndexChanged, &dlg,
                     [updateDefaultName](int){ updateDefaultName(); });

    auto* form = new QFormLayout();
    form->addRow(tr("Mode:"), modeCombo);
    form->addRow(tr("Existing:"), existingCombo);
    form->addRow(tr("Type:"), typeCombo);
    form->addRow(tr("Name:"), nameEdit);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    auto* dlgLayout = new QVBoxLayout(&dlg);
    dlgLayout->addLayout(form);
    dlgLayout->addWidget(buttons);

    auto updateMode = [&] {
        const bool useExisting = modeCombo->currentData().toInt() == 1;
        existingCombo->setEnabled(useExisting && existingCombo->count() > 0);
        typeCombo->setEnabled(!useExisting);
        nameEdit->setEnabled(!useExisting);
    };
    updateMode();
    QObject::connect(modeCombo, &QComboBox::currentIndexChanged, &dlg,
                     [&updateMode](int){ updateMode(); });

    if (dlg.exec() != QDialog::Accepted) return;

    if (modeCombo->currentData().toInt() == 1) {
        m_scenes->addExistingSourceToCurrent(existingCombo->currentData().toInt());
        return;
    }

    const auto t = static_cast<Source::Type>(typeCombo->currentData().toInt());
    QString name = nameEdit->text().trimmed();
    if (name.isEmpty()) name = Source::typeToString(t);

    // ── AudioInput / "Microphone" branches off the generic addNewSourceToCurrent
    // path because the model already has a dedicated helper that takes the
    // WASAPI device GUID alongside the friendly name.
    if (t == Source::Type::AudioInput) {
        const auto pick = MicrophonePickerDialog::pick(m_audio, this);
        if (pick.first.isEmpty()) return;   // user cancelled or no devices
        // If the user kept the auto-generated default name ("Microphone 1"),
        // promote the friendly device name. If they typed something custom,
        // keep their choice.
        QString resolved = name;
        if (resolved.startsWith(tr("Microphone")) ||
            resolved.startsWith(tr("Audio Input")))
            resolved = pick.second;
        m_scenes->addAudioInputToCurrent(resolved, pick.first);
        return;
    }

    // ── Camera: pick a MediaFoundation video device up-front (like Window).
    if (t == Source::Type::Camera) {
        const QList<CameraCapture::Device> cams = CameraCapture::availableDevices();
        if (cams.isEmpty()) {
            QMessageBox::information(this, tr("No Cameras"),
                tr("No webcam or capture device was found."));
            return;
        }
        QStringList names;
        for (const CameraCapture::Device& c : cams) names << c.name;
        bool ok = false;
        const QString chosen = QInputDialog::getItem(
            this, tr("Choose Camera"), tr("Device:"), names, 0, false, &ok);
        if (!ok) return;
        const CameraCapture::Device dev = cams.at(qMax(0, names.indexOf(chosen)));
        QString resolved = name;
        if (resolved.startsWith(tr("Camera"))) resolved = dev.name;
        m_scenes->addCameraToCurrent(resolved, dev.id, dev.name);
        return;
    }

    QString textValue;
    QColor colorValue;
    int adapterIndex = -1;
    int outputIndex = 0;

    if (t == Source::Type::Text) {
        textValue = name;
    } else if (t == Source::Type::ColorBlock) {
        const QColor color = QColorDialog::getColor(QColor(46, 136, 255), this, tr("Choose Color Block Color"));
        if (color.isValid()) colorValue = color;
    }

    QString imagePathValue;
    if (t == Source::Type::Image) {
        imagePathValue = QFileDialog::getOpenFileName(
            this, tr("Choose Image File"), QString(),
            tr("Image Files (*.png *.jpg *.jpeg *.bmp *.gif *.webp);;All Files (*)"));
        // Empty is fine — user can set/change later via the Inspector
    }

    if (t == Source::Type::DisplayCapture) {
        MonitorPickerDialog picker(this);
        if (picker.exec() == QDialog::Accepted) {
            const MonitorInfo mi = picker.selectedMonitor();
            if (mi.adapterIndex >= 0) {
                adapterIndex = mi.adapterIndex;
                outputIndex = mi.outputIndex;
            }
        }
    }

    // For WindowCapture we pop the picker before creating the source. If the
    // user cancels the picker we don't create anything — same UX as the
    // existing DisplayCapture path (where cancelling the monitor picker
    // leaves adapterIndex = -1 and the source ends up unconfigured; for
    // WindowCapture an unconfigured source has no useful behavior, so we
    // require a picked window up-front).
    std::optional<std::pair<quintptr, QString>> pickedWindow;
    if (t == Source::Type::WindowCapture) {
        pickedWindow = WindowPickerDialog::pickWindow(this);
        if (!pickedWindow.has_value()) return;
    }

    m_scenes->addNewSourceToCurrent(name, t, textValue, colorValue, adapterIndex, outputIndex);

    if (t == Source::Type::Image && !imagePathValue.isEmpty()) {
        const int idx = m_scenes->currentItemIndex();
        if (idx >= 0) m_scenes->setCurrentSourceImagePath(idx, imagePathValue);
    }
    if (t == Source::Type::WindowCapture && pickedWindow.has_value()) {
        const int idx = m_scenes->currentItemIndex();
        if (idx >= 0)
            m_scenes->setCurrentSourceWindow(idx, pickedWindow->first, pickedWindow->second);
    }
}

void SourcesPanel::onRemoveClicked() {
    const int row = m_list->currentRow();
    if (row >= 0) m_scenes->removeCurrentItemAt(row);
}

void SourcesPanel::onDuplicateClicked() {
    const int row = m_list->currentRow();
    if (row >= 0) m_scenes->duplicateCurrentItemAt(row);
}

void SourcesPanel::onMoveUp() {
    const int row = m_list->currentRow();
    if (row > 0) {
        m_scenes->moveCurrentItem(row, row - 1);
        m_list->setCurrentRow(row - 1);
    }
}

void SourcesPanel::onMoveDown() {
    Scene* current = m_scenes->currentScene();
    if (!current) return;
    const int row = m_list->currentRow();
    if (row >= 0 && row < current->itemCount() - 1) {
        m_scenes->moveCurrentItem(row, row + 1);
        m_list->setCurrentRow(row + 1);
    }
}

void SourcesPanel::onSelectionChanged() {
    if (m_updating) return;
    m_scenes->selectCurrentItemAt(m_list->currentRow());
}

void SourcesPanel::onItemDoubleClicked(QListWidgetItem* item) {
    if (!item) return;
    const int row = m_list->row(item);
    SceneItem* sceneItem = m_scenes->currentScene() ? m_scenes->currentScene()->itemAt(row) : nullptr;
    Source* source = m_scenes->sourceForItem(sceneItem);
    if (!source) return;

    bool ok = false;
    const QString next = QInputDialog::getText(this, tr("Rename Layer"), tr("Name:"),
                                               QLineEdit::Normal, source->name(), &ok).trimmed();
    if (ok && !next.isEmpty()) m_scenes->renameCurrentItemAt(row, next);
}

void SourcesPanel::onCurrentItemChanged(int index) {
    if (m_list->currentRow() == index) return;
    m_updating = true;
    m_list->setCurrentRow(index);
    m_updating = false;
}
