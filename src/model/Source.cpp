#include "Source.h"
#include <algorithm>

Source::Source(int id, const QString& name, Type type, QObject* parent)
    : QObject(parent), m_id(id), m_name(name), m_type(type)
{
    if (m_type == Type::Text) {
        m_text = QStringLiteral("MalloyStudio");
    }
}

Source::Source(const QString& name, Type type, QObject* parent)
    : Source(InvalidId, name, type, parent)
{}

void Source::setIdForLoad(int id) {
    if (id <= InvalidId || m_id == id) return;
    m_id = id;
    emit idChanged();
}

void Source::setName(const QString& name) {
    if (m_name == name) return;
    m_name = name;
    emit nameChanged();
}

void Source::setText(const QString& text) {
    if (m_text == text) return;
    m_text = text;
    emit settingsChanged();
}

void Source::setColor(const QColor& color) {
    if (m_color == color) return;
    m_color = color;
    emit settingsChanged();
}

void Source::setMonitor(int adapterIndex, int outputIndex) {
    if (m_adapterIndex == adapterIndex && m_outputIndex == outputIndex) return;
    m_adapterIndex = adapterIndex;
    m_outputIndex  = outputIndex;
    emit monitorChanged();
}

void Source::setImagePath(const QString& path) {
    if (m_imagePath == path) return;
    m_imagePath = path;
    emit settingsChanged();
}

void Source::setWindow(quintptr hwnd, const QString& title) {
    if (m_hwnd == hwnd && m_windowTitle == title) return;
    m_hwnd = hwnd;
    m_windowTitle = title;
    emit settingsChanged();
}

void Source::setAudioDeviceId(const QString& id) {
    if (m_audioDeviceId == id) return;
    m_audioDeviceId = id;
    emit settingsChanged();
}

void Source::setBrowserUrl(const QString& url) {
    if (m_browserUrl == url) return;
    m_browserUrl = url;
    emit settingsChanged();
}

void Source::setBrowserRefreshHz(int hz) {
    hz = std::max(1, std::min(60, hz));
    if (m_browserRefreshHz == hz) return;
    m_browserRefreshHz = hz;
    emit settingsChanged();
}

QString Source::typeToString(Type t) {
    switch (t) {
        case Type::DisplayCapture: return QStringLiteral("Display Capture");
        case Type::Image:          return QStringLiteral("Image");
        case Type::Text:           return QStringLiteral("Text");
        case Type::ColorBlock:     return QStringLiteral("Color Block");
        case Type::Browser:        return QStringLiteral("Browser");
        case Type::WindowCapture:  return QStringLiteral("Window Capture");
        case Type::AudioInput:     return QStringLiteral("Audio Input");
    }
    return QStringLiteral("Unknown");
}

QString Source::typeToId(Type t) {
    switch (t) {
        case Type::DisplayCapture: return QStringLiteral("display_capture");
        case Type::Image:          return QStringLiteral("image");
        case Type::Text:           return QStringLiteral("text");
        case Type::ColorBlock:     return QStringLiteral("color_block");
        case Type::Browser:        return QStringLiteral("browser");
        case Type::WindowCapture:  return QStringLiteral("window_capture");
        case Type::AudioInput:     return QStringLiteral("audio_input");
    }
    return QStringLiteral("unknown");
}

Source::Type Source::typeFromId(const QString& id, bool* ok) {
    const QString normalized = id.trimmed().toLower();
    if (ok) *ok = true;
    if (normalized == QStringLiteral("display_capture")) return Type::DisplayCapture;
    if (normalized == QStringLiteral("image")) return Type::Image;
    if (normalized == QStringLiteral("text")) return Type::Text;
    if (normalized == QStringLiteral("color_block")) return Type::ColorBlock;
    if (normalized == QStringLiteral("browser")) return Type::Browser;
    if (normalized == QStringLiteral("window_capture")) return Type::WindowCapture;
    if (normalized == QStringLiteral("audio_input")) return Type::AudioInput;
    if (ok) *ok = false;
    return Type::ColorBlock;
}
