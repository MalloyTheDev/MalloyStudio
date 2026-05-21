#include "ui/workspaces/SettingsWorkspace.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"
#include "recording/OutputSettings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace {

QLabel* lbl(const QString& s, const QString& tone = QString(), int px = 13, bool bold = false) {
    auto* l = new QLabel(s);
    if (!tone.isEmpty()) l->setProperty("tone", tone);
    QFont f = l->font(); f.setPixelSize(px); if (bold) f.setWeight(QFont::DemiBold);
    l->setFont(f);
    return l;
}

struct Item { QString label; QString hint; QWidget* control; };

QFrame* settingsBlock(const QString& title, const QVector<Item>& items) {
    auto* holder = new QFrame;
    auto* hv = new QVBoxLayout(holder);
    hv->setContentsMargins(0, 0, 0, 0);
    hv->setSpacing(12);

    auto* h3 = lbl(title.toUpper(), QStringLiteral("mute"), 12, true);
    { QFont f = h3->font(); f.setLetterSpacing(QFont::AbsoluteSpacing, 0.6); h3->setFont(f); }
    hv->addWidget(h3);

    auto* card = new QFrame; card->setObjectName(QStringLiteral("card"));
    auto* cv = new QVBoxLayout(card);
    cv->setContentsMargins(0, 0, 0, 0);
    cv->setSpacing(0);
    for (int i = 0; i < items.size(); ++i) {
        const Item& it = items[i];
        auto* rowW = new QWidget;
        auto* rh = new QHBoxLayout(rowW);
        rh->setContentsMargins(16, 12, 16, 12);
        rh->setSpacing(16);
        auto* tv = new QVBoxLayout; tv->setSpacing(2);
        tv->addWidget(lbl(it.label, QString(), 13, true));
        auto* hint = lbl(it.hint, QStringLiteral("mute"), 11); hint->setWordWrap(true);
        tv->addWidget(hint);
        rh->addLayout(tv, 1);
        if (it.control) rh->addWidget(it.control, 0, Qt::AlignVCenter);
        cv->addWidget(rowW);
        if (i < items.size() - 1) {
            auto* div = new QFrame; div->setObjectName(QStringLiteral("divider")); div->setFixedHeight(1);
            cv->addWidget(div);
        }
    }
    hv->addWidget(card);
    return holder;
}

QComboBox* combo(std::initializer_list<QString> opts, int w = 260) {
    auto* c = new QComboBox;
    for (const QString& o : opts) c->addItem(o);
    c->setFixedWidth(w);
    return c;
}
QLineEdit* line(const QString& text, int w = 260) {
    auto* e = new QLineEdit(text);
    e->setFixedWidth(w);
    return e;
}
QCheckBox* check(bool on) { auto* c = new QCheckBox; c->setChecked(on); return c; }

} // namespace

SettingsWorkspace::SettingsWorkspace(QWidget* parent) : QWidget(parent) {
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);

    // Left group list
    auto* nav = new QListWidget(this);
    nav->setObjectName(QStringLiteral("settingsNav"));
    nav->setFixedWidth(240);
    nav->setFrameShape(QFrame::NoFrame);
    nav->setIconSize(QSize(14, 14));
    struct G { QString label, icon; };
    const QVector<G> groups = {
        {tr("General"), QStringLiteral("settings")}, {tr("Recording"), QStringLiteral("record")},
        {tr("Streaming"), QStringLiteral("stream")}, {tr("Video"), QStringLiteral("editor")},
        {tr("Audio"), QStringLiteral("speaker")},    {tr("Hotkeys"), QStringLiteral("command")},
        {tr("Storage"), QStringLiteral("disk")},     {tr("Performance"), QStringLiteral("cpu")},
        {tr("Appearance"), QStringLiteral("image")}, {tr("Accounts"), QStringLiteral("projects")},
        {tr("Experimental"), QStringLiteral("bolt")},{tr("AI placeholders"), QStringLiteral("ai")},
    };
    for (const G& g : groups) {
        auto* it = new QListWidgetItem(Icons::icon(g.icon, Theme::TextMute, 14), g.label, nav);
        it->setSizeHint(QSize(0, 34));
    }
    auto* navWrap = new QWidget(this);
    navWrap->setObjectName(QStringLiteral("recSideCol"));
    auto* nw = new QVBoxLayout(navWrap);
    nw->setContentsMargins(8, 8, 8, 8);
    nw->addWidget(nav);
    row->addWidget(navWrap);

    // Right content
    m_stack = new QStackedWidget(this);
    for (const G& g : groups)
        m_stack->addWidget(g.label == tr("Recording") ? buildRecordingPage() : buildGenericPage(g.label));
    row->addWidget(m_stack, 1);

    connect(nav, &QListWidget::currentRowChanged, m_stack, &QStackedWidget::setCurrentIndex);
    nav->setCurrentRow(1);  // Recording
}

