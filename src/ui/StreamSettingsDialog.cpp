#include "StreamSettingsDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

StreamSettingsDialog::StreamSettingsDialog(const StreamSettings& current, QWidget* parent)
    : QDialog(parent)
    , m_settings(current)
{
    setWindowTitle(tr("Stream Settings"));
    setMinimumWidth(420);

    auto* mainLayout = new QVBoxLayout(this);

    // --- Service group ---
    auto* serviceGroup  = new QGroupBox(tr("Service"), this);
    auto* serviceLayout = new QFormLayout(serviceGroup);

    m_serviceCombo = new QComboBox(serviceGroup);
    m_serviceCombo->addItem(StreamSettings::displayName(StreamSettings::Service::Twitch));
    m_serviceCombo->addItem(StreamSettings::displayName(StreamSettings::Service::YouTube));
    m_serviceCombo->addItem(StreamSettings::displayName(StreamSettings::Service::Custom));
    m_serviceCombo->setCurrentIndex(static_cast<int>(current.service));
    serviceLayout->addRow(tr("Platform:"), m_serviceCombo);

    // Custom RTMP URL row (hidden unless Custom is selected)
    m_customUrlEdit = new QLineEdit(current.customUrl, serviceGroup);
    m_customUrlEdit->setPlaceholderText(QStringLiteral("rtmp://your-server.example.com/live/{key}"));
    serviceLayout->addRow(tr("Custom URL:"), m_customUrlEdit);

    // Live URL preview (read-only, updates as the user types the key)
    m_urlPreviewLabel = new QLabel(serviceGroup);
    m_urlPreviewLabel->setWordWrap(true);
    m_urlPreviewLabel->setStyleSheet(QStringLiteral("color: gray; font-size: 10px;"));
    serviceLayout->addRow(tr("RTMP URL:"), m_urlPreviewLabel);

    mainLayout->addWidget(serviceGroup);

    // --- Authentication group ---
    auto* authGroup  = new QGroupBox(tr("Authentication"), this);
    auto* authLayout = new QFormLayout(authGroup);

    m_keyEdit = new QLineEdit(current.streamKey, authGroup);
    m_keyEdit->setEchoMode(QLineEdit::Password);
    m_keyEdit->setPlaceholderText(tr("Paste your stream key here"));

    auto* showHideBtn = new QPushButton(tr("Show"), authGroup);
    showHideBtn->setCheckable(true);
    connect(showHideBtn, &QPushButton::toggled, this, [this, showHideBtn](bool checked) {
        m_keyEdit->setEchoMode(checked ? QLineEdit::Normal : QLineEdit::Password);
        showHideBtn->setText(checked ? tr("Hide") : tr("Show"));
    });

    auto* keyRow = new QHBoxLayout;
    keyRow->addWidget(m_keyEdit, 1);
    keyRow->addWidget(showHideBtn);
    authLayout->addRow(tr("Stream Key:"), keyRow);

    mainLayout->addWidget(authGroup);

    // --- Encoder group ---
    auto* encGroup  = new QGroupBox(tr("Encoder"), this);
    auto* encLayout = new QFormLayout(encGroup);

    m_bitrateSpinBox = new QSpinBox(encGroup);
    m_bitrateSpinBox->setRange(500, 51000);
    m_bitrateSpinBox->setSingleStep(500);
    m_bitrateSpinBox->setValue(current.bitrateKbps);
    m_bitrateSpinBox->setSuffix(tr(" kbps"));
    encLayout->addRow(tr("Video Bitrate:"), m_bitrateSpinBox);

    m_keyframeSpinBox = new QSpinBox(encGroup);
    m_keyframeSpinBox->setRange(1, 10);
    m_keyframeSpinBox->setValue(current.keyframeSec);
    m_keyframeSpinBox->setSuffix(tr(" s"));
    encLayout->addRow(tr("Keyframe Interval:"), m_keyframeSpinBox);

    mainLayout->addWidget(encGroup);

    // --- Buttons ---
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    mainLayout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &StreamSettingsDialog::onAccepted);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // --- Initial state ---
    onServiceChanged(m_serviceCombo->currentIndex());

    // Live preview update
    auto updatePreview = [this] {
        StreamSettings tmp = m_settings;
        tmp.service    = static_cast<StreamSettings::Service>(m_serviceCombo->currentIndex());
        tmp.customUrl  = m_customUrlEdit->text();
        tmp.streamKey  = m_keyEdit->text();
        const QString url = tmp.rtmpUrl();
        // Mask the key in the preview display
        QString display = url;
        if (!tmp.streamKey.isEmpty())
            display.replace(tmp.streamKey, QStringLiteral("***"));
        m_urlPreviewLabel->setText(display);
    };

    connect(m_serviceCombo, &QComboBox::currentIndexChanged, this, updatePreview);
    connect(m_customUrlEdit, &QLineEdit::textChanged, this, updatePreview);
    connect(m_keyEdit, &QLineEdit::textChanged, this, updatePreview);
    connect(m_serviceCombo, &QComboBox::currentIndexChanged,
            this, &StreamSettingsDialog::onServiceChanged);

    updatePreview();
}

void StreamSettingsDialog::onServiceChanged(int index) {
    const bool isCustom = (index == static_cast<int>(StreamSettings::Service::Custom));
    m_customUrlEdit->setVisible(isCustom);
}

void StreamSettingsDialog::onAccepted() {
    m_settings.service     = static_cast<StreamSettings::Service>(m_serviceCombo->currentIndex());
    m_settings.customUrl   = m_customUrlEdit->text().trimmed();
    m_settings.streamKey   = m_keyEdit->text();
    m_settings.bitrateKbps = m_bitrateSpinBox->value();
    m_settings.keyframeSec = m_keyframeSpinBox->value();
    m_settings.save();
    accept();
}
