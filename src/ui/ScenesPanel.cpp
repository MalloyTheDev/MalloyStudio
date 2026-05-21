#include "ScenesPanel.h"
#include "model/SceneCollection.h"
#include "model/Scene.h"

#include <QLabel>
#include <QListWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QShortcut>
#include <QKeySequence>

ScenesPanel::ScenesPanel(SceneCollection* scenes, QWidget* parent)
    : QWidget(parent), m_scenes(scenes)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    // Empty-state hint shown when the project has zero scenes (Tier 3 polish).
    m_emptyLabel = new QLabel(tr("No scenes yet — click + to add one."), this);
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setWordWrap(true);
    m_emptyLabel->setStyleSheet(QStringLiteral("color: #70737a; padding: 24px;"));
    m_emptyLabel->setVisible(false);
    layout->addWidget(m_emptyLabel);

    m_list = new QListWidget(this);
    m_list->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    layout->addWidget(m_list, 1);

    auto* btnRow = new QHBoxLayout();
    auto* addBtn    = new QPushButton(QStringLiteral("+"), this);
    auto* removeBtn = new QPushButton(QString::fromUtf8("\xE2\x88\x92"), this); // minus sign
    auto* renameBtn = new QPushButton(tr("Rename"), this);
    addBtn->setToolTip(tr("Add Scene"));
    removeBtn->setToolTip(tr("Remove Scene"));
    renameBtn->setToolTip(tr("Rename Scene (F2)"));
    addBtn->setFixedWidth(28);
    removeBtn->setFixedWidth(28);
    btnRow->addWidget(addBtn);
    btnRow->addWidget(removeBtn);
    btnRow->addWidget(renameBtn);
    btnRow->addStretch();
    layout->addLayout(btnRow);

    connect(addBtn,    &QPushButton::clicked, this, &ScenesPanel::onAddClicked);
    connect(removeBtn, &QPushButton::clicked, this, &ScenesPanel::onRemoveClicked);
    connect(renameBtn, &QPushButton::clicked, this, &ScenesPanel::onRenameClicked);
    connect(m_list, &QListWidget::itemChanged,         this, &ScenesPanel::onItemChanged);
    connect(m_list, &QListWidget::itemSelectionChanged, this, &ScenesPanel::onSelectionChanged);

    auto* f2 = new QShortcut(QKeySequence(Qt::Key_F2), this);
    connect(f2, &QShortcut::activated, this, &ScenesPanel::onRenameClicked);

    connect(m_scenes, &SceneCollection::sceneAdded,    this, [this](int){ rebuild(); });
    connect(m_scenes, &SceneCollection::sceneRemoved,  this, [this](int){ rebuild(); });
    connect(m_scenes, &SceneCollection::sceneRenamed,  this, [this](int){ rebuild(); });
    connect(m_scenes, &SceneCollection::collectionReset, this, &ScenesPanel::rebuild);
    connect(m_scenes, &SceneCollection::currentChanged, this, &ScenesPanel::onCurrentSceneChanged);

    rebuild();
}

void ScenesPanel::rebuild() {
    m_updating = true;
    m_list->clear();
    for (int i = 0; i < m_scenes->sceneCount(); ++i) {
        auto* item = new QListWidgetItem(m_scenes->sceneAt(i)->name());
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        m_list->addItem(item);
    }
    const int cur = m_scenes->currentIndex();
    if (cur >= 0 && cur < m_list->count()) {
        m_list->setCurrentRow(cur);
    }
    // Empty-state visibility — show the helpful label only when there are no
    // scenes; otherwise hide it and let the list take the panel.
    const bool empty = m_list->count() == 0;
    if (m_emptyLabel) m_emptyLabel->setVisible(empty);
    m_list->setVisible(!empty);
    m_updating = false;
}

void ScenesPanel::onAddClicked() {
    m_scenes->addScene();
}

void ScenesPanel::onRemoveClicked() {
    const int row = m_list->currentRow();
    if (row >= 0) m_scenes->removeSceneAt(row);
}

void ScenesPanel::onRenameClicked() {
    if (auto* item = m_list->currentItem()) m_list->editItem(item);
}

void ScenesPanel::onItemChanged(QListWidgetItem* item) {
    if (m_updating) return;
    const int row = m_list->row(item);
    m_scenes->renameSceneAt(row, item->text());
}

void ScenesPanel::onSelectionChanged() {
    if (m_updating) return;
    m_scenes->setCurrentIndex(m_list->currentRow());
}

void ScenesPanel::onCurrentSceneChanged(int index) {
    if (m_list->currentRow() != index) {
        m_updating = true;
        m_list->setCurrentRow(index);
        m_updating = false;
    }
}
