#include "ui/workspaces/LibraryWorkspaces.h"
#include "ui/components/PanelFrame.h"
#include "ui/components/Placeholder.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"
#include "project/ClipsRegistry.h"
#include "project/ProjectRegistry.h"
#include "project/MediaRegistry.h"

#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace {

QLabel* lbl(const QString& s, const QString& tone = QString(), int px = 13, bool bold = false, bool mono = false) {
    auto* l = new QLabel(s);
    if (!tone.isEmpty()) l->setProperty("tone", tone);
    if (mono) l->setProperty("mono", true);
    QFont f = l->font(); f.setPixelSize(px); if (bold) f.setWeight(QFont::DemiBold);
    l->setFont(f);
    return l;
}

QLineEdit* search(const QString& ph, int w = 320) {
    auto* e = new QLineEdit;
    e->setPlaceholderText(ph);
    e->addAction(Icons::icon(QStringLiteral("search"), Theme::TextMute, 14), QLineEdit::LeadingPosition);
    if (w > 0) e->setFixedWidth(w);
    return e;
}

QWidget* filterRow(const QString& icon, const QString& label, int count, bool active = false) {
    auto* w = new QWidget;
    w->setObjectName(QStringLiteral("row"));
    if (active) w->setProperty("active", true);
    w->setFixedHeight(30);
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(10, 0, 10, 0);
    h->setSpacing(8);
    auto* i = new QLabel; i->setPixmap(Icons::pixmap(icon, Theme::TextMute, 12));
    h->addWidget(i);
    h->addWidget(lbl(label, QStringLiteral("dim"), 12));
    h->addStretch();
    if (count >= 0) h->addWidget(lbl(QString::number(count), QStringLiteral("mute"), 11, false, true));
    return w;
}

QScrollArea* scrollArea(QWidget* content) {
    auto* s = new QScrollArea;
    s->setWidgetResizable(true);
    s->setFrameShape(QFrame::NoFrame);
    s->setWidget(content);
    return s;
}

QPushButton* ghost(const QString& text, const QString& icon = QString()) {
    auto* b = icon.isEmpty() ? new QPushButton(text)
                             : new QPushButton(Icons::icon(icon, Theme::TextDim, 12), QStringLiteral(" ") + text);
    Theme::setVariant(b, QStringLiteral("ghost"));
    return b;
}

} // namespace

// ───────────────────────────── Clips ──────────────────────────────────────
ClipsWorkspace::ClipsWorkspace(ClipsRegistry* registry, QWidget* parent)
    : QWidget(parent), m_registry(registry) {
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);

    int total = registry ? registry->count() : 0;
    int favCount = 0;
    if (registry) for (const ClipInfo& c : registry->clips()) if (c.favorite) ++favCount;

    // Left filters
    auto* aside = new QWidget(this);
    aside->setObjectName(QStringLiteral("recSideCol"));
    aside->setFixedWidth(240);
    auto* av = new QVBoxLayout(aside);
    av->setContentsMargins(6, 8, 6, 8);
    av->setSpacing(2);
    av->addWidget(Theme::makeSectionHeader(tr("Smart filters")));
    av->addWidget(filterRow(QStringLiteral("clips"), tr("All clips"), total, true));
    av->addWidget(filterRow(QStringLiteral("star"), tr("Favorites"), favCount));
    av->addWidget(filterRow(QStringLiteral("folder"), tr("Archived"), 0));
    av->addWidget(Theme::makeSectionHeader(tr("Tags")));
    auto* tagWrap = new QWidget;
    auto* tg = new QHBoxLayout(tagWrap);
    tg->setContentsMargins(12, 0, 12, 0); tg->setSpacing(4);
    for (const QString& t : {tr("no-hit"), tr("highlight"), tr("fail"), tr("reveal")})
        tg->addWidget(Theme::makeTag(t));
    tg->addStretch();
    av->addWidget(tagWrap);
    av->addStretch();
    row->addWidget(aside);

    // Main
    auto* main = new QWidget(this);
    auto* mv = new QVBoxLayout(main);
    mv->setContentsMargins(0, 0, 0, 0);
    mv->setSpacing(0);

    auto* toolbar = new QWidget(main);
    auto* tb = new QHBoxLayout(toolbar);
    tb->setContentsMargins(16, 12, 16, 12);
    tb->setSpacing(8);
    tb->addWidget(search(tr("Search clips, tags, project…")));
    auto* sort = new QComboBox; sort->addItems({tr("Newest first"), tr("Oldest first"), tr("Longest")});
    sort->setFixedWidth(140);
    tb->addWidget(sort);
    m_countLabel = lbl(QString(), QStringLiteral("mute"), 11, false, true);
    tb->addWidget(m_countLabel);
    tb->addStretch();
    auto* exp = new QPushButton(Icons::icon(QStringLiteral("upload"), Theme::Text, 12), tr(" Export selected"));
    tb->addWidget(exp);
    mv->addWidget(toolbar);
    auto* div = new QFrame; div->setObjectName(QStringLiteral("divider")); div->setFixedHeight(1);
    mv->addWidget(div);

    m_scroll = new QScrollArea(main);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    mv->addWidget(m_scroll, 1);
    row->addWidget(main, 1);

    if (m_registry)
        connect(m_registry, &ClipsRegistry::changed, this, &ClipsWorkspace::rebuild);
    rebuild();
}

