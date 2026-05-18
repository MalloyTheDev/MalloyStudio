#pragma once

#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSlider;
class QSpinBox;
class QStackedWidget;
class AudioController;
class CaptureController;
class SceneCollection;

class InspectorPanel : public QWidget {
    Q_OBJECT
public:
    explicit InspectorPanel(SceneCollection* scenes,
                            CaptureController* captureController,
                            AudioController* audio,
                            QWidget* parent = nullptr);

private slots:
    void rebuild();
    void applyTransform();
    void pickMonitor();
    void pickWindow();
    void chooseColor();
    void onAddFilter();
    void onRemoveFilter();
    void onMoveFilterUp();
    void onMoveFilterDown();
    void onFilterSelectionChanged();
    void applyCurrentFilterProps();
    void onFilterEnabledToggled(bool enabled);

private:
    void setControlsEnabled(bool enabled);
    void rebuildFilterProps();

    SceneCollection*   m_scenes = nullptr;
    CaptureController* m_captureController = nullptr;
    AudioController*   m_audio = nullptr;
    bool m_updating = false;

    // Header
    QLabel*  m_title = nullptr;
    QLabel*  m_type = nullptr;

    // Toggles
    QCheckBox* m_visible = nullptr;
    QCheckBox* m_locked = nullptr;

    // Transform
    QDoubleSpinBox* m_x = nullptr;
    QDoubleSpinBox* m_y = nullptr;
    QDoubleSpinBox* m_w = nullptr;
    QDoubleSpinBox* m_h = nullptr;

    // Action buttons
    QPushButton* m_fit    = nullptr;
    QPushButton* m_fill   = nullptr;
    QPushButton* m_center = nullptr;
    QPushButton* m_reset  = nullptr;

    // Source-type-specific
    QLineEdit*   m_text        = nullptr;
    QPushButton* m_color       = nullptr;
    QLabel*      m_monitor     = nullptr;
    QPushButton* m_pickMonitor = nullptr;
    QLabel*      m_imagePath   = nullptr;
    QPushButton* m_browseImage = nullptr;
    QLabel*      m_windowLabel = nullptr;   // WindowCapture: current window title
    QPushButton* m_pickWindow  = nullptr;   // WindowCapture: "Change Window…"
    QLabel*      m_audioLabel  = nullptr;   // AudioInput: label
    QComboBox*   m_audioDevice = nullptr;   // AudioInput: device picker
    QLabel*      m_browserUrlLabel    = nullptr;  // Browser: URL label
    QLineEdit*   m_browserUrlEdit     = nullptr;  // Browser: URL edit
    QLabel*      m_browserHzLabel     = nullptr;  // Browser: refresh rate label
    QSpinBox*    m_browserRefreshHz   = nullptr;  // Browser: refresh rate spinbox

    // Filters section
    QGroupBox*    m_filtersGroup  = nullptr;
    QListWidget*  m_filterList    = nullptr;
    QCheckBox*    m_filterEnabled = nullptr;   // Tier 3: toggle the currently-selected filter on/off
    // Per-filter property pages (stacked widget)
    QStackedWidget* m_filterProps        = nullptr;
    int             m_filterPageEmpty    = 0;
    int             m_filterPageCrop     = 0;
    int             m_filterPageOpacity  = 0;
    int             m_filterPageColor    = 0;
    int             m_filterPageChromaKey = 0;
    int             m_filterPageBlur     = 0;
    int             m_filterPageScroll   = 0;
    // Crop page
    QDoubleSpinBox* m_cropTop    = nullptr;
    QDoubleSpinBox* m_cropLeft   = nullptr;
    QDoubleSpinBox* m_cropBottom = nullptr;
    QDoubleSpinBox* m_cropRight  = nullptr;
    // Opacity page
    QSlider* m_opacitySlider = nullptr;
    QLabel*  m_opacityLabel  = nullptr;
    // Color correction page
    QSlider* m_brightnessSlider = nullptr;
    QLabel*  m_brightnessLabel  = nullptr;
    QSlider* m_contrastSlider   = nullptr;
    QLabel*  m_contrastLabel    = nullptr;
    QSlider* m_saturationSlider = nullptr;
    QLabel*  m_saturationLabel  = nullptr;
    // Chroma key page
    QPushButton* m_chromaKeyColor      = nullptr;
    QSlider*     m_chromaTolSlider     = nullptr;
    QLabel*      m_chromaTolLabel      = nullptr;
    QSlider*     m_chromaSmoothSlider  = nullptr;
    QLabel*      m_chromaSmoothLabel   = nullptr;
    // Blur page
    QSlider* m_blurRadiusSlider = nullptr;
    QLabel*  m_blurRadiusLabel  = nullptr;
    // Scroll page
    QDoubleSpinBox* m_scrollSpeedX = nullptr;
    QDoubleSpinBox* m_scrollSpeedY = nullptr;
};
