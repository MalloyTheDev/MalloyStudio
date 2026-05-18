#include "InspectorPanel.h"
#include "MonitorPickerDialog.h"
#include "WindowPickerDialog.h"
#include "audio/AudioController.h"
#include "capture/CaptureController.h"
#include "model/Canvas.h"
#include "model/FilterEffect.h"
#include "model/SceneCollection.h"
#include "model/SceneItem.h"
#include "model/Source.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace {
QDoubleSpinBox* makeSpin(QWidget* parent, qreal max) {
    auto* spin = new QDoubleSpinBox(parent);
    spin->setRange(0.0, max);
    spin->setDecimals(0);
    spin->setSingleStep(1.0);
    spin->setAccelerated(true);
    return spin;
}

QDoubleSpinBox* makePercentSpin(QWidget* parent) {
    auto* spin = new QDoubleSpinBox(parent);
    spin->setRange(0.0, 100.0);
    spin->setDecimals(1);
    spin->setSingleStep(1.0);
    spin->setSuffix(QStringLiteral("%"));
    spin->setAccelerated(true);
    return spin;
}

QSlider* makeHSlider(int min, int max, QWidget* parent) {
    auto* s = new QSlider(Qt::Horizontal, parent);
    s->setRange(min, max);
    return s;
}
} // namespace

