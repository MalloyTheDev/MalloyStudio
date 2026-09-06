#include "recording/RenderPipeline.h"
#include "recording/TimelineGraphBuilder.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>

namespace {
// Cap on the ffmpeg stderr we keep for failure messages.
constexpr int kStderrTailChars = 4000;
// How long to wait for a terminated ffmpeg before killing it.
constexpr int kTerminateGraceMs = 2000;
}  // namespace

RenderPipeline::RenderPipeline(QObject* parent) : QObject(parent) {
    // Same discovery the capture pipeline uses, so both agree on which ffmpeg
    // this install is running.
    m_ffmpegPath = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
}

RenderPipeline::~RenderPipeline() {
    if (m_proc) cancel();
}

bool RenderPipeline::start(const RenderJob& job, QString* error) {
    auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };

    if (m_proc)
        return fail(tr("A render is already running."));
    if (m_ffmpegPath.isEmpty())
        return fail(tr("ffmpeg was not found on PATH, so nothing can be rendered."));
    if (job.outputPath.isEmpty())
        return fail(tr("The job has no output file."));
    if (QFileInfo::exists(job.outputPath))
        return fail(tr("The output file already exists: %1")
                        .arg(QDir::toNativeSeparators(job.outputPath)));

    const RenderGraph graph = TimelineGraphBuilder::build(job.timeline, job.output);
    if (!graph.ok)
        return fail(graph.error);

    // The graph goes to a file rather than the command line: it grows with the
    // clip count, and the Windows command line is capped at about 32k.
    const QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    m_graphPath = QDir(tempDir).filePath(
        QStringLiteral("malloy_render_%1.txt")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    {
        QSaveFile f(m_graphPath);
        if (!f.open(QIODevice::WriteOnly))
            return fail(tr("Could not write the render graph to %1")
                            .arg(QDir::toNativeSeparators(m_graphPath)));
        f.write(graph.filterGraph.toUtf8());
        if (!f.commit())
            return fail(tr("Could not write the render graph to %1")
                            .arg(QDir::toNativeSeparators(m_graphPath)));
    }

    QStringList args;
    args << QStringLiteral("-hide_banner")
         << QStringLiteral("-loglevel") << QStringLiteral("error")
         << QStringLiteral("-nostdin")
         << QStringLiteral("-progress") << QStringLiteral("pipe:1")
         << QStringLiteral("-nostats");
    args << graph.inputArgs;                       // media paths, argv only
    args << QStringLiteral("-filter_complex_script") << m_graphPath;
    args << graph.outputArgs;
    args << job.outputPath;

    m_outputPath = job.outputPath;
    m_totalUs = graph.durationSecs * 1000000.0;
    m_percent = 0;
    m_cancelled = false;
    m_stderrTail.clear();
    m_pendingOut.clear();

    m_proc = new QProcess(this);
    connect(m_proc, &QProcess::readyReadStandardOutput, this, &RenderPipeline::onProgressOutput);
    connect(m_proc, &QProcess::readyReadStandardError, this, &RenderPipeline::onProcessError);
    connect(m_proc, &QProcess::finished, this,
            [this](int code, QProcess::ExitStatus) { onProcessFinished(code); });

    m_proc->start(m_ffmpegPath, args);
    if (!m_proc->waitForStarted(5000)) {
        const QString message = m_proc->errorString();
        cleanup();
        return fail(tr("ffmpeg failed to start: %1").arg(message));
    }
    if (error) error->clear();
    return true;
}

void RenderPipeline::cancel() {
    if (!m_proc) return;
    m_cancelled = true;
    m_proc->terminate();
    if (!m_proc->waitForFinished(kTerminateGraceMs)) {
        m_proc->kill();
        m_proc->waitForFinished(kTerminateGraceMs);
    }
    cleanup();
    // A half-written file would otherwise be indexed as media by the registries.
    removePartialOutput();
}

void RenderPipeline::onProgressOutput() {
    if (!m_proc) return;
    m_pendingOut += QString::fromUtf8(m_proc->readAllStandardOutput());

    int newline = -1;
    while ((newline = m_pendingOut.indexOf(QLatin1Char('\n'))) >= 0) {
        const QString line = m_pendingOut.left(newline).trimmed();
        m_pendingOut.remove(0, newline + 1);

        if (line.startsWith(QLatin1String("out_time_us=")) && m_totalUs > 0.0) {
            bool ok = false;
            const double us = line.mid(12).toDouble(&ok);
            if (ok && us >= 0.0) {
                const int pct = int(std::lround(std::min(100.0, us / m_totalUs * 100.0)));
                // ffmpeg's closing block can report a lower time than the peak,
                // so progress only ever moves forward.
                if (pct > m_percent) {
                    m_percent = pct;
                    emit progress(m_percent);
                }
            }
        }
    }
}

void RenderPipeline::onProcessError() {
    if (!m_proc) return;
    m_stderrTail += QString::fromUtf8(m_proc->readAllStandardError());
    if (m_stderrTail.size() > kStderrTailChars)
        m_stderrTail = m_stderrTail.right(kStderrTailChars);
}

void RenderPipeline::onProcessFinished(int exitCode) {
    if (m_cancelled) { cleanup(); return; }   // cancel() already reported

    onProcessError();                          // drain whatever stderr is left
    const QString tail = m_stderrTail.trimmed();
    const QString output = m_outputPath;
    cleanup();

    if (exitCode != 0) {
        removePartialOutput();
        QString message = tr("ffmpeg exited with code %1.").arg(exitCode);
        if (!tail.isEmpty()) message += QStringLiteral("\n\n") + tail;
        emit failed(message);
        return;
    }
    if (!QFileInfo::exists(output)) {
        emit failed(tr("ffmpeg reported success but wrote no file."));
        return;
    }
    if (m_percent < 100) {
        m_percent = 100;
        emit progress(100);
    }
    emit finished();
}

void RenderPipeline::cleanup() {
    if (m_proc) {
        m_proc->disconnect(this);
        m_proc->deleteLater();
        m_proc = nullptr;
    }
    if (!m_graphPath.isEmpty()) {
        QFile::remove(m_graphPath);
        m_graphPath.clear();
    }
}

void RenderPipeline::removePartialOutput() {
    if (m_outputPath.isEmpty()) return;
    QFile::remove(m_outputPath);
    m_outputPath.clear();
}
