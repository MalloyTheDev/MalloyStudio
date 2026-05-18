#include "SourcesPanel.h"
#include "MonitorPickerDialog.h"
#include "model/SceneCollection.h"
#include "model/Scene.h"
#include "model/SceneItem.h"
#include "model/Source.h"

#include <QListWidget>
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
#include <QFileDialog>

SourcesPanel::SourcesPanel(SceneCollection* scenes, QWidget* parent)
    : QWidget(parent), m_scenes(scenes)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
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
        m_updating = false;
        return;
    }

    for (int i = 0; i < current->itemCount(); ++i) {
        auto* listItem = new QListWidgetItem();
        listItem->setData(Qt::UserRole, i);
        listItem->setSizeHint(QSize(240, 38));
        m_list->addItem(listItem);
        m_list->setItemWidget(listItem, createLayerRow(i));
    }

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
    type->setStyleSheet(QStringLiteral("color: #9a9a9a;"));

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
    typeCombo->addItem(tr("Image"),           static_cast<int>(Source::Type::Image));
    typeCombo->addItem(tr("Text"),            static_cast<int>(Source::Type::Text));
    typeCombo->addItem(tr("Color Block"),     static_cast<int>(Source::Type::ColorBlock));
    typeCombo->addItem(tr("Browser"),         static_cast<int>(Source::Type::Browser));

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

    m_scenes->addNewSourceToCurrent(name, t, textValue, colorValue, adapterIndex, outputIndex);

    if (t == Source::Type::Image && !imagePathValue.isEmpty()) {
        const int idx = m_scenes->currentItemIndex();
        if (idx >= 0) m_scenes->setCurrentSourceImagePath(idx, imagePathValue);
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
