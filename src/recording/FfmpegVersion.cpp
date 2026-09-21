#include "recording/FfmpegVersion.h"
#include "platform/ProcessTree.h"

#include <QProcess>
#include <QRegularExpression>
#include <QStringList>
#include <QTimer>

QString FfmpegVersion::parse(QStringView versionOutput) {
    // Only the first line; the rest describes how the build was configured.
    const QString output = versionOutput.toString();
    const QString line = output.left(output.indexOf(QLatin1Char('\n'))).trimmed();
    static const QRegularExpression space(QStringLiteral("\\s+"));
    const QStringList words = line.split(space, Qt::SkipEmptyParts);
    if (words.size() < 3 || words[0] != QLatin1String("ffmpeg")
        || words[1] != QLatin1String("version"))
        return {};

    // A release number, which some builds prefix with the tag's "n", ended by
    // anything that cannot continue it: "8.1.1-essentials_build" is 8.1.1, and
    // "2024-12-19-git" is not a release at all.
    static const QRegularExpression release(
        QStringLiteral("^n?(\\d+\\.\\d+(?:\\.\\d+)?)(?![\\d.])"));
    const QRegularExpressionMatch match = release.match(words[2]);
    return match.hasMatch() ? match.captured(1) : words[2];
}

void FfmpegVersion::probe(const QString& ffmpegPath, QObject* context,
                          std::function<void(const Result&)> done, int timeoutMs) {
    if (ffmpegPath.isEmpty()) {
        done(Result{});
        return;
    }

    auto* proc = new QProcess(context);
    // Nothing reads it, and left connected it would be buffered for nothing.
    proc->setStandardErrorFile(QProcess::nullDevice());

    // Exactly one of these ends each probe: finished for a process that
    // started, including one ended by the timeout below, and FailedToStart for
    // one that did not, which never finishes.
    QObject::connect(proc, &QProcess::finished, proc,
                     [proc, ffmpegPath, done](int code, QProcess::ExitStatus status) {
        const bool answered = status == QProcess::NormalExit && code == 0;
        done(Result{ffmpegPath,
                    answered ? parse(QString::fromLocal8Bit(proc->readAllStandardOutput()))
                             : QString()});
        proc->deleteLater();
    });
    QObject::connect(proc, &QProcess::errorOccurred, proc,
                     [proc, ffmpegPath, done](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;   // the others still finish
        done(Result{ffmpegPath, QString()});
        proc->deleteLater();
    });
    QTimer::singleShot(timeoutMs, proc, [proc] {
        // The whole tree; see ProcessTree. finished follows.
        if (proc->state() != QProcess::NotRunning)
            ProcessTree::kill(quint32(proc->processId()));
    });
    proc->start(ffmpegPath, {QStringLiteral("-version")});
}
