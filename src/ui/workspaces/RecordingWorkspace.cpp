#include "ui/workspaces/RecordingWorkspace.h"
#include "ui/components/PanelFrame.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QVBoxLayout>

namespace {
QFrame* vDivider(QWidget* parent) {
    auto* d = new QFrame(parent);
    d->setObjectName(QStringLiteral("divider"));
    d->setFixedWidth(1);
    return d;
}
QFrame* hDivider(QWidget* parent) {
    auto* d = new QFrame(parent);
    d->setObjectName(QStringLiteral("divider"));
    d->setFixedHeight(1);
    return d;
}
} // namespace

RecordingWorkspace::RecordingWorkspace(QWidget* scenes, QWidget* sources,
                                       QWidget* previewArea, QWidget* controls,
                                       QWidget* mixer, QWidget* inspector,
                                       QWidget* parent)
    : QWidget(parent) {
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);

    // ── Left column: Scenes + Sources ──────────────────────────────────────
    auto* left = new QWidget(this);
    left->setObjectName(QStringLiteral("recSideCol"));
    left->setFixedWidth(280);
    auto* leftV = new QVBoxLayout(left);
    leftV->setContentsMargins(8, 8, 8, 8);
    leftV->setSpacing(8);

    auto* scenesPanel = new PanelFrame(QStringLiteral("Scenes"), QStringLiteral("layers"), left);
    scenesPanel->setContent(scenes);
    leftV->addWidget(scenesPanel, 0);

    auto* sourcesPanel = new PanelFrame(QStringLiteral("Sources"), QStringLiteral("display"), left);
    sourcesPanel->setContent(sources);
    leftV->addWidget(sourcesPanel, 1);

    row->addWidget(left);
    row->addWidget(vDivider(this));

    // ── Center: preview + controls ─────────────────────────────────────────
    auto* center = new QWidget(this);
    auto* centerV = new QVBoxLayout(center);
    centerV->setContentsMargins(0, 0, 0, 0);
    centerV->setSpacing(0);

    auto* previewWrap = new QWidget(center);
    auto* previewV = new QVBoxLayout(previewWrap);
    previewV->setContentsMargins(16, 16, 16, 16);
    previewV->setSpacing(0);
    if (previewArea) previewV->addWidget(previewArea, 1);
    centerV->addWidget(previewWrap, 1);

    centerV->addWidget(hDivider(center));
    if (controls) {
        controls->setParent(center);
        centerV->addWidget(controls, 0);
    }
    row->addWidget(center, 1);

    row->addWidget(vDivider(this));

    // ── Right column: Audio Mixer + Inspector ──────────────────────────────
    auto* right = new QWidget(this);
    right->setObjectName(QStringLiteral("recSideCol"));
    right->setFixedWidth(360);
    auto* rightV = new QVBoxLayout(right);
    rightV->setContentsMargins(8, 8, 8, 8);
    rightV->setSpacing(8);

    auto* mixerPanel = new PanelFrame(QStringLiteral("Audio Mixer"), QStringLiteral("speaker"), right);
    mixerPanel->setContent(mixer);
    rightV->addWidget(mixerPanel, 0);

    auto* inspectorPanel = new PanelFrame(QStringLiteral("Inspector"), QStringLiteral("settings"), right);
    inspectorPanel->setContent(inspector);
    rightV->addWidget(inspectorPanel, 1);

    row->addWidget(right);
}
