#include "ui/workspaces/LibraryWorkspaces.h"
#include "ui/components/PanelFrame.h"
#include "ui/components/Placeholder.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"

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
ClipsWorkspace::ClipsWorkspace(QWidget* parent) : QWidget(parent) {
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);

    // Left filters
    auto* aside = new QWidget(this);
    aside->setObjectName(QStringLiteral("recSideCol"));
    aside->setFixedWidth(240);
    auto* av = new QVBoxLayout(aside);
    av->setContentsMargins(6, 8, 6, 8);
    av->setSpacing(2);
    av->addWidget(Theme::makeSectionHeader(tr("Smart filters")));
    av->addWidget(filterRow(QStringLiteral("clips"), tr("All clips"), 42, true));
    av->addWidget(filterRow(QStringLiteral("star"), tr("Favorites"), 7));
    av->addWidget(filterRow(QStringLiteral("clips"), tr("Last session"), 12));
    av->addWidget(filterRow(QStringLiteral("folder"), tr("Archived"), 18));
    av->addWidget(Theme::makeSectionHeader(tr("Source")));
    av->addWidget(filterRow(QStringLiteral("folder"), tr("Spire of the Hollow Sun"), 18));
    av->addWidget(filterRow(QStringLiteral("folder"), tr("Coding Sessions"), 9));
    av->addWidget(filterRow(QStringLiteral("folder"), tr("Tuesday Vlog"), 7));
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
    tb->addWidget(lbl(tr("42 clips · 3.2 GB"), QStringLiteral("mute"), 11, false, true));
    tb->addStretch();
    auto* exp = new QPushButton(Icons::icon(QStringLiteral("upload"), Theme::Text, 12), tr(" Export selected"));
    tb->addWidget(exp);
    mv->addWidget(toolbar);
    auto* div = new QFrame; div->setObjectName(QStringLiteral("divider")); div->setFixedHeight(1);
    mv->addWidget(div);

    auto* grid = new QWidget;
    grid->setObjectName(QStringLiteral("workspaceBody"));
    auto* gl = new QGridLayout(grid);
    gl->setContentsMargins(16, 16, 16, 16);
    gl->setSpacing(14);
    struct C { QString title, src, when; bool fav; };
    const QVector<C> clips = {
        {tr("No-hit boss phase 3"), tr("Spire"), tr("Today, 1h ago"), true},
        {tr("Dodge into riposte"), tr("Spire"), tr("Today, 1h ago"), false},
        {tr("Boss death animation"), tr("Spire"), tr("Today, 2h ago"), true},
        {tr("Bit about coffee"), tr("Vlog"), tr("Yesterday"), false},
        {tr("Refactor reveal"), tr("Coding"), tr("Yesterday"), false},
        {tr("Death — corrupted run"), tr("Spire"), tr("2 days"), false},
        {tr("Reaction — first boss"), tr("Spire"), tr("3 days"), true},
        {tr("Tutorial — vim binding"), tr("Coding"), tr("1 week"), true},
    };
    int i = 0;
    for (const C& c : clips) {
        auto* cell = new QWidget;
        auto* cv = new QVBoxLayout(cell);
        cv->setContentsMargins(0, 0, 0, 0); cv->setSpacing(6);
        cv->addWidget(new Placeholder(c.title, 16, 9));
        cv->addWidget(lbl(c.title, QString(), 12, true));
        auto* meta = new QHBoxLayout; meta->setSpacing(6);
        meta->addWidget(lbl(QStringLiteral("%1 · %2").arg(c.src, c.when), QStringLiteral("mute"), 10, false, true));
        meta->addStretch();
        if (c.fav) { auto* s = new QLabel; s->setPixmap(Icons::pixmap(QStringLiteral("star"), Theme::Warn, 12)); meta->addWidget(s); }
        cv->addLayout(meta);
        gl->addWidget(cell, i / 4, i % 4);
        ++i;
    }
    gl->setRowStretch((i + 3) / 4, 1);
    mv->addWidget(scrollArea(grid), 1);
    row->addWidget(main, 1);
}

