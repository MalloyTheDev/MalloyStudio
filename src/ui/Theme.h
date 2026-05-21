#pragma once

// MalloyStudio design tokens + widget helpers.
//
// The oklch tokens from MalloyStudioJS/tokens.css are baked to sRGB QColors
// here so C++ painting code (VU meters, custom-drawn rows, etc.) shares the
// exact same palette as the global stylesheet (resources/styles/dark.qss).

#include <QColor>
#include <QString>

class QApplication;
class QLabel;
class QPushButton;
class QWidget;

namespace Theme {

// ── Token colors (charcoal bg, blue accent — the default variant) ──────────
inline const QColor BgBase       {0x0d, 0x0e, 0x11};
inline const QColor Surface0     {0x14, 0x15, 0x18};
inline const QColor Surface1     {0x1b, 0x1c, 0x1f};
inline const QColor Surface2     {0x23, 0x24, 0x27};
inline const QColor Surface3     {0x2d, 0x2e, 0x31};
inline const QColor Border       {0x2d, 0x2f, 0x33};
inline const QColor BorderStrong {0x41, 0x44, 0x49};
inline const QColor Text         {0xf7, 0xf7, 0xf7};
inline const QColor TextDim      {0xae, 0xb1, 0xb8};
inline const QColor TextMute     {0x70, 0x73, 0x7a};
inline const QColor TextFaint    {0x4b, 0x4d, 0x53};
inline const QColor Accent       {0x57, 0x9b, 0xff};
inline const QColor AccentHi     {0x70, 0xb5, 0xff};
inline const QColor AccentDim    {0x2b, 0x64, 0xc3};
inline const QColor Rec          {0xfc, 0x44, 0x47};
inline const QColor RecHi        {0xff, 0x62, 0x5f};
inline const QColor Success      {0x51, 0xc6, 0x72};
inline const QColor Warn         {0xf5, 0xae, 0x39};
inline const QColor MeterGreen   {0x32, 0xc3, 0x64};
inline const QColor MeterYellow  {0xe9, 0xc1, 0x00};
inline const QColor MeterRed     {0xfc, 0x44, 0x47};

// ── App-level setup ────────────────────────────────────────────────────────
QString loadStyleSheet();                 // reads :/styles/dark.qss
void    applyTheme(QApplication& app);    // palette + font + stylesheet

// ── Dynamic-property helpers ────────────────────────────────────────────────
// Qt resolves [prop="value"] selectors at polish time; changing a property at
// runtime needs an explicit repolish() to take effect.
void setProp(QWidget* w, const char* name, const QVariant& value);
void repolish(QWidget* w);

void setMono(QWidget* w, bool on = true);
void setTone(QWidget* w, const QString& tone);          // dim/mute/faint/accent/rec/success/warn
void setVariant(QPushButton* b, const QString& variant); // primary/rec/ghost/outlineRec

// ── Widget factories matching the prototype primitives ──────────────────────
QLabel* makeTag(const QString& text, const QString& tone = QString(), QWidget* parent = nullptr);
QLabel* makeKbd(const QString& text, QWidget* parent = nullptr);
QLabel* makeSectionHeader(const QString& text, QWidget* parent = nullptr);

} // namespace Theme