InspectorPanel::InspectorPanel(SceneCollection* scenes,
                               CaptureController* captureController,
                               AudioController* audio,
                               QWidget* parent)
    : QWidget(parent), m_scenes(scenes), m_captureController(captureController), m_audio(audio)
{
    // Use a scroll area so the inspector never clips its content when docked small.
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* topLayout = new QVBoxLayout(this);
    topLayout->setContentsMargins(0,0,0,0);
    topLayout->addWidget(scroll);

    auto* inner = new QWidget(scroll);
    scroll->setWidget(inner);
    auto* root = new QVBoxLayout(inner);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    // Header
    m_title = new QLabel(tr("No layer selected"), inner);
    QFont titleFont = m_title->font();
    titleFont.setBold(true);
    m_title->setFont(titleFont);
    m_type = new QLabel(inner);
    m_type->setStyleSheet(QStringLiteral("color: #a0a0a0;"));

    // Toggles
    m_visible = new QCheckBox(tr("Visible"), inner);
    m_locked  = new QCheckBox(tr("Locked"), inner);
    auto* toggles = new QHBoxLayout();
    toggles->addWidget(m_visible);
    toggles->addWidget(m_locked);
    toggles->addStretch();

    // Transform
    m_x = makeSpin(inner, MalloyCanvas::Width);
    m_y = makeSpin(inner, MalloyCanvas::Height);
    m_w = makeSpin(inner, MalloyCanvas::Width);
    m_h = makeSpin(inner, MalloyCanvas::Height);
    auto* transformGrid = new QGridLayout();
    transformGrid->addWidget(new QLabel(tr("X"), inner), 0, 0);
    transformGrid->addWidget(m_x, 0, 1);
    transformGrid->addWidget(new QLabel(tr("Y"), inner), 0, 2);
    transformGrid->addWidget(m_y, 0, 3);
    transformGrid->addWidget(new QLabel(tr("W"), inner), 1, 0);
    transformGrid->addWidget(m_w, 1, 1);
    transformGrid->addWidget(new QLabel(tr("H"), inner), 1, 2);
    transformGrid->addWidget(m_h, 1, 3);

    // Action buttons
    m_fit    = new QPushButton(tr("Fit"),    inner);
    m_fill   = new QPushButton(tr("Fill"),   inner);
    m_center = new QPushButton(tr("Center"), inner);
    m_reset  = new QPushButton(tr("Reset"),  inner);
    auto* actions = new QHBoxLayout();
    actions->addWidget(m_fit);
    actions->addWidget(m_fill);
    actions->addWidget(m_center);
    actions->addWidget(m_reset);

    // Source-type-specific form
    m_text        = new QLineEdit(inner);
    m_color       = new QPushButton(tr("Choose Color"), inner);
    m_monitor     = new QLabel(inner);
    m_monitor->setWordWrap(true);
    m_pickMonitor = new QPushButton(tr("Pick Monitor…"), inner);
    m_imagePath   = new QLabel(inner);
    m_imagePath->setWordWrap(true);
    m_imagePath->setStyleSheet(QStringLiteral("color: #a0a0c0;"));
    m_browseImage = new QPushButton(tr("Browse…"), inner);
    m_windowLabel = new QLabel(inner);
    m_windowLabel->setWordWrap(true);
    m_windowLabel->setStyleSheet(QStringLiteral("color: #a0c0a0;"));
    m_pickWindow  = new QPushButton(tr("Change Window…"), inner);
    m_audioLabel  = new QLabel(tr("Capture device:"), inner);
    m_audioDevice = new QComboBox(inner);
    m_audioDevice->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_browserUrlLabel = new QLabel(tr("URL:"), inner);
    m_browserUrlEdit  = new QLineEdit(inner);
    m_browserUrlEdit->setPlaceholderText(QStringLiteral("https://example.com"));
    m_browserHzLabel    = new QLabel(tr("Refresh (Hz):"), inner);
    m_browserRefreshHz  = new QSpinBox(inner);
    m_browserRefreshHz->setRange(1, 60);
    m_browserRefreshHz->setValue(10);
    m_browserRefreshHz->setSuffix(tr(" Hz"));

    auto* form = new QFormLayout();
    form->addRow(tr("Text"),    m_text);
    form->addRow(tr("Color"),   m_color);
    form->addRow(tr("Monitor"), m_monitor);
    form->addRow(QString(),     m_pickMonitor);
    form->addRow(tr("Image"),   m_imagePath);
    form->addRow(QString(),     m_browseImage);
    form->addRow(tr("Window"),  m_windowLabel);
    form->addRow(QString(),     m_pickWindow);
    form->addRow(m_audioLabel,  m_audioDevice);
    form->addRow(m_browserUrlLabel, m_browserUrlEdit);
    form->addRow(m_browserHzLabel,  m_browserRefreshHz);

    // ---- Filters section ----
    m_filtersGroup = new QGroupBox(tr("Filters"), inner);
    auto* filtersRoot = new QVBoxLayout(m_filtersGroup);
    filtersRoot->setSpacing(4);

    m_filterList = new QListWidget(m_filtersGroup);
    m_filterList->setMaximumHeight(100);
    filtersRoot->addWidget(m_filterList);

    auto* filterBtns = new QHBoxLayout();
    auto* addBtn  = new QPushButton(tr("+ Add"), m_filtersGroup);
    auto* remBtn  = new QPushButton(tr("✕ Remove"), m_filtersGroup);
    auto* upBtn   = new QPushButton(tr("↑"), m_filtersGroup);
    auto* downBtn = new QPushButton(tr("↓"), m_filtersGroup);
    filterBtns->addWidget(addBtn);
    filterBtns->addWidget(remBtn);
    filterBtns->addStretch();
    filterBtns->addWidget(upBtn);
    filterBtns->addWidget(downBtn);
    filtersRoot->addLayout(filterBtns);

    // Tier 3: per-filter enable toggle. Disabled filters stay in the chain
    // (and in the project JSON) but are skipped during compositing — a quick
    // way to A/B test a filter without losing its settings.
    m_filterEnabled = new QCheckBox(tr("Enabled"), m_filtersGroup);
    m_filterEnabled->setToolTip(tr("Skip this filter during rendering. The filter's settings are preserved."));
    m_filterEnabled->setEnabled(false);
    filtersRoot->addWidget(m_filterEnabled);

    // Property pages stacked widget
    m_filterProps = new QStackedWidget(m_filtersGroup);

    // Page 0: empty (no filter selected)
    auto* emptyPage = new QLabel(tr("Select a filter above to edit its properties"), m_filterProps);
    emptyPage->setAlignment(Qt::AlignCenter);
    emptyPage->setWordWrap(true);
    emptyPage->setStyleSheet(QStringLiteral("color: #808080;"));
    m_filterPageEmpty = m_filterProps->addWidget(emptyPage);

    // Page 1: Crop
    auto* cropPage = new QWidget(m_filterProps);
    auto* cropForm = new QFormLayout(cropPage);
    m_cropTop    = makePercentSpin(cropPage); cropForm->addRow(tr("Top"),    m_cropTop);
    m_cropLeft   = makePercentSpin(cropPage); cropForm->addRow(tr("Left"),   m_cropLeft);
    m_cropBottom = makePercentSpin(cropPage); cropForm->addRow(tr("Bottom"), m_cropBottom);
    m_cropRight  = makePercentSpin(cropPage); cropForm->addRow(tr("Right"),  m_cropRight);
    m_filterPageCrop = m_filterProps->addWidget(cropPage);

    // Page 2: Opacity
    auto* opacityPage = new QWidget(m_filterProps);
    auto* opacityForm = new QFormLayout(opacityPage);
    m_opacitySlider = makeHSlider(0, 100, opacityPage);
    m_opacityLabel  = new QLabel(QStringLiteral("100%"), opacityPage);
    auto* opRow = new QHBoxLayout();
    opRow->addWidget(m_opacitySlider);
    opRow->addWidget(m_opacityLabel);
    opacityForm->addRow(tr("Opacity"), opRow);
    m_filterPageOpacity = m_filterProps->addWidget(opacityPage);

    // Page 3: Color Correction
    auto* ccPage = new QWidget(m_filterProps);
    auto* ccForm = new QFormLayout(ccPage);
    auto makeSliderRow = [&](QSlider*& slider, QLabel*& label, QWidget* parent) -> QHBoxLayout* {
        slider = makeHSlider(0, 200, parent);
        label  = new QLabel(QStringLiteral("1.00"), parent);
        auto* row = new QHBoxLayout();
        row->addWidget(slider); row->addWidget(label);
        return row;
    };
    ccForm->addRow(tr("Brightness"), makeSliderRow(m_brightnessSlider, m_brightnessLabel, ccPage));
    ccForm->addRow(tr("Contrast"),   makeSliderRow(m_contrastSlider,   m_contrastLabel,   ccPage));
    ccForm->addRow(tr("Saturation"), makeSliderRow(m_saturationSlider, m_saturationLabel, ccPage));
    m_filterPageColor = m_filterProps->addWidget(ccPage);

    // Page 4: Chroma Key
    auto* ckPage = new QWidget(m_filterProps);
    auto* ckForm = new QFormLayout(ckPage);
    m_chromaKeyColor = new QPushButton(tr("Key Color"), ckPage);
    m_chromaKeyColor->setToolTip(tr("Click to choose the color to key out"));
    ckForm->addRow(tr("Key color"), m_chromaKeyColor);
    m_chromaTolSlider = makeHSlider(0, 100, ckPage);
    m_chromaTolLabel  = new QLabel(QStringLiteral("20%"), ckPage);
    { auto* row = new QHBoxLayout(); row->addWidget(m_chromaTolSlider); row->addWidget(m_chromaTolLabel);
      ckForm->addRow(tr("Tolerance"), row); }
    m_chromaSmoothSlider = makeHSlider(0, 100, ckPage);
    m_chromaSmoothLabel  = new QLabel(QStringLiteral("10%"), ckPage);
    { auto* row = new QHBoxLayout(); row->addWidget(m_chromaSmoothSlider); row->addWidget(m_chromaSmoothLabel);
      ckForm->addRow(tr("Smoothness"), row); }
    m_filterPageChromaKey = m_filterProps->addWidget(ckPage);

    // Page 5: Blur
    auto* blurPage = new QWidget(m_filterProps);
    auto* blurForm = new QFormLayout(blurPage);
    m_blurRadiusSlider = makeHSlider(0, 32, blurPage);
    m_blurRadiusLabel  = new QLabel(QStringLiteral("4 px"), blurPage);
    { auto* row = new QHBoxLayout(); row->addWidget(m_blurRadiusSlider); row->addWidget(m_blurRadiusLabel);
      blurForm->addRow(tr("Radius"), row); }
    m_filterPageBlur = m_filterProps->addWidget(blurPage);

    // Page 6: Scroll
    auto* scrollPage = new QWidget(m_filterProps);
    auto* scrollForm = new QFormLayout(scrollPage);
    m_scrollSpeedX = new QDoubleSpinBox(scrollPage);
    m_scrollSpeedX->setRange(-2000.0, 2000.0); m_scrollSpeedX->setSingleStep(10.0);
    m_scrollSpeedX->setDecimals(1); m_scrollSpeedX->setSuffix(tr(" px/s"));
    m_scrollSpeedY = new QDoubleSpinBox(scrollPage);
    m_scrollSpeedY->setRange(-2000.0, 2000.0); m_scrollSpeedY->setSingleStep(10.0);
    m_scrollSpeedY->setDecimals(1); m_scrollSpeedY->setSuffix(tr(" px/s"));
    scrollForm->addRow(tr("Speed X  (← –, → +)"), m_scrollSpeedX);
    scrollForm->addRow(tr("Speed Y  (↑ –, ↓ +)"), m_scrollSpeedY);
    m_filterPageScroll = m_filterProps->addWidget(scrollPage);

    m_filterProps->setCurrentIndex(m_filterPageEmpty);
    filtersRoot->addWidget(m_filterProps);

    // Assemble root layout
    root->addWidget(m_title);
    root->addWidget(m_type);
    root->addLayout(toggles);
    root->addLayout(transformGrid);
    root->addLayout(actions);
    root->addLayout(form);
    root->addWidget(m_filtersGroup);
    root->addStretch();

    // ---------- Connect signals ----------

    const auto spinChanged = qOverload<double>(&QDoubleSpinBox::valueChanged);
    connect(m_x, spinChanged, this, &InspectorPanel::applyTransform);
    connect(m_y, spinChanged, this, &InspectorPanel::applyTransform);
    connect(m_w, spinChanged, this, &InspectorPanel::applyTransform);
    connect(m_h, spinChanged, this, &InspectorPanel::applyTransform);
    for (QDoubleSpinBox* spin : {m_x, m_y, m_w, m_h}) {
        connect(spin, &QDoubleSpinBox::editingFinished, this, [this] {
            if (!m_updating) m_scenes->commitEditSession(QStringLiteral("Transform Layer"));
        });
    }

    connect(m_visible, &QCheckBox::toggled, this, [this](bool checked) {
        if (!m_updating) m_scenes->setCurrentItemVisible(m_scenes->currentItemIndex(), checked);
    });
    connect(m_locked, &QCheckBox::toggled, this, [this](bool checked) {
        if (!m_updating) m_scenes->setCurrentItemLocked(m_scenes->currentItemIndex(), checked);
    });
    connect(m_text, &QLineEdit::textEdited, this, [this](const QString& text) {
        if (!m_scenes->editSessionActive()) m_scenes->beginEditSession();
        m_scenes->setCurrentSourceText(m_scenes->currentItemIndex(), text, false);
    });
    connect(m_text, &QLineEdit::editingFinished, this, [this] {
        if (!m_updating) m_scenes->commitEditSession(QStringLiteral("Edit Text"));
    });
    connect(m_color,       &QPushButton::clicked, this, &InspectorPanel::chooseColor);
    connect(m_pickMonitor, &QPushButton::clicked, this, &InspectorPanel::pickMonitor);
    connect(m_pickWindow,  &QPushButton::clicked, this, &InspectorPanel::pickWindow);

    connect(m_browseImage, &QPushButton::clicked, this, [this] {
        SceneItem* item = m_scenes->currentItem();
        Source* source = m_scenes->sourceForItem(item);
        if (!source || source->type() != Source::Type::Image) return;
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Choose Image"), source->imagePath(),
            tr("Image Files (*.png *.jpg *.jpeg *.bmp *.gif *.webp);;All Files (*)"));
        if (!path.isEmpty())
            m_scenes->setCurrentSourceImagePath(m_scenes->currentItemIndex(), path);
    });

    connect(m_audioDevice, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        if (m_updating) return;
        const QString id = m_audioDevice->currentData().toString();
        if (!id.isEmpty())
            m_scenes->setCurrentSourceAudioDevice(m_scenes->currentItemIndex(), id);
    });

    connect(m_browserUrlEdit, &QLineEdit::textEdited, this, [this](const QString& url) {
        if (!m_scenes->editSessionActive()) m_scenes->beginEditSession();
        m_scenes->setCurrentSourceBrowserUrl(m_scenes->currentItemIndex(), url, false);
    });
    connect(m_browserUrlEdit, &QLineEdit::editingFinished, this, [this] {
        if (!m_updating) m_scenes->commitEditSession(QStringLiteral("Edit Browser URL"));
    });
    connect(m_browserRefreshHz, qOverload<int>(&QSpinBox::valueChanged), this, [this](int hz) {
        if (!m_updating)
            m_scenes->setCurrentSourceBrowserRefreshHz(m_scenes->currentItemIndex(), hz);
    });

    connect(m_fit, &QPushButton::clicked, this, [this] {
        if (auto* item = m_scenes->currentItem()) {
            const QRectF current = item->transform();
            const qreal aspect = current.height() > 0.0 ? current.width() / current.height() : 16.0 / 9.0;
            m_scenes->setCurrentItemTransform(m_scenes->currentItemIndex(), MalloyCanvas::fitRectForAspect(aspect));
        }
    });
    connect(m_fill, &QPushButton::clicked, this, [this] {
        m_scenes->setCurrentItemTransform(m_scenes->currentItemIndex(),
                                          {0.0, 0.0, MalloyCanvas::Width, MalloyCanvas::Height});
    });
    connect(m_center, &QPushButton::clicked, this, [this] {
        if (auto* item = m_scenes->currentItem()) {
            QRectF r = item->transform();
            r.moveCenter({MalloyCanvas::Width / 2.0, MalloyCanvas::Height / 2.0});
            m_scenes->setCurrentItemTransform(m_scenes->currentItemIndex(), r);
        }
    });
    connect(m_reset, &QPushButton::clicked, this, [this] {
        m_scenes->resetCurrentItemTransform(m_scenes->currentItemIndex());
    });

    // Filters buttons
    connect(addBtn,  &QPushButton::clicked, this, &InspectorPanel::onAddFilter);
    connect(remBtn,  &QPushButton::clicked, this, &InspectorPanel::onRemoveFilter);
    connect(upBtn,   &QPushButton::clicked, this, &InspectorPanel::onMoveFilterUp);
    connect(downBtn, &QPushButton::clicked, this, &InspectorPanel::onMoveFilterDown);
    connect(m_filterList, &QListWidget::currentRowChanged, this, [this](int) {
        onFilterSelectionChanged();
    });
    connect(m_filterEnabled, &QCheckBox::toggled, this,
            &InspectorPanel::onFilterEnabledToggled);

    // Crop spinboxes
    const auto cropSpin = qOverload<double>(&QDoubleSpinBox::valueChanged);
    connect(m_cropTop,    cropSpin, this, &InspectorPanel::applyCurrentFilterProps);
    connect(m_cropLeft,   cropSpin, this, &InspectorPanel::applyCurrentFilterProps);
    connect(m_cropBottom, cropSpin, this, &InspectorPanel::applyCurrentFilterProps);
    connect(m_cropRight,  cropSpin, this, &InspectorPanel::applyCurrentFilterProps);
    for (QDoubleSpinBox* s : {m_cropTop, m_cropLeft, m_cropBottom, m_cropRight}) {
        connect(s, &QDoubleSpinBox::editingFinished, this, [this] {
            if (!m_updating) m_scenes->commitEditSession(QStringLiteral("Edit Crop Filter"));
        });
    }

    // Opacity slider
    connect(m_opacitySlider, &QSlider::valueChanged, this, [this](int v) {
        m_opacityLabel->setText(QStringLiteral("%1%").arg(v));
        if (!m_updating) applyCurrentFilterProps();
    });
    connect(m_opacitySlider, &QSlider::sliderReleased, this, [this] {
        if (!m_updating) m_scenes->commitEditSession(QStringLiteral("Edit Opacity Filter"));
    });

    // CC sliders
    auto connectCCSlider = [&](QSlider* slider, QLabel* label) {
        connect(slider, &QSlider::valueChanged, this, [this, slider, label](int v) {
            label->setText(QStringLiteral("%1").arg(v / 100.0, 0, 'f', 2));
            Q_UNUSED(slider);
            if (!m_updating) applyCurrentFilterProps();
        });
        connect(slider, &QSlider::sliderReleased, this, [this] {
            if (!m_updating) m_scenes->commitEditSession(QStringLiteral("Edit Color Correction"));
        });
    };
    connectCCSlider(m_brightnessSlider, m_brightnessLabel);
    connectCCSlider(m_contrastSlider,   m_contrastLabel);
    connectCCSlider(m_saturationSlider, m_saturationLabel);

    // Chroma key
    connect(m_chromaKeyColor, &QPushButton::clicked, this, [this] {
        SceneItem* item = m_scenes->currentItem();
        const int row = m_filterList->currentRow();
        if (!item || row < 0 || row >= item->filters().size()) return;
        FilterEffect* f = item->filters().at(row);
        if (f->type() != FilterEffect::Type::ChromaKey) return;
        const QColor c = QColorDialog::getColor(
            static_cast<ChromaKeyFilter*>(f)->key(), this, tr("Key Color"));
        if (c.isValid()) {
            if (!m_scenes->editSessionActive()) m_scenes->beginEditSession();
            static_cast<ChromaKeyFilter*>(f)->setKey(c);
            m_chromaKeyColor->setStyleSheet(
                QStringLiteral("background-color:%1;").arg(c.name()));
            m_scenes->commitEditSession(tr("Edit Chroma Key"));
        }
    });
    auto connectCKSlider = [&](QSlider* slider, QLabel* label, const QString& suffix) {
        connect(slider, &QSlider::valueChanged, this, [this, slider, label, suffix](int v) {
            label->setText(QStringLiteral("%1%2").arg(v).arg(suffix));
            Q_UNUSED(slider);
            if (!m_updating) applyCurrentFilterProps();
        });
        connect(slider, &QSlider::sliderReleased, this, [this] {
            if (!m_updating) m_scenes->commitEditSession(QStringLiteral("Edit Chroma Key"));
        });
    };
    connectCKSlider(m_chromaTolSlider,    m_chromaTolLabel,    QStringLiteral("%"));
    connectCKSlider(m_chromaSmoothSlider, m_chromaSmoothLabel, QStringLiteral("%"));

    // Blur
    connect(m_blurRadiusSlider, &QSlider::valueChanged, this, [this](int v) {
        m_blurRadiusLabel->setText(QStringLiteral("%1 px").arg(v));
        if (!m_updating) applyCurrentFilterProps();
    });
    connect(m_blurRadiusSlider, &QSlider::sliderReleased, this, [this] {
        if (!m_updating) m_scenes->commitEditSession(QStringLiteral("Edit Blur"));
    });

    // Scroll speed spinboxes
    const auto dblChanged = qOverload<double>(&QDoubleSpinBox::valueChanged);
    connect(m_scrollSpeedX, dblChanged, this, [this](double) {
        if (!m_updating) applyCurrentFilterProps();
    });
    connect(m_scrollSpeedY, dblChanged, this, [this](double) {
        if (!m_updating) applyCurrentFilterProps();
    });
    for (QDoubleSpinBox* s : {m_scrollSpeedX, m_scrollSpeedY}) {
        connect(s, &QDoubleSpinBox::editingFinished, this, [this] {
            if (!m_updating) m_scenes->commitEditSession(QStringLiteral("Edit Scroll Filter"));
        });
    }

    // Model signals
    connect(m_scenes, &SceneCollection::currentChanged, this, [this](int){ rebuild(); });
    connect(m_scenes, &SceneCollection::itemsChanged,   this, &InspectorPanel::rebuild);
    connect(m_scenes, &SceneCollection::itemSelectionChanged, this, [this](int){ rebuild(); });
    connect(m_captureController, &CaptureController::monitorStatusChanged,
            this, [this](int, int, const QString&){ rebuild(); });
    connect(m_captureController, &CaptureController::statusChanged,
            this, [this](const QString&){ rebuild(); });

    rebuild();
}

