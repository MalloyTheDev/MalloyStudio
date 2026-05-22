#pragma once
#include <QObject>
#include <QString>
#include <QColor>

class Source : public QObject {
    Q_OBJECT
public:
    static constexpr int InvalidId = 0;

    enum class Type {
        DisplayCapture,
        Image,
        Text,
        ColorBlock,
        Browser,
        WindowCapture,  // Captures a specific application window (HWND via PrintWindow/BitBlt)
        AudioInput,     // Audio-only source; links a device to the AudioController mixing bus
        Camera,         // Webcam / capture-card video via MediaFoundation IMFSourceReader
    };
    Q_ENUM(Type)

    explicit Source(int id, const QString& name, Type type, QObject* parent = nullptr);
    explicit Source(const QString& name, Type type, QObject* parent = nullptr);

    int     id() const { return m_id; }
    QString name() const { return m_name; }
    Type    type() const { return m_type; }
    void    setName(const QString& name);

    QString text() const { return m_text; }
    void    setText(const QString& text);

    QColor color() const { return m_color; }
    void   setColor(const QColor& color);

    // Only meaningful when type == DisplayCapture.
    int  adapterIndex() const { return m_adapterIndex; }
    int  outputIndex()  const { return m_outputIndex; }
    void setMonitor(int adapterIndex, int outputIndex);
    bool hasMonitorConfig() const { return m_adapterIndex >= 0; }

    // Only meaningful when type == Image.
    QString imagePath() const { return m_imagePath; }
    void    setImagePath(const QString& path);

    // Only meaningful when type == WindowCapture.
    quintptr windowHandle() const { return m_hwnd; }
    QString  windowTitle()  const { return m_windowTitle; }
    void     setWindow(quintptr hwnd, const QString& title);
    bool     hasWindowConfig() const { return m_hwnd != 0; }

    // Only meaningful when type == AudioInput.
    QString audioDeviceId() const { return m_audioDeviceId; }
    void    setAudioDeviceId(const QString& id);

    // Only meaningful when type == Camera. deviceId is the MediaFoundation
    // symbolic link; name is the friendly device name (for display).
    QString cameraDeviceId() const { return m_cameraDeviceId; }
    QString cameraName()     const { return m_cameraName; }
    void    setCamera(const QString& deviceId, const QString& name);
    bool    hasCameraConfig() const { return !m_cameraDeviceId.isEmpty(); }

    // Only meaningful when type == Browser.
    // Qt6::WebEngineWidgets is required for actual rendering; when it is
    // unavailable the source displays a blank frame.
    QString browserUrl()       const { return m_browserUrl; }
    int     browserRefreshHz() const { return m_browserRefreshHz; }
    void    setBrowserUrl(const QString& url);
    void    setBrowserRefreshHz(int hz);
    bool    hasBrowserConfig() const { return !m_browserUrl.isEmpty(); }

    static QString typeToString(Type t);
    static QString typeToId(Type t);
    static Type typeFromId(const QString& id, bool* ok = nullptr);

signals:
    void idChanged();
    void nameChanged();
    void settingsChanged();
    void monitorChanged();

private:
    void setIdForLoad(int id);
    friend class SceneCollection;

    int      m_id = InvalidId;
    QString  m_name;
    Type     m_type;
    QString  m_text;
    QColor   m_color = QColor(46, 136, 255);
    int      m_adapterIndex = -1;
    int      m_outputIndex  =  0;
    QString  m_imagePath;
    quintptr m_hwnd = 0;
    QString  m_windowTitle;
    QString  m_audioDeviceId;
    QString  m_cameraDeviceId;
    QString  m_cameraName;
    QString  m_browserUrl;
    int      m_browserRefreshHz = 10;
};