void ClipsWorkspace::rebuild() {
    const QVector<ClipInfo> clips = m_registry ? m_registry->clips() : QVector<ClipInfo>{};
    qint64 totalBytes = 0;
    for (const ClipInfo& c : clips) totalBytes += c.sizeBytes;
    if (m_countLabel) {
        const double mb = totalBytes / (1024.0 * 1024.0);
        const QString sz = mb >= 1024.0 ? QStringLiteral("%1 GB").arg(mb / 1024.0, 0, 'f', 1)
                                        : QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
        m_countLabel->setText(tr("%1 clips · %2").arg(clips.size()).arg(sz));
    }

    auto* content = new QWidget;
    content->setObjectName(QStringLiteral("workspaceBody"));

    if (clips.isEmpty()) {
        auto* v = new QVBoxLayout(content);
        v->setAlignment(Qt::AlignCenter);
        v->setSpacing(8);
        auto* icon = new QLabel;
        icon->setPixmap(Icons::pixmap(QStringLiteral("clips"), Theme::TextFaint, 40));
        icon->setAlignment(Qt::AlignCenter);
        v->addWidget(icon, 0, Qt::AlignHCenter);
        auto* t = lbl(tr("No clips yet"), QString(), 16, true);
        t->setAlignment(Qt::AlignCenter);
        v->addWidget(t, 0, Qt::AlignHCenter);
        auto* s = lbl(tr("Save the last 30 seconds with the replay hotkey.\n"
                         "Enable the replay buffer in Settings ▸ Recording first."),
                      QStringLiteral("mute"), 12);
        s->setAlignment(Qt::AlignCenter);
        v->addWidget(s, 0, Qt::AlignHCenter);
    } else {
        auto* gl = new QGridLayout(content);
        gl->setContentsMargins(16, 16, 16, 16);
        gl->setSpacing(14);
        int i = 0;
        for (const ClipInfo& c : clips) {
            auto* cell = new QWidget;
            auto* cv = new QVBoxLayout(cell);
            cv->setContentsMargins(0, 0, 0, 0); cv->setSpacing(6);
            cv->addWidget(new Placeholder(c.name, 16, 9));
            cv->addWidget(lbl(c.name, QString(), 12, true));
            auto* meta = new QHBoxLayout; meta->setSpacing(6);
            const QString when = c.recordedAt.isValid()
                ? c.recordedAt.toString(QStringLiteral("MMM d · h:mm AP")) : QString();
            const QString src = c.sourceProject.isEmpty() ? tr("Unsorted") : c.sourceProject;
            meta->addWidget(lbl(QStringLiteral("%1 · %2 · %3").arg(src, when, c.durationText()),
                                QStringLiteral("mute"), 10, false, true));
            meta->addStretch();
            auto* fav = new QPushButton(Icons::icon(QStringLiteral("star"),
                                        c.favorite ? Theme::Warn : Theme::TextFaint, 12), QString());
            Theme::setVariant(fav, QStringLiteral("ghost"));
            fav->setFixedWidth(24);
            fav->setCursor(Qt::PointingHandCursor);
            const QString id = c.id; const bool cur = c.favorite;
            connect(fav, &QPushButton::clicked, this, [this, id, cur] {
                if (m_registry) m_registry->setFavorite(id, !cur);
            });
            meta->addWidget(fav);
            cv->addLayout(meta);
            gl->addWidget(cell, i / 4, i % 4);
            ++i;
        }
        gl->setRowStretch((i + 3) / 4, 1);
        gl->setColumnStretch(4, 0);
    }
    m_scroll->setWidget(content);
}