void InspectorPanel::rebuild() {
    m_updating = true;
    SceneItem* item = m_scenes->currentItem();
    if (!item) {
        m_title->setText(tr("No layer selected"));
        m_type->clear();
        setControlsEnabled(false);
        m_filtersGroup->setVisible(false);
        m_updating = false;
        return;
    }

    Source* source = m_scenes->sourceForItem(item);
    if (!source) {
        m_title->setText(tr("Missing source"));
        m_type->clear();
        setControlsEnabled(false);
        m_filtersGroup->setVisible(false);
        m_updating = false;
        return;
    }

    const QRectF r = item->transform();
    m_title->setText(source->name());
    m_type->setText(Source::typeToString(source->type()));
    m_visible->setChecked(item->isVisible());
    m_locked->setChecked(item->isLocked());
    m_x->setValue(r.x());
    m_y->setValue(r.y());
    m_w->setValue(r.width());
    m_h->setValue(r.height());
    m_text->setText(source->text());

    const bool isText    = source->type() == Source::Type::Text;
    const bool isColor   = source->type() == Source::Type::ColorBlock;
    const bool isDisplay = source->type() == Source::Type::DisplayCapture;
    const bool isImage   = source->type() == Source::Type::Image;
    const bool isWindow  = source->type() == Source::Type::WindowCapture;
    const bool isAudio   = source->type() == Source::Type::AudioInput;
    const bool isBrowser = source->type() == Source::Type::Browser;

    m_text->setVisible(isText);
    m_color->setVisible(isColor);
    m_monitor->setVisible(isDisplay);
    m_pickMonitor->setVisible(isDisplay);
    m_imagePath->setVisible(isImage);
    m_browseImage->setVisible(isImage);
    m_windowLabel->setVisible(isWindow);
    m_pickWindow->setVisible(isWindow);
    m_audioLabel->setVisible(isAudio);
    m_audioDevice->setVisible(isAudio);
    m_browserUrlLabel->setVisible(isBrowser);
    m_browserUrlEdit->setVisible(isBrowser);
    m_browserHzLabel->setVisible(isBrowser);
    m_browserRefreshHz->setVisible(isBrowser);

    if (isImage) {
        const QString& p = source->imagePath();
        m_imagePath->setText(p.isEmpty() ? tr("(none)") : QFileInfo(p).fileName());
        m_imagePath->setToolTip(p);
    }

    if (isDisplay) {
        m_monitor->setText(source->hasMonitorConfig()
            ? tr("Adapter %1, output %2\n%3")
                .arg(source->adapterIndex())
                .arg(source->outputIndex())
                .arg(m_captureController->monitorStatus(source->adapterIndex(), source->outputIndex()))
            : tr("No monitor selected"));
    }

    if (isWindow) {
        m_windowLabel->setText(source->hasWindowConfig()
            ? source->windowTitle()
            : tr("(no window selected)"));
    }

    if (isAudio && m_audio) {
        QSignalBlocker sb(m_audioDevice);
        m_audioDevice->clear();
        const auto devices = m_audio->enumerateInputDevices();
        for (const auto& [id, name] : devices)
            m_audioDevice->addItem(name, id);
        // Select the current device
        for (int i = 0; i < m_audioDevice->count(); ++i) {
            if (m_audioDevice->itemData(i).toString() == source->audioDeviceId()) {
                m_audioDevice->setCurrentIndex(i);
                break;
            }
        }
    }

    if (isBrowser) {
        QSignalBlocker sb1(m_browserUrlEdit);
        QSignalBlocker sb2(m_browserRefreshHz);
        m_browserUrlEdit->setText(source->browserUrl());
        m_browserRefreshHz->setValue(source->browserRefreshHz());
    }

    const QColor c = source->color();
    m_color->setStyleSheet(QStringLiteral("background-color: %1; color: white;").arg(c.name()));

    // Filters
    m_filtersGroup->setVisible(true);
    rebuildFilterProps();

    setControlsEnabled(true);
    m_updating = false;
}