// ───────────────────────────── Media ──────────────────────────────────────
MediaWorkspace::MediaWorkspace(QWidget* parent) : QWidget(parent) {
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);

    auto* aside = new QWidget(this);
    aside->setObjectName(QStringLiteral("recSideCol"));
    aside->setFixedWidth(240);
    auto* av = new QVBoxLayout(aside);
    av->setContentsMargins(6, 8, 6, 8);
    av->setSpacing(2);
    av->addWidget(Theme::makeSectionHeader(tr("Folders")));
    av->addWidget(filterRow(QStringLiteral("folder"), tr("All media"), 284, true));
    av->addWidget(filterRow(QStringLiteral("folder"), tr("Footage"), 142));
    av->addWidget(filterRow(QStringLiteral("folder"), tr("Audio"), 48));
    av->addWidget(filterRow(QStringLiteral("folder"), tr("Images"), 62));
    av->addWidget(filterRow(QStringLiteral("folder"), tr("Projects"), 32));
    av->addWidget(Theme::makeSectionHeader(tr("File types")));
    av->addWidget(filterRow(QStringLiteral("media"), tr("Video"), 142));
    av->addWidget(filterRow(QStringLiteral("speaker"), tr("Audio"), 48));
    av->addWidget(filterRow(QStringLiteral("image"), tr("Image"), 62));
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
    tb->addWidget(lbl(tr("284 files · 412 GB"), QStringLiteral("mute"), 11, false, true));
    tb->addWidget(Theme::makeTag(tr("1 missing"), QStringLiteral("warn")));
    tb->addStretch();
    tb->addWidget(new QPushButton(Icons::icon(QStringLiteral("upload"), Theme::Text, 12), tr(" Import")));
    mv->addWidget(toolbar);

    // Missing media banner
    auto* banner = new QFrame(main);
    banner->setStyleSheet(QStringLiteral("background: rgba(245,174,57,0.08); border: none; border-bottom: 1px solid rgba(245,174,57,0.4);"));
    auto* bh = new QHBoxLayout(banner);
    bh->setContentsMargins(16, 10, 16, 10); bh->setSpacing(8);
    auto* warn = new QLabel; warn->setPixmap(Icons::pixmap(QStringLiteral("alert"), Theme::Warn, 14));
    bh->addWidget(warn);
    bh->addWidget(lbl(tr("One file is missing — Thumbnail — final.psd, referenced by 1 project."), QStringLiteral("dim"), 12));
    bh->addStretch();
    bh->addWidget(new QPushButton(tr("Locate…")));
    bh->addWidget(ghost(tr("Skip")));
    mv->addWidget(banner);

    // Table (manual: header + rows in a card)
    auto* body = new QWidget;
    body->setObjectName(QStringLiteral("workspaceBody"));
    auto* bodyV = new QVBoxLayout(body);
    bodyV->setContentsMargins(16, 16, 16, 16);
    auto* card = new QFrame; card->setObjectName(QStringLiteral("card"));
    auto* cv = new QVBoxLayout(card); cv->setContentsMargins(0, 0, 0, 0); cv->setSpacing(0);

    auto makeRow = [&](const QString& name, const QString& props, const QString& size,
                       const QString& used, const QString& when, bool header, bool missing) {
        auto* w = new QWidget;
        auto* g = new QGridLayout(w);
        g->setContentsMargins(12, header ? 8 : 10, 12, header ? 8 : 10);
        g->setHorizontalSpacing(8);
        const QString tone = header ? QStringLiteral("mute") : (missing ? QStringLiteral("warn") : QString());
        auto* n = lbl(name, tone, header ? 11 : 13, header);
        auto* p = lbl(props, header ? QStringLiteral("mute") : QStringLiteral("mute"), 11, false, !header);
        auto* s = lbl(size, QStringLiteral("mute"), 11, false, !header);
        auto* u = lbl(used, header ? QStringLiteral("mute") : QString(), 11);
        auto* m = lbl(when, QStringLiteral("mute"), 11);
        g->addWidget(n, 0, 0); g->addWidget(p, 0, 1); g->addWidget(s, 0, 2);
        g->addWidget(u, 0, 3); g->addWidget(m, 0, 4);
        g->setColumnStretch(0, 3); g->setColumnStretch(1, 2);
        return w;
    };
    cv->addWidget(makeRow(tr("Name"), tr("Properties"), tr("Size"), tr("Used"), tr("Modified"), true, false));
    struct M { QString name, props, size, used, when; bool missing; };
    const QVector<M> media = {
        {tr("Spire — Ep 14 raw.mkv"), QStringLiteral("1920×1080 60p"), QStringLiteral("14.2 GB"), QStringLiteral("●"), tr("1h ago"), false},
        {tr("Webcam α7C — Ep 14.mkv"), QStringLiteral("1920×1080 60p"), QStringLiteral("4.8 GB"), QStringLiteral("●"), tr("1h ago"), false},
        {tr("Mic — SM7B (cleaned).wav"), QStringLiteral("48 kHz · Stereo"), QStringLiteral("1.1 GB"), QStringLiteral("●"), tr("45 min ago"), false},
        {tr("Intro card v3.mp4"), QStringLiteral("3840×2160 60p"), QStringLiteral("92 MB"), QStringLiteral("●"), tr("Feb 28"), false},
        {tr("B-roll — sunset.mp4"), QStringLiteral("3840×2160 30p"), QStringLiteral("220 MB"), tr("unused"), tr("Feb 12"), false},
        {tr("Thumbnail — final.psd"), QStringLiteral("1920×1080"), QStringLiteral("24 MB"), tr("missing"), tr("Feb 02"), true},
    };
    for (int r = 0; r < media.size(); ++r) {
        auto* divr = new QFrame; divr->setObjectName(QStringLiteral("divider")); divr->setFixedHeight(1);
        cv->addWidget(divr);
        const M& m = media[r];
        cv->addWidget(makeRow(m.name, m.props, m.size, m.used, m.when, false, m.missing));
    }
    bodyV->addWidget(card);
    bodyV->addStretch();
    mv->addWidget(scrollArea(body), 1);
    row->addWidget(main, 1);
}