// ───────────────────────────── Media ──────────────────────────────────────
MediaWorkspace::MediaWorkspace(MediaRegistry* registry, QWidget* parent)
    : QWidget(parent), m_registry(registry) {
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);

    auto* aside = new QWidget(this);
    aside->setObjectName(QStringLiteral("recSideCol"));
    aside->setFixedWidth(240);
    auto* av = new QVBoxLayout(aside);
    av->setContentsMargins(6, 8, 6, 8);
    av->setSpacing(2);
    av->addWidget(Theme::makeSectionHeader(tr("Library")));
    av->addWidget(filterRow(QStringLiteral("media"), tr("All media"),
                            registry ? registry->count() : 0, true));
    av->addWidget(Theme::makeSectionHeader(tr("File types")));
    av->addWidget(filterRow(QStringLiteral("editor"), tr("Video"),
                            registry ? registry->countOfKind(MediaInfo::Video) : 0));
    av->addWidget(filterRow(QStringLiteral("speaker"), tr("Audio"),
                            registry ? registry->countOfKind(MediaInfo::Audio) : 0));
    av->addWidget(filterRow(QStringLiteral("image"), tr("Image"),
                            registry ? registry->countOfKind(MediaInfo::Image) : 0));
    av->addStretch();
    row->addWidget(aside);

    auto* main = new QWidget(this);
    auto* mv = new QVBoxLayout(main);
    mv->setContentsMargins(0, 0, 0, 0);
    mv->setSpacing(0);

    auto* toolbar = new QWidget(main);
    auto* tb = new QHBoxLayout(toolbar);
    tb->setContentsMargins(16, 12, 16, 12); tb->setSpacing(8);
    tb->addWidget(search(tr("Search media…")));
    m_countLabel = lbl(QString(), QStringLiteral("mute"), 11, false, true);
    tb->addWidget(m_countLabel);
    tb->addStretch();
    tb->addWidget(new QPushButton(Icons::icon(QStringLiteral("upload"), Theme::Text, 12), tr(" Import")));
    mv->addWidget(toolbar);
    auto* div = new QFrame; div->setObjectName(QStringLiteral("divider")); div->setFixedHeight(1);
    mv->addWidget(div);

    m_scroll = new QScrollArea(main);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    mv->addWidget(m_scroll, 1);
    row->addWidget(main, 1);

    if (m_registry)
        connect(m_registry, &MediaRegistry::changed, this, &MediaWorkspace::rebuild);
    rebuild();
}