void InspectorPanel::rebuildFilterProps() {
    SceneItem* item = m_scenes->currentItem();
    if (!item) { m_filterList->clear(); return; }

    const int prevRow = m_filterList->currentRow();
    {
        QSignalBlocker sb(m_filterList);
        m_filterList->clear();
        for (const FilterEffect* f : item->filters())
            m_filterList->addItem(f->label());
    }
    // Restore selection
    const int rowToSelect = qBound(-1, prevRow, m_filterList->count() - 1);
    m_filterList->setCurrentRow(rowToSelect);
    onFilterSelectionChanged();
}

void InspectorPanel::onFilterSelectionChanged() {
    SceneItem* item = m_scenes->currentItem();
    const int row = m_filterList->currentRow();
    if (!item || row < 0 || row >= item->filters().size()) {
        m_filterProps->setCurrentIndex(m_filterPageEmpty);
        if (m_filterEnabled) {
            m_filterEnabled->setEnabled(false);
            m_filterEnabled->setChecked(true);
        }
        return;
    }

    const FilterEffect* f = item->filters().at(row);
    m_updating = true;
    if (m_filterEnabled) {
        m_filterEnabled->setEnabled(true);
        m_filterEnabled->setChecked(f->isEnabled());
    }
    switch (f->type()) {
        case FilterEffect::Type::Crop: {
            const auto* cf = static_cast<const CropFilter*>(f);
            m_cropTop->setValue(cf->top()    * 100.0);
            m_cropLeft->setValue(cf->left()  * 100.0);
            m_cropBottom->setValue(cf->bottom() * 100.0);
            m_cropRight->setValue(cf->right()   * 100.0);
            m_filterProps->setCurrentIndex(m_filterPageCrop);
            break;
        }
        case FilterEffect::Type::Opacity: {
            const auto* of = static_cast<const OpacityFilter*>(f);
            m_opacitySlider->setValue(static_cast<int>(of->opacity() * 100.0f));
            m_opacityLabel->setText(QStringLiteral("%1%").arg(m_opacitySlider->value()));
            m_filterProps->setCurrentIndex(m_filterPageOpacity);
            break;
        }
        case FilterEffect::Type::ColorCorrection: {
            const auto* cc = static_cast<const ColorCorrectionFilter*>(f);
            m_brightnessSlider->setValue(static_cast<int>(cc->brightness() * 100.0f));
            m_contrastSlider->setValue(static_cast<int>(cc->contrast()     * 100.0f));
            m_saturationSlider->setValue(static_cast<int>(cc->saturation() * 100.0f));
            m_brightnessLabel->setText(QStringLiteral("%1").arg(cc->brightness(), 0, 'f', 2));
            m_contrastLabel->setText(QStringLiteral("%1").arg(cc->contrast(),     0, 'f', 2));
            m_saturationLabel->setText(QStringLiteral("%1").arg(cc->saturation(), 0, 'f', 2));
            m_filterProps->setCurrentIndex(m_filterPageColor);
            break;
        }
        case FilterEffect::Type::ChromaKey: {
            const auto* ck = static_cast<const ChromaKeyFilter*>(f);
            m_chromaTolSlider->setValue(static_cast<int>(ck->tolerance()  * 100.0f));
            m_chromaSmoothSlider->setValue(static_cast<int>(ck->smoothness() * 100.0f));
            m_chromaTolLabel->setText(QStringLiteral("%1%").arg(m_chromaTolSlider->value()));
            m_chromaSmoothLabel->setText(QStringLiteral("%1%").arg(m_chromaSmoothSlider->value()));
            m_chromaKeyColor->setStyleSheet(
                QStringLiteral("background-color:%1;").arg(ck->key().name()));
            m_filterProps->setCurrentIndex(m_filterPageChromaKey);
            break;
        }
        case FilterEffect::Type::Blur: {
            const auto* bl = static_cast<const BlurFilter*>(f);
            m_blurRadiusSlider->setValue(bl->radius());
            m_blurRadiusLabel->setText(QStringLiteral("%1 px").arg(bl->radius()));
            m_filterProps->setCurrentIndex(m_filterPageBlur);
            break;
        }
        case FilterEffect::Type::Scroll: {
            const auto* sc = static_cast<const ScrollFilter*>(f);
            m_scrollSpeedX->setValue(static_cast<double>(sc->speedX()));
            m_scrollSpeedY->setValue(static_cast<double>(sc->speedY()));
            m_filterProps->setCurrentIndex(m_filterPageScroll);
            break;
        }
    }
    m_updating = false;
}

