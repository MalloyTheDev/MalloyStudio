#include "ui/OnboardingOverlay.h"
#include "audio/AudioController.h"
#include "capture/CameraCapture.h"
#include "capture/MonitorInfo.h"
#include "project/ByteSize.h"
#include "project/RecentRecordings.h"
#include "recording/OutputSettings.h"
#include "ui/components/Placeholder.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"

#include <QButtonGroup>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QStorageInfo>
#include <QVBoxLayout>

namespace {

QLabel* lbl(const QString& s, const QString& tone = QString(), int px = 13, bool bold = false) {
    return Theme::label(s, tone, px, bold, false, /*wrap=*/true);  // onboarding copy wraps
}

QString brandSvg() {
    return QStringLiteral(
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'>"
        "<rect x='3' y='3' width='26' height='26' rx='6' fill='%1' stroke='%2'/>"
        "<path d='M9 22V10l4 6 3-5 3 5 4-6v12' stroke='%3' stroke-width='2' fill='none' "
        "stroke-linecap='round' stroke-linejoin='round'/>"
        "<circle cx='23' cy='9' r='2' fill='%4'/></svg>")
        .arg(Theme::Surface2.name(), Theme::BorderStrong.name(), Theme::Text.name(), Theme::Accent.name());
}

// A selectable card (icon + title + sub); clicking highlights it within a group.
QPushButton* optionCard(const QString& icon, const QString& title, const QString& sub) {
    auto* b = new QPushButton;
    b->setObjectName(QStringLiteral("actionButton"));
    b->setCheckable(true);
    b->setCursor(Qt::PointingHandCursor);
    b->setFixedHeight(60);   // so a stretched grid cell can't distort the content
    auto* h = new QHBoxLayout(b);
    h->setContentsMargins(12, 10, 12, 10); h->setSpacing(12);
    if (!icon.isEmpty()) {
        auto* chip = new QFrame; chip->setObjectName(QStringLiteral("iconChip")); chip->setFixedSize(32, 32);
        auto* cv = new QVBoxLayout(chip); cv->setContentsMargins(0, 0, 0, 0);
        auto* ci = new QLabel; ci->setAlignment(Qt::AlignCenter); ci->setPixmap(Icons::pixmap(icon, Theme::TextDim, 16));
        cv->addWidget(ci);
        h->addWidget(chip);
    }
    auto* tv = new QVBoxLayout; tv->setSpacing(0);
    // Single-line title/sub — the local lbl() enables wordWrap, which overlaps
    // when stacked spacing-0 inside this button, so turn it off here.
    auto* tl = lbl(title, QString(), 13, true);          tl->setWordWrap(false);
    auto* sl = lbl(sub, QStringLiteral("mute"), 11);     sl->setWordWrap(false);
    sl->setObjectName(QStringLiteral("optionSub"));
    tv->addWidget(tl);
    tv->addWidget(sl);
    h->addLayout(tv); h->addStretch();
    return b;
}

// The two fields a quality choice sets, named as Settings > Recording names them.
QString qualitySummary(int fps, int crf) {
    return QObject::tr("%1 fps · CRF %2").arg(fps).arg(crf);
}

// One line of the devices step. The mark says what was found: a check when
// something was, a warning when nothing was, and a neutral mark when nobody
// has looked yet, so a machine that was not checked is not reported as empty.
enum class Found { Yes, No, NotChecked };

QWidget* deviceRow(const QString& kind, const QString& detail, Found found,
                   const QString& objectName) {
    auto* r = new QWidget;
    auto* h = new QHBoxLayout(r); h->setContentsMargins(16, 12, 16, 12); h->setSpacing(12);
    auto* mark = new QLabel;
    const QString icon = found == Found::Yes ? QStringLiteral("check")
                       : found == Found::No  ? QStringLiteral("alert")
                                             : QStringLiteral("info");
    const QColor tint = found == Found::Yes ? Theme::Success
                      : found == Found::No  ? Theme::Warn
                                            : Theme::TextMute;
    mark->setPixmap(Icons::pixmap(icon, tint, 14));
    h->addWidget(mark);
    auto* tv = new QVBoxLayout; tv->setSpacing(0);
    tv->addWidget(lbl(kind, QString(), 13, true));
    auto* nm = lbl(detail, QStringLiteral("mute"), 11); nm->setProperty("mono", true);
    nm->setObjectName(objectName);
    tv->addWidget(nm);
    h->addLayout(tv, 1);
    return r;
}

} // namespace