void MediaWorkspace::rebuild() {
    const QVector<MediaInfo> media = m_registry ? m_registry->media() : QVector<MediaInfo>{};
    qint64 totalBytes = 0;
    for (const MediaInfo& m : media) totalBytes += m.sizeBytes;
    if (m_countLabel) {
        const double mb = totalBytes / (1024.0 * 1024.0);
        const QString sz = mb >= 1024.0 ? QStringLiteral("%1 GB").arg(mb / 1024.0, 0, 'f', 1)
                                        : QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
        m_countLabel->setText(tr("%1 files · %2").arg(media.size()).arg(sz));
    }

    auto* body = new QWidget;
    body->setObjectName(QStringLiteral("workspaceBody"));

    if (media.isEmpty()) {
        auto* v = new QVBoxLayout(body);
        v->setAlignment(Qt::AlignCenter); v->setSpacing(8);
        auto* icon = new QLabel;
        icon->setPixmap(Icons::pixmap(QStringLiteral("media"), Theme::TextFaint, 40));
        icon->setAlignment(Qt::AlignCenter);
        v->addWidget(icon, 0, Qt::AlignHCenter);
        auto* t = lbl(tr("No media found"), QString(), 16, true);
        t->setAlignment(Qt::AlignCenter);
        v->addWidget(t, 0, Qt::AlignHCenter);
        auto* s = lbl(tr("Video, audio, and image files in your Movies, Pictures, and Music\n"
                         "folders appear here. Recordings you save will show up automatically."),
                      QStringLiteral("mute"), 12);
        s->setAlignment(Qt::AlignCenter);
        v->addWidget(s, 0, Qt::AlignHCenter);
        m_scroll->setWidget(body);
        return;
    }

    auto* bodyV = new QVBoxLayout(body);
    bodyV->setContentsMargins(16, 16, 16, 16);
    auto* card = new QFrame; card->setObjectName(QStringLiteral("card"));
    auto* cv = new QVBoxLayout(card); cv->setContentsMargins(0, 0, 0, 0); cv->setSpacing(0);

    auto rowWidget = [&](const QString& name, const QString& type, const QString& size,
                         const QString& when, bool header) {
        auto* w = new QWidget;
        auto* g = new QGridLayout(w);
        g->setContentsMargins(12, header ? 8 : 10, 12, header ? 8 : 10);
        g->setHorizontalSpacing(8);
        g->addWidget(lbl(name, header ? QStringLiteral("mute") : QString(), header ? 11 : 13, header), 0, 0);
        g->addWidget(lbl(type, QStringLiteral("mute"), 11, false, !header), 0, 1);
        g->addWidget(lbl(size, QStringLiteral("mute"), 11, false, !header), 0, 2);
        g->addWidget(lbl(when, QStringLiteral("mute"), 11), 0, 3);
        g->setColumnStretch(0, 3); g->setColumnStretch(1, 1);
        return w;
    };
    cv->addWidget(rowWidget(tr("Name"), tr("Type"), tr("Size"), tr("Modified"), true));
    for (const MediaInfo& m : media) {
        auto* divr = new QFrame; divr->setObjectName(QStringLiteral("divider")); divr->setFixedHeight(1);
        cv->addWidget(divr);
        cv->addWidget(rowWidget(m.name,
                                m.propsText(),
                                m.sizeText(),
                                m.modified.toString(QStringLiteral("MMM d, yyyy")), false));
    }
    bodyV->addWidget(card);
    bodyV->addStretch();
    m_scroll->setWidget(body);
}

// ──────────────────────────── Projects ────────────────────────────────────
ProjectsWorkspace::ProjectsWorkspace(ProjectRegistry* registry, QWidget* parent)
    : QWidget(parent), m_registry(registry) {
    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);

    auto* toolbar = new QWidget(this);
    auto* tb = new QHBoxLayout(toolbar);
    tb->setContentsMargins(16, 12, 16, 12); tb->setSpacing(8);
    tb->addWidget(search(tr("Search projects…")));
    m_countLabel = lbl(QString(), QStringLiteral("mute"), 11, false, true);
    tb->addWidget(m_countLabel);
    tb->addStretch();
    auto* neu = new QPushButton(Icons::icon(QStringLiteral("plus"), Theme::BgBase, 12), tr(" New project"));
    Theme::setVariant(neu, QStringLiteral("primary"));
    connect(neu, &QPushButton::clicked, this, &ProjectsWorkspace::newRequested);
    tb->addWidget(neu);
    col->addWidget(toolbar);
    auto* div = new QFrame; div->setObjectName(QStringLiteral("divider")); div->setFixedHeight(1);
    col->addWidget(div);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    col->addWidget(m_scroll, 1);

    if (m_registry)
        connect(m_registry, &ProjectRegistry::changed, this, &ProjectsWorkspace::rebuild);
    rebuild();
}