void InspectorPanel::applyCurrentFilterProps() {
    if (m_updating) return;
    SceneItem* item = m_scenes->currentItem();
    const int row = m_filterList->currentRow();
    if (!item || row < 0 || row >= item->filters().size()) return;

    if (!m_scenes->editSessionActive()) m_scenes->beginEditSession();

    FilterEffect* f = item->filters().at(row);
    switch (f->type()) {
        case FilterEffect::Type::Crop: {
            auto* cf = static_cast<CropFilter*>(f);
            cf->setTop(static_cast<float>(m_cropTop->value()    / 100.0));
            cf->setLeft(static_cast<float>(m_cropLeft->value()  / 100.0));
            cf->setBottom(static_cast<float>(m_cropBottom->value() / 100.0));
            cf->setRight(static_cast<float>(m_cropRight->value()   / 100.0));
            break;
        }
        case FilterEffect::Type::Opacity: {
            auto* of = static_cast<OpacityFilter*>(f);
            of->setOpacity(m_opacitySlider->value() / 100.0f);
            break;
        }
        case FilterEffect::Type::ColorCorrection: {
            auto* cc = static_cast<ColorCorrectionFilter*>(f);
            cc->setBrightness(m_brightnessSlider->value() / 100.0f);
            cc->setContrast(m_contrastSlider->value()     / 100.0f);
            cc->setSaturation(m_saturationSlider->value() / 100.0f);
            break;
        }
        case FilterEffect::Type::ChromaKey: {
            auto* ck = static_cast<ChromaKeyFilter*>(f);
            ck->setTolerance(m_chromaTolSlider->value()    / 100.0f);
            ck->setSmoothness(m_chromaSmoothSlider->value() / 100.0f);
            // Key color is applied directly from the color dialog, not here.
            break;
        }
        case FilterEffect::Type::Blur: {
            static_cast<BlurFilter*>(f)->setRadius(m_blurRadiusSlider->value());
            break;
        }
        case FilterEffect::Type::Scroll: {
            auto* sc = static_cast<ScrollFilter*>(f);
            sc->setSpeedX(static_cast<float>(m_scrollSpeedX->value()));
            sc->setSpeedY(static_cast<float>(m_scrollSpeedY->value()));
            break;
        }
    }
}

