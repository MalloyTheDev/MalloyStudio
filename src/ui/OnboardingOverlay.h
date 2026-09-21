#pragma once

#include <QString>
#include <QStringList>
#include <QWidget>

class AudioController;
class QButtonGroup;
class QFrame;
class QLabel;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;
struct OutputSettings;

// First-run setup wizard (secondary.jsx Onboarding): a dim full-window overlay
// with a centered card stepping through welcome → folder → devices → quality →
// done. Everything it shows is read from this machine when it opens, and the
// last step saves what was chosen: the recording folder and the quality.
// Closing or skipping it saves nothing.
class OnboardingOverlay : public QWidget {
    Q_OBJECT
public:
    // What the devices step lists. Reading it opens no device: microphones are
    // the plugged-in capture endpoints, cameras come from CameraCapture's
    // cache, and displays from the monitor enumeration.
    struct Devices {
        bool        audioChecked = false;     // false when there was nothing to ask
        QStringList microphones;
        QString     desktopAudio;             // the loopback input, empty when there is none
        bool        camerasChecked = false;   // false until a camera enumeration has finished
        QStringList cameras;
        QStringList displays;                 // one "width × height" per monitor
    };
    static Devices detectDevices(AudioController* audio);

    // The quality step's choices. Each sets only the frame rate and the quality
    // value, which the step says; Keep changes nothing.
    enum class Quality { Keep, Standard, Archival, Lightweight };
    static OutputSettings withQuality(const OutputSettings& current, Quality quality);

    explicit OnboardingOverlay(QWidget* parent = nullptr);

    // Where microphones and desktop audio are read from.
    void setAudioController(AudioController* audio) { m_audio = audio; }

    // Reads the devices, the recording folder and the settings afresh, then
    // shows the first step.
    void openWizard();

    // What the devices step shows. openWizard() passes what it detected.
    void showDevices(const Devices& devices);

    // The folder the folder step offers, with the free space on its drive.
    // Choosing one in the step comes through here too.
    void setRecordingFolder(const QString& path);

signals:
    // The last step was confirmed and its choices saved. MainWindow opens the
    // dashboard, as the button says.
    void finished();

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void goToStep(int step);
    void loadChoices();
    void finishWizard();

    AudioController* m_audio = nullptr;
    QStackedWidget* m_steps = nullptr;
    QProgressBar* m_progress = nullptr;
    QLabel* m_stepCount = nullptr;
    QLabel* m_title = nullptr;
    QLabel* m_subtitle = nullptr;
    QPushButton* m_back = nullptr;
    QPushButton* m_next = nullptr;
    QFrame* m_card = nullptr;
    QVBoxLayout* m_deviceRows = nullptr;
    QLabel* m_folderPath = nullptr;
    QLabel* m_folderSpace = nullptr;
    QString m_folder;
    QString m_folderAtOpen;       // what the setting held, so an unchanged folder is not written
    QButtonGroup* m_quality = nullptr;
    QLabel* m_keepSummary = nullptr;
    int m_step = 0;
    int m_count = 5;
};
