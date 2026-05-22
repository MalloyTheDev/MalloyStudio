#include "ui/workspaces/SettingsWorkspace.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"
#include "recording/OutputSettings.h"
#include "recording/EncoderRegistry.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace {

QLabel* lbl(const QString& s, const QString& tone = QString(), int px = 13, bool bold = false) {
    return Theme::label(s, tone, px, bold);
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

// --- QSettings-backed controls: load current value, persist on every change. ---
QCheckBox* prefCheck(const QString& key, bool def) {
    auto* c = new QCheckBox;
    c->setChecked(QSettings().value(key, def).toBool());
    QObject::connect(c, &QCheckBox::toggled, c, [key](bool on) { QSettings().setValue(key, on); });
    return c;
}
QComboBox* prefCombo(const QString& key, std::initializer_list<QString> opts, int defIndex, int w = 220) {
    auto* c = new QComboBox;
    for (const QString& o : opts) c->addItem(o);
    c->setFixedWidth(w);
    c->setCurrentIndex(QSettings().value(key, defIndex).toInt());
    QObject::connect(c, &QComboBox::currentIndexChanged, c, [key](int i) { QSettings().setValue(key, i); });
    return c;
}
QLineEdit* prefLine(const QString& key, const QString& def, int w = 260) {
    auto* e = new QLineEdit(QSettings().value(key, def).toString());
    e->setFixedWidth(w);
    QObject::connect(e, &QLineEdit::editingFinished, e, [key, e] { QSettings().setValue(key, e->text()); });
    return e;
}
// A line edit + Browse… button bound to a directory QSettings key.
QWidget* folderRow(const QString& key, const QString& def, int w = 260) {
    auto* host = new QWidget;
    auto* h = new QHBoxLayout(host);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(6);
    auto* e = new QLineEdit(QSettings().value(key, def).toString());
    e->setFixedWidth(w);
    auto* b = new QPushButton(QObject::tr("Browse…"));
    Theme::setVariant(b, QStringLiteral("ghost"));
    QObject::connect(e, &QLineEdit::editingFinished, e, [key, e] { QSettings().setValue(key, e->text()); });
    QObject::connect(b, &QPushButton::clicked, host, [host, e, key] {
        const QString d = QFileDialog::getExistingDirectory(host, QObject::tr("Choose Folder"), e->text());
        if (!d.isEmpty()) { e->setText(d); QSettings().setValue(key, d); }
    });
    h->addWidget(e);
    h->addWidget(b);
    return host;
}

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
    for (const G& g : groups) {
        QWidget* page = nullptr;
        if (g.label == tr("Recording"))    page = buildRecordingPage();
        else if (g.label == tr("General")) page = buildGeneralPage();
        else                               page = buildGenericPage(g.label);
        m_stack->addWidget(page);
    }
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

    // Encoder list comes from EncoderRegistry (detected ffmpeg encoders);
    // item data is the ffmpeg codec id stored in OutputSettings.videoCodec.
    m_encoderCombo = new QComboBox;
    m_encoderCombo->setFixedWidth(260);
    for (const auto& e : EncoderRegistry::available())
        m_encoderCombo->addItem(e.display, e.id);
    connect(m_encoderCombo, &QComboBox::currentIndexChanged, this,
            [this](int) { updateEncoderDerived(); });

    // Rate control is derived from the encoder (hardware → CBR, software → CRF),
    // so it's shown but not independently editable.
    m_rateCombo = combo({tr("CRF (quality-based)"), tr("CBR (constant bitrate)")});
    m_rateCombo->setEnabled(false);

    // Storage + auto-action controls. Persisted via QSettings on Apply; the
    // folder feeds recording/lastDir (the dir the Save dialog opens to) and the
    // filename pattern feeds recording/filenamePattern (expanded by MainWindow).
    m_folderEdit = new QLineEdit;
    m_folderEdit->setFixedWidth(260);
    auto* folderW = new QWidget;
    auto* folderH = new QHBoxLayout(folderW);
    folderH->setContentsMargins(0, 0, 0, 0);
    folderH->setSpacing(6);
    auto* browseBtn = new QPushButton(tr("Browse…"));
    Theme::setVariant(browseBtn, QStringLiteral("ghost"));
    connect(browseBtn, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(
            this, tr("Recording Folder"), m_folderEdit->text());
        if (!d.isEmpty()) m_folderEdit->setText(d);
    });
    folderH->addWidget(m_folderEdit);
    folderH->addWidget(browseBtn);

    m_filenameEdit = new QLineEdit;
    m_filenameEdit->setFixedWidth(320);
    m_autoStart = check(false);
    m_autoStop  = check(true);
    m_saveClip  = check(true);

    col->addWidget(settingsBlock(tr("Encoder"), {
        {tr("Video encoder"), tr("Detected ffmpeg encoders. Hardware encoders use CBR; x264 uses CRF."),
         m_encoderCombo},
        {tr("Rate control"), tr("Follows the encoder — CBR for hardware, CRF for x264."), m_rateCombo},
        {tr("CRF"), tr("Lower = higher quality. 18–23 is typical (x264 only)."), m_crfEdit},
        {tr("Container"), tr("MKV survives crashes; MP4 is more compatible."), m_containerCombo},
    }));

    col->addWidget(settingsBlock(tr("Resolution & Framerate"), {
        {tr("Canvas resolution"), tr("Your scene composes to this size."), m_resCombo},
        {tr("Recording FPS"), tr("Matched by all capture sources where possible."), m_fpsCombo},
    }));

    col->addWidget(settingsBlock(tr("Storage"), {
        {tr("Recording folder"), tr("Where the Save dialog opens. Use a fast NVMe — recordings get large."),
         folderW},
        {tr("Filename pattern"), tr("Tokens: {project} {scene} {date} {time}."),
         m_filenameEdit},
        {tr("Replay buffer"), tr("In-RAM ring of the last 30 seconds."), m_replayCheck},
    }));

    col->addWidget(settingsBlock(tr("Auto-actions"), {
        {tr("Auto-start when game launches"), tr("Detect from a process list."), m_autoStart},
        {tr("Auto-stop on inactivity"), tr("Helpful for unattended capture."), m_autoStop},
        {tr("Save clip on hotkey"), tr("Auto-name with timestamp."), m_saveClip},
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

QWidget* SettingsWorkspace::buildGeneralPage() {
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

    col->addWidget(lbl(tr("General"), QString(), 20, true));
    col->addWidget(lbl(tr("Startup, project defaults, and app behaviour."), QStringLiteral("mute"), 13));

    // "Show setup guide" maps to the onboarding/completed flag (inverted): ticking
    // it clears the flag so the first-run wizard runs again next launch.
    auto* onboardingCheck = new QCheckBox;
    onboardingCheck->setChecked(!QSettings().value(QStringLiteral("onboarding/completed"), false).toBool());
    connect(onboardingCheck, &QCheckBox::toggled, this, [](bool on) {
        QSettings().setValue(QStringLiteral("onboarding/completed"), !on);
    });

    col->addWidget(settingsBlock(tr("Startup"), {
        {tr("Restore last project on launch"),
         tr("Reopen the project you had open when you last quit."),
         prefCheck(QStringLiteral("app/restoreLastProject"), false)},
        {tr("Show setup guide on next launch"),
         tr("Re-run the first-run onboarding wizard the next time MalloyStudio starts."),
         onboardingCheck},
    }));

    col->addWidget(settingsBlock(tr("Projects"), {
        {tr("Default project folder"),
         tr("Where new projects are saved unless you choose otherwise."),
         folderRow(QStringLiteral("app/defaultProjectDir"),
                   QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))},
        {tr("Confirm before exit"),
         tr("Warn if there are unsaved changes when you close the app."),
         prefCheck(QStringLiteral("app/confirmExit"), true)},
    }));

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

void SettingsWorkspace::updateEncoderDerived() {
    if (!m_encoderCombo) return;
    const QString id = m_encoderCombo->currentData().toString();
    const EncoderRegistry::Encoder* enc = EncoderRegistry::find(id);
    const bool hw = enc && enc->isHardware;
    if (m_rateCombo) m_rateCombo->setCurrentIndex(hw ? 1 : 0);  // CBR : CRF
    if (m_crfEdit) m_crfEdit->setEnabled(!hw);                  // CRF only for x264
}

void SettingsWorkspace::loadRecordingSettings() {
    const OutputSettings o = OutputSettings::load();
    if (m_encoderCombo) {
        const int ei = m_encoderCombo->findData(o.videoCodec);
        m_encoderCombo->setCurrentIndex(ei >= 0 ? ei : 0);
    }
    updateEncoderDerived();
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

    QSettings s;
    if (m_folderEdit)
        m_folderEdit->setText(s.value(QStringLiteral("recording/lastDir"),
            QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)).toString());
    if (m_filenameEdit)
        m_filenameEdit->setText(s.value(QStringLiteral("recording/filenamePattern"),
            QStringLiteral("{project} — {date} {time}")).toString());
    if (m_autoStart) m_autoStart->setChecked(s.value(QStringLiteral("recording/autoStart"),        false).toBool());
    if (m_autoStop)  m_autoStop->setChecked( s.value(QStringLiteral("recording/autoStop"),         true ).toBool());
    if (m_saveClip)  m_saveClip->setChecked( s.value(QStringLiteral("recording/saveClipOnHotkey"), true ).toBool());
}

void SettingsWorkspace::applyRecordingSettings() {
    OutputSettings o = OutputSettings::load();  // preserve fields we don't expose
    if (m_encoderCombo) o.videoCodec = m_encoderCombo->currentData().toString();
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

    QSettings s;
    if (m_folderEdit && !m_folderEdit->text().trimmed().isEmpty())
        s.setValue(QStringLiteral("recording/lastDir"), m_folderEdit->text().trimmed());
    if (m_filenameEdit && !m_filenameEdit->text().trimmed().isEmpty())
        s.setValue(QStringLiteral("recording/filenamePattern"), m_filenameEdit->text().trimmed());
    if (m_autoStart) s.setValue(QStringLiteral("recording/autoStart"),        m_autoStart->isChecked());
    if (m_autoStop)  s.setValue(QStringLiteral("recording/autoStop"),         m_autoStop->isChecked());
    if (m_saveClip)  s.setValue(QStringLiteral("recording/saveClipOnHotkey"), m_saveClip->isChecked());

    emit recordingSettingsApplied();
}