void InspectorPanel::onFilterEnabledToggled(bool enabled) {
    if (m_updating) return;
    SceneItem* item = m_scenes->currentItem();
    const int row = m_filterList->currentRow();
    if (!item || row < 0 || row >= item->filters().size()) return;

    FilterEffect* f = item->filters().at(row);
    if (f->isEnabled() == enabled) return;

    // Snapshot-based undo (same pattern Add/Remove/Reorder Filter use).
    const QJsonObject before = m_scenes->snapshot();
    f->setEnabled(enabled);
    m_scenes->pushSnapshotCommand(tr("Toggle Filter"), before, m_scenes->snapshot());
}

void InspectorPanel::onAddFilter() {
    QMenu menu(this);
    QAction* cropAct   = menu.addAction(tr("Crop"));
    QAction* opAct     = menu.addAction(tr("Opacity"));
    QAction* ccAct     = menu.addAction(tr("Color Correction"));
    QAction* ckAct     = menu.addAction(tr("Chroma Key"));
    QAction* blurAct   = menu.addAction(tr("Blur"));
    QAction* scrollAct = menu.addAction(tr("Scroll"));

    QAction* chosen = menu.exec(QCursor::pos());
    if (!chosen) return;

    SceneItem* item = m_scenes->currentItem();
    if (!item) return;

    const QJsonObject before = m_scenes->snapshot();
    FilterEffect* f = nullptr;
    if      (chosen == cropAct)   f = new CropFilter(item);
    else if (chosen == opAct)     f = new OpacityFilter(item);
    else if (chosen == ccAct)     f = new ColorCorrectionFilter(item);
    else if (chosen == ckAct)     f = new ChromaKeyFilter(item);
    else if (chosen == blurAct)   f = new BlurFilter(item);
    else if (chosen == scrollAct) f = new ScrollFilter(item);
    if (!f) return;

    item->addFilter(f);
    m_scenes->pushSnapshotCommand(tr("Add Filter"), before, m_scenes->snapshot());
    rebuild();
    m_filterList->setCurrentRow(item->filters().size() - 1);
}

