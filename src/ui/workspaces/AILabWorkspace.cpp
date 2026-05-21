#include "ui/workspaces/AILabWorkspace.h"
#include "ui/IconFactory.h"
#include "ui/Theme.h"

#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace {
QLabel* lbl(const QString& s, const QString& tone = QString(), int px = 13, bool bold = false) {
    auto* l = new QLabel(s);
    if (!tone.isEmpty()) l->setProperty("tone", tone);
    QFont f = l->font(); f.setPixelSize(px); if (bold) f.setWeight(QFont::DemiBold);
    l->setFont(f);
    return l;
}
} // namespace

AILabWorkspace::AILabWorkspace(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("workspaceBody"));
    auto* pageRow = new QHBoxLayout(page);
    pageRow->setContentsMargins(32, 32, 32, 32);

    auto* content = new QWidget;
    content->setMaximumWidth(960);
    auto* col = new QVBoxLayout(content);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(24);

    // Hero
    auto* hero = new QHBoxLayout;
    hero->setSpacing(16);
    auto* badge = new QFrame;
    badge->setObjectName(QStringLiteral("iconChip"));
    badge->setFixedSize(56, 56);
    { auto* bl = new QVBoxLayout(badge); bl->setContentsMargins(0,0,0,0);
      auto* bi = new QLabel; bi->setAlignment(Qt::AlignCenter);
      bi->setPixmap(Icons::pixmap(QStringLiteral("ai"), Theme::AccentHi, 28)); bl->addWidget(bi); }
    hero->addWidget(badge, 0, Qt::AlignTop);

    auto* heroText = new QVBoxLayout;
    heroText->setSpacing(6);
    auto* tagRow = new QHBoxLayout; tagRow->setSpacing(8);
    tagRow->addWidget(Theme::makeTag(tr("Preview"), QStringLiteral("accent")));
    tagRow->addWidget(lbl(tr("Planned for v0.3 · Q3 2026"), QStringLiteral("mute"), 12));
    tagRow->addStretch();
    heroText->addLayout(tagRow);
    auto* title = lbl(tr("AI Lab"), QString(), 28, true);
    title->setObjectName(QStringLiteral("heroTitle"));
    heroText->addWidget(title);
    auto* desc = lbl(tr("A workshop for clip detection, silence removal, subtitles, chapters, thumbnails, "
                        "and conversational editing. Nothing here ships yet — the surfaces are wired early."),
                     QStringLiteral("dim"), 13);
    desc->setWordWrap(true); desc->setMaximumWidth(560);
    heroText->addWidget(desc);
    auto* btns = new QHBoxLayout; btns->setSpacing(8);
    auto* notify = new QPushButton(Icons::icon(QStringLiteral("bell"), Theme::BgBase, 12), tr(" Notify me when AI ships"));
    Theme::setVariant(notify, QStringLiteral("primary"));
    auto* road = new QPushButton(tr("Read the roadmap"));
    Theme::setVariant(road, QStringLiteral("ghost"));
    btns->addWidget(notify); btns->addWidget(road); btns->addStretch();
    heroText->addLayout(btns);
    hero->addLayout(heroText, 1);
    col->addLayout(hero);

    // Feature cards
    struct F { QString icon, name, desc; };
    const QVector<F> feats = {
        {QStringLiteral("scissors"), tr("Smart clip detection"), tr("Auto-detect highlights from gameplay audio + game events.")},
        {QStringLiteral("mic"),      tr("Silence removal"),      tr("One-pass dialogue cleanup across the timeline.")},
        {QStringLiteral("text"),     tr("Auto subtitles"),       tr("Whisper-driven transcription + speaker diarization.")},
        {QStringLiteral("sparkle"),  tr("Auto chapters"),        tr("Timeline markers + chapter list from your content.")},
        {QStringLiteral("image"),    tr("Thumbnail variations"), tr("Generate 4–6 thumbnail directions from the best frame.")},
        {QStringLiteral("bolt"),     tr("Title & description ideas"), tr("Pull a hooky title from your transcript + tags.")},
        {QStringLiteral("editor"),   tr("Editing assistant"),    tr("Conversational edit: \"tighten the intro, drop dead air\".")},
        {QStringLiteral("stream"),   tr("Stream copilot"),       tr("Suggests scene changes during slow moments.")},
        {QStringLiteral("projects"), tr("Project organizer"),    tr("Group orphaned clips into draft projects.")},
        {QStringLiteral("clips"),    tr("Repurposing engine"),   tr("Cut shorts/reels from your long-form episodes.")},
    };
    auto* grid = new QGridLayout;
    grid->setSpacing(12);
    int i = 0;
    for (const F& f : feats) {
        auto* c = new QFrame; c->setObjectName(QStringLiteral("card"));
        auto* h = new QHBoxLayout(c);
        h->setContentsMargins(14, 14, 14, 14);
        h->setSpacing(12);
        auto* chip = new QFrame; chip->setObjectName(QStringLiteral("iconChip")); chip->setFixedSize(32, 32);
        { auto* cl = new QVBoxLayout(chip); cl->setContentsMargins(0,0,0,0);
          auto* ci = new QLabel; ci->setAlignment(Qt::AlignCenter);
          ci->setPixmap(Icons::pixmap(f.icon, Theme::TextDim, 14)); cl->addWidget(ci); }
        h->addWidget(chip, 0, Qt::AlignTop);
        auto* tv = new QVBoxLayout; tv->setSpacing(3);
        auto* nameRow = new QHBoxLayout;
        nameRow->addWidget(lbl(f.name, QString(), 13, true));
        nameRow->addStretch();
        nameRow->addWidget(Theme::makeTag(tr("Soon")));
        tv->addLayout(nameRow);
        auto* d = lbl(f.desc, QStringLiteral("mute"), 11); d->setWordWrap(true);
        tv->addWidget(d);
        h->addLayout(tv, 1);
        grid->addWidget(c, i / 2, i % 2);
        ++i;
    }
    col->addLayout(grid);

    // Local-first stack note
    auto* stack = new QFrame; stack->setObjectName(QStringLiteral("card"));
    auto* sv = new QVBoxLayout(stack);
    sv->setContentsMargins(14, 12, 14, 14);
    sv->setSpacing(6);
    sv->addWidget(Theme::makeSectionHeader(tr("Local-first stack")));
    auto* note = lbl(tr("AI tasks run locally via Whisper (subtitles), CLIP/BLIP (thumbnails), and a small "
                        "fine-tuned LLM for titles & chapters. Cloud is optional and opt-in."),
                     QStringLiteral("dim"), 12);
    note->setWordWrap(true);
    sv->addWidget(note);
    col->addWidget(stack);
    col->addStretch();

    pageRow->addWidget(content, 1);
    scroll->setWidget(page);
}
