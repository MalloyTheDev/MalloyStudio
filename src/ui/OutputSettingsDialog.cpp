#include "OutputSettingsDialog.h"
#include "recording/EncoderRegistry.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

OutputSettingsDialog::OutputSettingsDialog(const OutputSettings& s, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Output Settings"));
    setMinimumWidth(400);

    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    // ── Resolution ──────────────────────────────────────────────────────────
    m_width  = new QSpinBox(this);
    m_height = new QSpinBox(this);
    m_width->setRange(320, 7680);
    m_height->setRange(180, 4320);
    m_width->setSingleStep(2);
    m_height->setSingleStep(2);
    m_width->setValue(s.width);
    m_height->setValue(s.height);

    auto* resRow = new QHBoxLayout;
    resRow->addWidget(m_width);
    resRow->addWidget(new QLabel(QStringLiteral("×"), this));
    resRow->addWidget(m_height);
    resRow->addStretch();
    form->addRow(tr("Resolution"), resRow);

    auto* resPreset = new QComboBox(this);
    resPreset->addItem(QStringLiteral("1920 × 1080"), QVariant::fromValue(QSize(1920, 1080)));
    resPreset->addItem(QStringLiteral("1280 × 720"),  QVariant::fromValue(QSize(1280, 720)));
    resPreset->addItem(QStringLiteral("2560 × 1440"), QVariant::fromValue(QSize(2560, 1440)));
    resPreset->addItem(QStringLiteral("3840 × 2160"), QVariant::fromValue(QSize(3840, 2160)));
    resPreset->setCurrentIndex(-1);
    connect(resPreset, &QComboBox::currentIndexChanged, this, [this, resPreset](int i) {
        if (i < 0) return;
        const QSize sz = resPreset->itemData(i).value<QSize>();
        m_width->setValue(sz.width());
        m_height->setValue(sz.height());
    });
    form->addRow(tr("Preset"), resPreset);

    // ── Frame rate ───────────────────────────────────────────────────────────
    m_fps = new QSpinBox(this);
    m_fps->setRange(1, 120);
    m_fps->setValue(s.fps);
    m_fps->setSuffix(tr(" fps"));
    form->addRow(tr("Frame rate"), m_fps);

    // ── Video codec (from EncoderRegistry) ──────────────────────────────────
    m_videoCodec = new QComboBox(this);
    const auto& encoders = EncoderRegistry::available();
    for (const auto& enc : encoders)
        m_videoCodec->addItem(enc.display, enc.id);
    {
        const int idx = m_videoCodec->findData(s.videoCodec);
        m_videoCodec->setCurrentIndex(idx >= 0 ? idx : 0);
    }
    form->addRow(tr("Video codec"), m_videoCodec);

    // ── CRF (software quality, hidden for hardware encoders) ─────────────────
    m_crf      = new QSlider(Qt::Horizontal, this);
    m_crfLabel = new QLabel(this);
    m_crf->setRange(0, 51);
    m_crf->setValue(s.crf);
    m_crfLabel->setText(QString::number(s.crf));
    connect(m_crf, &QSlider::valueChanged, this, [this](int v) {
        m_crfLabel->setText(QString::number(v));
    });
    {
        m_crfRow = new QWidget(this);
        auto* rl = new QHBoxLayout(m_crfRow);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->addWidget(m_crf);
        rl->addWidget(m_crfLabel);
        form->addRow(tr("CRF quality (0=best)"), m_crfRow);
    }

    // ── Bitrate (hardware CBR, hidden for software encoders) ─────────────────
    m_bitrateKbps = new QSpinBox(this);
    m_bitrateKbps->setRange(500, 51000);
    m_bitrateKbps->setSingleStep(500);
    m_bitrateKbps->setValue(s.bitrateKbps);
    m_bitrateKbps->setSuffix(tr(" kbps"));
    m_bitrateRow = new QWidget(this);
    {
        auto* bl = new QHBoxLayout(m_bitrateRow);
        bl->setContentsMargins(0, 0, 0, 0);
        bl->addWidget(m_bitrateKbps);
        bl->addStretch();
        form->addRow(tr("Bitrate (CBR)"), m_bitrateRow);
    }

    // ── Preset ───────────────────────────────────────────────────────────────
    m_preset = new QComboBox(this);
    const QStringList presets{
        QStringLiteral("ultrafast"), QStringLiteral("superfast"),
        QStringLiteral("veryfast"),  QStringLiteral("faster"),
        QStringLiteral("fast"),      QStringLiteral("medium"),
        QStringLiteral("slow"),      QStringLiteral("slower"),
        QStringLiteral("veryslow"),
    };
    for (const QString& p : presets) m_preset->addItem(p, p);
    {
        const int idx = m_preset->findData(s.preset);
        m_preset->setCurrentIndex(idx >= 0 ? idx : 2);
    }
    form->addRow(tr("Preset (speed)"), m_preset);

    // ── Audio bitrate ────────────────────────────────────────────────────────
    m_audioBitrate = new QSpinBox(this);
    m_audioBitrate->setRange(64, 320);
    m_audioBitrate->setSingleStep(32);
    m_audioBitrate->setValue(s.audioBitratekbps);
    m_audioBitrate->setSuffix(tr(" kbps"));
    form->addRow(tr("Audio bitrate"), m_audioBitrate);

    // ── Container ────────────────────────────────────────────────────────────
    m_container = new QComboBox(this);
    m_container->addItem(QStringLiteral("MP4  (.mp4)"), QStringLiteral("mp4"));
    m_container->addItem(QStringLiteral("Matroska (.mkv)"), QStringLiteral("mkv"));
    {
        const int idx = m_container->findData(s.container);
        m_container->setCurrentIndex(idx >= 0 ? idx : 0);
    }
    form->addRow(tr("Container"), m_container);

    // ── Replay buffer ─────────────────────────────────────────────────────────
    m_replayBuffer = new QSpinBox(this);
    m_replayBuffer->setRange(0, 300);
    m_replayBuffer->setValue(s.replayBufferSeconds);
    m_replayBuffer->setSuffix(tr(" s  (0 = disabled)"));
    m_replayBuffer->setSpecialValueText(tr("Disabled"));
    form->addRow(tr("Replay buffer"), m_replayBuffer);

    // ── Buttons ──────────────────────────────────────────────────────────────
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* root = new QVBoxLayout(this);
    root->addLayout(form);
    root->addWidget(buttons);

    // Wire codec change → show/hide CRF vs bitrate rows
    connect(m_videoCodec, &QComboBox::currentIndexChanged,
            this, &OutputSettingsDialog::onCodecChanged);
    onCodecChanged(m_videoCodec->currentIndex());
}

void OutputSettingsDialog::onCodecChanged(int index) {
    const QString id = m_videoCodec->itemData(index).toString();
    const EncoderRegistry::Encoder* enc = EncoderRegistry::find(id);
    const bool isHardware = enc && enc->isHardware;
    m_crfRow->setVisible(!isHardware);
    m_bitrateRow->setVisible(isHardware);
}

OutputSettings OutputSettingsDialog::settings() const {
    OutputSettings o;
    o.width            = m_width->value();
    o.height           = m_height->value();
    o.fps              = m_fps->value();
    o.videoCodec       = m_videoCodec->currentData().toString();
    o.crf              = m_crf->value();
    o.bitrateKbps      = m_bitrateKbps->value();
    o.preset           = m_preset->currentData().toString();
    o.audioBitratekbps = m_audioBitrate->value();
    o.audioCodec          = QStringLiteral("aac");
    o.container           = m_container->currentData().toString();
    o.replayBufferSeconds = m_replayBuffer->value();
    return o;
}