void InspectorPanel::onRemoveFilter() {
    SceneItem* item = m_scenes->currentItem();
    const int row = m_filterList->currentRow();
    if (!item || row < 0 || row >= item->filters().size()) return;

    const QJsonObject before = m_scenes->snapshot();
    item->removeFilterAt(row);
    m_scenes->pushSnapshotCommand(tr("Remove Filter"), before, m_scenes->snapshot());
    rebuild();
}

void InspectorPanel::onMoveFilterUp() {
    SceneItem* item = m_scenes->currentItem();
    const int row = m_filterList->currentRow();
    if (!item || row <= 0) return;

    const QJsonObject before = m_scenes->snapshot();
    item->moveFilterUp(row);
    m_scenes->pushSnapshotCommand(tr("Reorder Filter"), before, m_scenes->snapshot());
    m_filterList->setCurrentRow(row - 1);
    rebuild();
}

void InspectorPanel::onMoveFilterDown() {
    SceneItem* item = m_scenes->currentItem();
    const int row = m_filterList->currentRow();
    if (!item || row < 0 || row >= item->filters().size() - 1) return;

    const QJsonObject before = m_scenes->snapshot();
    item->moveFilterDown(row);
    m_scenes->pushSnapshotCommand(tr("Reorder Filter"), before, m_scenes->snapshot());
    m_filterList->setCurrentRow(row + 1);
    rebuild();
}

