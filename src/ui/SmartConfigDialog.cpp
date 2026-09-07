#include "ui/SmartConfigDialog.h"

#include "project/ByteSize.h"
#include "ui/Theme.h"

#include <QDialogButtonBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMap>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {

QLabel* text(const QString& s, const QString& tone = QString(), int px = 13, bool bold = false) {
    return Theme::label(s, tone, px, bold);
}

// One "Setting: current -> proposed" row with its reason underneath.
QWidget* proposalRow(const Recommendation::Note& note, const QString& currentValue) {
    auto* row = new QFrame;
    row->setObjectName(QStringLiteral("statusItem"));
    auto* v = new QVBoxLayout(row);
    v->setContentsMargins(12, 10, 12, 10);
    v->setSpacing(3);

    auto* head = new QHBoxLayout;
    head->setSpacing(8);
    head->addWidget(text(note.field, QString(), 13, true));
    head->addStretch();

    const bool changes = !currentValue.isEmpty() && currentValue != note.value;
    if (changes) {
        head->addWidget(text(currentValue, QStringLiteral("mute"), 12));
        head->addWidget(text(QStringLiteral("->"), QStringLiteral("mute"), 12));
    }
    head->addWidget(text(note.value, changes ? QStringLiteral("accent") : QStringLiteral("mute"),
                         12, changes));
    v->addLayout(head);

    auto* why = text(note.why, QStringLiteral("dim"), 11);
    why->setWordWrap(true);
    v->addWidget(why);
    return row;
}

}  // namespace

SmartConfigDialog::SmartConfigDialog(const SystemProfile& profile, const OutputSettings& current,
                                     QWidget* parent)
    : QDialog(parent), m_profile(profile), m_current(current) {
    setWindowTitle(tr("Recommended settings"));
    resize(680, 640);

    auto* outer = new QVBoxLayout(this);
    outer->setSpacing(12);

    // ---- What was found --------------------------------------------------
    QStringList detected;
    if (!profile.encoders.isEmpty()) {
        QString encoderLine;
        for (const auto& e : profile.encoders) {
            if (!encoderLine.isEmpty()) encoderLine += QStringLiteral(", ");
            encoderLine += e.display;
        }
        detected << tr("Encoders: %1").arg(encoderLine);
    }
    if (profile.cpuThreads > 0)
        detected << tr("Processor: %1 threads").arg(profile.cpuThreads);
    if (profile.monitorWidth > 0)
        detected << tr("Display: %1 x %2").arg(profile.monitorWidth).arg(profile.monitorHeight);
    if (!profile.microphonesChecked)
        detected << tr("Microphone: not checked");
    else
        detected << (profile.microphoneName.isEmpty()
                         ? tr("Microphone: none detected")
                         : tr("Microphone: %1").arg(profile.microphoneName));
    detected << (profile.hasCamera ? tr("Camera: detected") : tr("Camera: none detected"));
    if (profile.freeDiskBytes > 0)
        detected << tr("Free space: %1 on %2")
                        .arg(formatByteSize(profile.freeDiskBytes), profile.recordingVolume);

    outer->addWidget(text(tr("Detected on this PC"), QStringLiteral("mute"), 11, true));
    auto* detectedLabel = text(detected.join(QStringLiteral("\n")), QStringLiteral("dim"), 12);
    detectedLabel->setWordWrap(true);
    outer->addWidget(detectedLabel);

    // ---- Upload speed ----------------------------------------------------
    // Bandwidth is not measured yet, so the number can be supplied by hand and
    // the recommendation follows it immediately.
    auto* uploadRow = new QHBoxLayout;
    uploadRow->setSpacing(8);
    uploadRow->addWidget(text(tr("Upload speed"), QString(), 13));
    m_upload = new QSpinBox;
    m_upload->setRange(0, 200000);
    m_upload->setSingleStep(500);
    m_upload->setSuffix(tr(" kbps"));
    m_upload->setSpecialValueText(tr("Not measured"));
    m_upload->setValue(profile.uploadKbps);
    m_upload->setFixedWidth(160);
    uploadRow->addWidget(m_upload);
    auto* uploadHint = text(tr("Enter your upload speed to size the bitrate to your connection."),
                            QStringLiteral("dim"), 11);
    uploadHint->setWordWrap(true);
    uploadRow->addWidget(uploadHint, 1);
    outer->addLayout(uploadRow);

    connect(m_upload, &QSpinBox::valueChanged, this, [this](int kbps) {
        m_profile.uploadKbps = kbps;
        rebuild();
    });

    // ---- Proposals -------------------------------------------------------
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* bodyHost = new QWidget;
    m_body = new QVBoxLayout(bodyHost);
    m_body->setContentsMargins(0, 0, 0, 0);
    m_body->setSpacing(8);
    scroll->setWidget(bodyHost);
    outer->addWidget(scroll, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    auto* apply = buttons->addButton(tr("Apply these settings"), QDialogButtonBox::AcceptRole);
    apply->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    outer->addWidget(buttons);

    rebuild();
}

void SmartConfigDialog::rebuild() {
    m_recommendation = SettingsRecommender::recommend(m_profile, m_current);

    while (QLayoutItem* item = m_body->takeAt(0)) {
        if (QWidget* w = item->widget()) w->deleteLater();
        if (QLayout* l = item->layout()) l->deleteLater();
        delete item;
    }

    // The encoder note carries a display name, so the current codec has to be
    // resolved to one too. Comparing a raw id against a display name would
    // report a change on every open, even when the encoder already matches.
    QString currentEncoder = m_current.videoCodec;
    for (const auto& e : m_profile.encoders)
        if (e.id == m_current.videoCodec) currentEncoder = e.display;

    // Current values, so a row can show what would actually change.
    const QMap<QString, QString> currentByField = {
        {tr("Encoder"), currentEncoder},
        {tr("Preset"), m_current.preset},
        {tr("Bitrate"), tr("%1 kbps").arg(m_current.bitrateKbps)},
        {tr("Resolution"), QStringLiteral("%1 x %2").arg(m_current.width).arg(m_current.height)},
        {tr("Framerate"), tr("%1 fps").arg(m_current.fps)},
        {tr("Audio bitrate"), tr("%1 kbps").arg(m_current.audioBitratekbps)},
        {tr("Keyframe interval"), tr("%1 s").arg(m_current.keyframeSec)},
    };

    for (const Recommendation::Note& note : m_recommendation.notes)
        m_body->addWidget(proposalRow(note, currentByField.value(note.field)));

    if (!m_recommendation.warnings.isEmpty()) {
        m_body->addWidget(text(tr("Worth knowing"), QStringLiteral("mute"), 11, true));
        for (const QString& w : m_recommendation.warnings) {
            auto* warning = text(w, QStringLiteral("warn"), 12);
            warning->setWordWrap(true);
            m_body->addWidget(warning);
        }
    }
    m_body->addStretch();
}