void ProjectsWorkspace::rebuild() {
    const QVector<ProjectInfo> projects = m_registry ? m_registry->projects() : QVector<ProjectInfo>{};
    qint64 totalBytes = 0;
    for (const ProjectInfo& p : projects) totalBytes += p.sizeBytes;
    if (m_countLabel) {
        const double mb = totalBytes / (1024.0 * 1024.0);
        const QString sz = mb >= 1024.0 ? QStringLiteral("%1 GB").arg(mb / 1024.0, 0, 'f', 1)
                                        : QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
        m_countLabel->setText(tr("%1 projects · %2").arg(projects.size()).arg(sz));
    }

    auto* body = new QWidget;
    body->setObjectName(QStringLiteral("workspaceBody"));

    if (projects.isEmpty()) {
        auto* v = new QVBoxLayout(body);
        v->setAlignment(Qt::AlignCenter); v->setSpacing(8);
        auto* icon = new QLabel;
        icon->setPixmap(Icons::pixmap(QStringLiteral("projects"), Theme::TextFaint, 40));
        icon->setAlignment(Qt::AlignCenter);
        v->addWidget(icon, 0, Qt::AlignHCenter);
        auto* t = lbl(tr("No projects found"), QString(), 16, true);
        t->setAlignment(Qt::AlignCenter);
        v->addWidget(t, 0, Qt::AlignHCenter);
        auto* s = lbl(tr("Create one with File ▸ New, then Save.\n"
                         ".malloy.json projects in your Movies and Documents folders appear here."),
                      QStringLiteral("mute"), 12);
        s->setAlignment(Qt::AlignCenter);
        v->addWidget(s, 0, Qt::AlignHCenter);
    } else {
        auto* gl = new QGridLayout(body);
        gl->setContentsMargins(16, 16, 16, 16);
        gl->setSpacing(16);
        int i = 0;
        for (const ProjectInfo& p : projects) {
            auto* card = new QFrame; card->setObjectName(QStringLiteral("card"));
            auto* cv = new QVBoxLayout(card);
            cv->setContentsMargins(12, 12, 12, 12); cv->setSpacing(8);
            cv->addWidget(new Placeholder(p.name, 16, 9));
            cv->addWidget(lbl(p.name, QString(), 13, true));
            const QString sub = p.sceneCount >= 0
                ? tr("%n scene(s)", nullptr, p.sceneCount) : tr("MalloyStudio project");
            cv->addWidget(lbl(sub, QStringLiteral("mute"), 11));
            auto* metaRow = new QHBoxLayout;
            metaRow->addWidget(lbl(p.sizeText(), QStringLiteral("mute"), 10, false, true));
            metaRow->addStretch();
            metaRow->addWidget(lbl(p.modified.toString(QStringLiteral("MMM d, yyyy")), QStringLiteral("mute"), 10));
            cv->addLayout(metaRow);
            auto* btns = new QHBoxLayout; btns->setSpacing(4);
            auto* open = new QPushButton(Icons::icon(QStringLiteral("play"), Theme::Text, 11), tr(" Open"));
            const QString path = p.filePath;
            connect(open, &QPushButton::clicked, this, [this, path] { emit openRequested(path); });
            btns->addWidget(open, 1);
            cv->addLayout(btns);
            gl->addWidget(card, i / 3, i % 3);
            ++i;
        }
        gl->setRowStretch((i + 2) / 3, 1);
    }
    m_scroll->setWidget(body);
}