void InspectorPanel::applyTransform() {
    if (m_updating) return;
    if (!m_scenes->editSessionActive()) m_scenes->beginEditSession();
    m_scenes->setCurrentItemTransform(m_scenes->currentItemIndex(),
                                      QRectF(m_x->value(), m_y->value(), m_w->value(), m_h->value()),
                                      false);
}

void InspectorPanel::pickMonitor() {
    SceneItem* item = m_scenes->currentItem();
    Source* source = m_scenes->sourceForItem(item);
    if (!source || source->type() != Source::Type::DisplayCapture) return;

    MonitorPickerDialog picker(this);
    if (picker.exec() != QDialog::Accepted) return;
    const MonitorInfo info = picker.selectedMonitor();
    if (info.adapterIndex >= 0)
        m_scenes->setCurrentSourceMonitor(m_scenes->currentItemIndex(), info.adapterIndex, info.outputIndex);
}

void InspectorPanel::pickWindow() {
    SceneItem* item = m_scenes->currentItem();
    Source* source = m_scenes->sourceForItem(item);
    if (!source || source->type() != Source::Type::WindowCapture) return;

    auto result = WindowPickerDialog::pickWindow(this);
    if (!result) return;
    m_scenes->setCurrentSourceWindow(m_scenes->currentItemIndex(),
                                     result->first, result->second);
}

void InspectorPanel::chooseColor() {
    SceneItem* item = m_scenes->currentItem();
    Source* source = m_scenes->sourceForItem(item);
    if (!source || source->type() != Source::Type::ColorBlock) return;

    const QColor color = QColorDialog::getColor(source->color(), this, tr("Choose Color"));
    if (color.isValid()) m_scenes->setCurrentSourceColor(m_scenes->currentItemIndex(), color);
}

void InspectorPanel::setControlsEnabled(bool enabled) {
    for (QWidget* w : {static_cast<QWidget*>(m_visible), static_cast<QWidget*>(m_locked),
                       static_cast<QWidget*>(m_x),   static_cast<QWidget*>(m_y),
                       static_cast<QWidget*>(m_w),   static_cast<QWidget*>(m_h),
                       static_cast<QWidget*>(m_text),        static_cast<QWidget*>(m_color),
                       static_cast<QWidget*>(m_monitor),     static_cast<QWidget*>(m_pickMonitor),
                       static_cast<QWidget*>(m_imagePath),   static_cast<QWidget*>(m_browseImage),
                       static_cast<QWidget*>(m_windowLabel), static_cast<QWidget*>(m_pickWindow),
                       static_cast<QWidget*>(m_audioLabel),  static_cast<QWidget*>(m_audioDevice),
                       static_cast<QWidget*>(m_browserUrlLabel), static_cast<QWidget*>(m_browserUrlEdit),
                       static_cast<QWidget*>(m_browserHzLabel),  static_cast<QWidget*>(m_browserRefreshHz),
                       static_cast<QWidget*>(m_fit),   static_cast<QWidget*>(m_fill),
                       static_cast<QWidget*>(m_center), static_cast<QWidget*>(m_reset)}) {
        w->setEnabled(enabled);
    }
}