QWidget* SettingsWorkspace::buildRecordingPage() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("workspaceBody"));
    auto* outer = new QHBoxLayout(page);
    outer->setContentsMargins(24, 24, 24, 24);

    auto* content = new QWidget;
    content->setMaximumWidth(800);
    auto* col = new QVBoxLayout(content);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(24);

    col->addWidget(lbl(tr("Recording"), QString(), 20, true));
    auto* sub = lbl(tr("Encoder, container, quality, and where files are saved."), QStringLiteral("mute"), 13);
    col->addWidget(sub);

    // Controls bound to the real OutputSettings (encoder/codec is left to the
    // proven Edit ▸ Output Settings dialog to avoid touching the ffmpeg call).
    m_crfEdit = line(QStringLiteral("20"), 100);
    m_containerCombo = combo({tr("MKV (Matroska) · recommended"), tr("MP4"), tr("MOV")});
    m_resCombo = combo({QStringLiteral("1920 × 1080"), QStringLiteral("2560 × 1440"), QStringLiteral("3840 × 2160")});
    m_fpsCombo = combo({QStringLiteral("60"), QStringLiteral("30"), QStringLiteral("24")});
    m_replayCheck = check(true);

    col->addWidget(settingsBlock(tr("Encoder"), {
        {tr("Hardware encoder"), tr("Detected: NVENC AV1, H.264. Falls back to x264. Edit via Edit ▸ Output Settings."),
         combo({tr("NVENC H.264 (recommended)"), tr("NVENC AV1"), tr("QuickSync H.264"), tr("x264 (CPU)")})},
        {tr("Rate control"), tr("CBR for streams, CRF for archival recordings."),
         combo({tr("CRF (quality-based)"), tr("CBR (constant bitrate)")})},
        {tr("CRF"), tr("Lower = higher quality. 18–23 is typical."), m_crfEdit},
        {tr("Container"), tr("MKV survives crashes; MP4 is more compatible."), m_containerCombo},
    }));

    col->addWidget(settingsBlock(tr("Resolution & Framerate"), {
        {tr("Canvas resolution"), tr("Your scene composes to this size."), m_resCombo},
        {tr("Recording FPS"), tr("Matched by all capture sources where possible."), m_fpsCombo},
    }));

    col->addWidget(settingsBlock(tr("Storage"), {
        {tr("Recording folder"), tr("Use a fast NVMe — recordings get large."),
         line(QStringLiteral("D:\\Renders\\Sessions"), 320)},
        {tr("Filename pattern"), tr("Tokens: {project} {scene} {date} {time} {n}."),
         line(QStringLiteral("{project} — {date} {time}"), 320)},
        {tr("Replay buffer"), tr("In-RAM ring of the last 30 seconds."), m_replayCheck},
    }));

    col->addWidget(settingsBlock(tr("Auto-actions"), {
        {tr("Auto-start when game launches"), tr("Detect from a process list."), check(false)},
        {tr("Auto-stop on inactivity"), tr("Helpful for unattended capture."), check(true)},
        {tr("Save clip on hotkey"), tr("Auto-name with timestamp."), check(true)},
    }));

    loadRecordingSettings();

    auto* applyRow = new QHBoxLayout;
    applyRow->addStretch();
    auto* apply = new QPushButton(tr("Apply"));
    Theme::setVariant(apply, QStringLiteral("primary"));
    connect(apply, &QPushButton::clicked, this, &SettingsWorkspace::applyRecordingSettings);
    applyRow->addWidget(apply);
    col->addLayout(applyRow);
    col->addStretch();

    outer->addWidget(content, 1);
    scroll->setWidget(page);
    return scroll;
}

QWidget* SettingsWorkspace::buildGenericPage(const QString& title) {
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("workspaceBody"));
    auto* outer = new QHBoxLayout(page);
    outer->setContentsMargins(24, 24, 24, 24);
    auto* content = new QWidget;
    content->setMaximumWidth(800);
    auto* col = new QVBoxLayout(content);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(12);
    col->addWidget(lbl(title, QString(), 20, true));
    col->addWidget(lbl(tr("This section follows the same Setting / Field / Hint pattern as Recording."),
                       QStringLiteral("mute"), 13));
    col->addWidget(settingsBlock(tr("Sample section"), {
        {tr("Some option"), tr("Hint text under the control."), combo({tr("Option A"), tr("Option B")}, 220)},
        {tr("Toggle option"), tr("Hint text."), check(true)},
    }));
    col->addStretch();
    outer->addWidget(content, 1);
    return page;
}

void SettingsWorkspace::loadRecordingSettings() {
    const OutputSettings o = OutputSettings::load();
    if (m_resCombo)
        m_resCombo->setCurrentIndex(o.width >= 3840 ? 2 : o.width >= 2560 ? 1 : 0);
    if (m_fpsCombo) {
        const int fi = m_fpsCombo->findText(QString::number(o.fps));
        m_fpsCombo->setCurrentIndex(fi >= 0 ? fi : 0);
    }
    if (m_containerCombo)
        m_containerCombo->setCurrentIndex(
            o.container == QLatin1String("mov") ? 2 : o.container == QLatin1String("mp4") ? 1 : 0);
    if (m_crfEdit) m_crfEdit->setText(QString::number(o.crf));
    if (m_replayCheck) m_replayCheck->setChecked(o.replayBufferSeconds > 0);
}

void SettingsWorkspace::applyRecordingSettings() {
    OutputSettings o = OutputSettings::load();  // preserve encoder/codec/etc.
    static const int dims[3][2] = {{1920, 1080}, {2560, 1440}, {3840, 2160}};
    const int ri = m_resCombo ? qBound(0, m_resCombo->currentIndex(), 2) : 0;
    o.width = dims[ri][0];
    o.height = dims[ri][1];
    if (m_fpsCombo) o.fps = m_fpsCombo->currentText().toInt();
    if (m_containerCombo) {
        const int ci = m_containerCombo->currentIndex();
        o.container = ci == 2 ? QStringLiteral("mov") : ci == 1 ? QStringLiteral("mp4") : QStringLiteral("mkv");
    }
    if (m_crfEdit) {
        bool ok = false;
        const int c = m_crfEdit->text().toInt(&ok);
        if (ok) o.crf = qBound(0, c, 51);
    }
    if (m_replayCheck) o.replayBufferSeconds = m_replayCheck->isChecked() ? 30 : 0;
    o.save();
    emit recordingSettingsApplied();
}