OnboardingOverlay::Devices OnboardingOverlay::detectDevices(AudioController* audio) {
    Devices d;
    if (audio) {
        d.audioChecked = true;
        for (const auto& device : audio->enumerateInputDevices())
            d.microphones << device.second;
        for (const AudioInput& in : audio->inputs()) {
            if (in.loopback && in.connected) { d.desktopAudio = in.name; break; }
        }
    }
    // The cache only: enumerating here would block for seconds on a machine
    // with no camera. openWizard() fills it in the background when nothing
    // has looked yet.
    d.camerasChecked = CameraCapture::hasEnumerated();
    for (const CameraCapture::Device& c : CameraCapture::cachedDevices())
        d.cameras << c.name;
    for (const MonitorInfo& m : enumerateMonitors())
        d.displays << QStringLiteral("%1 × %2").arg(m.geometry.width()).arg(m.geometry.height());
    return d;
}

OutputSettings OnboardingOverlay::withQuality(const OutputSettings& current, Quality quality) {
    OutputSettings o = current;
    switch (quality) {
    case Quality::Keep:        break;
    case Quality::Standard:    o.fps = 60; o.crf = 23; break;
    case Quality::Archival:    o.fps = 60; o.crf = 18; break;
    case Quality::Lightweight: o.fps = 30; o.crf = 28; break;
    }
    return o;
}