// ─────────────────────────── Render Queue ─────────────────────────────────
RenderWorkspace::RenderWorkspace(QWidget* parent) : QWidget(parent) {
    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);

    auto* toolbar = new QWidget(this);
    auto* tb = new QHBoxLayout(toolbar);
    tb->setContentsMargins(16, 12, 16, 12); tb->setSpacing(8);
    for (const QString& t : {tr("Active 2"), tr("Pending 3"), tr("Completed 47"), tr("Failed 1")}) {
        auto* b = new QPushButton(t);
        Theme::setVariant(b, QStringLiteral("ghost"));
        tb->addWidget(b);
    }
    tb->addStretch();
    tb->addWidget(new QPushButton(Icons::icon(QStringLiteral("plus"), Theme::Text, 12), tr(" New render")));
    col->addWidget(toolbar);
    auto* div = new QFrame; div->setObjectName(QStringLiteral("divider")); div->setFixedHeight(1);
    col->addWidget(div);

    auto* body = new QWidget;
    body->setObjectName(QStringLiteral("workspaceBody"));
    auto* bv = new QVBoxLayout(body);
    bv->setContentsMargins(16, 16, 16, 16);
    bv->setSpacing(12);

    bv->addWidget(Theme::makeSectionHeader(tr("Active")));
    auto activeJob = [&](const QString& name, const QString& meta, int pct, const QString& eta) {
        auto* card = new QFrame; card->setObjectName(QStringLiteral("card"));
        auto* cv = new QVBoxLayout(card); cv->setContentsMargins(14, 14, 14, 14); cv->setSpacing(8);
        auto* top = new QHBoxLayout;
        auto* tv = new QVBoxLayout; tv->setSpacing(2);
        tv->addWidget(lbl(name, QString(), 13, true));
        tv->addWidget(lbl(meta, QStringLiteral("mute"), 11, false, true));
        top->addLayout(tv, 1);
        auto* pv = new QVBoxLayout; pv->setSpacing(0);
        pv->addWidget(lbl(QStringLiteral("%1%").arg(pct), QStringLiteral("accent"), 14, true, true), 0, Qt::AlignRight);
        pv->addWidget(lbl(tr("ETA %1").arg(eta), QStringLiteral("mute"), 11, false, true), 0, Qt::AlignRight);
        top->addLayout(pv);
        cv->addLayout(top);
        auto* bar = new QProgressBar; bar->setRange(0, 100); bar->setValue(pct);
        bar->setTextVisible(false); bar->setFixedHeight(6);
        cv->addWidget(bar);
        return card;
    };
    bv->addWidget(activeJob(tr("Ep 14 — Highlights.mp4"), tr("Spire · 1080p60 · NVENC · 24 Mb/s"), 64, QStringLiteral("3:42")));
    bv->addWidget(activeJob(tr("Ep 14 — Full episode.mp4"), tr("Spire · 1080p60 · NVENC · 18 Mb/s"), 12, QStringLiteral("14:08")));

    bv->addWidget(Theme::makeSectionHeader(tr("Failed")));
    auto* fail = new QFrame; fail->setObjectName(QStringLiteral("card"));
    fail->setStyleSheet(QStringLiteral("QFrame#card{background: rgba(252,68,71,0.06); border-color: rgba(252,68,71,0.45);}"));
    auto* fh = new QHBoxLayout(fail); fh->setContentsMargins(12, 12, 12, 12); fh->setSpacing(10);
    auto* fi = new QLabel; fi->setPixmap(Icons::pixmap(QStringLiteral("alert"), Theme::RecHi, 16));
    fh->addWidget(fi);
    auto* fv = new QVBoxLayout; fv->setSpacing(2);
    fv->addWidget(lbl(tr("Ep 14 — Director's cut.mp4"), QString(), 13, true));
    fv->addWidget(lbl(tr("ffmpeg exited with code 234 · libx264 failed at frame 412,108"), QStringLiteral("mute"), 11, false, true));
    fh->addLayout(fv, 1);
    fh->addWidget(ghost(tr("View log")));
    fh->addWidget(new QPushButton(Icons::icon(QStringLiteral("refresh"), Theme::Text, 11), tr(" Retry")));
    bv->addWidget(fail);

    bv->addWidget(Theme::makeSectionHeader(tr("Recently completed")));
    auto* done = new QFrame; done->setObjectName(QStringLiteral("card"));
    auto* dv = new QVBoxLayout(done); dv->setContentsMargins(0, 0, 0, 0); dv->setSpacing(0);
    struct D { QString name, dur, size, when; };
    const QVector<D> dones = {
        {tr("Ep 13 — Highlights.mp4"), QStringLiteral("22:18 · 1080p60"), QStringLiteral("1.4 GB"), tr("Yesterday 10:42 PM")},
        {tr("Ep 13 — Full.mp4"), QStringLiteral("1:51:00 · 1080p60"), QStringLiteral("4.2 GB"), tr("Yesterday 10:08 PM")},
        {tr("Vlog — Week 21.mp4"), QStringLiteral("14:12 · 1080p30"), QStringLiteral("880 MB"), tr("Mon 3:18 PM")},
    };
    for (int r = 0; r < dones.size(); ++r) {
        if (r) { auto* d2 = new QFrame; d2->setObjectName(QStringLiteral("divider")); d2->setFixedHeight(1); dv->addWidget(d2); }
        const D& d = dones[r];
        auto* w = new QWidget; auto* h = new QHBoxLayout(w); h->setContentsMargins(12, 10, 12, 10); h->setSpacing(10);
        auto* ck = new QLabel; ck->setPixmap(Icons::pixmap(QStringLiteral("check"), Theme::Success, 14));
        h->addWidget(ck);
        h->addWidget(lbl(d.name, QString(), 13));
        h->addStretch();
        h->addWidget(lbl(d.dur, QStringLiteral("mute"), 11, false, true));
        h->addWidget(lbl(d.size, QStringLiteral("mute"), 11, false, true));
        h->addWidget(lbl(d.when, QStringLiteral("mute"), 11));
        dv->addWidget(w);
    }
    bv->addWidget(done);
    bv->addStretch();
    col->addWidget(scrollArea(body), 1);
}