// ──────────────────────────── Projects ────────────────────────────────────
ProjectsWorkspace::ProjectsWorkspace(QWidget* parent) : QWidget(parent) {
    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);

    auto* toolbar = new QWidget(this);
    auto* tb = new QHBoxLayout(toolbar);
    tb->setContentsMargins(16, 12, 16, 12); tb->setSpacing(8);
    tb->addWidget(search(tr("Search projects…")));
    tb->addWidget(lbl(tr("6 projects · 305 GB"), QStringLiteral("mute"), 11, false, true));
    tb->addStretch();
    auto* neu = new QPushButton(Icons::icon(QStringLiteral("plus"), Theme::BgBase, 12), tr(" New project"));
    Theme::setVariant(neu, QStringLiteral("primary"));
    tb->addWidget(neu);
    col->addWidget(toolbar);
    auto* div = new QFrame; div->setObjectName(QStringLiteral("divider")); div->setFixedHeight(1);
    col->addWidget(div);

    auto* body = new QWidget;
    body->setObjectName(QStringLiteral("workspaceBody"));
    auto* gl = new QGridLayout(body);
    gl->setContentsMargins(16, 16, 16, 16);
    gl->setSpacing(16);
    struct P { QString name, sub, cover, meta, date; bool current; };
    const QVector<P> projects = {
        {tr("Spire of the Hollow Sun"), tr("Stream series · 14 eps"), tr("Gameplay · Spire"), tr("142 GB · 14 sessions"), tr("2 hr ago"), true},
        {tr("Tuesday Vlog — Week 22"), tr("Vlog · weekly"), tr("Webcam · A-roll"), tr("38 GB · 7 sessions"), tr("Yesterday"), false},
        {tr("Coding Sessions · S3"), tr("Coding series · 6 eps"), tr("Editor capture"), tr("88 GB · 6 sessions"), tr("4 days"), false},
        {tr("Boss Rush — Compilation"), tr("One-off · 30 min"), tr("Final cut · v3"), tr("21 GB · 1 session"), tr("2 weeks"), false},
        {tr("Game Dev Diary · Pilot"), tr("Pilot · 12 min"), tr("Concept"), tr("4 GB · 2 sessions"), tr("1 month"), false},
        {tr("Late-night Tutorial #07"), tr("Tutorial · 22 min"), tr("Talking head"), tr("12 GB · 3 sessions"), tr("3 months"), false},
    };
    int i = 0;
    for (const P& p : projects) {
        auto* card = new QFrame; card->setObjectName(QStringLiteral("card"));
        auto* cv = new QVBoxLayout(card);
        cv->setContentsMargins(12, 12, 12, 12); cv->setSpacing(8);
        cv->addWidget(new Placeholder(p.cover, 16, 9));
        auto* nameRow = new QHBoxLayout; nameRow->setSpacing(6);
        nameRow->addWidget(lbl(p.name, QString(), 13, true));
        if (p.current) nameRow->addWidget(Theme::makeTag(tr("Current"), QStringLiteral("accent")));
        nameRow->addStretch();
        cv->addLayout(nameRow);
        cv->addWidget(lbl(p.sub, QStringLiteral("mute"), 11));
        auto* metaRow = new QHBoxLayout;
        metaRow->addWidget(lbl(p.meta, QStringLiteral("mute"), 10, false, true));
        metaRow->addStretch();
        metaRow->addWidget(lbl(p.date, QStringLiteral("mute"), 10));
        cv->addLayout(metaRow);
        auto* btns = new QHBoxLayout; btns->setSpacing(4);
        auto* open = new QPushButton(Icons::icon(QStringLiteral("play"), Theme::Text, 11), tr(" Open"));
        btns->addWidget(open, 1);
        btns->addWidget(ghost(QString(), QStringLiteral("folder")));
        cv->addLayout(btns);
        gl->addWidget(card, i / 3, i % 3);
        ++i;
    }
    gl->setRowStretch((i + 2) / 3, 1);
    col->addWidget(scrollArea(body), 1);
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