OnboardingOverlay::OnboardingOverlay(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("paletteOverlay"));
    hide();
    if (parent) parent->installEventFilter(this);  // track parent resize

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setAlignment(Qt::AlignCenter);

    m_card = new QFrame(this);
    m_card->setObjectName(QStringLiteral("onboardCard"));
    m_card->setFixedWidth(720);
    auto* cv = new QVBoxLayout(m_card);
    cv->setContentsMargins(0, 0, 0, 0);
    cv->setSpacing(0);

    // Header
    auto* header = new QWidget(m_card);
    auto* hh = new QHBoxLayout(header);
    hh->setContentsMargins(18, 14, 18, 14); hh->setSpacing(10);
    auto* brand = new QLabel; brand->setPixmap(Icons::renderSvg(brandSvg(), 22));
    hh->addWidget(brand);
    hh->addWidget(lbl(tr("Setup"), QString(), 13, true));
    m_stepCount = lbl(QString(), QStringLiteral("mute"), 11);   // set by goToStep()
    m_stepCount->setProperty("mono", true);
    hh->addWidget(m_stepCount);
    hh->addStretch();
    auto* close = new QPushButton(Icons::icon(QStringLiteral("close"), Theme::TextDim, 12), QString());
    Theme::setVariant(close, QStringLiteral("ghost")); close->setFixedWidth(28);
    connect(close, &QPushButton::clicked, this, &QWidget::hide);
    hh->addWidget(close);
    cv->addWidget(header);

    auto* topDiv = new QFrame; topDiv->setObjectName(QStringLiteral("divider")); topDiv->setFixedHeight(1);
    cv->addWidget(topDiv);

    m_progress = new QProgressBar(m_card);
    m_progress->setRange(0, m_count);
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(3);
    cv->addWidget(m_progress);

    // Body: title + subtitle + step stack
    auto* body = new QWidget(m_card);
    auto* bv = new QVBoxLayout(body);
    bv->setContentsMargins(28, 24, 28, 24); bv->setSpacing(16);
    m_title = lbl(QString(), QString(), 20, true);
    bv->addWidget(m_title);
    m_subtitle = lbl(QString(), QStringLiteral("mute"), 13);
    bv->addWidget(m_subtitle);

    m_steps = new QStackedWidget(body);

    // Step 0 — welcome
    {
        auto* w = new QFrame; w->setObjectName(QStringLiteral("card"));
        auto* h = new QHBoxLayout(w); h->setContentsMargins(16, 16, 16, 16); h->setSpacing(14);
        auto* ph = new Placeholder(tr("Preview · animated demo"), 16, 9); ph->setFixedWidth(280);
        h->addWidget(ph);
        auto* tv = new QVBoxLayout; tv->setSpacing(4);
        tv->addWidget(lbl(tr("What's inside"), QString(), 13, true));
        tv->addWidget(lbl(tr("• Capture display, window, camera, and audio\n"
                             "• Compose scenes; record & stream simultaneously\n"
                             "• Trim and assemble in the built-in editor\n"
                             "• Auto-clip with the replay buffer"), QStringLiteral("dim"), 12));
        tv->addStretch();
        h->addLayout(tv, 1);
        m_steps->addWidget(w);
    }
    // Step 1: the recording folder, read from the setting when the wizard
    // opens, with the free space on its drive.
    {
        auto* w = new QWidget; auto* v = new QVBoxLayout(w); v->setContentsMargins(0,0,0,0); v->setSpacing(10);
        auto* c = new QFrame; c->setObjectName(QStringLiteral("card"));
        auto* h = new QHBoxLayout(c); h->setContentsMargins(12, 12, 12, 12); h->setSpacing(12);
        auto* fi = new QLabel; fi->setPixmap(Icons::pixmap(QStringLiteral("folder"), Theme::TextMute, 16));
        h->addWidget(fi);
        auto* tv = new QVBoxLayout; tv->setSpacing(0);
        m_folderPath = lbl(QString(), QString(), 13, true); m_folderPath->setProperty("mono", true);
        m_folderPath->setObjectName(QStringLiteral("folderPath"));
        tv->addWidget(m_folderPath);
        m_folderSpace = lbl(QString(), QStringLiteral("mute"), 11);
        m_folderSpace->setObjectName(QStringLiteral("folderSpace"));
        tv->addWidget(m_folderSpace);
        h->addLayout(tv, 1);
        auto* choose = new QPushButton(tr("Choose…")); Theme::setVariant(choose, QStringLiteral("ghost"));
        connect(choose, &QPushButton::clicked, this, [this] {
            const QString dir = QFileDialog::getExistingDirectory(this, tr("Recording folder"), m_folder);
            if (!dir.isEmpty()) setRecordingFolder(dir);
        });
        h->addWidget(choose);
        v->addWidget(c);
        v->addStretch();
        m_steps->addWidget(w);
    }
    // Step 2: devices, filled by showDevices().
    {
        auto* card = new QFrame; card->setObjectName(QStringLiteral("card"));
        m_deviceRows = new QVBoxLayout(card); m_deviceRows->setContentsMargins(0,0,0,0); m_deviceRows->setSpacing(0);
        m_steps->addWidget(card);
    }
    // Step 3: quality. Each card says exactly what it changes, and nothing
    // else is touched, so resolution and encoder stay as the user had them.
    {
        auto* w = new QWidget; auto* g = new QGridLayout(w); g->setContentsMargins(0,0,0,0); g->setSpacing(10);
        struct Q { Quality quality; QString id, name, sub; };
        const auto summary = [](Quality q) {
            const OutputSettings o = withQuality(OutputSettings{}, q);
            return qualitySummary(o.fps, o.crf);
        };
        const QVector<Q> qs = {
            {Quality::Keep,        QStringLiteral("quality.keep"),        tr("Keep current"), QString()},
            {Quality::Standard,    QStringLiteral("quality.standard"),    tr("Standard"),
             summary(Quality::Standard)},
            {Quality::Archival,    QStringLiteral("quality.archival"),    tr("Archival"),
             tr("%1 · larger files").arg(summary(Quality::Archival))},
            {Quality::Lightweight, QStringLiteral("quality.lightweight"), tr("Lightweight"),
             tr("%1 · smaller files").arg(summary(Quality::Lightweight))},
        };
        m_quality = new QButtonGroup(w);
        m_quality->setExclusive(true);
        for (int i = 0; i < qs.size(); ++i) {
            auto* c = optionCard(QString(), qs[i].name, qs[i].sub);
            c->setObjectName(qs[i].id);
            m_quality->addButton(c, static_cast<int>(qs[i].quality));
            if (qs[i].quality == Quality::Keep) {
                c->setChecked(true);
                m_keepSummary = c->findChild<QLabel*>(QStringLiteral("optionSub"));
            }
            g->addWidget(c, i / 2, i % 2);
        }
        g->setRowStretch(2, 1);   // anchor the 2x2 grid to the top
        m_steps->addWidget(w);
    }
    // Step 4: done
    {
        auto* w = new QWidget; auto* v = new QVBoxLayout(w);
        v->setAlignment(Qt::AlignCenter); v->setSpacing(12);
        auto* chip = new QFrame; chip->setObjectName(QStringLiteral("iconChip")); chip->setFixedSize(56, 56);
        auto* cl = new QVBoxLayout(chip); cl->setContentsMargins(0,0,0,0);
        auto* ci = new QLabel; ci->setAlignment(Qt::AlignCenter); ci->setPixmap(Icons::pixmap(QStringLiteral("check"), Theme::Success, 28));
        cl->addWidget(ci);
        v->addWidget(chip, 0, Qt::AlignHCenter);
        v->addWidget(lbl(tr("All set."), QString(), 16, true), 0, Qt::AlignHCenter);
        auto* tip = lbl(tr("Tip: press Ctrl+K any time to find a feature or run a command."), QStringLiteral("mute"), 12);
        tip->setAlignment(Qt::AlignCenter); tip->setMaximumWidth(360);
        v->addWidget(tip, 0, Qt::AlignHCenter);
        m_steps->addWidget(w);
    }

    bv->addWidget(m_steps, 1);
    cv->addWidget(body, 1);

    auto* botDiv = new QFrame; botDiv->setObjectName(QStringLiteral("divider")); botDiv->setFixedHeight(1);
    cv->addWidget(botDiv);

    // Footer
    auto* footer = new QWidget(m_card);
    auto* fh = new QHBoxLayout(footer);
    fh->setContentsMargins(18, 14, 18, 14); fh->setSpacing(8);
    m_back = new QPushButton(tr("Back")); Theme::setVariant(m_back, QStringLiteral("ghost"));
    connect(m_back, &QPushButton::clicked, this, [this] { goToStep(m_step - 1); });
    fh->addWidget(m_back);
    auto* skip = new QPushButton(tr("Skip setup")); Theme::setVariant(skip, QStringLiteral("ghost"));
    connect(skip, &QPushButton::clicked, this, &QWidget::hide);
    fh->addWidget(skip);
    fh->addStretch();
    m_next = new QPushButton(tr("Continue")); Theme::setVariant(m_next, QStringLiteral("primary"));
    m_next->setObjectName(QStringLiteral("onboardingNext"));
    connect(m_next, &QPushButton::clicked, this, [this] {
        if (m_step < m_count - 1) goToStep(m_step + 1);
        else finishWizard();
    });
    fh->addWidget(m_next);
    cv->addWidget(footer);

    outer->addWidget(m_card);

    loadChoices();
    goToStep(0);
}

