#include "recording/RenderPipeline.h"
#include "platform/ProcessTree.h"
#include "recording/TimelineGraphBuilder.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace {
// Cap on the ffmpeg stderr we keep for failure messages.
constexpr int kStderrTailChars = 4000;
// How long to wait for a terminated ffmpeg before killing it.
constexpr int kTerminateGraceMs = 2000;
// How long one source may take to report its length. A local file answers in
// well under a second; this is for a file ffprobe hangs on, which is then
// rendered as though it were long enough rather than holding the queue.
constexpr int kProbeTimeoutMs = 10000;
}  // namespace

RenderPipeline::RenderPipeline(QObject* parent)
    : QObject(parent), m_probeTimeoutMs(kProbeTimeoutMs) {
    // Same discovery the capture pipeline uses, so both agree on which ffmpeg
    // this install is running. The resolved paths are kept rather than bare
    // names, so a launch never depends on the process search order.
    m_ffmpegPath = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    m_ffprobePath = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
}

RenderPipeline::~RenderPipeline() {
    if (isRunning()) cancel();
}

void RenderPipeline::setProbeCommandForTesting(const QString& program,
                                               const QStringList& leadingArgs, int timeoutMs) {
    m_ffprobePath = program;
    m_probeLeadingArgs = leadingArgs;
    m_probeTimeoutMs = timeoutMs;
}

bool RenderPipeline::start(const RenderJob& job, QString* error) {
    auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };

    if (isRunning())
        return fail(tr("A render is already running."));
    if (m_ffmpegPath.isEmpty())
        return fail(tr("ffmpeg was not found on PATH, so nothing can be rendered."));
    if (job.outputPath.isEmpty())
        return fail(tr("The job has no output file."));
    if (QFileInfo::exists(job.outputPath))
        return fail(tr("The output file already exists: %1")
                        .arg(QDir::toNativeSeparators(job.outputPath)));

    // Everything that can be refused without knowing how long the sources are
    // is refused here, so the caller still hears about it from start(). It is
    // also what vouches for every sourcePath before one is handed to ffprobe:
    // present, local, and not unlinked.
    const RenderGraph check = TimelineGraphBuilder::build(job.timeline, job.output);
    if (!check.ok)
        return fail(check.error);

    m_job = job;
    m_sourceSeconds.clear();
    m_toProbe.clear();

    if (m_ffprobePath.isEmpty()) {
        // Without ffprobe nothing can be measured, and the render goes ahead
        // as it did before lengths were checked: nothing is cut or reported.
        QString launchError;
        if (!launch(&launchError))
            return fail(launchError);
        if (error) error->clear();
        return true;
    }

    for (const QJsonValue& v : job.timeline) {
        const QString path = v.toObject().value(QStringLiteral("sourcePath")).toString();
        if (!m_toProbe.contains(path)) m_toProbe << path;
    }
    m_probing = true;
    const quint64 run = ++m_run;
    // Queued, so nothing the probes lead to, a failure or the encoder
    // starting, can be emitted before the caller has heard that start()
    // succeeded and recorded which job is running.
    QMetaObject::invokeMethod(this, [this, run] { probeNext(run); }, Qt::QueuedConnection);
    if (error) error->clear();
    return true;
}

void RenderPipeline::probeNext(quint64 run) {
    if (run != m_run || !m_probing) return;   // cancelled, or a later job

    if (m_toProbe.isEmpty()) {
        m_probing = false;
        QString error, note;
        if (!launch(&error, &note)) {
            emit failed(error);
            return;
        }
        if (!note.isEmpty()) emit adjusted(note);
        return;
    }

    const QString path = m_toProbe.takeFirst();
    auto* proc = new QProcess(this);
    m_probe = proc;
    // Every way a probe can end comes through here once: it answered, it
    // failed, it was killed for taking too long, or it never started. Only an
    // answer records a length; anything else leaves the source unmeasured.
    const auto done = [this, proc, run, path](bool answered) {
        if (m_probe != proc) return;          // cancel() has taken it over
        m_probe = nullptr;
        if (answered) {
            // Whatever the file says about itself, so it is parsed rather than
            // trusted; "N/A", which a still image gives, is not a number.
            bool ok = false;
            const double seconds = QString::fromLatin1(proc->readAllStandardOutput())
                                       .trimmed().toDouble(&ok);
            if (ok && std::isfinite(seconds) && seconds > 0.0)
                m_sourceSeconds.insert(path, seconds);
        }
        proc->deleteLater();
        probeNext(run);
    };
    connect(proc, &QProcess::finished, this, [done](int code, QProcess::ExitStatus status) {
        done(status == QProcess::NormalExit && code == 0);
    });
    connect(proc, &QProcess::errorOccurred, this, [done](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) done(false);   // the others still finish
    });
    // Set up before the start, which can fail synchronously into done().
    QTimer::singleShot(m_probeTimeoutMs, proc, [proc] {
        // The whole tree, or a launcher's real ffprobe stays hung; see
        // ProcessTree. finished follows.
        if (proc->state() != QProcess::NotRunning) ProcessTree::kill(quint32(proc->processId()));
    });
    proc->start(m_ffprobePath,
                m_probeLeadingArgs + QStringList{
                    QStringLiteral("-v"), QStringLiteral("error"),
                    QStringLiteral("-show_entries"), QStringLiteral("format=duration"),
                    QStringLiteral("-of"), QStringLiteral("default=noprint_wrappers=1:nokey=1"),
                    path});
}

bool RenderPipeline::launch(QString* error, QString* note) {
    auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };

    // Asked again: measuring the sources takes time, and ffmpeg failing on a
    // file that appeared meanwhile would have that file removed as partial
    // output.
    if (QFileInfo::exists(m_job.outputPath))
        return fail(tr("The output file already exists: %1")
                        .arg(QDir::toNativeSeparators(m_job.outputPath)));

    const RenderGraph graph = TimelineGraphBuilder::build(m_job.timeline, m_job.output,
                                                          m_sourceSeconds);
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
    args << m_job.outputPath;

    if (note) *note = TimelineGraphBuilder::describeClamps(graph.clamped);
    m_outputPath = m_job.outputPath;
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
    if (m_probing) {
        // Still measuring: nothing has been written, so there is nothing to
        // remove, only the probe to end. Bumping the run drops the queued
        // first probe too, if it has not started yet.
        m_probing = false;
        ++m_run;
        if (QProcess* probe = m_probe) {
            m_probe = nullptr;
            probe->disconnect(this);
            if (probe->state() != QProcess::NotRunning) {
                ProcessTree::kill(quint32(probe->processId()));   // see ProcessTree
                probe->waitForFinished(kTerminateGraceMs);
            }
            probe->deleteLater();
        }
        return;
    }
    if (!m_proc) return;
    m_cancelled = true;
    // Ended at once, with everything it started. terminate() asks a process
    // to close its windows, which a windowless ffmpeg never has, so every
    // cancel waited out the grace period on the GUI thread; and kill() ended
    // only a package manager's launcher, leaving the real ffmpeg rendering.
    // The partial output is deleted below, so nothing is lost by not asking.
    ProcessTree::kill(quint32(m_proc->processId()));
    m_proc->waitForFinished(kTerminateGraceMs);
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
