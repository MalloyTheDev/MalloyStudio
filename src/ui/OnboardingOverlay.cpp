#include "ui/OnboardingOverlay.h"
#include "ui/components/Placeholder.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"

#include <QButtonGroup>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace {

QLabel* lbl(const QString& s, const QString& tone = QString(), int px = 13, bool bold = false) {
    auto* l = new QLabel(s);
    if (!tone.isEmpty()) l->setProperty("tone", tone);
    QFont f = l->font(); f.setPixelSize(px); if (bold) f.setWeight(QFont::DemiBold);
    l->setFont(f);
    l->setWordWrap(true);
    return l;
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
    auto* h = new QHBoxLayout(b);
    h->setContentsMargins(12, 12, 12, 12); h->setSpacing(12);
    if (!icon.isEmpty()) {
        auto* chip = new QFrame; chip->setObjectName(QStringLiteral("iconChip")); chip->setFixedSize(32, 32);
        auto* cv = new QVBoxLayout(chip); cv->setContentsMargins(0, 0, 0, 0);
        auto* ci = new QLabel; ci->setAlignment(Qt::AlignCenter); ci->setPixmap(Icons::pixmap(icon, Theme::TextDim, 16));
        cv->addWidget(ci);
        h->addWidget(chip);
    }
    auto* tv = new QVBoxLayout; tv->setSpacing(0);
    tv->addWidget(lbl(title, QString(), 13, true));
    tv->addWidget(lbl(sub, QStringLiteral("mute"), 11));
    h->addLayout(tv); h->addStretch();
    return b;
}

} // namespace

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
    m_stepCount = lbl(QStringLiteral("1 / 7"), QStringLiteral("mute"), 11);
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
    // Step 1 — usage
    {
        auto* w = new QWidget; auto* g = new QGridLayout(w); g->setContentsMargins(0,0,0,0); g->setSpacing(12);
        auto* grp = new QButtonGroup(w); grp->setExclusive(true);
        struct O { QString icon, label, sub; };
        const QVector<O> opts = {
            {QStringLiteral("record"), tr("Record"), tr("Sessions, replays, raw footage")},
            {QStringLiteral("stream"), tr("Stream"), tr("Twitch · YouTube · TikTok")},
            {QStringLiteral("editor"), tr("Edit"),   tr("Timeline, clips, render")},
            {QStringLiteral("layers"), tr("All-in-one"), tr("I do everything")},
        };
        int i = 0;
        for (const O& o : opts) {
            auto* c = optionCard(o.icon, o.label, o.sub);
            grp->addButton(c);
            if (i == 3) c->setChecked(true);
            g->addWidget(c, i / 2, i % 2);
            ++i;
        }
        m_steps->addWidget(w);
    }
    // Step 2 — folder
    {
        auto* w = new QWidget; auto* v = new QVBoxLayout(w); v->setContentsMargins(0,0,0,0); v->setSpacing(10);
        struct F { QString name, free, tag; };
        const QVector<F> folders = {
            {QStringLiteral("D:\\Renders"), tr("412 GB free of 2 TB"), tr("fastest")},
            {QStringLiteral("E:\\Footage Archive"), tr("8 TB free of 16 TB"), tr("archive")},
            {QStringLiteral("C:\\Users\\jess\\Videos"), tr("64 GB free of 1 TB"), tr("system")},
        };
        auto* grp = new QButtonGroup(w);
        int i = 0;
        for (const F& f : folders) {
            auto* c = new QFrame; c->setObjectName(QStringLiteral("card"));
            auto* h = new QHBoxLayout(c); h->setContentsMargins(12, 12, 12, 12); h->setSpacing(12);
            auto* radio = new QRadioButton; grp->addButton(radio); if (i == 0) radio->setChecked(true);
            h->addWidget(radio);
            auto* fi = new QLabel; fi->setPixmap(Icons::pixmap(QStringLiteral("folder"), Theme::TextMute, 16));
            h->addWidget(fi);
            auto* tv = new QVBoxLayout; tv->setSpacing(0);
            auto* nm = lbl(f.name, QString(), 13, true); nm->setProperty("mono", true);
            tv->addWidget(nm);
            tv->addWidget(lbl(f.free, QStringLiteral("mute"), 11));
            h->addLayout(tv, 1);
            h->addWidget(Theme::makeTag(f.tag));
            v->addWidget(c);
            ++i;
        }
        v->addStretch();
        m_steps->addWidget(w);
    }
    // Step 3 — devices
    {
        auto* card = new QFrame; card->setObjectName(QStringLiteral("card"));
        auto* v = new QVBoxLayout(card); v->setContentsMargins(0,0,0,0); v->setSpacing(0);
        struct D { QString kind, name; };
        const QVector<D> devs = {
            {tr("Microphone"), tr("Shure SM7B (USB)")},
            {tr("Webcam"), tr("Sony α7C · 1080p60")},
            {tr("Display capture"), tr("DXGI ready · 2 monitors")},
            {tr("Desktop audio"), tr("WASAPI loopback · 48 kHz")},
            {tr("Window capture"), tr("Spire of the Hollow Sun · detected")},
        };
        for (int i = 0; i < devs.size(); ++i) {
            if (i) { auto* d = new QFrame; d->setObjectName(QStringLiteral("divider")); d->setFixedHeight(1); v->addWidget(d); }
            auto* r = new QWidget; auto* h = new QHBoxLayout(r); h->setContentsMargins(16, 12, 16, 12); h->setSpacing(12);
            auto* ck = new QLabel; ck->setPixmap(Icons::pixmap(QStringLiteral("check"), Theme::Success, 14));
            h->addWidget(ck);
            auto* tv = new QVBoxLayout; tv->setSpacing(0);
            tv->addWidget(lbl(devs[i].kind, QString(), 13, true));
            auto* nm = lbl(devs[i].name, QStringLiteral("mute"), 11); nm->setProperty("mono", true);
            tv->addWidget(nm);
            h->addLayout(tv, 1);
            auto* test = new QPushButton(tr("Test")); Theme::setVariant(test, QStringLiteral("ghost"));
            h->addWidget(test);
            v->addWidget(r);
        }
        m_steps->addWidget(card);
    }
    // Step 4 — quality
    {
        auto* w = new QWidget; auto* g = new QGridLayout(w); g->setContentsMargins(0,0,0,0); g->setSpacing(10);
        struct Q { QString name, sub; };
        const QVector<Q> qs = {
            {tr("Standard"), tr("1080p60 · 8 Mb/s · NVENC")},
            {tr("Archival"), tr("1080p60 · CRF 18 · big files")},
            {tr("Lightweight"), tr("1080p30 · 6 Mb/s · low CPU")},
        };
        auto* grp = new QButtonGroup(w);
        for (int i = 0; i < qs.size(); ++i) {
            auto* c = optionCard(QString(), qs[i].name, qs[i].sub);
            grp->addButton(c); if (i == 0) c->setChecked(true);
            g->addWidget(c, 0, i);
        }
        m_steps->addWidget(w);
    }
    // Step 5 — density
    {
        auto* w = new QWidget; auto* g = new QGridLayout(w); g->setContentsMargins(0,0,0,0); g->setSpacing(10);
        struct Dn { QString name, sub; };
        const QVector<Dn> ds = {
            {tr("Comfortable"), tr("Negative space, bigger targets")},
            {tr("Compact"), tr("Default · Linear-style density")},
            {tr("Dense"), tr("For pros · maximum surface area")},
        };
        auto* grp = new QButtonGroup(w);
        for (int i = 0; i < ds.size(); ++i) {
            auto* c = optionCard(QString(), ds[i].name, ds[i].sub);
            grp->addButton(c); if (i == 1) c->setChecked(true);
            g->addWidget(c, 0, i);
        }
        m_steps->addWidget(w);
    }
    // Step 6 — done
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
    connect(m_next, &QPushButton::clicked, this, [this] {
        if (m_step < m_count - 1) goToStep(m_step + 1);
        else hide();
    });
    fh->addWidget(m_next);
    cv->addWidget(footer);

    outer->addWidget(m_card);
}

void OnboardingOverlay::openWizard() {
    if (parentWidget()) setGeometry(parentWidget()->rect());
    goToStep(0);
    show();
    raise();
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
        "Welcome to MalloyStudio", "How will you mostly use the app?",
        "Where should your work live?", "Let's find your devices",
        "Pick a recording quality", "Choose a density", "You're ready",
    };
    static const char* subs[] = {
        "Record, stream, clip, and edit — in one workstation.",
        "You can change this any time.",
        "Pick a fast NVMe if you can — recordings get large.",
        "We'll detect what you've got plugged in.",
        "You can tweak this per project later.",
        "How compact should the UI feel?",
        "Drop into the dashboard — or jump straight into recording.",
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