void OnboardingOverlay::openWizard() {
    loadChoices();
    showDevices(detectDevices(m_audio));
    // Nothing has enumerated cameras yet, so ask for it off the UI thread and
    // update the list when it lands. It lists devices; it opens none.
    if (!CameraCapture::hasEnumerated()) {
        CameraCapture::refreshDevicesAsync(this, [this](const QList<CameraCapture::Device>&) {
            showDevices(detectDevices(m_audio));
        });
    }
    if (parentWidget()) setGeometry(parentWidget()->rect());
    goToStep(0);
    show();
    raise();
}

void OnboardingOverlay::loadChoices() {
    const QString folder = RecentRecordings::outputDir();
    m_folderAtOpen = folder;
    setRecordingFolder(folder);

    const OutputSettings current = OutputSettings::load();
    if (m_keepSummary) m_keepSummary->setText(qualitySummary(current.fps, current.crf));
    if (QAbstractButton* keep = m_quality->button(static_cast<int>(Quality::Keep)))
        keep->setChecked(true);
}

void OnboardingOverlay::setRecordingFolder(const QString& path) {
    m_folder = path;
    m_folderPath->setText(QDir::toNativeSeparators(path));
    const QStorageInfo storage(path);
    if (!QFileInfo(path).isDir() || !storage.isValid() || !storage.isReady()) {
        m_folderSpace->setText(tr("This folder does not exist on this computer yet."));
        return;
    }
    m_folderSpace->setText(tr("%1 free of %2")
                               .arg(formatByteSize(storage.bytesAvailable()),
                                    formatByteSize(storage.bytesTotal())));
}

void OnboardingOverlay::showDevices(const Devices& devices) {
    while (QLayoutItem* item = m_deviceRows->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    const auto listed = [](const QStringList& names) {
        return names.join(QStringLiteral(", "));
    };
    struct Row { QString kind, detail; Found found; QString name; };
    QVector<Row> rows;

    if (!devices.audioChecked) {
        rows.append({tr("Microphones"), tr("Not checked"), Found::NotChecked,
                     QStringLiteral("deviceMicrophones")});
        rows.append({tr("Desktop audio"), tr("Not checked"), Found::NotChecked,
                     QStringLiteral("deviceDesktopAudio")});
    } else {
        rows.append(devices.microphones.isEmpty()
            ? Row{tr("Microphones"), tr("None found"), Found::No, QStringLiteral("deviceMicrophones")}
            : Row{tr("Microphones"), listed(devices.microphones), Found::Yes,
                  QStringLiteral("deviceMicrophones")});
        rows.append(devices.desktopAudio.isEmpty()
            ? Row{tr("Desktop audio"), tr("No playback device found"), Found::No,
                  QStringLiteral("deviceDesktopAudio")}
            : Row{tr("Desktop audio"), devices.desktopAudio, Found::Yes,
                  QStringLiteral("deviceDesktopAudio")});
    }
    if (!devices.camerasChecked)
        rows.append({tr("Cameras"), tr("Still looking"), Found::NotChecked,
                     QStringLiteral("deviceCameras")});
    else
        rows.append(devices.cameras.isEmpty()
            ? Row{tr("Cameras"), tr("None found"), Found::No, QStringLiteral("deviceCameras")}
            : Row{tr("Cameras"), listed(devices.cameras), Found::Yes, QStringLiteral("deviceCameras")});
    rows.append(devices.displays.isEmpty()
        ? Row{tr("Displays"), tr("None found"), Found::No, QStringLiteral("deviceDisplays")}
        : Row{tr("Displays"), listed(devices.displays), Found::Yes, QStringLiteral("deviceDisplays")});

    for (int i = 0; i < rows.size(); ++i) {
        if (i) {
            auto* d = new QFrame; d->setObjectName(QStringLiteral("divider")); d->setFixedHeight(1);
            m_deviceRows->addWidget(d);
        }
        m_deviceRows->addWidget(deviceRow(rows[i].kind, rows[i].detail, rows[i].found, rows[i].name));
    }
}

void OnboardingOverlay::finishWizard() {
    // Only what was changed is written, so pressing straight through leaves
    // the settings exactly as they were.
    if (m_folder != m_folderAtOpen)
        QSettings().setValue(QStringLiteral("recording/lastDir"), m_folder);
    const int chosen = m_quality->checkedId();
    if (chosen >= 0 && chosen != static_cast<int>(Quality::Keep))
        withQuality(OutputSettings::load(), static_cast<Quality>(chosen)).save();
    hide();
    emit finished();
}

void OnboardingOverlay::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (parentWidget()) setGeometry(parentWidget()->rect());
}

void OnboardingOverlay::goToStep(int step) {
    m_step = qBound(0, step, m_count - 1);
    m_steps->setCurrentIndex(m_step);
    m_progress->setValue(m_step + 1);
    m_stepCount->setText(QStringLiteral("%1 / %2").arg(m_step + 1).arg(m_count));

    static const char* titles[] = {
        "Welcome to MalloyStudio",
        "Where should recordings go?", "Let's find your devices",
        "Pick a recording quality", "You're ready",
    };
    static const char* subs[] = {
        "Record, stream, clip, and edit — in one workstation.",
        "Pick a fast NVMe if you can — recordings get large.",
        "We'll detect what you've got plugged in.",
        "Sets the frame rate and quality in Settings > Recording. "
        "Resolution and encoder stay as they are.",
        "Your choices are saved when you open the dashboard.",
    };
    m_title->setText(tr(titles[m_step]));
    m_subtitle->setText(tr(subs[m_step]));
    m_back->setEnabled(m_step > 0);
    m_next->setText(m_step < m_count - 1 ? tr("Continue") : tr("Open dashboard"));
}

void OnboardingOverlay::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) { hide(); return; }
    QWidget::keyPressEvent(event);
}

bool OnboardingOverlay::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() && event->type() == QEvent::Resize) {
        if (isVisible()) setGeometry(parentWidget()->rect());
    }
    return QWidget::eventFilter(watched, event);
}

void OnboardingOverlay::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0, 0, 0, 150));
}
