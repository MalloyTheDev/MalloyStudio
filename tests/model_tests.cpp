#include "audio/AudioController.h"
#include "audio/AudioMix.h"
#include "audio/Resampler.h"
#include "capture/CaptureBackend.h"
#include "capture/CaptureController.h"
#include "capture/WgcCapture.h"
#include "capture/WindowCapture.h"
#include "capture/WorkerRetirement.h"
#include "platform/FrameProfile.h"
#include "ui/PreviewWidget.h"
#include "input/HotkeyManager.h"
#include "model/Canvas.h"
#include "model/FilterEffect.h"
#include "model/Scene.h"
#include "model/SceneCollection.h"
#include "model/SceneItem.h"
#include "model/Source.h"
#include "project/ProjectDocument.h"
#include "project/MediaPathPolicy.h"
#include "project/ClipsRegistry.h"
#include "project/ProjectRegistry.h"
#include "project/MediaRegistry.h"
#include "project/RecentRecordings.h"
#include "recording/RenderQueue.h"
#include "recording/TimelineGraphBuilder.h"
#include "recording/OutputSettings.h"
#include "platform/MachineLoad.h"
#include "recording/EncoderPipeline.h"
#include "recording/MediaController.h"
#include "recording/RingTimedPcmSource.h"
#include "recording/StreamSettings.h"
#include "recording/RtmpKeyRelay.h"
#include "recording/RingTimedFrameSource.h"
#include "platform/TwitchAuth.h"
#include "platform/TwitchApi.h"
#include "platform/CredentialStore.h"
#include "platform/SmartConfig.h"
#include "recording/StreamingPipeline.h"
#include "recording/EncoderRegistry.h"
#include "ui/workspaces/EditorWorkspace.h"
#include "ui/workspaces/TimelineEdits.h"
#include "ui/shell/EditingFocus.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QSettings>
#include <QSignalSpy>
#include <QBuffer>
#include <QTemporaryDir>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QSemaphore>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QSpinBox>
#include <limits>
#include <QTimer>
#include <QtTest/QtTest>
#include <QUndoStack>

#include <algorithm>
#include <chrono>
#include <cstdint>

class FakeCaptureSession final : public CaptureSession {
public:
    FakeCaptureSession(int adapterIndex, int outputIndex, QObject* parent = nullptr)
        : CaptureSession(parent), m_key(CaptureController::keyFor(adapterIndex, outputIndex))
    { created.append(this); }

    void startCapture() override { started.append(m_key); }
    void stopCapture() override { stopped.append(m_key); }

    // Stands in for a backend's own counters, so a test can say what a session
    // produced and lost without running a capture.
    CaptureStats stats() const override { return m_stats; }
    void setStats(int produced, int dropped) {
        m_stats.framesProduced = produced;
        m_stats.framesDropped  = dropped;
    }

    static QStringList started;
    static QStringList stopped;
    static QList<FakeCaptureSession*> created;

private:
    QString      m_key;
    CaptureStats m_stats;
};

QStringList FakeCaptureSession::started;
QStringList FakeCaptureSession::stopped;
QList<FakeCaptureSession*> FakeCaptureSession::created;

// A worker that takes no notice of a request to stop until it is let go. It
// stands in for one blocked in a call that someone else services, such as
// PrintWindow on a window whose application has stopped responding.
class StuckWorker final : public QThread {
public:
    void requestStop() { stopRequests.fetch_add(1); }
    void release() { m_release.release(); }
    std::atomic<int> stopRequests{0};

protected:
    void run() override { m_release.acquire(); }

private:
    QSemaphore m_release;
};

// A local stand-in for Twitch. Each request gets the next queued answer, or a
// 500 once they run out. While holding, requests wait unanswered until
// release(), so a test can act while one is in flight.
class FakeTwitch final : public QObject {
public:
    FakeTwitch() {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* s = m_server.nextPendingConnection()) {
                connect(s, &QTcpSocket::readyRead, this, [this, s] { onReadable(s); });
                if (s->bytesAvailable() > 0) onReadable(s);
            }
        });
        m_server.listen(QHostAddress::LocalHost);
    }

    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }
    void answer(int status, const QByteArray& body) { m_answers.append({status, body}); }
    void hold() { m_holding = true; }
    void release() {
        m_holding = false;
        const QList<QTcpSocket*> held = std::exchange(m_held, {});
        for (QTcpSocket* s : held) respond(s);
    }
    int waiting() const { return int(m_held.size()); }

    QStringList requests;   // "METHOD /path", in arrival order

private:
    struct Answer { int status; QByteArray body; };

    void onReadable(QTcpSocket* s) {
        QByteArray& buf = m_buffers[s];
        buf += s->readAll();
        const qsizetype end = buf.indexOf("\r\n\r\n");
        if (end < 0) return;
        qsizetype length = 0;
        for (const QByteArray& line : buf.left(end).split('\n')) {
            const QByteArray l = line.trimmed().toLower();
            if (l.startsWith("content-length:")) length = l.mid(15).trimmed().toLongLong();
        }
        if (buf.size() < end + 4 + length) return;
        const QList<QByteArray> requestLine = buf.left(buf.indexOf("\r\n")).split(' ');
        requests << QString::fromLatin1(requestLine.value(0) + ' ' + requestLine.value(1));
        m_buffers.remove(s);
        if (m_holding) m_held.append(s);
        else respond(s);
    }

    void respond(QTcpSocket* s) {
        const Answer a = m_answers.isEmpty() ? Answer{500, {}} : m_answers.takeFirst();
        s->write("HTTP/1.1 " + QByteArray::number(a.status) + " Stand-in\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: " + QByteArray::number(a.body.size()) + "\r\n"
                 "Connection: close\r\n\r\n" + a.body);
        s->disconnectFromHost();
    }

    QTcpServer m_server;
    QHash<QTcpSocket*, QByteArray> m_buffers;
    QList<QTcpSocket*> m_held;
    QList<Answer> m_answers;
    bool m_holding = false;
};

// Tests keep Twitch tokens under their own credential name, never the one a
// real sign-in uses, and erase it when done.
const QString kTestTwitchCredential = QStringLiteral("MalloyStudioTests_TwitchTokens");

void seedTwitchTokens(bool expired) {
    TwitchTokens tokens;
    tokens.accessToken  = QStringLiteral("old-access");
    tokens.refreshToken = QStringLiteral("old-refresh");
    tokens.expiresAt    = QDateTime::currentDateTimeUtc().addSecs(expired ? -60 : 3600);
    CredentialStore::save(kTestTwitchCredential, tokens.toJson());
}

const QByteArray kFreshTokens =
    R"({"access_token":"new-access","refresh_token":"new-refresh","expires_in":14400,"scope":["channel:read:stream_key"]})";
const QByteArray kDeviceCode =
    R"({"device_code":"dev-secret","user_code":"ABCDEFGH","verification_uri":"https://www.twitch.tv/activate","expires_in":1800,"interval":1})";

class MalloyModelTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void reusableSourcesShareSettingsAndGc();
    void v1ProjectMigratesAndV2RoundTrips();
    void undoRedoAndEditCoalescing();
    void captureControllerReconcilesVisibleDisplaySources();
    void audioReservationInProjectJson();
    void recorderConstructsRegardlessOfFfmpegPresence();
    void recorderCanRestartAfterFinalization();
    void controllerCanReplacePipelineFromFinishedSignal();
    void audioControllerHasDefaultLoopbackInput();
    void audioControllerPersistsVolumeAndMute();
    // v5 new tests
    void windowCaptureKeyIsStable();
    void filterChainRoundTrips();
    void outputSettingsRoundtrip();
    void perSourceAudioReconcileActivates();
    // v6 Sub-A new tests
    void encoderRegistryAlwaysListsLibx264();
    void streamSettingsRtmpUrlTemplatesExpandCorrectly();
    // v6 Sub-B/C/D new tests
    void encoderPipelineFactoryDispatchesByTarget();
    void sceneCollectionStudioModeProgramAndPreviewIndependent();
    void hotkeyManagerRoundTripsBindingsThroughQSettings();
    void replayBufferTrimsToLastNSeconds();
    void audioMixerSumsTwoSignalsWithoutClipping();
    void chromaKeyFilterRemovesTargetColor();
    // Regression: ffmpeg recording crashed with EINVAL (-22) when no audio
    // input had data flowing because the mixer's early-return skipped emitting
    // any bytes on the pcm bus. Now it must emit silence every tick.
    void audioMixerEmitsSilenceWhenAllRingsAreEmpty();
    // Regression: hevc_nvenc + a libx264-style preset name ("faster") produced
    // EINVAL from ffmpeg because EncoderPipeline::buildOutputArgs hard-coded
    // `-preset <s.preset> -crf` instead of consulting EncoderRegistry for the
    // selected codec's per-encoder builder.
    void encoderPipelineRespectsRegistryPerCodecArgs();
    // v7 Tier 1 regression tests: the same -22 class of bug existed in the
    // streaming path (StreamingPipeline never consulted EncoderRegistry), and
    // StreamSettings.keyframeSec was being silently dropped instead of plumbed
    // through to ffmpeg.
    void streamingPipelineUsesRegistryNotHardcodedX264();
    void streamingPipelineHonorsKeyframeSec();
    void streamingPipelineForcesGopWhenSoftwareEncoderOmitsIt();
    // v7 Tier 2: prove that the UI-added Microphone flow does the right thing
    // at the model level — adds an AudioInput source with the given device id,
    // fires audioInputsChanged so reconcileInputs starts a worker, and
    // contributes to gatherVisibleAudioIds() the way MainWindow expects.
    void addAudioInputFromUiCreatesScopedSource();
    void addCameraCreatesScopedSourceAndRoundTrips();
    void editorClipRoundTripPreservesV4Fields();
    void editorClipFromLegacyV3JsonAppliesDefaults();
    void audioControllerEmitsInputControlChangedOnValueChange();
    void addingAudioInputTriggersAudioInputsChanged();
    void togglingAudioInputVisibilityChangesGatherList();
    // v7 Tier 3: per-filter visibility toggle and the ffmpeg progress parser.
    // The filter-enabled flag is a small surface but easy to regress on
    // JSON round-trip, so we lock the schema with a test. The progress-line
    // parser anchors the streaming stats display.
    void filterEnabledFlagRoundtripsJson();
    void streamProgressLineParsesBitrateAndDrops();
    // Machine load: the pure percentage math, including every case where two
    // readings say nothing and the honest answer is to report unknown.
    void machineLoadReportsUnknownRatherThanGuessing();
    // Capture cadence belongs to the source, output cadence to the sink. This
    // is the rule that separates them, and getting it wrong either invents
    // media or starves an ingest.
    void sinkCadenceDecidesWhenAFrameIsDue();
    // v7 Tier 4: the audio.mute.<id> action dispatch toggles the matching
    // AudioController input's mute flag. Tests the contract MainWindow's
    // hotkey dispatcher relies on without spinning up the global hotkey
    // RegisterHotKey machinery (which needs a real Win32 message pump).
    void audioMuteActionIdTogglesInput();
    // Clips registry: metadata persists to JSON and survives a reload, and
    // favorites toggle. Backs the Clips workspace.
    void clipsRegistryRoundTrips();
    // Project registry: scans a dir for *.malloy.json, ignores other files,
    // and parses scene counts. Backs the Projects workspace.
    void projectRegistryScansMalloyFiles();
    // Media registry: classifies files by extension and ignores non-media.
    void mediaRegistryClassifiesByExtension();
    // Render queue: promotes, progresses, completes, persists.
    void renderQueueProcessesAndPersists();
    // Render queue control surface: retry (Failed→Pending), cancel (drops
    // Active/Pending, ignores Completed/Failed), clearCompleted.
    void renderQueueRetryCancelClear();
    // Render queue pause gates promotion: paused queues hold Pending jobs.
    void renderQueuePauseHoldsPendingJobs();
    // ProjectDocument v3: editor timeline round-trips inside .malloy.json and
    // older files (no "timeline" key) load with an empty timeline.
    void projectDocumentV3TimelineRoundTrips();
    // Registries degrade gracefully: missing/corrupt stores load empty, ops on
    // unknown ids no-op, scans of empty/nonexistent dirs yield nothing.
    void registriesDegradeGracefullyOnBadInput();
    // Recent recordings: the dashboard panel lists real capture files, so the
    // scan must filter by extension, order newest first, honour the limit and
    // tolerate a missing folder.
    void recentRecordingsScanFiltersOrdersAndLimits();
    void recentRecordingsRelativeTimeBuckets();
    // ADR-0001: clips carry the media they play plus an in-point, so a render
    // can resolve them. The schema has to round-trip, older projects have to
    // load as unlinked, and the trim/split arithmetic has to keep the timeline
    // and the source in step.
    // ADR-0002: a job carries the settings and the timeline snapshot it renders
    // with, so a queued render is reproducible and a retry re-renders the same
    // thing. Jobs from before the schema cannot run and are retired on sight.
    void outputSettingsJsonRoundTrips();
    void renderJobCarriesSettingsAndSnapshot();
    void renderQueueRetiresJobsWithoutASnapshot();
    void renderQueueRejectsUnrenderableRequests();
    // ADR-0003: the timeline becomes an ffmpeg filter graph. The builder is pure,
    // so placement, trimming, mixing and every refusal are testable with no
    // encoder present. Media paths must reach ffmpeg as arguments only.
    // Regression: the blur seeded its sliding window with an unclamped upper
    // index, so any image narrower or shorter than the radius (which goes to 32)
    // read past the row or column. A source only a few pixels wide is ordinary.
    // Regression: the mixer summed each input's left and right together before
    // applying pan, so every stereo source reached the recording and the stream
    // as mono. Pan is a balance control on a stereo bus.
    // Regression: project names kept a stray ".malloy" because
    // QFileInfo::completeBaseName only strips the last suffix, so recordings
    // and renders were named "project.malloy-20260906.mp4".
    void projectDisplayNameStripsCompoundExtension();
    // Regression: the mixer popped one device chunk per tick and discarded
    // whatever did not fit, so any chunk size that disagreed with the tick size
    // lost audio and crackled.
    // Regression: capture forwarded device samples at whatever rate the device
    // ran at, so a 44.1 kHz microphone was recorded as if it were 48 kHz and
    // played back fast, sharp and drifting.
    // Regression: a shortcut Windows refused was still stored and displayed as
    // bound, so the user saw a working hotkey that never fired and the failure
    // repeated silently on every launch.
    // Regression: creating a configured layer pushed two undo commands, so the
    // first undo stripped the image path or window and left an empty layer
    // behind instead of removing what the user had just added.
    // Regression: ffmpeg prints the destination URL on a failed connect even at
    // the production log level, and that tail is shown in an error dialog, so
    // the stream key could be screenshotted or pasted into a bug report.
    // The relay keeps the stream key out of ffmpeg's command line by giving it a
    // placeholder and substituting on the way upstream. The substitution has to
    // be length-preserving, has to survive an occurrence split across two reads,
    // and must not withhold bytes that will never be completed.
    // Twitch device code flow: the request bodies and the response parsing are
    // the whole protocol surface, and they have to be right before anything
    // touches the network. The waiting states in particular must not be read as
    // failures, or sign-in gives up while the user is still approving.
    // Smart config: the recommendation is a pure function of a hardware
    // profile, so the rules are checked against fixed machines rather than
    // against whichever one the tests happen to run on.
    void smartConfigRecommendsForTheHardware();
    void smartConfigDerivesShapeFromBitrate();
    void smartConfigWarnsRatherThanGuessing();
    void twitchAuthBuildsProtocolRequests();
    void twitchAuthReadsDeviceAndTokenResponses();
    void twitchTokensExpireAndRoundTrip();
    void twitchApiParsesHelixPayloads();
    void rtmpRelaySubstitutesAcrossReadBoundaries();
    void rtmpRelayTransportFollowsTheScheme();
    void aSlowUplinkBacksUpIntoFfmpegNotMemory();
    void aRelayThatHeldBackStillDeliversEverything();
    void loadedProjectHoldsItsDevicesUntilAllowed();
    void undoAndRedoKeepADeclinedDeviceHeld();
    void sourceIdsNearTheTopDoNotOverflow();
    void anEditSessionEndsWithTheStateItBeganIn();
    void aCommandDuringAnEditSessionKeepsItsOwnUndoStep();
    void fileDevicesAreHeldBeforeTheLoadIsAnnounced();
    void userChosenSharePathSurvivesUndo();
    void everyMicChangeIsAnnouncedStructurally();
    void undoInStudioModeLeavesTheProgramOnAir();
    void removingAScenePreservesWhatIsOnAir();
    void stagingASceneKeepsTheProgramCaptureRunning();
    void projectMediaPathsMustBeLocalFiles();
    void encoderRedactsTheStreamKeyFromFfmpegOutput();
    void addingAConfiguredLayerIsOneUndoStep();
    void hotkeyManagerReportsRefusedBindings();
    void resamplerPreservesPitchAcrossRates();
    void pcmFifoKeepsTheSampleStreamContinuous();
    void mixerKeepsStereoSeparation();
    void blurHandlesImagesSmallerThanItsRadius();
    void timelineGraphPlacesTrimsAndScalesClips();
    void timelineGraphMixesAudioAndKeepsPathsOutOfTheGraph();
    void timelineGraphRefusesWhatItCannotRender();
    void timelineGraphBoundsNumbersBeforeArithmetic();
    void restoredRenderJobsAreValidatedLikeEnqueuedOnes();
    void restoredRenderJobsMustWriteToALocalDrive();
    void clipsLongerThanTheTimelineArePlacedNotAborted();
    void undoKeepsTheLiveCaptureFrameOnAir();
    void aSavedReplayPlaysInRealTime();
    void aWorkerThatWillNotStopIsCutLooseNotDestroyed();
    void aStopRequestedBeforeTheWorkerRunsIsKept();
    void aSignOutDuringARefreshStaysSignedOut();
    void aRefreshTwitchDidNotAnswerKeepsTheAccount();
    void aCancelledSignInIgnoresALateCode();
    void aNetworkBlipWhilePollingDoesNotEndTheSignIn();
    void theChannelIdIsForgottenWhenTheAccountChanges();
    void replayAudioWaitsForTheEncoder();
    void closingDuringAReplaySaveFinishesItFirst();
    void theReplayBufferIsAFrameConsumer();
    void spinBoxesAndTextFieldsKeepTheirDigits();
    void editorClipRoundTripPreservesSourceReference();
    void editorLegacyClipLoadsAsUnlinked();
    void timelineTrimKeepsSourceInSync();
    void timelineSplitDerivesRightHandSourceIn();
    // Capture backend preference: parses what is in the settings file, and
    // anything it does not recognise means the backend every machine can run.
    void captureBackendPreferenceParses();
    // A WGC frame time is converted, not trusted: a stamp that does not share
    // this machine's clock base is refused in favour of the arrival time.
    void wgcBackendTimestampsAreCheckedNotTrusted();
    // A frame holds its slot in the bounded handoff for exactly as long as it
    // exists, including while it is moved through a queue.
    void capturedFrameHoldsItsHandoffSlot();
    // Backend counters survive the sessions that produced them, so a source
    // that is restarted mid-run does not reset what the run captured.
    void captureStatsAccumulateAcrossSessionChurn();
    // A file holds quality and a stream holds its bitrate, and the encoder
    // arguments say so.
    void rateControlFollowsWhereTheMediaIsGoing();
    // The rawvideo input declares the rate the sink is clocked at, which is
    // what stops ffmpeg quantising arrivals to its 25 fps default.
    void inputDeclaresTheConfiguredFrameRate();
    // The frame profiler: buckets keep their order, percentiles land where
    // the samples actually are, and nothing is recorded while it is off.
    void frameProfileSummarisesADistribution();
    // Composition happens because somebody consumes it, never because a
    // widget was painted. The rule that decides is this one.
    void compositionFollowsConsumersNotPaintEvents();
    // rawvideo cannot express a stride, so a padded frame has to be sent a
    // row at a time rather than as one block.
    void rawVideoDeclarationCoversSizeNotOnlyFormat();
    void rawVideoNeedsTightlyPackedRows();
    // A settings control must be able to show the rate that is stored,
    // because the page writes back what it shows.
    void frameRateChoicesAlwaysIncludeTheConfiguredRate();
    // Capture should run when something wants frames and not otherwise.
    void captureDemandFollowsConsumers();
};

// Creates a placeholder media file so a clip can pass the graph builder's
// existence check. The contents are irrelevant: no test here runs ffmpeg.
static QString makeMediaFile(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return QString();
    f.write("not really video");
    f.close();
    return path;
}

// One clip of `dur` seconds at `start`, reading `sourcePath` from `sourceIn`.
static QJsonObject makeClip(const QString& sourcePath, double start, double dur,
                            double sourceIn = 0.0, bool audio = false) {
    QJsonObject c;
    c.insert(QStringLiteral("track"), audio ? 3 : 0);
    c.insert(QStringLiteral("start"), start);
    c.insert(QStringLiteral("dur"), dur);
    c.insert(QStringLiteral("label"), QFileInfo(sourcePath).fileName());
    c.insert(QStringLiteral("audio"), audio);
    c.insert(QStringLiteral("sourcePath"), sourcePath);
    c.insert(QStringLiteral("sourceIn"), sourceIn);
    return c;
}

// A render request that passes enqueue validation: a one-clip timeline, default
// encoder settings and an output file inside an existing directory. The media it
// names does not exist, so the render itself fails deterministically without
// needing ffmpeg.
static RenderRequest makeRenderRequest(const QString& outputPath) {
    RenderRequest r;
    r.name        = QFileInfo(outputPath).fileName();
    r.project     = QStringLiteral("Proj");
    r.projectPath = QStringLiteral("C:/proj/proj.malloy.json");
    r.outputPath  = outputPath;
    QJsonObject clip;
    clip.insert(QStringLiteral("track"), 0);
    clip.insert(QStringLiteral("start"), 0.0);
    clip.insert(QStringLiteral("dur"),   5.0);
    clip.insert(QStringLiteral("sourcePath"), QStringLiteral("C:/media/a.mp4"));
    clip.insert(QStringLiteral("sourceIn"),   0.0);
    r.timeline = QJsonArray{clip};
    return r;
}

void MalloyModelTests::initTestCase() {
    // Isolate QSettings writes from real user state.
    QCoreApplication::setOrganizationName(QStringLiteral("MalloyStudioTests"));
    QCoreApplication::setApplicationName(QStringLiteral("ModelTests"));
}

void MalloyModelTests::reusableSourcesShareSettingsAndGc() {
    SceneCollection scenes;
    QUndoStack undo;
    scenes.setUndoStack(&undo);

    scenes.addScene(QStringLiteral("Scene"));
    SceneItem* first = scenes.addNewSourceToCurrent(
        QStringLiteral("Title"), Source::Type::Text, QStringLiteral("hello"));
    QVERIFY(first);
    const int sourceId = first->sourceId();

    scenes.addExistingSourceToCurrent(sourceId);
    QCOMPARE(scenes.currentScene()->itemCount(), 2);
    QCOMPARE(scenes.sourceCount(), 1);
    QCOMPARE(scenes.sourceReferenceCount(sourceId), 2);

    scenes.setCurrentSourceText(0, QStringLiteral("shared"));
    QCOMPARE(scenes.sourceById(sourceId)->text(), QStringLiteral("shared"));

    scenes.duplicateCurrentItemAt(0);
    QCOMPARE(scenes.currentScene()->itemCount(), 3);
    QCOMPARE(scenes.sourceReferenceCount(sourceId), 3);

    scenes.removeCurrentItemAt(0);
    QCOMPARE(scenes.sourceCount(), 1);
    QCOMPARE(scenes.sourceReferenceCount(sourceId), 2);
    scenes.removeCurrentItemAt(0);
    scenes.removeCurrentItemAt(0);
    QCOMPARE(scenes.sourceCount(), 0);
}

void MalloyModelTests::v1ProjectMigratesAndV2RoundTrips() {
    QJsonObject v1Source{
        {QStringLiteral("name"), QStringLiteral("Migrated Text")},
        {QStringLiteral("type"), QStringLiteral("text")},
        {QStringLiteral("text"), QStringLiteral("from v1")},
        {QStringLiteral("color"), QStringLiteral("#ff2e88ff")},
    };
    QJsonObject v1Item{
        {QStringLiteral("id"), 7},
        {QStringLiteral("visible"), true},
        {QStringLiteral("locked"), false},
        {QStringLiteral("transform"), QJsonObject{
            {QStringLiteral("x"), 10},
            {QStringLiteral("y"), 20},
            {QStringLiteral("w"), 300},
            {QStringLiteral("h"), 80},
        }},
        {QStringLiteral("source"), v1Source},
    };
    QJsonObject v1Root{
        {QStringLiteral("app"), QStringLiteral("MalloyStudio")},
        {QStringLiteral("version"), 1},
        {QStringLiteral("currentScene"), 0},
        {QStringLiteral("scenes"), QJsonArray{
            QJsonObject{
                {QStringLiteral("name"), QStringLiteral("Migrated Scene")},
                {QStringLiteral("selectedItem"), 0},
                {QStringLiteral("items"), QJsonArray{v1Item}},
            }
        }},
    };

    SceneCollection migrated;
    QString error;
    QVERIFY2(migrated.loadFromJson(v1Root, &error), qPrintable(error));
    QCOMPARE(migrated.sourceCount(), 1);
    QCOMPARE(migrated.toJson().value(QStringLiteral("version")).toInt(), 2);
    QVERIFY(migrated.toJson().contains(QStringLiteral("sources")));
    QCOMPARE(migrated.sourceAt(0)->text(), QStringLiteral("from v1"));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("roundtrip.malloy.json"));
    QVERIFY2(ProjectDocument::saveToFile(migrated, path, &error), qPrintable(error));

    SceneCollection loaded;
    QVERIFY2(ProjectDocument::loadFromFile(loaded, path, &error), qPrintable(error));
    QCOMPARE(loaded.sceneCount(), 1);
    QCOMPARE(loaded.sourceCount(), 1);
    QCOMPARE(loaded.currentScene()->itemCount(), 1);
    QCOMPARE(loaded.sourceAt(0)->text(), QStringLiteral("from v1"));
    QCOMPARE(loaded.currentScene()->itemAt(0)->sourceId(), loaded.sourceAt(0)->id());
}

void MalloyModelTests::undoRedoAndEditCoalescing() {
    SceneCollection scenes;
    QUndoStack undo;
    scenes.setUndoStack(&undo);

    scenes.addScene(QStringLiteral("Scene"));
    QCOMPARE(scenes.sceneCount(), 1);
    undo.undo();
    QCOMPARE(scenes.sceneCount(), 0);
    undo.redo();
    QCOMPARE(scenes.sceneCount(), 1);

    SceneItem* item = scenes.addNewSourceToCurrent(
        QStringLiteral("Block"), Source::Type::ColorBlock, QString(), QColor(10, 20, 30));
    QVERIFY(item);
    undo.clear();
    undo.setClean();

    const QRectF original = scenes.currentScene()->itemAt(0)->transform();
    const QRectF first = original.translated(50, 60);
    const QRectF second = original.translated(100, 120);
    scenes.beginCurrentItemTransformEdit(0);
    scenes.updateCurrentItemTransformEdit(0, first);
    scenes.updateCurrentItemTransformEdit(0, second);
    scenes.commitCurrentItemTransformEdit();
    QCOMPARE(undo.count(), 1);
    QCOMPARE(scenes.currentScene()->itemAt(0)->transform(), MalloyCanvas::clampRect(second));
    undo.undo();
    QCOMPARE(scenes.currentScene()->itemAt(0)->transform(), original);
    undo.redo();
    QCOMPARE(scenes.currentScene()->itemAt(0)->transform(), MalloyCanvas::clampRect(second));

    undo.clear();
    const int sourceId = scenes.currentScene()->itemAt(0)->sourceId();
    scenes.beginEditSession();
    scenes.setCurrentSourceText(0, QStringLiteral("a"), false);
    scenes.setCurrentSourceText(0, QStringLiteral("ab"), false);
    scenes.commitEditSession(QStringLiteral("Edit Text"));
    QCOMPARE(undo.count(), 1);
    QCOMPARE(scenes.sourceById(sourceId)->text(), QStringLiteral("ab"));
    undo.undo();
    QCOMPARE(scenes.sourceById(sourceId)->text(), QString());
}

void MalloyModelTests::captureControllerReconcilesVisibleDisplaySources() {
    FakeCaptureSession::started.clear();
    FakeCaptureSession::stopped.clear();

    SceneCollection scenes;
    QUndoStack undo;
    scenes.setUndoStack(&undo);
    scenes.addScene(QStringLiteral("Scene"));
    scenes.addNewSourceToCurrent(
        QStringLiteral("Display"),
        Source::Type::DisplayCapture,
        QString(),
        QColor(),
        1,
        2);

    CaptureController controller(
        &scenes,
        [](int adapterIndex, int outputIndex, QObject* parent) {
            return new FakeCaptureSession(adapterIndex, outputIndex, parent);
        });

    QCOMPARE(FakeCaptureSession::started.count(QStringLiteral("1:2")), 1);
    QCOMPARE(controller.activeSessionCount(), 1);

    scenes.setCurrentItemVisible(0, false);
    QCOMPARE(FakeCaptureSession::stopped.count(QStringLiteral("1:2")), 1);
    QCOMPARE(controller.activeSessionCount(), 0);

    undo.undo();
    QCOMPARE(FakeCaptureSession::started.count(QStringLiteral("1:2")), 2);
    QCOMPARE(controller.activeSessionCount(), 1);

    scenes.setCurrentSourceMonitor(0, 3, 4);
    QCOMPARE(FakeCaptureSession::stopped.count(QStringLiteral("1:2")), 2);
    QCOMPARE(FakeCaptureSession::started.count(QStringLiteral("3:4")), 1);
    QCOMPARE(controller.activeSessionCount(), 1);
}

void MalloyModelTests::audioReservationInProjectJson() {
    // v4 reserves a top-level "audio" object so v5 can land per-scene routing
    // without bumping the schema version.
    SceneCollection scenes;
    scenes.addScene(QStringLiteral("Scene"));
    const QJsonObject root = scenes.toJson();
    QVERIFY(root.contains(QStringLiteral("audio")));
    QVERIFY(root.value(QStringLiteral("audio")).isObject());
    QCOMPARE(root.value(QStringLiteral("version")).toInt(), 2);

    // Re-loading should be a no-op for the unknown-to-loader audio key.
    SceneCollection roundtrip;
    QString err;
    QVERIFY(roundtrip.loadFromJson(root, &err));
    QVERIFY(err.isEmpty());
}

void MalloyModelTests::recorderConstructsRegardlessOfFfmpegPresence() {
    // EncoderPipeline (the Recorder replacement) must construct and query
    // ffmpegAvailable() without crashing regardless of whether ffmpeg is on PATH.
    RecorderPipeline pipeline;
    Q_UNUSED(pipeline.ffmpegAvailable());  // either true or false is acceptable
    QVERIFY(!pipeline.isRunning());

    // start() with null frame/audio sources must fail synchronously with a
    // non-empty error message, not crash.
    EncoderPipeline::Target target;
    target.kind        = EncoderPipeline::Target::Kind::File;
    target.destination = QStringLiteral("nonexistent/out.mp4");
    target.output      = OutputSettings{};

    QString err;
    const bool ok = pipeline.start(target, nullptr, nullptr, &err);
    QVERIFY(!ok);
    QVERIFY(!err.isEmpty());
    QVERIFY(!pipeline.isRunning());

    // StreamSettings URL template expansion sanity check (no network or
    // credential access — purely a string operation).
    StreamSettings ss;
    ss.service   = StreamSettings::Service::Twitch;
    ss.streamKey = QStringLiteral("live_test_key_12345");
    const QString url = ss.rtmpUrl();
    QVERIFY(url.startsWith(QStringLiteral("rtmp://")));
    QVERIFY(url.contains(ss.streamKey));
}

namespace {
class RecordingTestFrames final : public TimedFrameSource {
public:
    RecordingTestFrames() : m_image(1920, 1080, QImage::Format_ARGB32) {
        m_image.fill(QColor(40, 110, 180));
    }
    QImage currentFrame() override { return m_image; }
    int nativeWidth() const override { return m_image.width(); }
    int nativeHeight() const override { return m_image.height(); }
private:
    QImage m_image;
};

class RecordingTestAudio final : public TimedPcmSource {
public:
    RecordingTestAudio() {
        connect(&m_timer, &QTimer::timeout, this, [this] {
            emit pcmReady(QByteArray(3840, '\0')); // 20 ms of 48 kHz stereo PCM
        });
        m_timer.start(20);
    }
    int sampleRate() const override { return 48000; }
    int channels() const override { return 2; }
private:
    QTimer m_timer;
};

OutputSettings recordingTestSettings() {
    OutputSettings settings;
    settings.width = 320;
    settings.height = 180;
    settings.fps = 10;
    settings.preset = QStringLiteral("ultrafast");
    return settings;
}

bool recordingDecodes(const QString& ffmpeg, const QString& path) {
    QProcess decoder;
    decoder.start(ffmpeg, {QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-i"), path, QStringLiteral("-map"), QStringLiteral("0:v:0"),
        QStringLiteral("-map"), QStringLiteral("0:a:0"),
        QStringLiteral("-f"), QStringLiteral("null"), QStringLiteral("-")});
    if (!decoder.waitForFinished(10000)) {
        decoder.kill();
        decoder.waitForFinished();
        return false;
    }
    const QByteArray errors = decoder.readAllStandardError();
    if (!errors.isEmpty()) qWarning().noquote() << errors;
    return decoder.exitStatus() == QProcess::NormalExit && decoder.exitCode() == 0
        && errors.isEmpty();
}
}

void MalloyModelTests::recorderCanRestartAfterFinalization() {
    RecorderPipeline pipeline;
    if (!pipeline.ffmpegAvailable()) QSKIP("Real encoder lifecycle requires ffmpeg in PATH");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    RecordingTestFrames frames;
    RecordingTestAudio audio;
    QSignalSpy finished(&pipeline, &EncoderPipeline::finished);
    QSignalSpy errors(&pipeline, &EncoderPipeline::errorOccurred);
    EncoderPipeline::Target target;
    target.output = recordingTestSettings();
    QString error;

    for (int run = 0; run < 2; ++run) {
        target.destination = dir.filePath(QStringLiteral("restart-%1.mp4").arg(run));
        QVERIFY2(pipeline.start(target, &frames, &audio, &error), qPrintable(error));
        QVERIFY(pipeline.isRunning());
        QTest::qWait(1500);
        pipeline.stop();
        QVERIFY(!pipeline.isRunning());
        QCOMPARE(finished.size(), run + 1);
        QCOMPARE(errors.size(), 0);
        QVERIFY(finished.last().at(1).toLongLong() > 0);
        QVERIFY(recordingDecodes(pipeline.ffmpegPath(), target.destination));
    }

    // Stop before queued pipe-connection callbacks are delivered, then reuse
    // the same object. A stop request must survive worker startup races.
    for (int run = 0; run < 8; ++run) {
        target.destination = dir.filePath(QStringLiteral("cancel-%1.mp4").arg(run));
        QVERIFY2(pipeline.start(target, &frames, &audio, &error), qPrintable(error));
        pipeline.stop();
        QVERIFY(!pipeline.isRunning());
        QCOMPARE(finished.size(), run + 3);
        QCoreApplication::processEvents();
        QVERIFY(!pipeline.isRunning());
    }
}

void MalloyModelTests::controllerCanReplacePipelineFromFinishedSignal() {
    RecordingTestFrames frames;
    RecordingTestAudio audio;
    MediaController controller(&frames, &audio);
    if (!controller.ffmpegAvailable()) QSKIP("Real encoder lifecycle requires ffmpeg in PATH");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const OutputSettings settings = recordingTestSettings();
    QString error;
    QSignalSpy finished(&controller, &MediaController::recordingFinished);
    QSignalSpy errors(&controller, &MediaController::errorOccurred);
    QPointer<RecorderPipeline> first;
    bool restarted = false;
    bool survivedCallback = false;
    connect(&controller, &MediaController::recordingFinished, &controller,
            [&](const QString&, qint64) {
        if (finished.size() != 1) return;
        restarted = controller.startRecording(dir.filePath(QStringLiteral("second.mp4")),
                                               settings, &error);
        survivedCallback = !first.isNull();
    });
    QVERIFY2(controller.startRecording(dir.filePath(QStringLiteral("first.mp4")),
                                        settings, &error), qPrintable(error));
    first = controller.findChild<RecorderPipeline*>();
    QVERIFY(first);
    const QString ffmpeg = first->ffmpegPath();
    QTest::qWait(1500);
    controller.stopRecording();
    QVERIFY2(restarted, qPrintable(error));
    QVERIFY(survivedCallback);
    QVERIFY(controller.isRecording());
    QTRY_VERIFY(first.isNull());
    QTest::qWait(1500);
    controller.stopRecording();
    QVERIFY(!controller.isRecording());
    QCOMPARE(finished.size(), 2);
    QCOMPARE(errors.size(), 0);
    QVERIFY(recordingDecodes(ffmpeg, dir.filePath(QStringLiteral("first.mp4"))));
    QVERIFY(recordingDecodes(ffmpeg, dir.filePath(QStringLiteral("second.mp4"))));
}

void MalloyModelTests::audioControllerHasDefaultLoopbackInput() {
    // After construction the controller should expose one input regardless
    // of whether WASAPI is healthy on the test host. The WasapiCapture
    // worker may fail to start (test machines often lack a default output
    // device); that's fine — the model state still reflects the input.
    AudioController c;
    QCOMPARE(c.inputs().size(), 1);
    QCOMPARE(c.inputs().first().id,   QStringLiteral("loopback:default"));
    QCOMPARE(c.inputs().first().name, QStringLiteral("Desktop Audio"));
    QVERIFY(c.inputs().first().loopback);
}

void MalloyModelTests::audioControllerPersistsVolumeAndMute() {
    const QString id = QStringLiteral("loopback:default");
    {
        AudioController c;
        c.setVolume(id, 0.42f);
        c.setMuted(id, true);
        QCOMPARE(c.inputs().first().volume, 0.42f);
        QVERIFY(c.inputs().first().muted);
    }
    {
        AudioController c;
        QCOMPARE(c.inputs().first().volume, 0.42f);
        QVERIFY(c.inputs().first().muted);
        c.setMuted(id, false);
        c.setVolume(id, 1.0f);
    }
}

// ---------------------------------------------------------------------------
// v5 new tests
// ---------------------------------------------------------------------------

void MalloyModelTests::windowCaptureKeyIsStable() {
    // The "window:0x<8-digit-hex>" key format must be stable so that
    // QHash lookups in CaptureController are reproducible across calls.
    QCOMPARE(CaptureController::keyForWindow(0x12345678u),
             QStringLiteral("window:0x12345678"));
    QCOMPARE(CaptureController::keyForWindow(0u),
             QStringLiteral("window:0x00000000"));
    // Also verify the reverse still works for display sessions.
    QCOMPARE(CaptureController::keyFor(1, 2), QStringLiteral("1:2"));
}

void MalloyModelTests::filterChainRoundTrips() {
    // Build an item with two filters, serialize to JSON, reload, and verify
    // the chain survives the round-trip with correct types and values.
    SceneCollection scenes;
    QUndoStack undo;
    scenes.setUndoStack(&undo);
    scenes.addScene(QStringLiteral("Scene"));
    SceneItem* item = scenes.addNewSourceToCurrent(
        QStringLiteral("Block"), Source::Type::ColorBlock);
    QVERIFY(item);

    auto* op = new OpacityFilter(item);
    op->setOpacity(0.5f);
    item->addFilter(op);

    auto* crop = new CropFilter(item);
    crop->setTop(0.1f);
    item->addFilter(crop);

    QCOMPARE(item->filters().size(), 2);
    QCOMPARE(item->filters().at(0)->type(), FilterEffect::Type::Opacity);
    QCOMPARE(item->filters().at(1)->type(), FilterEffect::Type::Crop);

    // Serialize and reload via SceneCollection JSON round-trip.
    const QJsonObject root = scenes.toJson();
    SceneCollection loaded;
    QString err;
    QVERIFY2(loaded.loadFromJson(root, &err), qPrintable(err));
    QCOMPARE(loaded.currentScene()->itemCount(), 1);

    SceneItem* reloaded = loaded.currentScene()->itemAt(0);
    QCOMPARE(reloaded->filters().size(), 2);
    QCOMPARE(reloaded->filters().at(0)->type(), FilterEffect::Type::Opacity);
    QCOMPARE(reloaded->filters().at(1)->type(), FilterEffect::Type::Crop);

    const auto* reloadedOp = static_cast<const OpacityFilter*>(reloaded->filters().at(0));
    QVERIFY(qAbs(reloadedOp->opacity() - 0.5f) < 0.001f);

    const auto* reloadedCrop = static_cast<const CropFilter*>(reloaded->filters().at(1));
    QVERIFY(qAbs(reloadedCrop->top() - 0.1f) < 0.001f);
}

void MalloyModelTests::outputSettingsRoundtrip() {
    // Verify OutputSettings round-trips through QSettings correctly.
    // initTestCase already isolated QSettings to MalloyStudioTests/ModelTests.
    {
        OutputSettings s;
        s.fps              = 60;
        s.crf              = 18;
        s.width            = 1280;
        s.height           = 720;
        s.videoCodec       = QStringLiteral("libx265");
        s.preset           = QStringLiteral("slow");
        s.audioBitratekbps = 320;
        s.container        = QStringLiteral("mkv");
        s.save();
    }
    {
        const OutputSettings r = OutputSettings::load();
        QCOMPARE(r.fps,              60);
        QCOMPARE(r.crf,              18);
        QCOMPARE(r.width,            1280);
        QCOMPARE(r.height,           720);
        QCOMPARE(r.videoCodec,       QStringLiteral("libx265"));
        QCOMPARE(r.preset,           QStringLiteral("slow"));
        QCOMPARE(r.audioBitratekbps, 320);
        QCOMPARE(r.container,        QStringLiteral("mkv"));
    }
    // Restore defaults so subsequent test runs start clean.
    OutputSettings{}.save();
}

void MalloyModelTests::perSourceAudioReconcileActivates() {
    // Verify reconcileInputs() correctly adds / removes non-loopback inputs.
    // The WasapiCapture worker for the fake device ID will fail to start
    // (the device doesn't exist), but the model state (m_inputs) is updated
    // synchronously before the worker thread even runs, so the count is stable.

    AudioController c;
    // Construction always adds loopback:default.
    QCOMPARE(c.inputs().size(), 1);
    QCOMPARE(c.inputs().first().loopback, true);

    // Reconcile with a fictitious device ID.
    const QString fakeId = QStringLiteral("{00000000-0000-0000-0000-000000000000}");
    c.reconcileInputs({fakeId});
    QCOMPARE(c.inputs().size(), 2);
    QVERIFY(!c.inputs().last().loopback);
    QCOMPARE(c.inputs().last().deviceId, fakeId);

    // Reconcile with an empty list: only loopback:default must remain.
    c.reconcileInputs({});
    QCOMPARE(c.inputs().size(), 1);
    QCOMPARE(c.inputs().first().id, QStringLiteral("loopback:default"));
}

// ---------------------------------------------------------------------------
// v6 Sub-A new tests
// ---------------------------------------------------------------------------

void MalloyModelTests::encoderRegistryAlwaysListsLibx264() {
    // The registry must always expose libx264 as a software fallback regardless
    // of whether ffmpeg is installed or hardware encoders are present.
    const auto& encoders = EncoderRegistry::available();
    QVERIFY(!encoders.isEmpty());

    bool foundLibx264 = false;
    bool foundLibx265 = false;
    for (const auto& enc : encoders) {
        if (enc.id == QStringLiteral("libx264")) foundLibx264 = true;
        if (enc.id == QStringLiteral("libx265")) foundLibx265 = true;
        // Software encoders must be marked !isHardware
        if (enc.id == QStringLiteral("libx264") || enc.id == QStringLiteral("libx265"))
            QVERIFY(!enc.isHardware);
        // Hardware encoders must have a non-null buildArgs
        if (enc.isHardware)
            QVERIFY(enc.buildArgs != nullptr);
    }
    QVERIFY(foundLibx264);
    QVERIFY(foundLibx265);

    // buildArgs for libx264 must emit -c:v libx264 and -crf
    const EncoderRegistry::Encoder* x264 = EncoderRegistry::find(QStringLiteral("libx264"));
    QVERIFY(x264 != nullptr);
    const QStringList args = x264->buildArgs(OutputSettings{},
                                             EncoderRegistry::Destination::File);
    QVERIFY(args.contains(QStringLiteral("libx264")));
    QVERIFY(args.contains(QStringLiteral("-crf")));

    // Software encoders hold quality either way, so the destination changes
    // nothing for them. Stated as a test because it is a decision and not an
    // oversight: CRF is already indifferent to the declared input rate.
    const QStringList streamArgs = x264->buildArgs(OutputSettings{},
                                                   EncoderRegistry::Destination::Stream);
    QCOMPARE(streamArgs, args);
}

void MalloyModelTests::streamSettingsRtmpUrlTemplatesExpandCorrectly() {
    // Twitch template
    {
        StreamSettings ss;
        ss.service   = StreamSettings::Service::Twitch;
        ss.streamKey = QStringLiteral("live_test_abc123");
        const QString url = ss.rtmpUrl();
        QVERIFY(url.startsWith(QStringLiteral("rtmp://live.twitch.tv/")));
        QVERIFY(url.contains(ss.streamKey));
        QVERIFY(!url.contains(QStringLiteral("{key}")));
    }
    // YouTube template
    {
        StreamSettings ss;
        ss.service   = StreamSettings::Service::YouTube;
        ss.streamKey = QStringLiteral("xxxx-yyyy-zzzz-wwww");
        const QString url = ss.rtmpUrl();
        QVERIFY(url.startsWith(QStringLiteral("rtmp://a.rtmp.youtube.com/")));
        QVERIFY(url.contains(ss.streamKey));
        QVERIFY(!url.contains(QStringLiteral("{key}")));
    }
    // Custom template
    {
        StreamSettings ss;
        ss.service   = StreamSettings::Service::Custom;
        ss.customUrl = QStringLiteral("rtmp://my-server.example.com/live/{key}");
        ss.streamKey = QStringLiteral("mykey");
        const QString url = ss.rtmpUrl();
        QCOMPARE(url, QStringLiteral("rtmp://my-server.example.com/live/mykey"));
    }
    // Empty key should leave "{key}" unexpanded — MediaController catches this
    {
        StreamSettings ss;
        ss.service   = StreamSettings::Service::Twitch;
        ss.streamKey = QString();
        const QString url = ss.rtmpUrl();
        QVERIFY(url.contains(QStringLiteral("{key}")));
    }
}

// ---------------------------------------------------------------------------
// v6 Sub-B / C / D new tests
// ---------------------------------------------------------------------------

void MalloyModelTests::encoderPipelineFactoryDispatchesByTarget() {
    // StreamingPipeline must be a distinct EncoderPipeline subclass that
    // accepts RTMP targets. Both pipeline types must fail gracefully with null
    // frame/audio sources (not crash).
    StreamingPipeline streamer;
    QVERIFY(!streamer.isRunning());
    QVERIFY(qobject_cast<EncoderPipeline*>(&streamer) != nullptr);

    EncoderPipeline::Target rtmp;
    rtmp.kind        = EncoderPipeline::Target::Kind::Rtmp;
    rtmp.destination = QStringLiteral("rtmp://live.twitch.tv/app/test_key");
    rtmp.output      = OutputSettings{};

    QString err;
    QVERIFY(!streamer.start(rtmp, nullptr, nullptr, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(!streamer.isRunning());

    // RecorderPipeline refuses File targets with null sources too.
    RecorderPipeline recorder;
    EncoderPipeline::Target file;
    file.kind        = EncoderPipeline::Target::Kind::File;
    file.destination = QStringLiteral("test.mp4");
    file.output      = OutputSettings{};
    err.clear();
    QVERIFY(!recorder.start(file, nullptr, nullptr, &err));
    QVERIFY(!err.isEmpty());
}

void MalloyModelTests::sceneCollectionStudioModeProgramAndPreviewIndependent() {
    SceneCollection scenes;
    QUndoStack undo;
    scenes.setUndoStack(&undo);
    scenes.addScene(QStringLiteral("Alpha"));
    scenes.addScene(QStringLiteral("Beta"));
    QCOMPARE(scenes.sceneCount(), 2);
    scenes.setCurrentIndex(0);

    // Studio mode off: programIndex follows the current selection.
    QVERIFY(!scenes.studioMode());
    QCOMPARE(scenes.programIndex(), 0);

    QSignalSpy programSpy(&scenes, &SceneCollection::programChanged);
    QSignalSpy previewSpy(&scenes, &SceneCollection::previewChanged);

    scenes.setStudioMode(true);
    QVERIFY(scenes.studioMode());

    // Selecting a different scene in studio mode updates preview only.
    scenes.setCurrentIndex(1);
    QCOMPARE(scenes.previewIndex(), 1);
    QCOMPARE(scenes.programIndex(), 0);   // program must remain unchanged

    // promotePreviewToProgram() swings program to 1 and emits programChanged.
    scenes.promotePreviewToProgram();
    QCOMPARE(scenes.programIndex(), 1);
    QVERIFY(!programSpy.isEmpty());

    // Turning studio mode off syncs program/preview to current.
    scenes.setStudioMode(false);
    QVERIFY(!scenes.studioMode());
}

void MalloyModelTests::hotkeyManagerRoundTripsBindingsThroughQSettings() {
    // Contract as of issue #13: a binding is persisted only when Windows
    // actually registered it. This test previously bound F12 and asserted the
    // key was stored "regardless" of whether RegisterHotKey succeeded, which is
    // precisely the behaviour that let an unusable shortcut look bound and be
    // retried silently on every launch. F12 is in fact refused on some machines.
    //
    // So: use an obscure combination, and assert the round trip only when the
    // registration really happened.
    const QKeySequence seq(Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::Key_F9);
    const QString action = QStringLiteral("test.roundtrip");

    bool registered = false;
    {
        HotkeyManager mgr;
        registered = mgr.setBinding(action, seq);
        if (!registered) {
            // Refused: nothing may be shown as bound and nothing may be stored.
            QVERIFY(mgr.binding(action).isEmpty());
            const QSettings s;
            QVERIFY(s.value(QStringLiteral("hotkeys/") + action).toString().isEmpty());
            QSKIP("This machine refuses the test shortcut, so the round trip cannot run.");
        }
        QCOMPARE(mgr.binding(action), seq);
    }

    // Verify QSettings was written.
    {
        const QSettings s;
        const QString stored = s.value(QStringLiteral("hotkeys/") + action).toString();
        QCOMPARE(QKeySequence(stored), seq);
    }

    // A fresh manager loading persisted bindings reproduces the same key.
    {
        HotkeyManager mgr;
        mgr.loadBindings();
        QCOMPARE(mgr.binding(action), seq);
    }

    // Unbinding always succeeds and clears the stored value.
    {
        HotkeyManager mgr;
        mgr.loadBindings();
        QVERIFY(mgr.setBinding(action, QKeySequence()));
        QVERIFY(mgr.binding(action).isEmpty());
        const QSettings s;
        QVERIFY(s.value(QStringLiteral("hotkeys/") + action).toString().isEmpty());
    }
}

void MalloyModelTests::replayBufferTrimsToLastNSeconds() {
    // Verify that the ring-trimming logic (used by both AudioController and
    // PreviewWidget) correctly evicts old entries so only the last N seconds
    // remain in the queue.
    const QByteArray silence(3840, '\0');  // 20 ms of silence
    const qint64 intervalUs = 20'000;     // 20 ms in microseconds

    // Build 60 chunks spanning 0 .. 1.18 s (indices 0 .. 59).
    QQueue<TimedPcm> ring;
    for (int i = 0; i < 60; ++i)
        ring.enqueue({silence, static_cast<qint64>(i) * intervalUs});

    // Trim to last 0.6 s = 600 000 µs (30 intervals, 31 endpoints).
    const qint64 maxSpan = 600'000;
    while (ring.size() > 1) {
        if (ring.back().ptsUs - ring.front().ptsUs <= maxSpan) break;
        ring.dequeue();
    }

    // 31 entries survive: indices 29 .. 59 (30 gaps × 20 ms = 600 ms).
    QCOMPARE(ring.size(), 31);
    QVERIFY(ring.back().ptsUs - ring.front().ptsUs <= maxSpan);
    QCOMPARE(ring.front().ptsUs, static_cast<qint64>(29) * intervalUs);
    QCOMPARE(ring.back().ptsUs,  static_cast<qint64>(59) * intervalUs);

    // Drive RingTimedPcmSource through a small ring (3 chunks).
    QQueue<TimedPcm> tiny;
    for (int i = 0; i < 3; ++i)
        tiny.enqueue({silence, static_cast<qint64>(i) * intervalUs});

    RingTimedPcmSource src(std::move(tiny));
    int emitted = 0;
    QObject::connect(&src, &TimedPcmSource::pcmReady,
                     [&emitted](const QByteArray&) { ++emitted; });
    QEventLoop loop;
    QObject::connect(&src, &RingTimedPcmSource::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);  // safety timeout
    src.start();
    loop.exec();
    QCOMPARE(emitted, 3);
}

void MalloyModelTests::audioMixerSumsTwoSignalsWithoutClipping() {
    // Verify that the int32 accumulate → int16 clamp strategy prevents overflow
    // even when two full-scale signals are summed (the core mixer contract).
    {
        // Two full-scale +32767 signals summed in int32 = 65534; clamped → 32767.
        const int32_t a = 32767, b = 32767;
        const int32_t sum = a + b;   // 65534, would wrap in int16
        const qint16 out = static_cast<qint16>(
            std::clamp(sum, INT32_C(-32768), INT32_C(32767)));
        QCOMPARE(static_cast<int>(out), 32767);

        // Same for negative peak.
        const qint16 negOut = static_cast<qint16>(
            std::clamp(-a - b, INT32_C(-32768), INT32_C(32767)));
        QCOMPARE(static_cast<int>(negOut), -32768);
    }

    // AudioController exposes pan and limiter controls (v6 mixer API).
    AudioController c;
    QCOMPARE(c.inputs().size(), 1);
    const QString id = c.inputs().first().id;

    c.setPan(id, -1.0f);
    QCOMPARE(c.inputs().first().pan, -1.0f);
    c.setPan(id, 2.0f);   // clamped to +1.0
    QCOMPARE(c.inputs().first().pan, 1.0f);
    c.setPan(id, 0.0f);

    QVERIFY(!c.limiterEnabled());
    c.setLimiterEnabled(true);
    QVERIFY(c.limiterEnabled());
    c.setLimiterThresholdDb(-6.0f);
    QCOMPARE(c.limiterThresholdDb(), -6.0f);
    c.setLimiterEnabled(false);
}

void MalloyModelTests::chromaKeyFilterRemovesTargetColor() {
    // A pixel exactly matching the key colour → fully transparent (alpha == 0).
    // A pixel far from the key (grey) → fully opaque (alpha == 255).
    ChromaKeyFilter ck;
    ck.setKey(QColor(0, 255, 0));   // pure green
    ck.setTolerance(0.20f);
    ck.setSmoothness(0.05f);

    // 2×1 test image: [exact green | neutral grey]
    QImage img(2, 1, QImage::Format_ARGB32);
    img.setPixel(0, 0, qRgba(0,   255, 0,   255));  // exact key → keyed out
    img.setPixel(1, 0, qRgba(128, 128, 128, 255));  // grey → opaque

    ck.apply(img);

    QCOMPARE(qAlpha(img.pixel(0, 0)), 0);    // fully transparent
    QCOMPARE(qAlpha(img.pixel(1, 0)), 255);  // fully opaque

    // JSON round-trip preserves key colour, tolerance, and smoothness.
    const QJsonObject json = ck.toJson();
    FilterEffect* reloaded = FilterEffect::fromJson(json);
    QVERIFY(reloaded != nullptr);
    QCOMPARE(reloaded->type(), FilterEffect::Type::ChromaKey);
    auto* rck = static_cast<ChromaKeyFilter*>(reloaded);
    QCOMPARE(rck->key(), QColor(0, 255, 0));
    QVERIFY(qAbs(rck->tolerance()  - 0.20f) < 0.001f);
    QVERIFY(qAbs(rck->smoothness() - 0.05f) < 0.001f);
    delete reloaded;
}

void MalloyModelTests::audioMixerEmitsSilenceWhenAllRingsAreEmpty() {
    // Regression for "ffmpeg exited with code -22": before the fix, if no
    // audio input had data flowing the mixer skipped emitting pcmReady
    // entirely. ffmpeg's named-pipe audio input would never see any bytes
    // and would exit with EINVAL, killing the recording.
    //
    // Now the mixer must emit a chunk every tick (~20 ms) regardless: empty
    // rings just contribute silence to the int32 accumulator, and the clamp
    // loop produces 3840 zero bytes (= 960 stereo frames * 2 channels * 2 B).
    AudioController c;
    // Mute every input so the silence assertion below tests the mixer rather
    // than the machine. This used to rely on WasapiCapture delivering nothing
    // in the test process, which is true only on a host with no working audio
    // endpoint; on a host that is actually playing something the loopback ring
    // fills and the assertion failed for a reason that had nothing to do with
    // the bug being guarded. Muted inputs exercise the same accumulator path
    // as empty ones, and the premise is now the test's to control.
    for (const AudioInput& in : c.inputs()) c.setMuted(in.id, true);

    int chunkCount = 0;
    qint64 totalNonZeroBytes = 0;
    int firstChunkSize = -1;
    QObject::connect(&c, &TimedPcmSource::pcmReady, &c,
        [&](const QByteArray& pcm) {
            ++chunkCount;
            if (firstChunkSize < 0) firstChunkSize = pcm.size();
            for (char b : pcm) if (b != 0) ++totalNonZeroBytes;
        });

    // Run the event loop long enough that the 50 Hz mixer fires several times.
    QEventLoop loop;
    QTimer::singleShot(220, &loop, &QEventLoop::quit);
    loop.exec();

    // At 50 Hz over 220 ms we expect ~11 emissions; allow generous slop for
    // timer scheduling on busy CI hosts.
    QVERIFY2(chunkCount >= 5,
             qPrintable(QStringLiteral("expected at least 5 mix chunks in 220 ms, got %1")
                           .arg(chunkCount)));

    // Each chunk should be exactly the configured tick size: 48000/50 frames
    // * 2 channels * 2 bytes = 3840 bytes.
    QCOMPARE(firstChunkSize, 3840);

    // With no input data flowing every byte must be zero (silence).
    QCOMPARE(totalNonZeroBytes, qint64(0));
}

// Test subclass that exposes the protected virtual buildOutputArgs() so we
// can inspect what gets fed to ffmpeg for any given OutputSettings shape.
// RecorderPipeline is `final`, so we derive directly from EncoderPipeline —
// buildOutputArgs() is virtual on the base class and produces the same
// args RecorderPipeline would emit.
class ProbeEncoderPipeline final : public EncoderPipeline {
public:
    QStringList probe(const Target& t) const { return buildOutputArgs(t); }
};

void MalloyModelTests::encoderPipelineRespectsRegistryPerCodecArgs() {
    // Reproduces the user's "-22" symptom: hevc_nvenc rejects libx264 preset
    // names like "faster". The fix routes the videoCodec through
    // EncoderRegistry::find()->buildArgs(), which translates per-encoder.

    ProbeEncoderPipeline p;
    EncoderPipeline::Target t;
    t.kind        = EncoderPipeline::Target::Kind::File;
    t.destination = QStringLiteral("test.mp4");

    // (1) libx264 always exists in the registry. A user-customised preset
    //     should pass through to ffmpeg's -preset flag.
    t.output            = OutputSettings{};
    t.output.videoCodec = QStringLiteral("libx264");
    t.output.preset     = QStringLiteral("faster");
    t.output.crf        = 18;
    {
        const QStringList args = p.probe(t);
        const int idxCodec = args.indexOf(QStringLiteral("-c:v"));
        QVERIFY(idxCodec >= 0);
        QCOMPARE(args.at(idxCodec + 1), QStringLiteral("libx264"));
        const int idxPreset = args.indexOf(QStringLiteral("-preset"));
        QVERIFY(idxPreset >= 0);
        QCOMPARE(args.at(idxPreset + 1), QStringLiteral("faster"));
        QVERIFY(args.contains(QStringLiteral("-crf")));
        QVERIFY(!args.contains(QStringLiteral("-rc")));   // libx264 doesn't use rc-cbr
    }

    // (2) Critical: the same libx264 preset name MUST NOT bleed through when
    //     hevc_nvenc is selected. NVENC uses its own `-preset p4` vocabulary.
    //     The registry's NVENC lambda hard-codes "p4", so even when the saved
    //     OutputSettings.preset is "faster" the emitted args never contain
    //     "-preset faster". (Test only runs when NVENC is available — most
    //     CI machines won't have it, in which case we skip silently.)
    //
    //     This target is a file, so the rate control is a quality target and
    //     not a bitrate. That distinction has its own test; what is checked
    //     here is that a file never carries a bitrate it did not ask for.
    if (EncoderRegistry::find(QStringLiteral("hevc_nvenc"))) {
        t.output.videoCodec = QStringLiteral("hevc_nvenc");
        t.output.preset     = QStringLiteral("faster");   // libx264 vocab
        t.output.bitrateKbps = 8000;
        const QStringList args = p.probe(t);
        const int idxCodec = args.indexOf(QStringLiteral("-c:v"));
        QVERIFY(idxCodec >= 0);
        QCOMPARE(args.at(idxCodec + 1), QStringLiteral("hevc_nvenc"));
        QVERIFY2(!args.contains(QStringLiteral("faster")),
                 qPrintable(QStringLiteral("libx264 preset leaked into NVENC args: %1")
                               .arg(args.join(QLatin1Char(' ')))));
        const int idxPreset = args.indexOf(QStringLiteral("-preset"));
        QVERIFY(idxPreset >= 0);
        QCOMPARE(args.at(idxPreset + 1), QStringLiteral("p4"));
        QVERIFY(args.contains(QStringLiteral("-rc")));
        QVERIFY(args.contains(QStringLiteral("constqp")));
        QVERIFY(args.contains(QStringLiteral("-qp")));
        QVERIFY(!args.contains(QStringLiteral("-b:v")));
        QVERIFY(!args.contains(QStringLiteral("cbr")));
    }

    // (3) An unknown codec id (corrupted QSettings, future build) must still
    //     produce a valid arg list — the fallback path. It includes -c:v with
    //     whatever id the user had, so ffmpeg returns a clean codec-not-found
    //     error instead of MalloyStudio crashing.
    t.output            = OutputSettings{};
    t.output.videoCodec = QStringLiteral("not_a_real_codec");
    const QStringList args = p.probe(t);
    const int idxCodec = args.indexOf(QStringLiteral("-c:v"));
    QVERIFY(idxCodec >= 0);
    QCOMPARE(args.at(idxCodec + 1), QStringLiteral("not_a_real_codec"));
}

// StreamingPipeline counterpart of ProbeEncoderPipeline — gives tests direct
// access to the protected buildOutputArgs() override that emits the RTMP
// argument vector. StreamingPipeline was originally marked `final`; v7
// removed that so this helper could inherit it (only RecorderPipeline is
// still final).
class ProbeStreamingPipeline final : public StreamingPipeline {
public:
    QStringList probe(const Target& t) const { return buildOutputArgs(t); }
};

void MalloyModelTests::streamingPipelineUsesRegistryNotHardcodedX264() {
    // Reproduces the streaming-side -22 bug: StreamingPipeline used to emit
    // `-c:v <codec> -preset <s.preset> -tune zerolatency -b:v ...` regardless
    // of which encoder the user picked. For hevc_nvenc that pushed a libx264
    // preset name into ffmpeg and tripped EINVAL on Start Stream.

    ProbeStreamingPipeline p;
    EncoderPipeline::Target t;
    t.kind        = EncoderPipeline::Target::Kind::Rtmp;
    t.destination = QStringLiteral("rtmp://example.com/live/key");

    // (1) libx264: registry returns CRF-style args; streaming layer must add
    //     `-tune zerolatency` (libx264's streaming tune). The libx264 preset
    //     name should pass through unchanged.
    t.output            = OutputSettings{};
    t.output.videoCodec = QStringLiteral("libx264");
    t.output.preset     = QStringLiteral("veryfast");
    t.output.fps        = 30;
    {
        const QStringList args = p.probe(t);
        const int idxCodec = args.indexOf(QStringLiteral("-c:v"));
        QVERIFY(idxCodec >= 0);
        QCOMPARE(args.at(idxCodec + 1), QStringLiteral("libx264"));
        const int idxTune = args.indexOf(QStringLiteral("-tune"));
        QVERIFY(idxTune >= 0);
        QCOMPARE(args.at(idxTune + 1), QStringLiteral("zerolatency"));
        // GOP must be forced (override): with fps=30, default keyframeSec=2 → -g 60.
        const int idxG = args.indexOf(QStringLiteral("-g"));
        QVERIFY(idxG >= 0);
        QCOMPARE(args.at(idxG + 1), QStringLiteral("60"));
    }

    // (2) hevc_nvenc (only if available on this machine): registry returns
    //     CBR-style args with -preset p4; streaming layer adds `-tune ull`
    //     (NVENC's ultra-low-latency tune). The libx264 preset name "faster"
    //     must NOT leak through, and "zerolatency" must NOT appear (that's
    //     a libx264-only token).
    if (EncoderRegistry::find(QStringLiteral("hevc_nvenc"))) {
        t.output.videoCodec  = QStringLiteral("hevc_nvenc");
        t.output.preset      = QStringLiteral("faster");   // libx264 vocab — must be ignored
        t.output.bitrateKbps = 8000;
        const QStringList args = p.probe(t);
        const int idxCodec = args.indexOf(QStringLiteral("-c:v"));
        QVERIFY(idxCodec >= 0);
        QCOMPARE(args.at(idxCodec + 1), QStringLiteral("hevc_nvenc"));
        QVERIFY2(!args.contains(QStringLiteral("faster")),
                 qPrintable(QStringLiteral("libx264 preset leaked into NVENC stream args: %1")
                               .arg(args.join(QLatin1Char(' ')))));
        QVERIFY(!args.contains(QStringLiteral("zerolatency")));   // libx264-only tune
        const int idxTune = args.indexOf(QStringLiteral("-tune"));
        QVERIFY(idxTune >= 0);
        QCOMPARE(args.at(idxTune + 1), QStringLiteral("ull"));
        QVERIFY(args.contains(QStringLiteral("-rc")));
        QVERIFY(args.contains(QStringLiteral("cbr")));
    }

    // (3) h264_qsv (only if available): QSV does NOT accept `-tune`. The
    //     streaming pipeline must omit it entirely.
    if (EncoderRegistry::find(QStringLiteral("h264_qsv"))) {
        t.output.videoCodec  = QStringLiteral("h264_qsv");
        t.output.bitrateKbps = 6000;
        const QStringList args = p.probe(t);
        QVERIFY(!args.contains(QStringLiteral("-tune")));
    }
}

void MalloyModelTests::streamingPipelineHonorsKeyframeSec() {
    // Bug 9 regression: StreamSettings.keyframeSec was being silently dropped
    // by MediaController, so the streaming pipeline always emitted a hardcoded
    // 2-second GOP. The plumbing now carries the field through OutputSettings.

    ProbeStreamingPipeline p;
    EncoderPipeline::Target t;
    t.kind        = EncoderPipeline::Target::Kind::Rtmp;
    t.destination = QStringLiteral("rtmp://example.com/live/key");

    t.output            = OutputSettings{};
    t.output.videoCodec = QStringLiteral("libx264");
    t.output.fps        = 30;

    // (1) Explicit 4-second keyframe → -g 120 (30 fps × 4 s).
    t.output.keyframeSec = 4;
    {
        const QStringList args = p.probe(t);
        const int idxG = args.indexOf(QStringLiteral("-g"));
        QVERIFY(idxG >= 0);
        QCOMPARE(args.at(idxG + 1), QStringLiteral("120"));
    }

    // (2) keyframeSec == 0 means "auto", which the pipeline maps to the
    //     2-second default → -g 60.
    t.output.keyframeSec = 0;
    {
        const QStringList args = p.probe(t);
        const int idxG = args.indexOf(QStringLiteral("-g"));
        QVERIFY(idxG >= 0);
        QCOMPARE(args.at(idxG + 1), QStringLiteral("60"));
    }

    // (3) 60 fps with keyframeSec=1 → -g 60 (one keyframe per second).
    t.output.fps         = 60;
    t.output.keyframeSec = 1;
    {
        const QStringList args = p.probe(t);
        const int idxG = args.indexOf(QStringLiteral("-g"));
        QVERIFY(idxG >= 0);
        QCOMPARE(args.at(idxG + 1), QStringLiteral("60"));
    }
}

void MalloyModelTests::streamingPipelineForcesGopWhenSoftwareEncoderOmitsIt() {
    // The libx264 EncoderRegistry builder emits -preset/-crf only; -g is
    // appended exclusively by StreamingPipeline. This test guards against a
    // regression where someone might move the -g into the registry builder
    // and then break streaming GOP for hardware encoders (or vice versa).

    ProbeStreamingPipeline p;
    EncoderPipeline::Target t;
    t.kind        = EncoderPipeline::Target::Kind::Rtmp;
    t.destination = QStringLiteral("rtmp://example.com/live/key");
    t.output            = OutputSettings{};
    t.output.videoCodec = QStringLiteral("libx264");
    t.output.fps        = 30;
    t.output.keyframeSec = 2;

    const QStringList args = p.probe(t);
    int gopCount = 0;
    for (const QString& a : args) if (a == QStringLiteral("-g")) ++gopCount;
    QCOMPARE(gopCount, 1);
}

// ---------------------------------------------------------------------------
// v7 Tier 2 — Microphone source via SourcesPanel / AudioMixerPanel quick-add
// ---------------------------------------------------------------------------

void MalloyModelTests::addAudioInputFromUiCreatesScopedSource() {
    // Simulates what the new SourcesPanel → Microphone flow does at the model
    // level: pick a device, call addAudioInputToCurrent(name, deviceId). The
    // resulting source must carry the correct type + device id and be
    // immediately visible in the current scene so reconcileInputs picks it up.

    SceneCollection scenes;
    QUndoStack undo;
    scenes.setUndoStack(&undo);
    scenes.addScene(QStringLiteral("Scene"));

    const QString fakeId = QStringLiteral("{aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee}");
    SceneItem* item = scenes.addAudioInputToCurrent(QStringLiteral("USB Mic"), fakeId);
    QVERIFY(item != nullptr);

    QCOMPARE(scenes.sourceCount(), 1);
    Source* src = scenes.sourceById(item->sourceId());
    QVERIFY(src != nullptr);
    QCOMPARE(src->type(), Source::Type::AudioInput);
    QCOMPARE(src->name(),           QStringLiteral("USB Mic"));
    QCOMPARE(src->audioDeviceId(),  fakeId);

    QVERIFY(item->isVisible());
    QCOMPARE(scenes.currentScene()->itemCount(), 1);

    // Undo must remove the source cleanly (no dangling item references).
    undo.undo();
    QCOMPARE(scenes.sourceCount(), 0);
    QCOMPARE(scenes.currentScene()->itemCount(), 0);
    undo.redo();
    QCOMPARE(scenes.sourceCount(), 1);
}

void MalloyModelTests::addCameraCreatesScopedSourceAndRoundTrips() {
    // Mirrors the SourcesPanel → Camera flow: pick a device, call
    // addCameraToCurrent(name, deviceId, deviceName). The source must carry the
    // Camera type + device id/name, survive a JSON round-trip, and undo cleanly.
    SceneCollection scenes;
    QUndoStack undo;
    scenes.setUndoStack(&undo);
    scenes.addScene(QStringLiteral("Scene"));

    const QString devId   = QStringLiteral("\\\\?\\usb#vid_046d&pid_0825#mf");
    const QString devName = QStringLiteral("HD Pro Webcam C920");
    SceneItem* item = scenes.addCameraToCurrent(QStringLiteral("Face cam"), devId, devName);
    QVERIFY(item != nullptr);

    Source* src = scenes.sourceById(item->sourceId());
    QVERIFY(src != nullptr);
    QCOMPARE(src->type(),            Source::Type::Camera);
    QCOMPARE(src->name(),            QStringLiteral("Face cam"));
    QCOMPARE(src->cameraDeviceId(),  devId);
    QCOMPARE(src->cameraName(),      devName);
    QVERIFY(src->hasCameraConfig());

    // JSON round-trip preserves the camera binding.
    const QJsonObject root = scenes.toJson();
    SceneCollection loaded;
    QString err;
    QVERIFY2(loaded.loadFromJson(root, &err), qPrintable(err));
    Source* reloaded = nullptr;
    for (Source* s : loaded.sources())
        if (s->type() == Source::Type::Camera) { reloaded = s; break; }
    QVERIFY(reloaded != nullptr);
    QCOMPARE(reloaded->cameraDeviceId(), devId);
    QCOMPARE(reloaded->cameraName(),     devName);

    // Undo removes the source cleanly.
    undo.undo();
    QCOMPARE(scenes.sourceCount(), 0);
    undo.redo();
    QCOMPARE(scenes.sourceCount(), 1);
}

void MalloyModelTests::addingAudioInputTriggersAudioInputsChanged() {
    // MainWindow listens to audioInputsChanged and forwards to
    // AudioController::reconcileInputs(gatherVisibleAudioIds()). If the UI
    // path forgets to emit the signal, microphones added via the new flow
    // would never start capturing.

    SceneCollection scenes;
    scenes.addScene(QStringLiteral("Scene"));

    QSignalSpy spy(&scenes, &SceneCollection::audioInputsChanged);
    QVERIFY(spy.isValid());

    scenes.addAudioInputToCurrent(QStringLiteral("Mic"),
                                  QStringLiteral("{fake-guid}"));
    QVERIFY2(spy.count() >= 1,
             qPrintable(QStringLiteral("audioInputsChanged was not emitted")));
}

// ---------------------------------------------------------------------------
// v8 — Editor per-clip inspector properties (transform/audio/speed)
// ---------------------------------------------------------------------------

void MalloyModelTests::editorClipRoundTripPreservesV4Fields() {
    // Building a clip JSON with all v4 fields and round-tripping it through
    // EditorWorkspace's timelineJson() must preserve every numeric value
    // bit-for-bit. (clipToJson always writes the new nested objects.)
    EditorWorkspace editor(nullptr);
    QJsonArray in;
    QJsonObject c;
    c.insert(QStringLiteral("track"), 1);
    c.insert(QStringLiteral("start"), 12.5);
    c.insert(QStringLiteral("dur"),   33.25);
    c.insert(QStringLiteral("label"), QStringLiteral("Take 2"));
    c.insert(QStringLiteral("tag"),   QStringLiteral("VID"));
    c.insert(QStringLiteral("color"), QStringLiteral("#5a96cd"));
    c.insert(QStringLiteral("audio"), false);
    QJsonObject xf;
    xf.insert(QStringLiteral("x"), 64);
    xf.insert(QStringLiteral("y"), -32);
    xf.insert(QStringLiteral("scale"), 87.5);
    xf.insert(QStringLiteral("rotation"), -12.5);
    xf.insert(QStringLiteral("opacity"), 78);
    c.insert(QStringLiteral("transform"), xf);
    QJsonObject ap;
    ap.insert(QStringLiteral("gainDb"), -6);
    ap.insert(QStringLiteral("pan"), 25);
    ap.insert(QStringLiteral("channels"), 1);
    c.insert(QStringLiteral("audioParams"), ap);
    QJsonObject sp;
    sp.insert(QStringLiteral("factor"), 1.75);
    c.insert(QStringLiteral("speed"), sp);
    in.append(c);

    editor.setTimelineJson(in);
    const QJsonArray out = editor.timelineJson();
    QCOMPARE(out.size(), 1);
    const QJsonObject got = out.at(0).toObject();
    QCOMPARE(got.value(QStringLiteral("track")).toInt(),    1);
    QCOMPARE(got.value(QStringLiteral("start")).toDouble(), 12.5);
    QCOMPARE(got.value(QStringLiteral("dur")).toDouble(),   33.25);
    QCOMPARE(got.value(QStringLiteral("label")).toString(), QStringLiteral("Take 2"));
    const QJsonObject gotXf = got.value(QStringLiteral("transform")).toObject();
    QCOMPARE(gotXf.value(QStringLiteral("x")).toInt(),          64);
    QCOMPARE(gotXf.value(QStringLiteral("y")).toInt(),         -32);
    QCOMPARE(gotXf.value(QStringLiteral("scale")).toDouble(),    87.5);
    QCOMPARE(gotXf.value(QStringLiteral("rotation")).toDouble(),-12.5);
    QCOMPARE(gotXf.value(QStringLiteral("opacity")).toInt(),    78);
    const QJsonObject gotAp = got.value(QStringLiteral("audioParams")).toObject();
    QCOMPARE(gotAp.value(QStringLiteral("gainDb")).toInt(),     -6);
    QCOMPARE(gotAp.value(QStringLiteral("pan")).toInt(),        25);
    QCOMPARE(gotAp.value(QStringLiteral("channels")).toInt(),    1);
    const QJsonObject gotSp = got.value(QStringLiteral("speed")).toObject();
    QCOMPARE(gotSp.value(QStringLiteral("factor")).toDouble(), 1.75);
}

void MalloyModelTests::editorClipFromLegacyV3JsonAppliesDefaults() {
    // A v3 .malloy.json (written before the Editor deep pass) has flat clip
    // entries with only track/start/dur/label/tag/color/audio — no transform /
    // audioParams / speed sub-objects. Loading it must NOT lose data, and a
    // subsequent save must materialize the new sub-objects with sane defaults
    // (x=0, scale=100%, opacity=100%, gainDb=0, factor=1.0). This is the
    // back-compat guarantee the new clipFromJson defaults exist to provide.
    EditorWorkspace editor(nullptr);
    QJsonArray in;
    QJsonObject c;
    c.insert(QStringLiteral("track"), 0);
    c.insert(QStringLiteral("start"), 0.0);
    c.insert(QStringLiteral("dur"),   8.0);
    c.insert(QStringLiteral("label"), QStringLiteral("Intro card"));
    c.insert(QStringLiteral("tag"),   QStringLiteral("IMG"));
    c.insert(QStringLiteral("color"), QStringLiteral("#966ed2"));
    c.insert(QStringLiteral("audio"), false);
    // Deliberately NO "transform", "audioParams", "speed".
    in.append(c);

    editor.setTimelineJson(in);
    const QJsonArray out = editor.timelineJson();
    QCOMPARE(out.size(), 1);
    const QJsonObject got = out.at(0).toObject();
    // Legacy fields preserved as-is.
    QCOMPARE(got.value(QStringLiteral("label")).toString(), QStringLiteral("Intro card"));
    QCOMPARE(got.value(QStringLiteral("dur")).toDouble(),   8.0);
    // New sub-objects materialized with their documented defaults.
    const QJsonObject gotXf = got.value(QStringLiteral("transform")).toObject();
    QVERIFY(!gotXf.isEmpty());
    QCOMPARE(gotXf.value(QStringLiteral("x")).toInt(),       0);
    QCOMPARE(gotXf.value(QStringLiteral("y")).toInt(),       0);
    QCOMPARE(gotXf.value(QStringLiteral("scale")).toDouble(),  100.0);
    QCOMPARE(gotXf.value(QStringLiteral("rotation")).toDouble(), 0.0);
    QCOMPARE(gotXf.value(QStringLiteral("opacity")).toInt(), 100);
    const QJsonObject gotAp = got.value(QStringLiteral("audioParams")).toObject();
    QCOMPARE(gotAp.value(QStringLiteral("gainDb")).toInt(),  0);
    QCOMPARE(gotAp.value(QStringLiteral("pan")).toInt(),     0);
    QCOMPARE(gotAp.value(QStringLiteral("channels")).toInt(),0);
    const QJsonObject gotSp = got.value(QStringLiteral("speed")).toObject();
    QCOMPARE(gotSp.value(QStringLiteral("factor")).toDouble(), 1.0);
}

void MalloyModelTests::audioControllerEmitsInputControlChangedOnValueChange() {
    // The Streaming Mix + Recording AudioMixerPanel both observe one shared
    // AudioController. When a user moves a slider in one panel, the other's
    // slider knob must re-seed to match — driven by AudioController emitting
    // inputControlChanged(id) from setVolume/setMuted/setPan. The signal MUST
    // skip the no-op path so it isn't spammy.
    AudioController ac;
    const QString id = ac.inputs().first().id;   // loopback:default is auto-added

    // Drive each control to a known anchor first, then attach the spy. Other
    // tests in this suite persist volume/mute/pan via QSettings, so the
    // controller's starting values aren't fixed; the anchor + spy.clear()
    // pattern keeps this case robust to whatever state was inherited.
    ac.setVolume(id, 0.40f);
    ac.setMuted (id, false);
    ac.setPan   (id, 0.00f);

    QSignalSpy spy(&ac, &AudioController::inputControlChanged);
    QVERIFY(spy.isValid());

    // setVolume: real change → emits exactly once with the right id.
    ac.setVolume(id, 0.85f);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).toString(), id);

    // setVolume to same value → silent no-op-skip in the setter.
    ac.setVolume(id, 0.85f);
    QCOMPARE(spy.count(), 0);

    // setMuted real change → emits; same value → silent.
    ac.setMuted(id, true);
    QCOMPARE(spy.count(), 1);
    spy.clear();
    ac.setMuted(id, true);
    QCOMPARE(spy.count(), 0);

    // setPan real change → emits; same value → silent.
    ac.setPan(id, 0.5f);
    QCOMPARE(spy.count(), 1);
    spy.clear();
    ac.setPan(id, 0.5f);
    QCOMPARE(spy.count(), 0);

    // Unknown id → no-op + no emit (defensive).
    ac.setVolume(QStringLiteral("does-not-exist"), 0.5f);
    QCOMPARE(spy.count(), 0);

    // Reset to defaults so we don't poison later tests that share QSettings —
    // audioMuteActionIdTogglesInput, for instance, asserts muted == false on
    // a fresh controller, and AudioInput's persisted values are global.
    ac.setVolume(id, 1.0f);
    ac.setMuted (id, false);
    ac.setPan   (id, 0.0f);
}

// ---------------------------------------------------------------------------
// v7 Tier 3 — Per-filter enable flag + ffmpeg progress-line parser
// ---------------------------------------------------------------------------

void MalloyModelTests::filterEnabledFlagRoundtripsJson() {
    // The new `enabled` flag must round-trip through toJson()/fromJson() AND
    // legacy project files (no `enabled` key) must load with `enabled = true`.

    // (1) Default-constructed filter is enabled.
    {
        CropFilter f;
        QVERIFY(f.isEnabled());
        const QJsonObject obj = f.toJson();
        // Default-true state is NOT serialized (keeps existing files byte-identical).
        QVERIFY(!obj.contains(QStringLiteral("enabled")));
    }

    // (2) Disabled filter serializes the flag.
    {
        OpacityFilter f;
        f.setOpacity(0.5f);
        f.setEnabled(false);
        const QJsonObject obj = f.toJson();
        QVERIFY(obj.contains(QStringLiteral("enabled")));
        QCOMPARE(obj.value(QStringLiteral("enabled")).toBool(), false);

        // Re-hydrate and confirm the flag survives.
        FilterEffect* loaded = FilterEffect::fromJson(obj);
        QVERIFY(loaded != nullptr);
        QVERIFY(!loaded->isEnabled());
        QCOMPARE(loaded->type(), FilterEffect::Type::Opacity);
        delete loaded;
    }

    // (3) Legacy JSON without the `enabled` key still loads as enabled=true.
    {
        QJsonObject legacy{
            {QStringLiteral("type"), QStringLiteral("color_correction")},
            {QStringLiteral("brightness"), 1.2},
            {QStringLiteral("contrast"),   1.0},
            {QStringLiteral("saturation"), 1.0},
        };
        FilterEffect* loaded = FilterEffect::fromJson(legacy);
        QVERIFY(loaded != nullptr);
        QVERIFY2(loaded->isEnabled(),
                 "Legacy project files (no 'enabled' key) must default to enabled=true");
        delete loaded;
    }

    // (4) clone() preserves the enabled flag.
    {
        BlurFilter b;
        b.setRadius(8);
        b.setEnabled(false);
        std::unique_ptr<FilterEffect> c(b.clone());
        QVERIFY(c != nullptr);
        QVERIFY(!c->isEnabled());
    }
}

void MalloyModelTests::streamProgressLineParsesBitrateAndDrops() {
    int kbps = -1, drops = -1, fps = -1;

    // Canonical line: bitrate, drop and fps together. fps is what the status
    // bar shows, and it is the reason that bar no longer invents a framerate.
    {
        const QString line = QStringLiteral(
            "frame= 1234 fps= 60 q=23.0 size= 4096kB time=00:00:20.00 "
            "bitrate=1700.6kbits/s drop=3 speed=1.0x");
        QVERIFY(EncoderPipeline::tryParseProgressLine(line, &kbps, &drops, &fps));
        QCOMPARE(kbps, 1701);
        QCOMPARE(drops, 3);
        QCOMPARE(fps, 60);
    }

    // Duplicates ffmpeg synthesised to hold a constant rate. A stream over a
    // still screen produces these, and they are not media this application
    // made: counting them as captured frames would overstate what was
    // recorded, and ignoring them would hide why a CFR output has more frames
    // than the compositor produced.
    {
        int dups = -1;
        const QString line = QStringLiteral(
            "frame= 900 fps= 60 q=21.0 size= 2048kB time=00:00:15.00 "
            "bitrate=1100.0kbits/s dup=42 drop=0 speed=1.0x");
        QVERIFY(EncoderPipeline::tryParseProgressLine(line, &kbps, &drops, &fps, &dups));
        QCOMPARE(dups, 42);
        QCOMPARE(drops, 0);
    }

    // A line with no `dup=` at all, which is every line of a VFR file
    // recording: zero duplicates, not an unparsed line.
    {
        int dups = -1;
        const QString line = QStringLiteral(
            "frame= 100 fps= 25 q=20.0 size= 512kB time=00:00:04.00 "
            "bitrate=1000.0kbits/s speed=1.0x");
        QVERIFY(EncoderPipeline::tryParseProgressLine(line, &kbps, &drops, &fps, &dups));
        QCOMPARE(dups, 0);
    }

    // Fractional rate rounds rather than truncating.
    {
        kbps = drops = fps = -1;
        const QString line = QStringLiteral(
            "frame= 900 fps=59.7 q=21.0 size= 2048kB time=00:00:15.00 "
            "bitrate=1100.0kbits/s drop=0 speed=1.0x");
        QVERIFY(EncoderPipeline::tryParseProgressLine(line, &kbps, &drops, &fps));
        QCOMPARE(fps, 60);
    }

    // Line without `drop=` (first second of a stream): drops defaults to 0.
    {
        kbps = drops = fps = -1;
        const QString line = QStringLiteral(
            "frame= 30 fps=30.0 q=18.0 size= 128kB time=00:00:01.00 "
            "bitrate=1024.0kbits/s speed=1.0x");
        QVERIFY(EncoderPipeline::tryParseProgressLine(line, &kbps, &drops, &fps));
        QCOMPARE(kbps, 1024);
        QCOMPARE(drops, 0);
        QCOMPARE(fps, 30);
    }

    // No `fps=` token at all: zero, which the status bar reads as "not
    // reported yet" and shows nothing for.
    {
        kbps = drops = fps = -1;
        const QString line = QStringLiteral(
            "size= 64kB time=00:00:00.50 bitrate=900.0kbits/s speed=1.0x");
        QVERIFY(EncoderPipeline::tryParseProgressLine(line, &kbps, &drops, &fps));
        QCOMPARE(fps, 0);
    }

    // Line with no `bitrate=` token (compile/info noise): parser must reject.
    {
        kbps = 99; drops = 99; fps = 99;
        const QString line = QStringLiteral("hevc_nvenc: GPU encoding session opened");
        QVERIFY(!EncoderPipeline::tryParseProgressLine(line, &kbps, &drops, &fps));
        // Out-params unchanged on failure.
        QCOMPARE(kbps, 99);
        QCOMPARE(drops, 99);
        QCOMPARE(fps, 99);
    }
}

void MalloyModelTests::sinkCadenceDecidesWhenAFrameIsDue() {
    using Cadence = EncoderPipeline::Cadence;
    constexpr bool kDue    = true;
    constexpr bool kNotDue = false;
    const quint64 unsequenced = TimedFrameSource::kUnsequenced;

    // --- A file follows the composition ----------------------------------
    // The same picture is not new media, and writing it again would be
    // inventing content rather than recording it. The wall-clock timestamps on
    // the input carry the gap instead.
    QVERIFY(!EncoderPipeline::shouldWriteFrame(Cadence::FollowSource, 7, 7, kDue));
    QVERIFY(EncoderPipeline::shouldWriteFrame(Cadence::FollowSource, 8, 7, kDue));

    // A gap in the sequence still means something new to record. Whatever was
    // skipped was lost upstream, and refusing to write what did arrive would
    // compound that loss rather than report it.
    QVERIFY(EncoderPipeline::shouldWriteFrame(Cadence::FollowSource, 40, 7, kDue));

    // The first frame of a run, before anything has been written.
    QVERIFY(EncoderPipeline::shouldWriteFrame(Cadence::FollowSource, 1, 0, kDue));

    // --- A stream owns its presentation clock ----------------------------
    // Due, and the picture has not moved: send the latest one again, because
    // the ingest negotiated a rate and silence is not a frame.
    QVERIFY(EncoderPipeline::shouldWriteFrame(Cadence::ConstantRate, 7, 7, kDue));

    // Not due: send nothing, whatever the compositor has been doing.
    QVERIFY(!EncoderPipeline::shouldWriteFrame(Cadence::ConstantRate, 7, 7, kNotDue));

    // The case worth pinning down. A newly composed picture must NOT pull a
    // stream frame forward. If a source event could make a frame due, the
    // presentation clock would belong to the source rather than to the sink,
    // and the negotiated cadence would drift with whatever the screen happened
    // to be doing.
    QVERIFY(!EncoderPipeline::shouldWriteFrame(Cadence::ConstantRate, 99, 7, kNotDue));

    // --- Sources that do not sequence ------------------------------------
    // Unsequenced means the source cannot say whether this picture is new, so
    // every observation counts as new. This is the behaviour that predates
    // sequencing, and it must not silently start dropping frames.
    QVERIFY(EncoderPipeline::shouldWriteFrame(Cadence::FollowSource,
                                              unsequenced, unsequenced, kDue));
    // Even unsequenced, a stream still answers to its own clock.
    QVERIFY(EncoderPipeline::shouldWriteFrame(Cadence::ConstantRate,
                                              unsequenced, unsequenced, kDue));
    QVERIFY(!EncoderPipeline::shouldWriteFrame(Cadence::ConstantRate,
                                               unsequenced, unsequenced, kNotDue));

    // And the contract's own default, so a source that ignores the method
    // keeps working.
    struct Unsequenced : TimedFrameSource {
        QImage currentFrame() override { return {}; }
        int nativeWidth() const override { return 1920; }
        int nativeHeight() const override { return 1080; }
    } src;
    QCOMPARE(src.compositionSequence(), TimedFrameSource::kUnsequenced);
    QVERIFY(EncoderPipeline::shouldWriteFrame(Cadence::FollowSource,
                                              src.compositionSequence(), 0, kDue));
}

void MalloyModelTests::machineLoadReportsUnknownRatherThanGuessing() {
    using MachineLoad::CpuSample;
    auto sample = [](quint64 idle, quint64 total) {
        CpuSample s; s.idleTicks = idle; s.totalTicks = total; s.valid = true; return s;
    };

    // Half the interval idle is half busy.
    QCOMPARE(MachineLoad::cpuBusyPercent(sample(100, 200), sample(150, 300)), 50.0);

    // Fully idle is zero, not a negative number.
    QCOMPARE(MachineLoad::cpuBusyPercent(sample(100, 200), sample(200, 300)), 0.0);

    // More idle ticks than total ticks cannot happen, but rounding across
    // counters has produced it. Clamp rather than report negative load.
    QCOMPARE(MachineLoad::cpuBusyPercent(sample(100, 200), sample(260, 300)), 0.0);

    // An invalid reading on either side says nothing.
    QVERIFY(MachineLoad::cpuBusyPercent(CpuSample{}, sample(150, 300)) < 0);
    QVERIFY(MachineLoad::cpuBusyPercent(sample(100, 200), CpuSample{}) < 0);

    // Two readings inside the same tick: no elapsed time to divide by.
    QVERIFY(MachineLoad::cpuBusyPercent(sample(100, 200), sample(100, 200)) < 0);

    // Counters moving backwards happens across a suspend. That is not a spike
    // to 100 percent, it is an absence of information.
    QVERIFY(MachineLoad::cpuBusyPercent(sample(100, 200), sample(90, 190)) < 0);
    QVERIFY(MachineLoad::cpuBusyPercent(sample(100, 200), sample(90, 300)) < 0);

    // And the live reading, which must be a real percentage or an admission.
    const double ram = MachineLoad::memoryUsedPercent();
    QVERIFY(ram < 0 || (ram >= 0.0 && ram <= 100.0));
}

// ---------------------------------------------------------------------------
// v7 Tier 4 — Per-source mute hotkeys
// ---------------------------------------------------------------------------

void MalloyModelTests::audioMuteActionIdTogglesInput() {
    // Mirrors what MainWindow's hotkey dispatcher does on receiving
    // `triggered("audio.mute.loopback:default")`: look up the matching
    // AudioController input by id, flip its `muted` flag. This is the
    // contract per-source Mute hotkeys depend on.

    AudioController c;
    QCOMPARE(c.inputs().size(), 1);                       // loopback:default
    QCOMPARE(c.inputs().first().id, QStringLiteral("loopback:default"));
    QCOMPARE(c.inputs().first().muted, false);

    auto dispatchMute = [&c](const QString& actionId) {
        if (!actionId.startsWith(QStringLiteral("audio.mute."))) return;
        const QString audioId = actionId.mid(11);
        const auto& inputs = c.inputs();
        auto it = std::find_if(inputs.cbegin(), inputs.cend(),
            [&](const AudioInput& in){ return in.id == audioId; });
        if (it != inputs.cend()) c.setMuted(audioId, !it->muted);
    };

    // First press: unmuted → muted.
    dispatchMute(QStringLiteral("audio.mute.loopback:default"));
    QCOMPARE(c.inputs().first().muted, true);

    // Second press: muted → unmuted.
    dispatchMute(QStringLiteral("audio.mute.loopback:default"));
    QCOMPARE(c.inputs().first().muted, false);

    // Unknown input id is a no-op (no crash, no state change).
    dispatchMute(QStringLiteral("audio.mute.does-not-exist"));
    QCOMPARE(c.inputs().first().muted, false);
}

void MalloyModelTests::togglingAudioInputVisibilityChangesGatherList() {
    // Hidden AudioInput items must be excluded from gatherVisibleAudioIds()
    // so reconcileInputs() can tear down their workers — matching how
    // DisplayCapture / WindowCapture sessions are scoped to visibility.

    SceneCollection scenes;
    QUndoStack undo;
    scenes.setUndoStack(&undo);
    scenes.addScene(QStringLiteral("Scene"));

    const QString fakeId = QStringLiteral("{11111111-2222-3333-4444-555555555555}");
    SceneItem* item = scenes.addAudioInputToCurrent(QStringLiteral("Mic"), fakeId);
    QVERIFY(item != nullptr);

    // Default state: item visible → gather returns the device.
    {
        const QStringList ids = scenes.gatherVisibleAudioIds();
        QVERIFY2(ids.contains(fakeId),
                 qPrintable(QStringLiteral("Expected gather to include %1, got: [%2]")
                               .arg(fakeId, ids.join(QLatin1Char(',')))));
    }

    // Hide it → gather must drop it.
    scenes.setCurrentItemVisible(0, false);
    {
        const QStringList ids = scenes.gatherVisibleAudioIds();
        QVERIFY(!ids.contains(fakeId));
    }

    // Show it again → back in the list.
    scenes.setCurrentItemVisible(0, true);
    {
        const QStringList ids = scenes.gatherVisibleAudioIds();
        QVERIFY(ids.contains(fakeId));
    }
}

void MalloyModelTests::clipsRegistryRoundTrips() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString store = dir.filePath(QStringLiteral("clips.json"));

    {
        ClipsRegistry reg;
        reg.setStorePath(store);
        QCOMPARE(reg.count(), 0);

        ClipInfo a;
        a.filePath = QStringLiteral("C:/clips/a.mp4");
        a.name = QStringLiteral("First clip");
        a.sourceProject = QStringLiteral("Spire");
        a.recordedAt = QDateTime(QDate(2026, 5, 21), QTime(12, 0));
        a.sizeBytes = 5 * 1024 * 1024;
        a.durationSecs = 30;
        a.tags = {QStringLiteral("highlight")};
        reg.addClip(a);

        ClipInfo b;
        b.name = QStringLiteral("Second clip");
        reg.addClip(b);

        // Newest first (prepend).
        QCOMPARE(reg.count(), 2);
        QCOMPARE(reg.clips().at(0).name, QStringLiteral("Second clip"));
        QCOMPARE(reg.clips().at(1).name, QStringLiteral("First clip"));

        // Favorite the first-added clip by id.
        reg.setFavorite(reg.clips().at(1).id, true);
        QVERIFY(reg.clips().at(1).favorite);
    }

    // Reload into a fresh registry: state must persist.
    ClipsRegistry reg2;
    reg2.setStorePath(store);
    QCOMPARE(reg2.count(), 2);
    const ClipInfo& first = reg2.clips().at(1);
    QCOMPARE(first.name, QStringLiteral("First clip"));
    QCOMPARE(first.sourceProject, QStringLiteral("Spire"));
    QCOMPARE(first.durationSecs, 30);
    QCOMPARE(first.sizeBytes, qint64(5 * 1024 * 1024));
    QVERIFY(first.favorite);
    QCOMPARE(first.tags, QStringList{QStringLiteral("highlight")});
    QCOMPARE(first.durationText(), QStringLiteral("0:30"));
}

void MalloyModelTests::projectRegistryScansMalloyFiles() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto write = [&](const QString& name, const QByteArray& content) {
        QFile f(dir.filePath(name));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(content);
        f.close();
    };
    write(QStringLiteral("alpha.malloy.json"), R"({"scenes":[{},{},{}]})");
    write(QStringLiteral("beta.malloy.json"),  R"({"scenes":[{}]})");
    write(QStringLiteral("notes.txt"),         "ignore me");
    write(QStringLiteral("plain.json"),        "{}");   // not *.malloy.json => ignored

    ProjectRegistry reg;
    reg.setSearchDirs({dir.path()});
    QCOMPARE(reg.count(), 2);

    QStringList names;
    int alphaScenes = -99, betaScenes = -99;
    for (const ProjectInfo& p : reg.projects()) {
        names << p.name;
        if (p.name == QLatin1String("alpha")) alphaScenes = p.sceneCount;
        if (p.name == QLatin1String("beta"))  betaScenes = p.sceneCount;
    }
    QVERIFY(names.contains(QStringLiteral("alpha")));
    QVERIFY(names.contains(QStringLiteral("beta")));
    QCOMPARE(alphaScenes, 3);
    QCOMPARE(betaScenes, 1);
}

void MalloyModelTests::mediaRegistryClassifiesByExtension() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto touch = [&](const QString& name) {
        QFile f(dir.filePath(name));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
        f.close();
    };
    touch(QStringLiteral("clip.mp4"));
    touch(QStringLiteral("song.wav"));
    touch(QStringLiteral("thumb.png"));
    touch(QStringLiteral("readme.txt"));        // ignored
    touch(QStringLiteral("scene.malloy.json")); // ignored (not a media ext)

    MediaRegistry reg;
    reg.setSearchDirs({dir.path()});
    QCOMPARE(reg.count(), 3);
    QCOMPARE(reg.countOfKind(MediaInfo::Video), 1);
    QCOMPARE(reg.countOfKind(MediaInfo::Audio), 1);
    QCOMPARE(reg.countOfKind(MediaInfo::Image), 1);
}

void MalloyModelTests::renderQueueProcessesAndPersists() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString store = dir.filePath(QStringLiteral("rq.json"));

    RenderQueue q;
    q.setStorePath(store);
    QCOMPARE(q.jobs().size(), 0);

    // The media these jobs name does not exist, so the pipeline refuses to
    // start them. That is the deterministic, ffmpeg-free path through the state
    // machine: a job that cannot start is failed with a reason rather than
    // sitting Active forever or blocking the jobs behind it.
    QVERIFY(!q.enqueue(makeRenderRequest(dir.filePath(QStringLiteral("a.mp4")))).isEmpty());
    QVERIFY(!q.enqueue(makeRenderRequest(dir.filePath(QStringLiteral("b.mp4")))).isEmpty());
    QCOMPARE(q.jobs().size(), 2);
    QCOMPARE(q.countOfState(RenderJob::Failed), 2);
    QCOMPARE(q.countOfState(RenderJob::Active), 0);
    QCOMPARE(q.countOfState(RenderJob::Pending), 0);
    for (const RenderJob& j : q.jobs()) {
        QVERIFY(!j.error.isEmpty());
        QVERIFY(j.error.contains(QStringLiteral("missing")));
    }

    // Reload: the failures, their reasons and their snapshots all survive.
    RenderQueue q2;
    q2.setStorePath(store);
    QCOMPARE(q2.jobs().size(), 2);
    QCOMPARE(q2.countOfState(RenderJob::Failed), 2);
    for (const RenderJob& j : q2.jobs()) {
        QVERIFY(!j.error.isEmpty());
        QCOMPARE(j.timeline.size(), 1);
        QVERIFY(j.renderable());
        QCOMPARE(j.output.width, OutputSettings{}.width);
        QVERIFY(!j.target.isEmpty());
    }
}

void MalloyModelTests::renderQueueRetryCancelClear() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString store = dir.filePath(QStringLiteral("rq.json"));

    // The simulated worker only ever *completes* jobs — it never fails one. To
    // exercise retry() we seed the store with a Failed job (state 3) plus a
    // Completed one (state 2), then load it. State enum: Pending=0, Active=1,
    // Completed=2, Failed=3.
    auto seed = [&](const QString& id, int state, int progress) {
        QJsonObject o;
        o.insert(QStringLiteral("id"), id);
        o.insert(QStringLiteral("name"), id);
        o.insert(QStringLiteral("state"), state);
        o.insert(QStringLiteral("progress"), progress);
        if (state == 3) o.insert(QStringLiteral("error"), QStringLiteral("boom"));
        // Seeded jobs carry a snapshot so they are retryable; a job without one
        // is covered by renderQueueRetiresJobsWithoutASnapshot().
        const RenderRequest r = makeRenderRequest(dir.filePath(id + QStringLiteral(".mp4")));
        o.insert(QStringLiteral("outputPath"), r.outputPath);
        o.insert(QStringLiteral("timeline"), r.timeline);
        o.insert(QStringLiteral("output"), r.output.toJson());
        return o;
    };
    {
        QFile f(store);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QJsonArray arr;
        arr.append(seed(QStringLiteral("fail-1"), 3, 40));
        arr.append(seed(QStringLiteral("done-1"), 2, 100));
        f.write(QJsonDocument(arr).toJson());
        f.close();
    }

    RenderQueue q;
    q.setStorePath(store);
    QCOMPARE(q.jobs().size(), 2);
    QCOMPARE(q.countOfState(RenderJob::Failed), 1);
    QCOMPARE(q.countOfState(RenderJob::Completed), 1);
    QCOMPARE(q.countOfState(RenderJob::Active), 0);   // nothing pending to promote

    // cancel() only removes Active/Pending jobs — Completed and Failed are
    // history and must be left intact.
    q.cancel(QStringLiteral("done-1"));
    q.cancel(QStringLiteral("fail-1"));
    QCOMPARE(q.jobs().size(), 2);

    // clearCompleted() drops only the Completed job.
    q.clearCompleted();
    QCOMPARE(q.countOfState(RenderJob::Completed), 0);
    QCOMPARE(q.jobs().size(), 1);

    // retry() re-attempts the job rather than just re-queueing it. The seeded
    // job names media that does not exist, so it fails again, but with the
    // pipeline's reason instead of the seeded "boom".
    QSignalSpy spy(&q, &RenderQueue::changed);
    q.retry(QStringLiteral("fail-1"));
    QVERIFY(spy.count() >= 1);
    QCOMPARE(q.countOfState(RenderJob::Failed), 1);
    QCOMPARE(q.countOfState(RenderJob::Active), 0);
    QCOMPARE(q.countOfState(RenderJob::Pending), 0);
    QVERIFY(!q.jobs().first().error.isEmpty());
    QVERIFY(q.jobs().first().error != QStringLiteral("boom"));

    // retry() on an unknown id is a no-op (no crash, no state change).
    q.retry(QStringLiteral("ghost"));
    QCOMPARE(q.jobs().size(), 1);

    // cancel() leaves a Failed job alone: it is history, like a completed one.
    q.cancel(QStringLiteral("fail-1"));
    QCOMPARE(q.jobs().size(), 1);
}

void MalloyModelTests::renderQueuePauseHoldsPendingJobs() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString store = dir.filePath(QStringLiteral("rq_pause.json"));

    RenderQueue q;
    q.setStorePath(store);
    QVERIFY(!q.paused());

    // Pause, then enqueue: startNext() must NOT promote while paused, so both
    // jobs stay Pending.
    q.setPaused(true);
    QVERIFY(q.paused());
    q.enqueue(makeRenderRequest(dir.filePath(QStringLiteral("a.mp4"))));
    q.enqueue(makeRenderRequest(dir.filePath(QStringLiteral("b.mp4"))));
    QCOMPARE(q.countOfState(RenderJob::Active), 0);
    QCOMPARE(q.countOfState(RenderJob::Pending), 2);
    // Nothing was attempted while paused, so nothing has failed either.
    QCOMPARE(q.countOfState(RenderJob::Failed), 0);

    // Resuming attempts them. Their media is missing, so both are refused at
    // start; what this asserts is that the pause gate was the only thing
    // holding them back.
    q.setPaused(false);
    QVERIFY(!q.paused());
    QCOMPARE(q.countOfState(RenderJob::Pending), 0);
    QCOMPARE(q.countOfState(RenderJob::Failed), 2);
}

void MalloyModelTests::projectDocumentV3TimelineRoundTrips() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    SceneCollection scenes;
    scenes.addScene(QStringLiteral("Scene"));

    // A timeline array mirroring EditorWorkspace's clip schema.
    QJsonArray timeline;
    timeline.append(QJsonObject{
        {QStringLiteral("track"), 0}, {QStringLiteral("start"), 0.0},
        {QStringLiteral("dur"), 8.0}, {QStringLiteral("label"), QStringLiteral("Intro")},
        {QStringLiteral("tag"), QStringLiteral("IMG")},
        {QStringLiteral("color"), QStringLiteral("#9678d2")}, {QStringLiteral("audio"), false}});
    timeline.append(QJsonObject{
        {QStringLiteral("track"), 3}, {QStringLiteral("start"), 8.0},
        {QStringLiteral("dur"), 280.0}, {QStringLiteral("label"), QStringLiteral("Mic")},
        {QStringLiteral("tag"), QStringLiteral("AUD")},
        {QStringLiteral("color"), QStringLiteral("#5fbe82")}, {QStringLiteral("audio"), true}});

    QString err;
    const QString path = dir.filePath(QStringLiteral("v3.malloy.json"));
    QVERIFY2(ProjectDocument::saveToFile(scenes, timeline, path, &err), qPrintable(err));

    // (1) The on-disk file carries a 2-element "timeline" array.
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        QVERIFY(root.contains(QStringLiteral("timeline")));
        QCOMPARE(root.value(QStringLiteral("timeline")).toArray().size(), 2);
    }

    // (2) loadFromFile restores the same timeline values and the scene data.
    SceneCollection loaded;
    QJsonArray loadedTimeline;
    QVERIFY2(ProjectDocument::loadFromFile(loaded, &loadedTimeline, path, &err), qPrintable(err));
    QCOMPARE(loaded.sceneCount(), 1);
    QCOMPARE(loadedTimeline.size(), 2);
    QCOMPARE(loadedTimeline.at(0).toObject().value(QStringLiteral("label")).toString(),
             QStringLiteral("Intro"));
    QCOMPARE(loadedTimeline.at(1).toObject().value(QStringLiteral("audio")).toBool(), true);
    QCOMPARE(loadedTimeline.at(1).toObject().value(QStringLiteral("dur")).toDouble(), 280.0);

    // (3) Back-compat: the legacy 2-arg save writes NO "timeline" key, and the
    //     timeline-aware load of such a file yields an empty timeline. Older
    //     v1/v2 projects therefore stay byte-identical and load unchanged.
    const QString v2path = dir.filePath(QStringLiteral("v2.malloy.json"));
    QVERIFY2(ProjectDocument::saveToFile(scenes, v2path, &err), qPrintable(err));
    {
        QFile f(v2path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        QVERIFY(!root.contains(QStringLiteral("timeline")));
    }
    SceneCollection v2loaded;
    QJsonArray v2timeline;
    QVERIFY2(ProjectDocument::loadFromFile(v2loaded, &v2timeline, v2path, &err), qPrintable(err));
    QVERIFY(v2timeline.isEmpty());

    // (4) Saving with an EMPTY timeline must also omit the key — this guards the
    //     `!timeline.isEmpty()` insert so a project with a cleared timeline does
    //     not start emitting an empty array that diffs against older files.
    const QString emptyPath = dir.filePath(QStringLiteral("empty.malloy.json"));
    QVERIFY2(ProjectDocument::saveToFile(scenes, QJsonArray{}, emptyPath, &err), qPrintable(err));
    {
        QFile f(emptyPath);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        QVERIFY(!root.contains(QStringLiteral("timeline")));
    }
}

void MalloyModelTests::registriesDegradeGracefullyOnBadInput() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // (1) ClipsRegistry: a missing store loads empty and setFavorite on an
    //     unknown id is a harmless no-op.
    {
        ClipsRegistry reg;
        reg.setStorePath(dir.filePath(QStringLiteral("missing.json")));
        QCOMPARE(reg.count(), 0);
        reg.setFavorite(QStringLiteral("no-such-id"), true);
        QCOMPARE(reg.count(), 0);
    }
    // (2) ClipsRegistry: a corrupt JSON store loads empty rather than crashing.
    {
        const QString corrupt = dir.filePath(QStringLiteral("corrupt.json"));
        QFile f(corrupt);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{ this is not valid json ]]]");
        f.close();
        ClipsRegistry reg;
        reg.setStorePath(corrupt);
        QCOMPARE(reg.count(), 0);
    }

    // (3) ProjectRegistry: nonexistent and empty search-dir sets yield nothing.
    {
        ProjectRegistry reg;
        reg.setSearchDirs({dir.filePath(QStringLiteral("nope_projects"))});
        QCOMPARE(reg.count(), 0);
        reg.setSearchDirs({});
        QCOMPARE(reg.count(), 0);
    }

    // (4) MediaRegistry: an empty dir and a nonexistent dir both yield nothing.
    {
        const QString emptyDir = dir.filePath(QStringLiteral("empty_media"));
        QVERIFY(QDir().mkpath(emptyDir));
        MediaRegistry reg;
        reg.setSearchDirs({emptyDir});
        QCOMPARE(reg.count(), 0);
        reg.setSearchDirs({dir.filePath(QStringLiteral("nonexistent_media"))});
        QCOMPARE(reg.count(), 0);
    }
}

// Helper: create `name` in `dir` with `bytes` of content and an explicit
// modification time, so ordering does not depend on how fast the test runs.
static QString makeTimedFile(const QString& dir, const QString& name,
                             int bytes, const QDateTime& modified) {
    const QString path = QDir(dir).filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return QString();
    f.write(QByteArray(bytes, 'x'));
    f.close();
    // setFileTime needs an open handle; reopen read-write for the timestamp.
    if (!f.open(QIODevice::ReadWrite)) return QString();
    f.setFileTime(modified, QFileDevice::FileModificationTime);
    f.close();
    return path;
}

void MalloyModelTests::recentRecordingsScanFiltersOrdersAndLimits() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QDateTime base = QDateTime::currentDateTime();
    QVERIFY(!makeTimedFile(dir.path(), QStringLiteral("oldest.mkv"),
                           3 * 1024 * 1024, base.addSecs(-7200)).isEmpty());
    QVERIFY(!makeTimedFile(dir.path(), QStringLiteral("middle.mp4"),
                           2 * 1024 * 1024, base.addSecs(-3600)).isEmpty());
    QVERIFY(!makeTimedFile(dir.path(), QStringLiteral("newest.mp3"),
                           1024 * 1024, base.addSecs(-60)).isEmpty());
    // Not a recording container: must be ignored.
    QVERIFY(!makeTimedFile(dir.path(), QStringLiteral("notes.txt"),
                           1024, base).isEmpty());

    const QVector<RecordingInfo> all = RecentRecordings::scan(dir.path(), 0);
    QCOMPARE(all.size(), 3);
    QCOMPARE(all.at(0).name, QStringLiteral("newest.mp3"));
    QCOMPARE(all.at(1).name, QStringLiteral("middle.mp4"));
    QCOMPARE(all.at(2).name, QStringLiteral("oldest.mkv"));
    QCOMPARE(all.at(0).sizeBytes, static_cast<qint64>(1024 * 1024));
    QCOMPARE(all.at(0).sizeText(), QStringLiteral("1.0 MB"));
    QVERIFY(all.at(0).filePath.endsWith(QStringLiteral("newest.mp3")));

    // The limit keeps the newest entries.
    const QVector<RecordingInfo> limited = RecentRecordings::scan(dir.path(), 2);
    QCOMPARE(limited.size(), 2);
    QCOMPARE(limited.at(0).name, QStringLiteral("newest.mp3"));
    QCOMPARE(limited.at(1).name, QStringLiteral("middle.mp4"));

    // Missing and empty paths yield an empty list rather than failing.
    QCOMPARE(RecentRecordings::scan(dir.filePath(QStringLiteral("nope")), 4).size(), 0);
    QCOMPARE(RecentRecordings::scan(QString(), 4).size(), 0);
}

void MalloyModelTests::recentRecordingsRelativeTimeBuckets() {
    const QDateTime now = QDateTime(QDate(2026, 3, 12), QTime(15, 0));
    auto whenFor = [&now](const QDateTime& modified) {
        RecordingInfo r;
        r.modified = modified;
        return r.relativeTimeText(now);
    };

    QCOMPARE(whenFor(now.addSecs(-30)), QStringLiteral("Just now"));
    QCOMPARE(whenFor(now.addSecs(-15 * 60)), QStringLiteral("15 min ago"));
    QCOMPARE(whenFor(now.addSecs(-3 * 3600)), QStringLiteral("3 hr ago"));
    QCOMPARE(whenFor(now.addDays(-1)), QStringLiteral("Yesterday"));
    QCOMPARE(whenFor(now.addDays(-3)), QDate(2026, 3, 9).toString(QStringLiteral("ddd")));
    QCOMPARE(whenFor(now.addDays(-30)), QStringLiteral("Feb 10"));
    QCOMPARE(whenFor(now.addDays(-400)), QStringLiteral("Feb 5 2025"));
    // A future timestamp (clock skew) must not render as a negative age.
    QCOMPARE(whenFor(now.addSecs(120)), QStringLiteral("Just now"));
    QCOMPARE(whenFor(QDateTime()), QStringLiteral("Unknown"));
}

void MalloyModelTests::editorClipRoundTripPreservesSourceReference() {
    EditorWorkspace editor(nullptr);
    QJsonArray in;
    QJsonObject c;
    c.insert(QStringLiteral("track"), 2);
    c.insert(QStringLiteral("start"), 8.0);
    c.insert(QStringLiteral("dur"),   132.0);
    c.insert(QStringLiteral("label"), QStringLiteral("spire-ep14.mkv"));
    c.insert(QStringLiteral("tag"),   QStringLiteral("VID"));
    c.insert(QStringLiteral("color"), QStringLiteral("#5087c3"));
    c.insert(QStringLiteral("audio"), false);
    c.insert(QStringLiteral("sourcePath"), QStringLiteral("F:/Captures/spire-ep14.mkv"));
    c.insert(QStringLiteral("sourceIn"),   12.5);
    in.append(c);

    editor.setTimelineJson(in);
    const QJsonArray out = editor.timelineJson();
    QCOMPARE(out.size(), 1);
    const QJsonObject got = out.at(0).toObject();
    QCOMPARE(got.value(QStringLiteral("sourcePath")).toString(),
             QStringLiteral("F:/Captures/spire-ep14.mkv"));
    QCOMPARE(got.value(QStringLiteral("sourceIn")).toDouble(), 12.5);
    // The rest of the clip is untouched by the new fields.
    QCOMPARE(got.value(QStringLiteral("start")).toDouble(), 8.0);
    QCOMPARE(got.value(QStringLiteral("dur")).toDouble(),   132.0);
}

void MalloyModelTests::editorLegacyClipLoadsAsUnlinked() {
    // Every project written before ADR-0001 has clips with no source keys.
    // They must load without complaint, as unlinked clips starting at 0, and a
    // re-save must record that state explicitly rather than omitting it.
    EditorWorkspace editor(nullptr);
    QJsonArray in;
    QJsonObject c;
    c.insert(QStringLiteral("track"), 0);
    c.insert(QStringLiteral("start"), 0.0);
    c.insert(QStringLiteral("dur"),   8.0);
    c.insert(QStringLiteral("label"), QStringLiteral("Intro card"));
    c.insert(QStringLiteral("audio"), false);
    in.append(c);

    editor.setTimelineJson(in);
    const QJsonArray out = editor.timelineJson();
    QCOMPARE(out.size(), 1);
    const QJsonObject got = out.at(0).toObject();
    QVERIFY(got.contains(QStringLiteral("sourcePath")));
    QVERIFY(got.value(QStringLiteral("sourcePath")).toString().isEmpty());
    QCOMPARE(got.value(QStringLiteral("sourceIn")).toDouble(), 0.0);
}

void MalloyModelTests::timelineTrimKeepsSourceInSync() {
    constexpr double kMinDur = 0.25;

    // Dragging the left edge right by 4 s consumes 4 s more of the source.
    TimelineTrim t = trimLeftEdge(10.0, 20.0, 5.0, 1.0, 14.0, true, kMinDur);
    QCOMPARE(t.start,    14.0);
    QCOMPARE(t.dur,      16.0);
    QCOMPARE(t.sourceIn,  9.0);

    // At 2x speed the same 4 s of timeline eats 8 s of source.
    t = trimLeftEdge(10.0, 20.0, 5.0, 2.0, 14.0, true, kMinDur);
    QCOMPARE(t.sourceIn, 13.0);

    // Dragging left gives time back, down to the head of the source: with
    // sourceIn 5 and speed 1 the edge cannot pass timeline 5.
    t = trimLeftEdge(10.0, 20.0, 5.0, 1.0, 2.0, true, kMinDur);
    QCOMPARE(t.start,    5.0);
    QCOMPARE(t.dur,      25.0);
    QCOMPARE(t.sourceIn, 0.0);

    // An unlinked clip has no source to run out of, so it stops at 0 instead.
    t = trimLeftEdge(10.0, 20.0, 0.0, 1.0, 2.0, false, kMinDur);
    QCOMPARE(t.start,    2.0);
    QCOMPARE(t.dur,      28.0);
    QCOMPARE(t.sourceIn, 0.0);

    // The minimum duration still wins over a right-ward drag.
    t = trimLeftEdge(10.0, 20.0, 5.0, 1.0, 999.0, true, kMinDur);
    QCOMPARE(t.dur, kMinDur);

    // The right edge never moves the in-point.
    TimelineTrim r = trimRightEdge(10.0, 5.0, 22.0, kMinDur, 360.0);
    QCOMPARE(r.start,    10.0);
    QCOMPARE(r.dur,      12.0);
    QCOMPARE(r.sourceIn,  5.0);
    // ... and is bounded by the timeline length and the minimum duration.
    r = trimRightEdge(10.0, 5.0, 900.0, kMinDur, 360.0);
    QCOMPARE(r.dur, 350.0);
    r = trimRightEdge(10.0, 5.0, 10.0, kMinDur, 360.0);
    QCOMPARE(r.dur, kMinDur);
}

void MalloyModelTests::timelineSplitDerivesRightHandSourceIn() {
    // Split 6 s into a clip that starts at timeline 10 with in-point 30.
    QCOMPARE(splitSourceIn(10.0, 30.0, 1.0, 16.0), 36.0);
    // Half speed consumes half as much source over the same timeline span.
    QCOMPARE(splitSourceIn(10.0, 30.0, 0.5, 16.0), 33.0);
    // Double speed consumes twice as much.
    QCOMPARE(splitSourceIn(10.0, 30.0, 2.0, 16.0), 42.0);
    // A cut at the clip head is a no-op on the in-point.
    QCOMPARE(splitSourceIn(10.0, 30.0, 1.0, 10.0), 30.0);
}

void MalloyModelTests::outputSettingsJsonRoundTrips() {
    OutputSettings in;
    in.width = 2560; in.height = 1440; in.fps = 120;
    in.videoCodec = QStringLiteral("h264_nvenc");
    in.crf = 19; in.preset = QStringLiteral("p5");
    in.audioCodec = QStringLiteral("opus"); in.audioBitratekbps = 256;
    in.container = QStringLiteral("mkv"); in.bitrateKbps = 24000; in.keyframeSec = 2;

    const OutputSettings out = OutputSettings::fromJson(in.toJson());
    QCOMPARE(out.width, 2560);
    QCOMPARE(out.height, 1440);
    QCOMPARE(out.fps, 120);
    QCOMPARE(out.videoCodec, QStringLiteral("h264_nvenc"));
    QCOMPARE(out.crf, 19);
    QCOMPARE(out.preset, QStringLiteral("p5"));
    QCOMPARE(out.audioCodec, QStringLiteral("opus"));
    QCOMPARE(out.audioBitratekbps, 256);
    QCOMPARE(out.container, QStringLiteral("mkv"));
    QCOMPARE(out.bitrateKbps, 24000);
    QCOMPARE(out.keyframeSec, 2);

    // An empty object yields the documented defaults rather than zeroes.
    const OutputSettings defaults = OutputSettings::fromJson(QJsonObject{});
    const OutputSettings expected;
    QCOMPARE(defaults.width, expected.width);
    QCOMPARE(defaults.videoCodec, expected.videoCodec);
    QCOMPARE(defaults.crf, expected.crf);
    QCOMPARE(defaults.container, expected.container);
}

void MalloyModelTests::renderJobCarriesSettingsAndSnapshot() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    RenderQueue q;
    q.setStorePath(dir.filePath(QStringLiteral("rq.json")));

    RenderRequest req = makeRenderRequest(dir.filePath(QStringLiteral("out.mp4")));
    req.output.width = 1280;
    req.output.height = 720;
    req.output.fps = 30;
    const QString id = q.enqueue(req);
    QVERIFY(!id.isEmpty());
    QCOMPARE(q.jobs().size(), 1);

    const RenderJob& j = q.jobs().first();
    QCOMPARE(j.output.width, 1280);
    QCOMPARE(j.output.fps, 30);
    QCOMPARE(j.timeline.size(), 1);
    QCOMPARE(j.projectPath, QStringLiteral("C:/proj/proj.malloy.json"));
    QVERIFY(j.renderable());
    // The target string is derived from the settings, not passed in.
    QCOMPARE(j.target, RenderQueue::describeTarget(req.output));
    QVERIFY(j.target.contains(QStringLiteral("1280x720")));

    // The snapshot is a copy: editing the timeline afterwards must not reach a
    // job that is already queued.
    req.timeline = QJsonArray{};
    QCOMPARE(q.jobs().first().timeline.size(), 1);
}

void MalloyModelTests::renderQueueRetiresJobsWithoutASnapshot() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString store = dir.filePath(QStringLiteral("rq_legacy.json"));

    // Schema 0: a bare array, jobs with no output settings and no timeline.
    {
        auto legacy = [](const QString& id, int state) {
            QJsonObject o;
            o.insert(QStringLiteral("id"), id);
            o.insert(QStringLiteral("name"), id);
            o.insert(QStringLiteral("state"), state);
            return o;
        };
        QJsonArray arr;
        arr.append(legacy(QStringLiteral("old-pending"), 0));
        arr.append(legacy(QStringLiteral("old-done"), 2));
        QFile f(store);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(arr).toJson());
        f.close();
    }

    RenderQueue q;
    q.setStorePath(store);
    QCOMPARE(q.jobs().size(), 2);
    // The pending job is retired with a reason instead of being run.
    QCOMPARE(q.countOfState(RenderJob::Failed), 1);
    QCOMPARE(q.countOfState(RenderJob::Active), 0);
    QCOMPARE(q.countOfState(RenderJob::Pending), 0);
    // History is left alone.
    QCOMPARE(q.countOfState(RenderJob::Completed), 1);

    const RenderJob* retired = nullptr;
    for (const RenderJob& j : q.jobs())
        if (j.id == QLatin1String("old-pending")) retired = &j;
    QVERIFY(retired);
    QVERIFY(!retired->error.isEmpty());
    QVERIFY(!retired->renderable());

    // Retrying it does not put an unrunnable job back in the queue.
    q.retry(QStringLiteral("old-pending"));
    QCOMPARE(q.countOfState(RenderJob::Failed), 1);
    QCOMPARE(q.countOfState(RenderJob::Active), 0);
}

void MalloyModelTests::renderQueueRejectsUnrenderableRequests() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    RenderQueue q;
    q.setStorePath(dir.filePath(QStringLiteral("rq.json")));

    QString error;
    // No timeline.
    RenderRequest empty = makeRenderRequest(dir.filePath(QStringLiteral("a.mp4")));
    empty.timeline = QJsonArray{};
    QVERIFY(q.enqueue(empty, &error).isEmpty());
    QVERIFY(!error.isEmpty());

    // No output file.
    RenderRequest noOut = makeRenderRequest(dir.filePath(QStringLiteral("b.mp4")));
    noOut.outputPath.clear();
    QVERIFY(q.enqueue(noOut, &error).isEmpty());
    QVERIFY(!error.isEmpty());

    // Output directory does not exist: caught now rather than at render time.
    RenderRequest badDir = makeRenderRequest(
        dir.filePath(QStringLiteral("nope/deeper/c.mp4")));
    QVERIFY(q.enqueue(badDir, &error).isEmpty());
    QVERIFY(error.contains(QStringLiteral("does not exist")));

    QCOMPARE(q.jobs().size(), 0);

    // A well-formed request still succeeds and clears the error.
    QVERIFY(!q.enqueue(makeRenderRequest(dir.filePath(QStringLiteral("d.mp4"))), &error).isEmpty());
    QVERIFY(error.isEmpty());
    QCOMPARE(q.jobs().size(), 1);
}

void MalloyModelTests::aSavedReplayPlaysInRealTime() {
    // A 30 second replay buffer as the preview fills it: five frames a second,
    // each tagged with its capture time.
    constexpr int kFrames = 150;
    constexpr qint64 kSpacingUs = 200000;
    QQueue<ReplayFrame> ring;
    for (int i = 0; i < kFrames; ++i) {
        QImage img(32, 18, QImage::Format_RGB32);
        img.fill(QColor(i % 256, 0, 0));
        QByteArray jpeg;
        QBuffer buf(&jpeg);
        buf.open(QIODevice::WriteOnly);
        QVERIFY(img.save(&buf, "JPEG"));
        ring.enqueue(ReplayFrame{jpeg, 1000000 + i * kSpacingUs});
    }

    RingTimedFrameSource source(ring, 32, 18);
    qint64 now = 0;
    source.setClockForTesting([&now] { return now; });
    int finishedAt = -1;
    QObject::connect(&source, &RingTimedFrameSource::exhausted, &source,
                     [&finishedAt, &now] { finishedAt = int(now / 1000); });

    // Asking repeatedly at one instant must not advance the replay. This is
    // exactly what it used to do: one frame per call, whatever the time.
    source.currentFrame();
    const quint64 atStart = source.compositionSequence();
    for (int i = 0; i < 20; ++i) source.currentFrame();
    QCOMPARE(source.compositionSequence(), atStart);
    QCOMPARE(source.servedFrames(), 1);

    // The encoder ticking at 30 Hz for 40 seconds.
    int distinct = 1;
    quint64 last = source.compositionSequence();
    for (int tick = 1; tick <= 1200 && finishedAt < 0; ++tick) {
        now = qint64(tick) * 1000000 / 30;
        source.currentFrame();
        const quint64 seq = source.compositionSequence();
        if (seq != last) { ++distinct; last = seq; }
        QVERIFY2(seq != TimedFrameSource::kUnsequenced,
                 "kUnsequenced would make a file encoder write every tick");
    }

    QCOMPARE(source.servedFrames(), kFrames);
    QCOMPARE(distinct, kFrames);
    // It ends once the last frame, at 29.8 s, has had its 200 ms on screen:
    // about 30 s of playback, not the 5 s it took to consume one per tick.
    QVERIFY2(finishedAt >= 29900 && finishedAt <= 30100,
             qPrintable(QStringLiteral("replay ended at %1 ms").arg(finishedAt)));
}

void MalloyModelTests::undoKeepsTheLiveCaptureFrameOnAir() {
    SceneCollection scenes;
    QUndoStack undo;
    scenes.setUndoStack(&undo);
    scenes.ensureCurrentScene();
    QVERIFY(scenes.addNewSourceToCurrent(QStringLiteral("Screen"), Source::Type::DisplayCapture,
                                         QString(), QColor(), 0, 0) != nullptr);
    undo.clear();

    // Never shown: recording is what makes it compose.
    PreviewWidget preview(&scenes, PreviewWidget::Role::Program);
    preview.setRecordingActive(true);

    QImage red(64, 36, QImage::Format_ARGB32_Premultiplied);
    red.fill(QColor(255, 0, 0));
    preview.updateFrame(0, 0, red);
    QCoreApplication::processEvents();
    const QPoint centre(int(MalloyCanvas::Width / 2), int(MalloyCanvas::Height / 2));
    QCOMPARE(preview.cachedComposedFrame().pixelColor(centre), QColor(255, 0, 0));

    // Any edit, then undo. The session is still running, so no new frame will
    // arrive on a still desktop; the one already delivered must stay on air
    // rather than being replaced by the waiting placeholder.
    scenes.setCurrentItemLocked(0, true);
    undo.undo();
    QCoreApplication::processEvents();
    QVERIFY2(preview.cachedComposedFrame().pixelColor(centre) == QColor(255, 0, 0),
             "undo must not blank a live capture in the recorded output");
}

void MalloyModelTests::theReplayBufferIsAFrameConsumer() {
    // The pure rule: with the picture moving and only the replay buffer
    // interested, composition must still happen.
    QVERIFY(PreviewWidget::compositionRequired(false, false, /*previewVisible=*/false,
                                               /*contentAdvanced=*/true, /*replayActive=*/true));
    QVERIFY(!PreviewWidget::compositionRequired(false, false, false, true, false));
    QVERIFY(!PreviewWidget::compositionRequired(false, false, false, /*contentAdvanced=*/false, true));

    // The widget: not shown, not recording, not streaming, which is the state
    // of a minimized window or another workspace. With the buffer on it still
    // wants frames, and asks for them.
    SceneCollection scenes;
    scenes.ensureCurrentScene();
    PreviewWidget preview(&scenes, PreviewWidget::Role::Program);
    bool wanted = false;
    QObject::connect(&preview, &PreviewWidget::consumerDemandChanged, &preview,
                     [&wanted](bool w) { wanted = w; });
    QVERIFY(!preview.consumersPresent());
    preview.setReplayBufferSeconds(30);
    QVERIFY2(preview.consumersPresent(), "a replay buffer needs frames with the window hidden");
    QVERIFY(wanted);
    preview.setReplayBufferSeconds(0);
    QVERIFY(!preview.consumersPresent());
    QVERIFY(!wanted);
}

void MalloyModelTests::clipsLongerThanTheTimelineArePlacedNotAborted() {
    constexpr double len = 360.0;

    // A 30 minute recording dropped anywhere. The placement code used to pass
    // qBound a maximum of len - dur, negative here, and qBound asserts when its
    // maximum is below its minimum: the application aborted.
    TimelinePlacement p = placeClip(120.0, 1800.0, len);
    QCOMPARE(p.dur, len);
    QCOMPARE(p.start, 0.0);
    QVERIFY(p.start + p.dur <= len);

    // Exactly the timeline's length fits only at the start.
    p = placeClip(50.0, len, len);
    QCOMPARE(p.start, 0.0);
    QCOMPARE(p.dur, len);

    // An ordinary clip keeps its length and is kept inside the timeline.
    p = placeClip(350.0, 30.0, len);
    QCOMPARE(p.dur, 30.0);
    QCOMPARE(p.start, 330.0);
    p = placeClip(-5.0, 30.0, len);
    QCOMPARE(p.start, 0.0);
    p = placeClip(100.0, 30.0, len);
    QCOMPARE(p.start, 100.0);

    // Nonsense in, something placeable out rather than a negative span.
    p = placeClip(10.0, -4.0, len);
    QCOMPARE(p.dur, 0.0);
    QVERIFY(p.start >= 0.0 && p.start <= len);
}

void MalloyModelTests::spinBoxesAndTextFieldsKeepTheirDigits() {
    // The application-wide filter switches workspace on a bare digit unless the
    // focused widget is being typed into. Only QLineEdit used to count, and a
    // spin box is the focus proxy of its own line edit, so digits typed into any
    // spin box were eaten and switched the workspace instead.
    QSpinBox spin;
    QDoubleSpinBox doubleSpin;
    QLineEdit line;
    QPlainTextEdit plain;
    QTextEdit rich;
    QKeySequenceEdit keys;
    QComboBox editableCombo;
    editableCombo.setEditable(true);
    QVERIFY2(isTakingTypedInput(&spin), "a focused spin box is being typed into");
    QVERIFY(isTakingTypedInput(&doubleSpin));
    QVERIFY(isTakingTypedInput(&line));
    QVERIFY(isTakingTypedInput(&plain));
    QVERIFY(isTakingTypedInput(&rich));
    QVERIFY2(isTakingTypedInput(&keys), "a shortcut editor must be able to record a digit");
    QVERIFY(isTakingTypedInput(&editableCombo));

    // Where a digit should still switch workspace.
    QComboBox fixedCombo;
    QPushButton button;
    QWidget plainWidget;
    QVERIFY(!isTakingTypedInput(&fixedCombo));
    QVERIFY(!isTakingTypedInput(&button));
    QVERIFY(!isTakingTypedInput(&plainWidget));
    QVERIFY(!isTakingTypedInput(nullptr));
}

void MalloyModelTests::restoredRenderJobsAreValidatedLikeEnqueuedOnes() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QJsonArray timeline;
    timeline.append(makeClip(dir.filePath(QStringLiteral("a.mp4")), 0.0, 2.0));

    // The rule enqueue applies, stated once so both callers can be held to it.
    QVERIFY(RenderQueue::whyNotRunnable(timeline,
                                        dir.filePath(QStringLiteral("out.mp4"))).isEmpty());

    // An empty timeline renders nothing.
    QVERIFY(!RenderQueue::whyNotRunnable(QJsonArray(),
                                         dir.filePath(QStringLiteral("out.mp4"))).isEmpty());
    // No output file.
    QVERIFY(!RenderQueue::whyNotRunnable(timeline, QString()).isEmpty());
    // A folder is not a file to write.
    QVERIFY(!RenderQueue::whyNotRunnable(timeline, dir.path()).isEmpty());
    // A folder that does not exist: caught before ffmpeg is spawned, which is
    // the case the restored path used to miss entirely.
    const QString missing = RenderQueue::whyNotRunnable(
        timeline, dir.filePath(QStringLiteral("nope/deeper/out.mp4")));
    QVERIFY(!missing.isEmpty());
    QVERIFY(missing.contains(QStringLiteral("does not exist")));
}

void MalloyModelTests::restoredRenderJobsMustWriteToALocalDrive() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString store = dir.filePath(QStringLiteral("rq.json"));

    // Pending jobs as anything able to write the store could leave them. The
    // host is this machine, so that if the check were missing the probe it
    // would make stays here.
    const QStringList refused = {
        QStringLiteral("\\\\127.0.0.1\\no-such-share\\x.mp4"),
        QStringLiteral("//127.0.0.1/no-such-share/x.mp4"),
        QStringLiteral("relative/x.mp4"),
        QStringLiteral("file:///C:/x.mp4"),
    };
    const RenderRequest valid = makeRenderRequest(dir.filePath(QStringLiteral("ok.mp4")));
    {
        QJsonArray arr;
        for (int i = 0; i < refused.size(); ++i) {
            QJsonObject o;
            o.insert(QStringLiteral("id"), QStringLiteral("job-%1").arg(i));
            o.insert(QStringLiteral("state"), 0);   // Pending
            o.insert(QStringLiteral("outputPath"), refused[i]);
            o.insert(QStringLiteral("timeline"), valid.timeline);
            o.insert(QStringLiteral("output"), valid.output.toJson());
            arr.append(o);
        }
        QFile f(store);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(arr).toJson());
    }

    RenderQueue q;
    q.setStorePath(store);
    QCOMPARE(q.jobs().size(), refused.size());
    QCOMPARE(q.countOfState(RenderJob::Failed), refused.size());
    // Refused for what it names, not for whatever the filesystem said when
    // asked about it: a missing folder is the answer the old check gave, and
    // getting it meant the path had already been probed.
    for (const RenderJob& j : q.jobs())
        QVERIFY2(j.error.contains(QStringLiteral("local drive")), qPrintable(j.error));
}

void MalloyModelTests::timelineGraphBoundsNumbersBeforeArithmetic() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString media = makeMediaFile(dir.filePath(QStringLiteral("a.mp4")));
    QVERIFY(!media.isEmpty());

    OutputSettings out;
    out.width = 1920; out.height = 1080; out.fps = 60;

    // These numbers come out of a project file and feed arithmetic that is
    // undefined rather than merely wrong at the extremes: scale reaches
    // std::lround and then a narrowing to int, start reaches llround. A
    // duration bounded only below also becomes the -t handed to the encoder, so
    // an absurd one is a render that never ends and a disk that fills.
    const auto refused = [&](const char* what, const QJsonObject& clip) {
        QJsonArray timeline;
        timeline.append(clip);
        const RenderGraph g = TimelineGraphBuilder::build(timeline, out);
        QVERIFY2(!g.ok, what);
        QVERIFY2(!g.error.isEmpty(), what);
    };

    const double huge = 1e300;
    const double nan  = std::numeric_limits<double>::quiet_NaN();
    const double inf  = std::numeric_limits<double>::infinity();

    QJsonObject c = makeClip(media, 0.0, 5.0);
    c.insert(QStringLiteral("transform"), QJsonObject{{QStringLiteral("scale"), huge}});
    refused("a scale of 1e300 must be refused before it reaches lround", c);

    c = makeClip(media, 0.0, 5.0);
    c.insert(QStringLiteral("transform"), QJsonObject{{QStringLiteral("scale"), nan}});
    refused("a scale of NaN must be refused", c);

    refused("an infinite duration must be refused", makeClip(media, 0.0, inf));
    refused("an absurd duration must be refused", makeClip(media, 0.0, huge));
    refused("an absurd start must be refused", makeClip(media, huge, 5.0));
    refused("a negative start must be refused", makeClip(media, -1.0, 5.0));
    refused("a NaN start must be refused", makeClip(media, nan, 5.0));

    c = makeClip(media, 0.0, 5.0);
    c.insert(QStringLiteral("transform"), QJsonObject{{QStringLiteral("opacity"), 5000}});
    refused("an opacity outside 0 to 100 must be refused", c);

    c = makeClip(media, 0.0, 5.0, 0.0, /*audio=*/true);
    c.insert(QStringLiteral("audioParams"), QJsonObject{{QStringLiteral("gainDb"), 100000}});
    refused("an absurd gain must be refused", c);

    // The ordinary case still builds, so the bounds are not so tight that they
    // refuse real timelines.
    QJsonArray fine;
    fine.append(makeClip(media, 4.0, 6.0, 12.5));
    const RenderGraph g = TimelineGraphBuilder::build(fine, out);
    QVERIFY2(g.ok, qPrintable(g.error));
}

void MalloyModelTests::timelineGraphPlacesTrimsAndScalesClips() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString media = makeMediaFile(dir.filePath(QStringLiteral("a.mp4")));
    QVERIFY(!media.isEmpty());

    OutputSettings out;
    out.width = 1920; out.height = 1080; out.fps = 60;

    // A clip at timeline 4s, 6s long, starting 12.5s into its source.
    QJsonArray timeline;
    timeline.append(makeClip(media, 4.0, 6.0, 12.5));
    const RenderGraph g = TimelineGraphBuilder::build(timeline, out);
    QVERIFY2(g.ok, qPrintable(g.error));
    QCOMPARE(g.durationSecs, 10.0);          // start + dur
    QCOMPARE(g.videoClips, 1);
    QCOMPARE(g.audioClips, 0);
    QVERIFY(!g.hasAudio);

    // The source segment is [sourceIn, sourceIn + dur * speed).
    QVERIFY(g.filterGraph.contains(QStringLiteral("trim=start=12.500000:end=18.500000")));
    // Placement puts the clip at its timeline start.
    QVERIFY(g.filterGraph.contains(QStringLiteral("+4.000000/TB")));
    // Background fixes geometry and length, so gaps stay black.
    QVERIFY(g.filterGraph.contains(QStringLiteral("color=c=black:s=1920x1080:r=60")));
    QVERIFY(g.filterGraph.contains(QStringLiteral("scale=1920:1080")));
    QVERIFY(g.filterGraph.contains(QStringLiteral("[vout]")));
    QVERIFY(g.outputArgs.contains(QStringLiteral("-map")));
    QVERIFY(g.outputArgs.contains(QStringLiteral("[vout]")));
    QVERIFY(!g.outputArgs.contains(QStringLiteral("[aout]")));

    // Double speed consumes twice the source over the same timeline span.
    QJsonObject fast = makeClip(media, 0.0, 5.0, 0.0);
    QJsonObject speed;
    speed.insert(QStringLiteral("factor"), 2.0);
    fast.insert(QStringLiteral("speed"), speed);
    QJsonArray fastTimeline;
    fastTimeline.append(fast);
    const RenderGraph fg = TimelineGraphBuilder::build(fastTimeline, out);
    QVERIFY2(fg.ok, qPrintable(fg.error));
    QVERIFY(fg.filterGraph.contains(QStringLiteral("trim=start=0.000000:end=10.000000")));
    QVERIFY(fg.filterGraph.contains(QStringLiteral("/2.000000+")));

    // Half scale renders at half the output geometry.
    QJsonObject small = makeClip(media, 0.0, 5.0, 0.0);
    QJsonObject xf;
    xf.insert(QStringLiteral("scale"), 50.0);
    small.insert(QStringLiteral("transform"), xf);
    QJsonArray smallTimeline;
    smallTimeline.append(small);
    const RenderGraph sg = TimelineGraphBuilder::build(smallTimeline, out);
    QVERIFY2(sg.ok, qPrintable(sg.error));
    QVERIFY(sg.filterGraph.contains(QStringLiteral("scale=960:540")));
}

void MalloyModelTests::timelineGraphMixesAudioAndKeepsPathsOutOfTheGraph() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // A file name carrying filter-graph metacharacters. If the builder ever
    // interpolated paths into the graph, this is what would break it, so the
    // test pins the rule that paths are arguments only.
    const QString video = makeMediaFile(dir.filePath(QStringLiteral("clip's [1]; odd.mp4")));
    const QString audioA = makeMediaFile(dir.filePath(QStringLiteral("music.wav")));
    const QString audioB = makeMediaFile(dir.filePath(QStringLiteral("voice.wav")));
    QVERIFY(!video.isEmpty() && !audioA.isEmpty() && !audioB.isEmpty());

    QJsonArray timeline;
    timeline.append(makeClip(video, 0.0, 8.0));
    timeline.append(makeClip(audioA, 0.0, 8.0, 0.0, true));
    timeline.append(makeClip(audioB, 2.0, 4.0, 1.0, true));

    const RenderGraph g = TimelineGraphBuilder::build(timeline, OutputSettings{});
    QVERIFY2(g.ok, qPrintable(g.error));
    QCOMPARE(g.videoClips, 1);
    QCOMPARE(g.audioClips, 2);
    QVERIFY(g.hasAudio);
    QVERIFY(g.filterGraph.contains(QStringLiteral("amix=inputs=2")));
    // The delayed clip is placed with adelay, in milliseconds.
    QVERIFY(g.filterGraph.contains(QStringLiteral("adelay=2000:all=1")));
    QVERIFY(g.outputArgs.contains(QStringLiteral("[aout]")));

    // Every media path is an argument, and none of them appear in the graph.
    QVERIFY(g.inputArgs.contains(video));
    QVERIFY(g.inputArgs.contains(audioA));
    QVERIFY(g.inputArgs.contains(audioB));
    QCOMPARE(g.inputArgs.count(QStringLiteral("-i")), 3);
    QVERIFY(!g.filterGraph.contains(video));
    QVERIFY(!g.filterGraph.contains(audioA));
    QVERIFY(!g.filterGraph.contains(QStringLiteral(".mp4")));
    QVERIFY(!g.filterGraph.contains(QStringLiteral(".wav")));
}

void MalloyModelTests::timelineGraphRefusesWhatItCannotRender() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString media = makeMediaFile(dir.filePath(QStringLiteral("a.mp4")));
    const OutputSettings out;

    // Empty timeline.
    QVERIFY(!TimelineGraphBuilder::build(QJsonArray{}, out).ok);

    // Unlinked clip: named in the error so a long timeline is actionable.
    {
        QJsonObject c = makeClip(media, 0.0, 4.0);
        c.insert(QStringLiteral("sourcePath"), QString());
        c.insert(QStringLiteral("label"), QStringLiteral("Intro card"));
        QJsonArray t;
        t.append(c);
        const RenderGraph g = TimelineGraphBuilder::build(t, out);
        QVERIFY(!g.ok);
        QVERIFY(g.error.contains(QStringLiteral("Intro card")));
    }

    // Source file that is gone.
    {
        QJsonArray t;
        t.append(makeClip(dir.filePath(QStringLiteral("ghost.mp4")), 0.0, 4.0));
        const RenderGraph g = TimelineGraphBuilder::build(t, out);
        QVERIFY(!g.ok);
        QVERIFY(g.error.contains(QStringLiteral("missing")));
    }

    // Audio-only timeline: there is no picture to render.
    {
        QJsonArray t;
        t.append(makeClip(media, 0.0, 4.0, 0.0, true));
        QVERIFY(!TimelineGraphBuilder::build(t, out).ok);
    }

    // Parameters this slice does not implement are refused rather than ignored,
    // so nothing renders silently wrong.
    auto withTransform = [&](const QString& key, double value) {
        QJsonObject c = makeClip(media, 0.0, 4.0);
        QJsonObject xf;
        xf.insert(key, value);
        c.insert(QStringLiteral("transform"), xf);
        QJsonArray t;
        t.append(c);
        return TimelineGraphBuilder::build(t, out);
    };
    QVERIFY(!withTransform(QStringLiteral("rotation"), 90.0).ok);

    auto withAudioParam = [&](const QString& key, int value) {
        QJsonObject c = makeClip(media, 0.0, 4.0, 0.0, true);
        QJsonObject ap;
        ap.insert(key, value);
        c.insert(QStringLiteral("audioParams"), ap);
        QJsonArray t;
        t.append(makeClip(media, 0.0, 4.0));
        t.append(c);
        return TimelineGraphBuilder::build(t, out);
    };
    QVERIFY(!withAudioParam(QStringLiteral("pan"), 50).ok);
    QVERIFY(!withAudioParam(QStringLiteral("channels"), 1).ok);

    // Opacity is implemented, so it must not be refused.
    QVERIFY(withTransform(QStringLiteral("opacity"), 50.0).ok);

    // Too many clips: refused with a number rather than handed to ffmpeg.
    {
        QJsonArray t;
        for (int i = 0; i < TimelineGraphBuilder::kMaxInputs + 1; ++i)
            t.append(makeClip(media, double(i), 1.0));
        const RenderGraph g = TimelineGraphBuilder::build(t, out);
        QVERIFY(!g.ok);
        QVERIFY(g.error.contains(QString::number(TimelineGraphBuilder::kMaxInputs)));
    }
}

void MalloyModelTests::blurHandlesImagesSmallerThanItsRadius() {
    BlurFilter blur;
    blur.setRadius(32);
    QCOMPARE(blur.radius(), 32);

    // A uniform image must survive any blur unchanged: every window, however
    // large, averages the same colour. With the seed reading past the buffer
    // this picked up whatever followed the allocation instead.
    QImage uniform(3, 3, QImage::Format_ARGB32);
    uniform.fill(qRgba(20, 140, 220, 255));
    blur.apply(uniform);
    for (int y = 0; y < uniform.height(); ++y) {
        for (int x = 0; x < uniform.width(); ++x) {
            const QRgb px = uniform.pixel(x, y);
            QCOMPARE(qRed(px), 20);
            QCOMPARE(qGreen(px), 140);
            QCOMPARE(qBlue(px), 220);
            QCOMPARE(qAlpha(px), 255);
        }
    }

    // A single pixel is the smallest case the radius can overrun.
    QImage one(1, 1, QImage::Format_ARGB32);
    one.fill(qRgba(10, 20, 30, 255));
    blur.apply(one);
    QCOMPARE(qRed(one.pixel(0, 0)), 10);
    QCOMPARE(qGreen(one.pixel(0, 0)), 20);
    QCOMPARE(qBlue(one.pixel(0, 0)), 30);

    // A two-tone image stays inside the range of its inputs: replicate padding
    // can only ever average the colours that are actually there.
    QImage pair(2, 1, QImage::Format_ARGB32);
    pair.setPixel(0, 0, qRgba(0, 0, 0, 255));
    pair.setPixel(1, 0, qRgba(200, 200, 200, 255));
    blur.apply(pair);
    for (int x = 0; x < 2; ++x) {
        const QRgb px = pair.pixel(x, 0);
        QVERIFY(qRed(px) >= 0 && qRed(px) <= 200);
        QVERIFY(qGreen(px) >= 0 && qGreen(px) <= 200);
        QVERIFY(qBlue(px) >= 0 && qBlue(px) <= 200);
    }
    // The darker pixel stays the darker of the two.
    QVERIFY(qRed(pair.pixel(0, 0)) < qRed(pair.pixel(1, 0)));

    // A tall, narrow image exercises the column seed rather than the row seed.
    QImage column(1, 4, QImage::Format_ARGB32);
    column.fill(qRgba(90, 90, 90, 255));
    blur.apply(column);
    for (int y = 0; y < column.height(); ++y)
        QCOMPARE(qRed(column.pixel(0, y)), 90);
}

void MalloyModelTests::mixerKeepsStereoSeparation() {
    float l = 0.0f, r = 0.0f;

    // Centre is unity on both sides: a stereo source is passed through, not
    // re-spread.
    balanceGains(0.0f, l, r);
    QCOMPARE(l, 1.0f);
    QCOMPARE(r, 1.0f);

    // Hard left mutes the right side and leaves the left alone.
    balanceGains(-1.0f, l, r);
    QCOMPARE(l, 1.0f);
    QCOMPARE(r, 0.0f);

    balanceGains(1.0f, l, r);
    QCOMPARE(l, 0.0f);
    QCOMPARE(r, 1.0f);

    // Partway is linear on the attenuated side only.
    balanceGains(-0.5f, l, r);
    QCOMPARE(l, 1.0f);
    QCOMPARE(r, 0.5f);

    // Out of range values are clamped rather than inverting the law.
    balanceGains(-4.0f, l, r);
    QCOMPARE(l, 1.0f);
    QCOMPARE(r, 0.0f);

    // A hard-panned source: everything on the left, silence on the right.
    const std::vector<int16_t> src = {1000, -2000, 3000, -4000};

    std::vector<int32_t> accum(4, 0);
    balanceGains(0.0f, l, r);
    mixStereoInto(accum, src.data(), int(src.size()), l, r);
    QCOMPARE(accum[0], 1000);
    QCOMPARE(accum[1], -2000);
    QCOMPARE(accum[2], 3000);
    QCOMPARE(accum[3], -4000);

    // The regression itself: a signal present only on the left must never
    // appear on the right, at any pan setting. The old mixer put half of it
    // there.
    const std::vector<int16_t> leftOnly = {8000, 0, 8000, 0};
    for (float pan : {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f}) {
        std::vector<int32_t> out(4, 0);
        balanceGains(pan, l, r);
        mixStereoInto(out, leftOnly.data(), int(leftOnly.size()), l, r);
        QCOMPARE(out[1], 0);
        QCOMPARE(out[3], 0);
    }

    // Mixing two inputs accumulates per channel without touching the other one.
    std::vector<int32_t> two(2, 0);
    const std::vector<int16_t> a = {100, 200};
    const std::vector<int16_t> b = {400, 800};
    balanceGains(0.0f, l, r);
    mixStereoInto(two, a.data(), int(a.size()), l, r);
    mixStereoInto(two, b.data(), int(b.size()), l, r);
    QCOMPARE(two[0], 500);
    QCOMPARE(two[1], 1000);

    // A short buffer never writes past the accumulator.
    std::vector<int32_t> small(2, 0);
    mixStereoInto(small, src.data(), int(src.size()), 1.0f, 1.0f);
    QCOMPARE(small.size(), size_t(2));
    QCOMPARE(small[0], 1000);
    QCOMPARE(small[1], -2000);
}

void MalloyModelTests::projectDisplayNameStripsCompoundExtension() {
    QCOMPARE(ProjectDocument::displayName(QStringLiteral("C:/p/spire.malloy.json")),
             QStringLiteral("spire"));
    // Case is not significant on Windows paths.
    QCOMPARE(ProjectDocument::displayName(QStringLiteral("C:/p/Spire.MALLOY.JSON")),
             QStringLiteral("Spire"));
    // A name that merely contains "malloy" keeps it.
    QCOMPARE(ProjectDocument::displayName(QStringLiteral("C:/p/malloy-notes.malloy.json")),
             QStringLiteral("malloy-notes"));
    // Dots inside the name survive.
    QCOMPARE(ProjectDocument::displayName(QStringLiteral("C:/p/ep.14.final.malloy.json")),
             QStringLiteral("ep.14.final"));
    // Other extensions fall back to the ordinary rule.
    QCOMPARE(ProjectDocument::displayName(QStringLiteral("C:/p/notes.json")),
             QStringLiteral("notes"));
    QCOMPARE(ProjectDocument::displayName(QStringLiteral("C:/p/plain")),
             QStringLiteral("plain"));
    QVERIFY(ProjectDocument::displayName(QString()).isEmpty());
}

void MalloyModelTests::pcmFifoKeepsTheSampleStreamContinuous() {
    PcmFifo fifo;
    QVERIFY(fifo.isEmpty());
    QCOMPARE(fifo.available(), 0);

    // A short read on an empty buffer is an underrun, not a crash.
    char out[16] = {};
    QCOMPARE(fifo.take(out, sizeof(out)), 0);

    // Leftovers survive: pushing 100 bytes and taking 60 leaves 40.
    std::vector<char> hundred(100, 'x');
    fifo.push(hundred.data(), int(hundred.size()));
    QCOMPARE(fifo.available(), 100);
    std::vector<char> sixty(60, 0);
    QCOMPARE(fifo.take(sixty.data(), 60), 60);
    QCOMPARE(fifo.available(), 40);

    // Taking more than is buffered returns what there is and empties it.
    std::vector<char> big(200, 0);
    QCOMPARE(fifo.take(big.data(), 200), 40);
    QVERIFY(fifo.isEmpty());

    // The heart of the bug: a ramp pushed in chunk sizes that do not divide the
    // tick size must come back out byte for byte, with nothing dropped or
    // repeated at the boundaries.
    fifo.clear();
    const int kTotal = 5000;
    std::vector<char> written;
    written.reserve(kTotal);
    for (int i = 0; i < kTotal; ++i)
        written.push_back(static_cast<char>(i % 251));   // 251 is coprime with the chunk sizes

    std::vector<char> read;
    read.reserve(kTotal);
    const int pushSizes[] = {700, 1100, 250, 1950, 1000};   // none is a multiple of 480
    int written_off = 0;
    for (int size : pushSizes) {
        fifo.push(written.data() + written_off, size);
        written_off += size;
        // Drain in tick-sized bites, exactly as the mixer does.
        for (;;) {
            std::vector<char> tick(480, 0);
            const int got = fifo.take(tick.data(), 480);
            if (got <= 0) break;
            read.insert(read.end(), tick.begin(), tick.begin() + got);
            if (got < 480) break;   // underrun: wait for the next push
        }
    }
    QCOMPARE(written_off, kTotal);
    QCOMPARE(int(read.size()), kTotal);
    QVERIFY(read == written);

    // Overrun policy: when the buffer runs long the OLDEST bytes go, so the mix
    // stays near live instead of playing a backlog.
    fifo.clear();
    std::vector<char> a(100, 'a'), b(100, 'b');
    fifo.push(a.data(), 100);
    fifo.push(b.data(), 100);
    fifo.trimToLast(100);
    QCOMPARE(fifo.available(), 100);
    std::vector<char> kept(100, 0);
    QCOMPARE(fifo.take(kept.data(), 100), 100);
    QCOMPARE(kept, b);

    // Trimming to more than is held is a no-op.
    fifo.clear();
    fifo.push(a.data(), 100);
    fifo.trimToLast(1000);
    QCOMPARE(fifo.available(), 100);

    // Muting clears the buffer rather than leaving stale audio to play later.
    fifo.clear();
    QVERIFY(fifo.isEmpty());
}

// Counts positive-going zero crossings on the left channel, which is a cheap
// proxy for the frequency of a clean tone.
static int countRisingZeroCrossings(const std::vector<int16_t>& pcm) {
    int crossings = 0;
    for (size_t f = 1; f * 2 < pcm.size(); ++f) {
        const int16_t prev = pcm[(f - 1) * 2];
        const int16_t cur  = pcm[f * 2];
        if (prev <= 0 && cur > 0) ++crossings;
    }
    return crossings;
}

// Interleaved stereo sine of `freq` Hz at `rate`, `seconds` long.
static std::vector<int16_t> makeTone(double freq, int rate, double seconds, double amp = 12000.0) {
    const int frames = int(rate * seconds);
    std::vector<int16_t> pcm;
    pcm.reserve(size_t(frames) * 2);
    for (int f = 0; f < frames; ++f) {
        const double t = double(f) / double(rate);
        const auto v = int16_t(std::lround(amp * std::sin(2.0 * 3.14159265358979323846 * freq * t)));
        pcm.push_back(v);
        pcm.push_back(v);
    }
    return pcm;
}

void MalloyModelTests::resamplerPreservesPitchAcrossRates() {
    // Matching rates are a straight copy: no interpolation, no drift, no cost.
    {
        StereoResampler same(48000, 48000);
        QVERIFY(!same.active());
        const std::vector<int16_t> in = {1, 2, 3, 4, 5, 6};
        std::vector<int16_t> out;
        same.process(in.data(), 3, out);
        QCOMPARE(out, in);
    }

    // 44.1 kHz to 48 kHz: one second of input must yield about one second of
    // output, and the tone must still be the same tone. Before the fix the
    // samples were passed through untouched, so a second of 44.1 kHz audio was
    // played as 0.92 seconds and every frequency rose with it.
    {
        StereoResampler up(44100, 48000);
        QVERIFY(up.active());
        const std::vector<int16_t> tone = makeTone(1000.0, 44100, 1.0);
        std::vector<int16_t> out;
        // Feed it in device-sized packets, so the fractional position and the
        // history have to carry across calls.
        const int packet = 441;
        for (int f = 0; f + packet <= int(tone.size() / 2); f += packet)
            up.process(tone.data() + size_t(f) * 2, packet, out);

        const int outFrames = int(out.size() / 2);
        // Within a few frames of one second at the output rate.
        QVERIFY2(std::abs(outFrames - 48000) < 200,
                 qPrintable(QStringLiteral("got %1 frames").arg(outFrames)));
        // A 1 kHz tone still crosses zero about 1000 times per second.
        const int crossings = countRisingZeroCrossings(out);
        QVERIFY2(std::abs(crossings - 1000) <= 5,
                 qPrintable(QStringLiteral("got %1 crossings").arg(crossings)));
    }

    // 96 kHz to 48 kHz, the other direction.
    {
        StereoResampler down(96000, 48000);
        const std::vector<int16_t> tone = makeTone(1000.0, 96000, 1.0);
        std::vector<int16_t> out;
        down.process(tone.data(), int(tone.size() / 2), out);
        const int outFrames = int(out.size() / 2);
        QVERIFY2(std::abs(outFrames - 48000) < 200,
                 qPrintable(QStringLiteral("got %1 frames").arg(outFrames)));
        const int crossings = countRisingZeroCrossings(out);
        QVERIFY2(std::abs(crossings - 1000) <= 5,
                 qPrintable(QStringLiteral("got %1 crossings").arg(crossings)));
    }

    // Content above the output Nyquist is filtered out rather than folded back
    // into the audible band as a false low tone.
    {
        StereoResampler down(96000, 48000);
        const std::vector<int16_t> ultrasonic = makeTone(36000.0, 96000, 0.25);
        std::vector<int16_t> out;
        down.process(ultrasonic.data(), int(ultrasonic.size() / 2), out);
        QVERIFY(!out.empty());

        double sum = 0.0;
        for (size_t i = 0; i < out.size(); i += 2)
            sum += double(out[i]) * double(out[i]);
        const double rms = std::sqrt(sum / double(out.size() / 2));
        // The input is 12000 peak, about 8485 RMS. Anything close to that means
        // the tone aliased through instead of being rejected.
        QVERIFY2(rms < 1500.0, qPrintable(QStringLiteral("alias rms %1").arg(rms)));
    }

    // Silence in, silence out: no ringing, no DC offset.
    {
        StereoResampler up(44100, 48000);
        const std::vector<int16_t> quiet(4410 * 2, 0);
        std::vector<int16_t> out;
        up.process(quiet.data(), 4410, out);
        for (int16_t v : out) QCOMPARE(v, int16_t(0));
    }
}

void MalloyModelTests::hotkeyManagerReportsRefusedBindings() {
    // A refusal reaches setBinding() the same way whichever cause produced it:
    // another application already owning the combination, or a key the Win32
    // mapping does not support. The second is the one a test can stage
    // deterministically, and it runs through the identical path.
    //
    // A same-thread duplicate registration is NOT a conflict: Windows 11 lets
    // one process register the same combination twice, so it cannot be used to
    // stage this.
    const QKeySequence unsupported(Qt::ControlModifier | Qt::AltModifier | Qt::Key_F22);
    const QKeySequence spare(Qt::ControlModifier | Qt::AltModifier
                             | Qt::ShiftModifier | Qt::Key_F10);
    const QString action = QStringLiteral("test.refused");

    HotkeyManager mgr;
    if (!mgr.setBinding(action, spare))
        QSKIP("Could not register the fallback shortcut on this machine.");
    QCOMPARE(mgr.binding(action), spare);

    QSignalSpy refusedSpy(&mgr, &HotkeyManager::bindingFailed);
    QVERIFY(!mgr.setBinding(action, unsupported));
    QCOMPARE(refusedSpy.count(), 1);
    QCOMPARE(refusedSpy.first().at(1).value<QKeySequence>(), unsupported);

    // The refused shortcut is not reported as bound, so the dialog cannot show
    // it as working, and the binding that did work is kept rather than lost.
    QVERIFY(mgr.binding(action) != unsupported);
    QCOMPARE(mgr.binding(action), spare);

    // Nothing unusable was persisted, so it will not be retried every launch.
    {
        const QSettings s;
        QCOMPARE(QKeySequence(s.value(QStringLiteral("hotkeys/") + action).toString()), spare);
    }

    QVERIFY(mgr.setBinding(action, QKeySequence()));
}

void MalloyModelTests::addingAConfiguredLayerIsOneUndoStep() {
    SceneCollection scenes;
    QUndoStack undo;
    scenes.setUndoStack(&undo);
    scenes.addScene(QStringLiteral("Scene"));
    undo.clear();

    // What SourcesPanel does for an image layer: create it, then set the path.
    scenes.beginEditGroup(QStringLiteral("Add Image"));
    scenes.addNewSourceToCurrent(QStringLiteral("Backdrop"), Source::Type::Image,
                                 QString(), QColor());
    const int idx = scenes.currentItemIndex();
    QVERIFY(idx >= 0);
    scenes.setCurrentSourceImagePath(idx, QStringLiteral("C:/pics/bg.png"));
    scenes.endEditGroup();

    QCOMPARE(scenes.currentScene()->itemCount(), 1);
    QCOMPARE(scenes.sourceForCurrentItem(0)->imagePath(), QStringLiteral("C:/pics/bg.png"));

    // One action in, one undo step out.
    QCOMPARE(undo.count(), 1);
    undo.undo();
    QCOMPARE(scenes.currentScene()->itemCount(), 0);

    // And redo brings back the configured layer, not a bare one.
    undo.redo();
    QCOMPARE(scenes.currentScene()->itemCount(), 1);
    QCOMPARE(scenes.sourceForCurrentItem(0)->imagePath(), QStringLiteral("C:/pics/bg.png"));

    // Ungrouped edits still record separately, which is what the group exists
    // to override.
    undo.clear();
    scenes.addNewSourceToCurrent(QStringLiteral("Second"), Source::Type::Image,
                                 QString(), QColor());
    const int second = scenes.currentItemIndex();
    scenes.setCurrentSourceImagePath(second, QStringLiteral("C:/pics/other.png"));
    QCOMPARE(undo.count(), 2);
}

void MalloyModelTests::encoderRedactsTheStreamKeyFromFfmpegOutput() {
    const QString url = QStringLiteral("rtmp://live.twitch.tv/app/live_999999_SECRETKEYVALUE");
    const QString key = QStringLiteral("live_999999_SECRETKEYVALUE");

    // What ffmpeg actually prints on a failed connect, twice, at -loglevel error.
    const QString stderrText =
        QStringLiteral("[flv @ 000001] Error opening output %1: Input/output error\n"
                       "Error opening output file %1.\n").arg(url);

    const QString safe = EncoderPipeline::redactDestination(stderrText, url);
    QVERIFY(!safe.contains(key));
    // The server stays visible: an error naming no destination is not useful.
    QVERIFY(safe.contains(QStringLiteral("rtmp://live.twitch.tv/app/")));
    QVERIFY(safe.contains(QStringLiteral("***")));
    QVERIFY(safe.contains(QStringLiteral("Input/output error")));

    // The key on its own, without the URL around it, is redacted too.
    const QString bare = EncoderPipeline::redactDestination(
        QStringLiteral("publishing to %1 failed").arg(key), url);
    QVERIFY(!bare.contains(key));

    // No destination means nothing to redact.
    QCOMPARE(EncoderPipeline::redactDestination(stderrText, QString()), stderrText);

    // A recording target is a file path, and its name is not a secret. The
    // caller passes an empty destination for file targets, so this is what a
    // recording error keeps.
    const QString fileErr = QStringLiteral("Error opening output file C:/Videos/take-3.mp4.");
    QCOMPARE(EncoderPipeline::redactDestination(fileErr, QString()), fileErr);

    // A short trailing segment is not a credential; replacing it blindly would
    // mangle ordinary words.
    const QString shortUrl = QStringLiteral("rtmp://example.com/live/ab");
    const QString text = QStringLiteral("ab is a common fragment, cabbage included");
    QVERIFY(EncoderPipeline::redactDestination(text, shortUrl).contains(QStringLiteral("cabbage")));
}

void MalloyModelTests::projectMediaPathsMustBeLocalFiles() {
    // Ordinary local media, in both slash forms.
    QVERIFY(MediaPathPolicy::isAllowed(QStringLiteral("C:\\Users\\me\\Videos\\clip.mp4")));
    QVERIFY(MediaPathPolicy::isAllowed(QStringLiteral("C:/Users/me/Videos/clip.mp4")));
    QVERIFY(MediaPathPolicy::isAllowed(QStringLiteral("e:/MalloyStudio/logo.png")));

    // UNC, which is the case that matters. Answering any question about one of
    // these, including whether it exists, makes Windows authenticate outbound
    // to a host the project chose, handing over an NTLMv2 response.
    QVERIFY(!MediaPathPolicy::isAllowed(QStringLiteral("\\\\attacker.example.com\\s\\x.png")));
    QVERIFY(!MediaPathPolicy::isAllowed(QStringLiteral("//attacker.example.com/s/x.png")));
    QVERIFY(!MediaPathPolicy::isAllowed(QStringLiteral("\\\\?\\C:\\Users\\me\\clip.mp4")));
    QVERIFY(!MediaPathPolicy::isAllowed(QStringLiteral("C:\\\\attacker\\share\\x.png")));

    // Protocol strings, which ffmpeg would read as an input of its own choosing
    // rather than as a file. These used to be stopped only by the accident that
    // QFileInfo::exists() is false for them.
    for (const QString& hostile : {QStringLiteral("http://attacker.example.com/x.mp4"),
                                   QStringLiteral("https://attacker.example.com/x.mp4"),
                                   QStringLiteral("concat:a.mp4|b.mp4"),
                                   QStringLiteral("tee:out.mp4"),
                                   QStringLiteral("file:C:/x.mp4"),
                                   QStringLiteral("data:text/plain,hello")}) {
        QVERIFY2(!MediaPathPolicy::isAllowed(hostile),
                 qPrintable(QStringLiteral("must refuse %1").arg(hostile)));
    }

    // Relative paths resolve against whatever the working directory happens to
    // be, which the project does not get to decide.
    QVERIFY(!MediaPathPolicy::isAllowed(QStringLiteral("clip.mp4")));
    QVERIFY(!MediaPathPolicy::isAllowed(QStringLiteral("..\\..\\secrets\\id_rsa")));
    QVERIFY(!MediaPathPolicy::isAllowed(QStringLiteral("/etc/passwd")));
    QVERIFY(!MediaPathPolicy::isAllowed(QString()));
    QVERIFY(!MediaPathPolicy::isAllowed(QStringLiteral("C:")));

    // And the boundary the policy actually defends: a UNC image path in a
    // project is dropped at load, so nothing downstream can reach it.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("unc.malloy.json"));
    {
        SceneCollection authored;
        authored.ensureCurrentScene();
        SceneItem* item = authored.addNewSourceToCurrent(QStringLiteral("Logo"),
                                                         Source::Type::Image);
        QVERIFY(item != nullptr);
        Source* src = authored.sourceById(item->sourceId());
        QVERIFY(src != nullptr);
        src->setImagePath(QStringLiteral("\\\\attacker.example.com\\share\\logo.png"));
        QVERIFY(ProjectDocument::saveToFile(authored, path));
    }

    SceneCollection opened;
    QString error;
    QVERIFY2(ProjectDocument::loadFromFile(opened, path, &error), qPrintable(error));
    Source* loaded = nullptr;
    for (Source* s : opened.sources()) {
        if (s && s->type() == Source::Type::Image) { loaded = s; break; }
    }
    QVERIFY(loaded != nullptr);
    QVERIFY2(loaded->imagePath().isEmpty(),
             "a UNC image path must not survive the load into the model");
}

void MalloyModelTests::fileDevicesAreHeldBeforeTheLoadIsAnnounced() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("announced.malloy.json"));
    {
        SceneCollection authored;
        authored.ensureCurrentScene();
        QVERIFY(authored.addCameraToCurrent(QStringLiteral("Front Camera"),
                                            QStringLiteral("\\\\?\\usb#vid_dead&pid_beef"),
                                            QStringLiteral("Front Camera")) != nullptr);
        QVERIFY(authored.addNewSourceToCurrent(QStringLiteral("Screen"),
                                               Source::Type::DisplayCapture) != nullptr);
        QVERIFY(ProjectDocument::saveToFile(authored, path));
    }

    // The capture reconciler is connected directly to these signals, so it runs
    // inside the emit. Whatever the hold says at that moment is what decides
    // whether the camera and the screen capture start. The hold used to be
    // applied only after the load returned, which was after these had fired.
    SceneCollection opened;
    bool announced = false;
    bool heldWhenAnnounced = true;
    const auto check = [&] {
        announced = true;
        for (const Source* s : opened.sources()) {
            if (!s) continue;
            const bool device = s->type() == Source::Type::Camera
                                || s->type() == Source::Type::AudioInput
                                || s->type() == Source::Type::DisplayCapture
                                || s->type() == Source::Type::WindowCapture;
            if (device && !opened.deviceHeld(s->id())) heldWhenAnnounced = false;
        }
    };
    QObject::connect(&opened, &SceneCollection::collectionReset, &opened, check);
    QObject::connect(&opened, &SceneCollection::sourcesChanged, &opened, check);
    QObject::connect(&opened, &SceneCollection::itemsChanged, &opened, check);

    QString error;
    QVERIFY2(ProjectDocument::loadFromFile(opened, path, &error), qPrintable(error));
    QVERIFY(announced);
    QVERIFY2(heldWhenAnnounced,
             "a file's devices must already be held when the load is first announced");
    QVERIFY(opened.deviceConsentPending());
}

void MalloyModelTests::undoInStudioModeLeavesTheProgramOnAir() {
    SceneCollection scenes;
    QUndoStack undo;
    scenes.setUndoStack(&undo);
    scenes.addScene(QStringLiteral("Live"));
    scenes.addNewSourceToCurrent(QStringLiteral("Title"), Source::Type::Text,
                                 QStringLiteral("on air"));
    scenes.addScene(QStringLiteral("Staged"));
    scenes.addNewSourceToCurrent(QStringLiteral("Title"), Source::Type::Text,
                                 QStringLiteral("not ready"));
    scenes.setCurrentIndex(0);

    scenes.setStudioMode(true);
    scenes.setCurrentIndex(1);                  // stage the unfinished scene
    QCOMPARE(scenes.programIndex(), 0);
    QCOMPARE(scenes.previewIndex(), 1);
    undo.clear();

    // An edit to the staged scene, then undo it. The restore used to reset
    // studio mode and cut the staged scene to air, silently.
    bool leftStudio = false;
    QObject::connect(&scenes, &SceneCollection::studioModeChanged, &scenes,
                     [&leftStudio](bool on) { if (!on) leftStudio = true; });
    scenes.setCurrentItemLocked(0, true);
    undo.undo();

    QVERIFY2(scenes.studioMode(), "undo must not leave studio mode");
    QVERIFY(!leftStudio);
    QCOMPARE(scenes.programIndex(), 0);
    QCOMPARE(scenes.programScene()->name(), QStringLiteral("Live"));
    QCOMPARE(scenes.previewIndex(), scenes.currentIndex());

    undo.redo();
    QVERIFY(scenes.studioMode());
    QCOMPARE(scenes.programScene()->name(), QStringLiteral("Live"));

    // A new project does leave studio mode, and says so.
    scenes.clear();
    QVERIFY(!scenes.studioMode());
    QVERIFY(leftStudio);
}

void MalloyModelTests::removingAScenePreservesWhatIsOnAir() {
    SceneCollection scenes;
    scenes.addScene(QStringLiteral("A"));
    scenes.addScene(QStringLiteral("B"));
    scenes.addScene(QStringLiteral("C"));

    // Live mode, the on-air scene removed: program must land on a real scene,
    // and on the same one the user is now looking at.
    scenes.setCurrentIndex(2);
    QCOMPARE(scenes.programIndex(), 2);
    scenes.removeSceneAt(2);
    QVERIFY2(scenes.programScene() != nullptr, "the recorded output must not point at nothing");
    QCOMPARE(scenes.programIndex(), scenes.currentIndex());
    QCOMPARE(scenes.programScene()->name(), QStringLiteral("B"));

    // Live mode, a scene below the on-air one removed: the same scene stays on
    // air, at its new index, rather than whatever slid into its old slot.
    scenes.addScene(QStringLiteral("C"));
    scenes.setCurrentIndex(1);                   // B on air
    QCOMPARE(scenes.programScene()->name(), QStringLiteral("B"));
    scenes.removeSceneAt(0);                     // remove A
    QCOMPARE(scenes.programScene()->name(), QStringLiteral("B"));

    // Studio mode: removing a staged scene leaves the program alone.
    SceneCollection studio;
    studio.addScene(QStringLiteral("A"));
    studio.addScene(QStringLiteral("B"));
    studio.addScene(QStringLiteral("C"));
    studio.setCurrentIndex(2);                   // C on air
    studio.setStudioMode(true);
    studio.setCurrentIndex(0);                   // stage A
    studio.removeSceneAt(0);
    QCOMPARE(studio.programScene()->name(), QStringLiteral("C"));
    QCOMPARE(studio.previewIndex(), studio.currentIndex());
}

void MalloyModelTests::stagingASceneKeepsTheProgramCaptureRunning() {
    FakeCaptureSession::started.clear();
    FakeCaptureSession::stopped.clear();
    FakeCaptureSession::created.clear();

    SceneCollection scenes;
    scenes.addScene(QStringLiteral("On air"));
    scenes.addNewSourceToCurrent(QStringLiteral("Display"), Source::Type::DisplayCapture,
                                 QString(), QColor(), 1, 2);
    scenes.addScene(QStringLiteral("Next"));
    scenes.addNewSourceToCurrent(QStringLiteral("Title"), Source::Type::Text,
                                 QStringLiteral("coming up"));
    scenes.setCurrentIndex(0);

    CaptureController controller(
        &scenes,
        [](int adapterIndex, int outputIndex, QObject* parent) {
            return new FakeCaptureSession(adapterIndex, outputIndex, parent);
        });
    QCOMPARE(FakeCaptureSession::started.count(QStringLiteral("1:2")), 1);

    // Studio mode, then stage a scene with no display source. The on-air scene
    // still needs its capture; reconciling against the staged scene alone used
    // to stop it, and the recording lost its screen.
    scenes.setStudioMode(true);
    scenes.setCurrentIndex(1);
    QCOMPARE(scenes.programIndex(), 0);
    QVERIFY2(FakeCaptureSession::stopped.count(QStringLiteral("1:2")) == 0,
             "staging a scene must not stop the program scene's capture");
    QCOMPARE(controller.activeSessionCount(), 1);

    // Leaving studio mode puts the current scene on air; its lack of a display
    // source now does stop the capture.
    scenes.setStudioMode(false);
    QCOMPARE(FakeCaptureSession::stopped.count(QStringLiteral("1:2")), 1);
    QCOMPARE(controller.activeSessionCount(), 0);
}

void MalloyModelTests::everyMicChangeIsAnnouncedStructurally() {
    // MainWindow reconciles microphones on these signals. Only audioInputsChanged
    // used to be connected, and most edits never emit it, so a deleted mic kept
    // its capture worker running. This pins the contract the wiring relies on:
    // each edit that changes the set of live microphones announces itself on at
    // least one of them.
    SceneCollection scenes;
    QUndoStack undo;
    scenes.setUndoStack(&undo);
    scenes.ensureCurrentScene();
    QVERIFY(scenes.addAudioInputToCurrent(QStringLiteral("Desk Mic"),
                                          QStringLiteral("{mic}")) != nullptr);
    undo.clear();
    const QStringList live{QStringLiteral("{mic}")};
    QCOMPARE(scenes.gatherVisibleAudioIds(), live);

    int announced = 0;
    const auto bump = [&announced] { ++announced; };
    QObject::connect(&scenes, &SceneCollection::audioInputsChanged, &scenes, bump);
    QObject::connect(&scenes, &SceneCollection::itemsChanged,       &scenes, bump);
    QObject::connect(&scenes, &SceneCollection::sourcesChanged,     &scenes, bump);
    QObject::connect(&scenes, &SceneCollection::collectionReset,    &scenes, bump);
    QObject::connect(&scenes, &SceneCollection::currentChanged,     &scenes, bump);
    QObject::connect(&scenes, &SceneCollection::programChanged,     &scenes, bump);
    QObject::connect(&scenes, &SceneCollection::studioModeChanged,  &scenes, bump);

    announced = 0;
    scenes.removeCurrentItemAt(0);
    QVERIFY2(announced > 0, "removing a mic layer must be announced");
    QVERIFY(scenes.gatherVisibleAudioIds().isEmpty());

    announced = 0;
    undo.undo();
    QVERIFY2(announced > 0, "undoing a removal must be announced");
    QCOMPARE(scenes.gatherVisibleAudioIds(), live);

    announced = 0;
    undo.redo();
    QVERIFY2(announced > 0, "redoing a removal must be announced");
    QVERIFY(scenes.gatherVisibleAudioIds().isEmpty());

    announced = 0;
    scenes.clear();
    QVERIFY2(announced > 0, "a new project must be announced");
}

void MalloyModelTests::userChosenSharePathSurvivesUndo() {
    // A path the user picked in the file dialog is their choice. The local-only
    // rule is for paths arriving in someone else's file, and applying it to this
    // application's own undo snapshots silently discarded a network share the
    // user had chosen.
    SceneCollection scenes;
    QUndoStack undo;
    scenes.setUndoStack(&undo);
    scenes.ensureCurrentScene();
    SceneItem* item = scenes.addNewSourceToCurrent(QStringLiteral("Logo"), Source::Type::Image);
    QVERIFY(item != nullptr);
    const int sourceId = item->sourceId();

    const QString share = QStringLiteral("//nas/media/logo.png");
    scenes.setCurrentSourceImagePath(0, share);
    QCOMPARE(scenes.sourceById(sourceId)->imagePath(), share);

    // Any later edit, then undo it: the restore goes through loadFromJson.
    scenes.setCurrentItemLocked(0, true);
    undo.undo();
    QVERIFY(scenes.sourceById(sourceId) != nullptr);
    QCOMPARE(scenes.sourceById(sourceId)->imagePath(), share);
    undo.redo();
    QCOMPARE(scenes.sourceById(sourceId)->imagePath(), share);

    // Undoing the image change itself goes back to the previous value rather
    // than to nothing.
    undo.undo();   // the lock
    undo.undo();   // the image path
    QVERIFY(scenes.sourceById(sourceId)->imagePath().isEmpty());
    undo.redo();
    QCOMPARE(scenes.sourceById(sourceId)->imagePath(), share);
}

void MalloyModelTests::undoAndRedoKeepADeclinedDeviceHeld() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("declined.malloy.json"));
    {
        SceneCollection authored;
        authored.ensureCurrentScene();
        QVERIFY(authored.addNewSourceToCurrent(QStringLiteral("Title"), Source::Type::Text,
                                               QStringLiteral("hello")) != nullptr);
        QVERIFY(authored.addAudioInputToCurrent(QStringLiteral("Desk Mic"),
                                                QStringLiteral("{mic-device-id}")) != nullptr);
        // Camera last, so it has the highest id. Only then does deleting it
        // let the id counter fall back to its id after a restore.
        QVERIFY(authored.addCameraToCurrent(QStringLiteral("Front Camera"),
                                            QStringLiteral("\\\\?\\usb#vid_dead&pid_beef"),
                                            QStringLiteral("Front Camera")) != nullptr);
        QVERIFY(ProjectDocument::saveToFile(authored, path));
    }

    // Opened and declined, the way MainWindow leaves it: the prompt answered
    // no, and the undo history starting from the loaded state.
    SceneCollection opened;
    QUndoStack undo;
    opened.setUndoStack(&undo);
    QString error;
    QVERIFY2(ProjectDocument::loadFromFile(opened, path, &error), qPrintable(error));
    undo.clear();
    QVERIFY(opened.deviceConsentPending());

    int cameraId = Source::InvalidId;
    for (Source* s : opened.sources())
        if (s && s->type() == Source::Type::Camera) cameraId = s->id();
    QVERIFY(cameraId != Source::InvalidId);

    // Any edit then undo. Undo restores a snapshot through loadFromJson, which
    // used to clear the hold and so start the declined camera and microphone.
    opened.setCurrentItemLocked(0, true);
    QCOMPARE(undo.count(), 1);
    undo.undo();
    QVERIFY2(opened.deviceHeld(cameraId), "undo must not release a declined camera");
    QVERIFY2(opened.gatherVisibleAudioIds().isEmpty(),
             "undo must not release a declined microphone");
    undo.redo();
    QVERIFY2(opened.deviceHeld(cameraId), "redo must not release a declined camera");

    // The longer route. Delete the camera, undo, redo, then create a source of
    // our own: the id counter restarts on every restore, so the new source can
    // reach the camera's id. It must not take that id, and must not be held
    // itself; undoing past it must bring the camera back still held.
    int cameraIndex = -1;
    Scene* scene = opened.currentScene();
    QVERIFY(scene != nullptr);
    for (int i = 0; i < scene->itemCount(); ++i)
        if (scene->itemAt(i)->sourceId() == cameraId) cameraIndex = i;
    QVERIFY(cameraIndex >= 0);

    undo.clear();
    opened.removeCurrentItemAt(cameraIndex);
    QVERIFY(opened.sourceById(cameraId) == nullptr);
    undo.undo();
    QVERIFY(opened.sourceById(cameraId) != nullptr);
    undo.redo();
    QVERIFY(opened.sourceById(cameraId) == nullptr);

    SceneItem* mine = opened.addNewSourceToCurrent(QStringLiteral("Mine"), Source::Type::Text,
                                                   QStringLiteral("mine"));
    QVERIFY(mine != nullptr);
    QVERIFY2(mine->sourceId() != cameraId,
             "a new source must not be handed a held source's id");
    QVERIFY2(!opened.deviceHeld(mine->sourceId()),
             "a source the user just created is never held");

    undo.undo();   // the new source
    undo.undo();   // the deletion
    QVERIFY(opened.sourceById(cameraId) != nullptr);
    QVERIFY2(opened.deviceHeld(cameraId),
             "the camera must come back still held after undoing past a new source");

    // A new project is the application's own state and holds nothing.
    opened.clear();
    QVERIFY(!opened.deviceConsentPending());
}

void MalloyModelTests::loadedProjectHoldsItsDevicesUntilAllowed() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("hostile.malloy.json"));

    // A project carrying a camera and a microphone, both visible, which is the
    // shape that used to open both the moment the file was opened.
    {
        SceneCollection authored;
        authored.ensureCurrentScene();
        QVERIFY(authored.addCameraToCurrent(QStringLiteral("Front Camera"),
                                            QStringLiteral("\\\\?\\usb#vid_dead&pid_beef"),
                                            QStringLiteral("Front Camera")) != nullptr);
        QVERIFY(authored.addAudioInputToCurrent(QStringLiteral("Desk Mic"),
                                                QStringLiteral("{mic-device-id}")) != nullptr);
        QVERIFY(ProjectDocument::saveToFile(authored, path));
    }

    SceneCollection opened;
    QString error;
    QVERIFY2(ProjectDocument::loadFromFile(opened, path, &error), qPrintable(error));

    // Held. The audio reconciler is driven entirely by this list, so an empty
    // one is what keeps the microphone shut.
    QVERIFY(opened.deviceConsentPending());
    QVERIFY(opened.gatherVisibleAudioIds().isEmpty());

    // The user is told what is being asked for, by name, rather than being
    // asked to approve an unexplained prompt.
    const QStringList wanted = opened.pendingDeviceRequests();
    QCOMPARE(wanted.size(), 2);
    QVERIFY(wanted.join(QLatin1Char('\n')).contains(QStringLiteral("Front Camera")));
    QVERIFY(wanted.join(QLatin1Char('\n')).contains(QStringLiteral("Desk Mic")));

    // Consent is per source. Allowing the camera must not also open the
    // microphone sitting next to it in the same project, which is the shape
    // that teaches people to approve a prompt without reading it.
    Source* camera = nullptr;
    Source* mic = nullptr;
    for (Source* s : opened.sources()) {
        if (!s) continue;
        if (s->type() == Source::Type::Camera)     camera = s;
        if (s->type() == Source::Type::AudioInput) mic = s;
    }
    QVERIFY(camera != nullptr);
    QVERIFY(mic != nullptr);
    QVERIFY(opened.deviceHeld(camera->id()));
    QVERIFY(opened.deviceHeld(mic->id()));

    opened.grantDeviceConsent(camera->id());
    QVERIFY(!opened.deviceHeld(camera->id()));
    QVERIFY2(opened.deviceHeld(mic->id()),
             "allowing the camera must not release the microphone");
    QVERIFY2(opened.deviceConsentPending(),
             "something is still held, so the project is not fully allowed");
    QVERIFY2(opened.gatherVisibleAudioIds().isEmpty(),
             "the microphone is still held, so it must still report nothing");
    QCOMPARE(opened.pendingDeviceRequests().size(), 1);

    // The prompt's single yes releases whatever is left.
    opened.grantDeviceConsent();
    QVERIFY(!opened.deviceConsentPending());
    QVERIFY(!opened.deviceHeld(mic->id()));
    QCOMPARE(opened.gatherVisibleAudioIds(), QStringList{QStringLiteral("{mic-device-id}")});

    // A project with nothing device backed must not prompt at all.
    const QString inert = dir.filePath(QStringLiteral("inert.malloy.json"));
    {
        SceneCollection authored;
        authored.ensureCurrentScene();
        QVERIFY(authored.addNewSourceToCurrent(QStringLiteral("Title"), Source::Type::Text,
                                               QStringLiteral("hello")) != nullptr);
        QVERIFY(ProjectDocument::saveToFile(authored, inert));
    }
    SceneCollection quiet;
    QVERIFY(ProjectDocument::loadFromFile(quiet, inert, &error));
    QVERIFY(!quiet.deviceConsentPending());
    QVERIFY(quiet.pendingDeviceRequests().isEmpty());
}

void MalloyModelTests::rtmpRelayTransportFollowsTheScheme() {
    bool tls = true;
    quint16 port = 0;

    // Cleartext RTMP, on the port it has always used.
    QVERIFY(RtmpKeyRelay::upstreamTransport(QStringLiteral("rtmp"), &tls, &port));
    QCOMPARE(tls, false);
    QCOMPARE(port, quint16(1935));

    // rtmps must be carried encrypted, and on 443 rather than the cleartext
    // port. This is the case that used to go out in the clear: QUrl knows no
    // default port for rtmps, so asking it for one yields 1935, and the relay
    // then opened a plain socket to it and wrote the real key into it.
    QVERIFY(RtmpKeyRelay::upstreamTransport(QStringLiteral("rtmps"), &tls, &port));
    QCOMPARE(tls, true);
    QCOMPARE(port, quint16(443));

    // Case and surrounding space come from a user-editable settings field.
    QVERIFY(RtmpKeyRelay::upstreamTransport(QStringLiteral("  RTMPS "), &tls, &port));
    QCOMPARE(tls, true);

    // Anything else is refused rather than mapped onto a transport that merely
    // resembles it. A scheme the relay cannot carry must stop the session, not
    // pick the nearest one it can.
    for (const QString& scheme : {QStringLiteral("http"), QStringLiteral("https"),
                                  QStringLiteral("rtmpe"), QStringLiteral("rtmpt"),
                                  QStringLiteral("srt"), QStringLiteral("")}) {
        QVERIFY2(!RtmpKeyRelay::upstreamTransport(scheme, &tls, &port),
                 qPrintable(QStringLiteral("scheme %1 must not be relayed").arg(scheme)));
    }

    // The mapping must be answerable without out-parameters, since start()
    // calls it once to decide whether to run at all.
    QVERIFY(RtmpKeyRelay::upstreamTransport(QStringLiteral("rtmp"), nullptr, nullptr));
    QVERIFY(!RtmpKeyRelay::upstreamTransport(QStringLiteral("gopher"), nullptr, nullptr));
}

void MalloyModelTests::rtmpRelaySubstitutesAcrossReadBoundaries() {
    const QByteArray placeholder = "PLACEHOLDERKEY0123456789";
    const QByteArray secret      = "live_999999_REALSECRET01";
    QCOMPARE(placeholder.size(), secret.size());   // the whole scheme rests on this

    // A generated placeholder matches the key length exactly, which is what
    // keeps every AMF string length and RTMP message length valid.
    for (int len : {8, 24, 41}) {
        const QString made = RtmpKeyRelay::makePlaceholder(len);
        QCOMPARE(made.size(), len);
        QCOMPARE(made.toUtf8().size(), len);       // must stay single-byte
        QVERIFY(!made.contains(QLatin1Char('/')));
    }
    // Two placeholders should not be the same.
    QVERIFY(RtmpKeyRelay::makePlaceholder(24) != RtmpKeyRelay::makePlaceholder(24));

    // Every occurrence is replaced, and the buffer length does not change.
    {
        QByteArray data = "AAA" + placeholder + "BBB" + placeholder + "CCC";
        const int before = data.size();
        const int n = RtmpKeyRelay::substituteAll(data, placeholder, secret);
        QCOMPARE(n, 2);
        QCOMPARE(data.size(), before);
        QVERIFY(!data.contains(placeholder));
        QCOMPARE(data.count(secret), 2);
    }

    // Hold-back: a trailing PROPER prefix of the placeholder is withheld, so an
    // occurrence split across two reads still gets substituted.
    {
        const QByteArray head = "xx" + placeholder.left(10);
        const int hold = RtmpKeyRelay::holdBackLength(head, placeholder);
        QCOMPARE(hold, 10);

        QByteArray forward = head.left(head.size() - hold);
        QByteArray pending = head.right(hold);
        QCOMPARE(RtmpKeyRelay::substituteAll(forward, placeholder, secret), 0);

        // Second read completes it.
        pending += placeholder.mid(10) + "yy";
        const int hold2 = RtmpKeyRelay::holdBackLength(pending, placeholder);
        QByteArray forward2 = pending.left(pending.size() - hold2);
        QCOMPARE(RtmpKeyRelay::substituteAll(forward2, placeholder, secret), 1);
        QCOMPARE(forward + forward2 + pending.right(hold2), QByteArray("xx") + secret + "yy");
    }

    // The rule that matters most: a buffer containing no part of the
    // placeholder must withhold NOTHING. Holding back a fixed
    // placeholder.size()-1 bytes instead deadlocks the 1537-byte RTMP
    // handshake, which carries no placeholder and so never completes it.
    {
        QByteArray handshake(1537, '\x03');
        QCOMPARE(RtmpKeyRelay::holdBackLength(handshake, placeholder), 0);
    }

    // A trailing full match is not a partial one: it is substituted, not held.
    {
        const QByteArray data = "zz" + placeholder;
        QCOMPARE(RtmpKeyRelay::holdBackLength(data, placeholder), 0);
    }

    // Byte-by-byte delivery, the worst case for a streaming substitution.
    {
        const QByteArray wire = "head" + placeholder + "tail" + placeholder;
        QByteArray pending, out;
        int count = 0;
        for (char c : wire) {
            pending.append(c);
            const int hold = RtmpKeyRelay::holdBackLength(pending, placeholder);
            QByteArray forward = pending.left(pending.size() - hold);
            pending = pending.right(hold);
            count += RtmpKeyRelay::substituteAll(forward, placeholder, secret);
            out += forward;
        }
        out += pending;   // flush at end of stream
        QCOMPARE(count, 2);
        QCOMPARE(out, QByteArray("head") + secret + "tail" + secret);
        QVERIFY(!out.contains(placeholder));
    }

    // Mismatched lengths are refused rather than corrupting the stream.
    {
        QByteArray data = "AAA" + placeholder;
        QCOMPARE(RtmpKeyRelay::substituteAll(data, placeholder, QByteArray("short")), 0);
    }
}

void MalloyModelTests::twitchAuthBuildsProtocolRequests() {
    const QStringList scopes = {QStringLiteral("channel:read:stream_key"),
                                QStringLiteral("channel:manage:broadcast")};

    const QString device = QString::fromUtf8(
        TwitchAuth::buildDeviceCodeBody(QStringLiteral("abc123"), scopes));
    QVERIFY(device.contains(QStringLiteral("client_id=abc123")));
    // Scopes are space delimited, which survives form encoding as %20 or +.
    QVERIFY(device.contains(QStringLiteral("scopes=")));
    QVERIFY(device.contains(QStringLiteral("channel:read:stream_key"))
            || device.contains(QStringLiteral("channel%3Aread%3Astream_key")));

    const QString token = QString::fromUtf8(
        TwitchAuth::buildDeviceTokenBody(QStringLiteral("abc123"), scopes,
                                         QStringLiteral("dev-code-xyz")));
    QVERIFY(token.contains(QStringLiteral("device_code=dev-code-xyz")));
    // The grant type is the exact URN, encoded or not.
    QVERIFY(token.contains(QStringLiteral("urn:ietf:params:oauth:grant-type:device_code"))
            || token.contains(QStringLiteral("urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code")));

    const QString refresh = QString::fromUtf8(
        TwitchAuth::buildRefreshBody(QStringLiteral("abc123"), QStringLiteral("r3fr3sh")));
    QVERIFY(refresh.contains(QStringLiteral("grant_type=refresh_token")));
    QVERIFY(refresh.contains(QStringLiteral("refresh_token=r3fr3sh")));
    QVERIFY(refresh.contains(QStringLiteral("client_id=abc123")));
}

void MalloyModelTests::twitchAuthReadsDeviceAndTokenResponses() {
    // Device authorization response.
    {
        const QByteArray json = R"({
            "device_code": "abcdef",
            "expires_in": 1800,
            "interval": 5,
            "user_code": "WXYZ1234",
            "verification_uri": "https://www.twitch.tv/activate?public=true"
        })";
        TwitchDeviceCode code;
        QString error;
        QVERIFY2(TwitchAuth::parseDeviceCode(json, &code, &error), qPrintable(error));
        QCOMPARE(code.deviceCode, QStringLiteral("abcdef"));
        QCOMPARE(code.userCode, QStringLiteral("WXYZ1234"));
        QCOMPARE(code.expiresInSecs, 1800);
        QCOMPARE(code.intervalSecs, 5);
        QVERIFY(code.verificationUri.startsWith(QStringLiteral("https://www.twitch.tv/activate")));
    }

    // A response with no device code is a failure, not an empty success.
    {
        TwitchDeviceCode code;
        QString error;
        QVERIFY(!TwitchAuth::parseDeviceCode(R"({"status":400,"message":"invalid client"})",
                                             &code, &error));
        QCOMPARE(error, QStringLiteral("invalid client"));
        QVERIFY(!TwitchAuth::parseDeviceCode("not json at all", &code, &error));
    }

    // Successful token exchange.
    {
        const QByteArray json = R"({
            "access_token": "at-1",
            "expires_in": 14124,
            "refresh_token": "rt-1",
            "scope": ["channel:read:stream_key","channel:manage:broadcast"],
            "token_type": "bearer"
        })";
        TwitchTokens tokens;
        QString pending, error;
        QVERIFY2(TwitchAuth::parseTokens(json, &tokens, &pending, &error), qPrintable(error));
        QCOMPARE(tokens.accessToken, QStringLiteral("at-1"));
        QCOMPARE(tokens.refreshToken, QStringLiteral("rt-1"));
        QCOMPARE(tokens.scopes.size(), 2);
        QVERIFY(tokens.expiresAt.isValid());
        QVERIFY(!tokens.needsRefresh());
        QVERIFY(pending.isEmpty());
    }

    // The waiting state: not an error, and the caller must keep polling.
    {
        TwitchTokens tokens;
        QString pending, error;
        QVERIFY(!TwitchAuth::parseTokens(R"({"status":400,"message":"authorization_pending"})",
                                         &tokens, &pending, &error));
        QCOMPARE(pending, QStringLiteral("authorization_pending"));
        QVERIFY(error.isEmpty());
    }

    // slow_down is also a waiting state, and tells the caller to back off.
    {
        TwitchTokens tokens;
        QString pending, error;
        QVERIFY(!TwitchAuth::parseTokens(R"({"status":400,"message":"slow_down"})",
                                         &tokens, &pending, &error));
        QCOMPARE(pending, QStringLiteral("slow_down"));
        QVERIFY(error.isEmpty());
    }

    // A refusal is a real failure and must stop the flow.
    {
        TwitchTokens tokens;
        QString pending, error;
        QVERIFY(!TwitchAuth::parseTokens(R"({"status":400,"message":"access_denied"})",
                                         &tokens, &pending, &error));
        QVERIFY(pending.isEmpty());
        QCOMPARE(error, QStringLiteral("access_denied"));
    }
}

void MalloyModelTests::twitchTokensExpireAndRoundTrip() {
    TwitchTokens t;
    t.accessToken = QStringLiteral("at");
    t.refreshToken = QStringLiteral("rt");
    t.scopes = {QStringLiteral("channel:read:stream_key")};

    // No expiry recorded is treated as needing a refresh rather than assumed good.
    QVERIFY(t.needsRefresh());

    t.expiresAt = QDateTime::currentDateTimeUtc().addSecs(3600);
    QVERIFY(!t.needsRefresh());
    // Refresh before expiry, not after: a token that dies mid-request is a
    // failed stream start.
    QVERIFY(t.needsRefresh(4000));

    t.expiresAt = QDateTime::currentDateTimeUtc().addSecs(-60);
    QVERIFY(t.needsRefresh());

    // Round trip through the form stored in the credential vault.
    t.expiresAt = QDateTime::currentDateTimeUtc().addSecs(1200);
    const TwitchTokens back = TwitchTokens::fromJson(t.toJson());
    QCOMPARE(back.accessToken, t.accessToken);
    QCOMPARE(back.refreshToken, t.refreshToken);
    QCOMPARE(back.scopes, t.scopes);
    QCOMPARE(back.expiresAt.toSecsSinceEpoch(), t.expiresAt.toSecsSinceEpoch());

    // Nothing stored yet reads as a disconnected account, not a crash.
    const TwitchTokens empty = TwitchTokens::fromJson(QString());
    QVERIFY(!empty.isValid());
    QVERIFY(empty.needsRefresh());
}

void MalloyModelTests::twitchApiParsesHelixPayloads() {
    QString error;

    // Get Users returns the signed-in user when called with no parameters.
    QCOMPARE(TwitchApi::parseUserId(R"({"data":[{"id":"141981764","login":"twitchdev"}]})", &error),
             QStringLiteral("141981764"));
    QVERIFY(error.isEmpty());

    QCOMPARE(TwitchApi::parseStreamKey(R"({"data":[{"stream_key":"live_44322889_a34ub"}]})", &error),
             QStringLiteral("live_44322889_a34ub"));

    QCOMPARE(TwitchApi::parseGameId(R"({"data":[{"id":"33214","name":"Fortnite"}]})", &error),
             QStringLiteral("33214"));

    // An empty data array for a game is not an error: the category is simply
    // left alone so an unknown name cannot cost the user their title.
    error = QStringLiteral("stale");
    QVERIFY(TwitchApi::parseGameId(R"({"data":[]})", &error).isEmpty());
    QVERIFY(error.isEmpty());

    // Missing data for the key or the user IS an error, and carries Twitch's
    // own wording so the user can act on it.
    QVERIFY(TwitchApi::parseStreamKey(R"({"error":"Unauthorized","status":401,"message":"Missing scope"})",
                                      &error).isEmpty());
    QVERIFY(error.contains(QStringLiteral("Missing scope")));
    QVERIFY(error.contains(QStringLiteral("Unauthorized")));

    QVERIFY(TwitchApi::parseUserId(R"({"data":[]})", &error).isEmpty());
    QVERIFY(!error.isEmpty());

    // Garbage must not be read as success.
    QVERIFY(TwitchApi::parseStreamKey("<html>502</html>", &error).isEmpty());
    QVERIFY(!error.isEmpty());

    // The scopes asked for at sign-in must cover both calls, because Twitch
    // grants them per authorization.
    QVERIFY(TwitchApi::requiredScopes().contains(QStringLiteral("channel:read:stream_key")));
    QVERIFY(TwitchApi::requiredScopes().contains(QStringLiteral("channel:manage:broadcast")));
}

// A machine with a hardware encoder, plenty of cores and a fast connection.
static SystemProfile makeStrongProfile() {
    SystemProfile p;
    p.encoders = {{QStringLiteral("libx264"), QStringLiteral("x264 (software)"), false},
                  {QStringLiteral("h264_nvenc"), QStringLiteral("NVIDIA NVENC H.264"), true}};
    p.cpuThreads = 24;
    p.monitorWidth = 2560;
    p.monitorHeight = 1440;
    p.microphoneName = QStringLiteral("Shure SM7B");
    p.microphonesChecked = true;
    p.microphoneConnected = true;
    p.micPeakObserved = 0.35f;     // watched, and it was producing sound
    p.camerasChecked = true;
    p.hasCamera = true;
    p.freeDiskBytes = 500LL * 1024 * 1024 * 1024;
    p.recordingVolume = QStringLiteral("D:\\");
    p.uploadKbps = 20000;
    return p;
}

// A modest laptop: no hardware encoder, few cores, slow upload, no mic.
static SystemProfile makeWeakProfile() {
    SystemProfile p;
    p.encoders = {{QStringLiteral("libx264"), QStringLiteral("x264 (software)"), false}};
    p.cpuThreads = 4;
    p.microphonesChecked = true;   // looked, and found none
    p.monitorWidth = 1366;
    p.monitorHeight = 768;
    p.freeDiskBytes = 5LL * 1024 * 1024 * 1024;
    p.recordingVolume = QStringLiteral("C:\\");
    p.uploadKbps = 3000;
    return p;
}

void MalloyModelTests::smartConfigRecommendsForTheHardware() {
    const OutputSettings current;

    // Strong machine: hardware encoder preferred, and the upload has room for
    // the full stream ceiling.
    {
        const Recommendation r = SettingsRecommender::recommend(makeStrongProfile(), current);
        QCOMPARE(r.output.videoCodec, QStringLiteral("h264_nvenc"));
        // 75 percent of 20000 is 15000, clamped to what services accept.
        QCOMPARE(r.output.bitrateKbps, SettingsRecommender::kMaxStreamKbps);
        QCOMPARE(r.output.width, 1920);
        QCOMPARE(r.output.height, 1080);
        QCOMPARE(r.output.fps, 60);
        QCOMPARE(r.output.keyframeSec, 2);
        // Nothing to warn about on this machine.
        QVERIFY2(r.warnings.isEmpty(), qPrintable(r.warnings.join(QStringLiteral(" | "))));
        // Every proposed value carries its reason.
        QVERIFY(r.notes.size() >= 5);
        for (const auto& note : r.notes) {
            QVERIFY(!note.field.isEmpty());
            QVERIFY(!note.value.isEmpty());
            QVERIFY2(!note.why.isEmpty(), qPrintable(note.field));
        }
    }

    // Weak machine: software encoder, a fast preset for four threads, and a
    // resolution the link can actually feed.
    {
        const Recommendation r = SettingsRecommender::recommend(makeWeakProfile(), current);
        QCOMPARE(r.output.videoCodec, QStringLiteral("libx264"));
        QCOMPARE(r.output.preset, QStringLiteral("veryfast"));
        QCOMPARE(r.output.bitrateKbps, 2250);      // 75 percent of 3000
        QCOMPARE(r.output.width, 1280);
        QCOMPARE(r.output.height, 720);
        QCOMPARE(r.output.fps, 30);
    }

    // A many-core machine without hardware encoding can afford a slower preset.
    {
        SystemProfile p = makeWeakProfile();
        p.cpuThreads = 24;
        p.uploadKbps = 20000;
        const Recommendation r = SettingsRecommender::recommend(p, current);
        QCOMPARE(r.output.preset, QStringLiteral("fast"));
    }

    // Applying a recommendation must not disturb settings it does not decide.
    {
        OutputSettings mine;
        mine.container = QStringLiteral("mkv");
        mine.audioCodec = QStringLiteral("opus");
        mine.replayBufferSeconds = 45;
        const Recommendation r = SettingsRecommender::recommend(makeStrongProfile(), mine);
        QCOMPARE(r.output.container, QStringLiteral("mkv"));
        QCOMPARE(r.output.audioCodec, QStringLiteral("opus"));
        QCOMPARE(r.output.replayBufferSeconds, 45);
    }
}

void MalloyModelTests::smartConfigDerivesShapeFromBitrate() {
    const OutputSettings current;
    SystemProfile p = makeStrongProfile();
    p.monitorWidth = 3840;
    p.monitorHeight = 2160;

    struct Case { int uploadKbps; int width; int height; int fps; };
    // The shape follows the bitrate, because a resolution the bitrate cannot
    // feed looks worse than a smaller one that it can.
    const QVector<Case> cases = {
        {12000, 1920, 1080, 60},   // 9000 clamped to 8000: comfortably 1080p60
        {8000,  1920, 1080, 60},   // 6000: exactly the 1080p60 threshold
        {7000,  1920, 1080, 30},   // 5250: 1080p, but not at 60
        {5000,  1280, 720,  60},   // 3750: 720p60, motion over resolution
        {2600,  1280, 720,  30},   // 1950 raised to the 1500 floor region
    };
    for (const Case& c : cases) {
        p.uploadKbps = c.uploadKbps;
        const Recommendation r = SettingsRecommender::recommend(p, current);
        QCOMPARE(r.output.width, c.width);
        QCOMPARE(r.output.height, c.height);
        QCOMPARE(r.output.fps, c.fps);
    }

    // Never propose encoding larger than the display being captured.
    p.uploadKbps = 20000;
    p.monitorWidth = 1366;
    p.monitorHeight = 768;
    const Recommendation r = SettingsRecommender::recommend(p, current);
    QCOMPARE(r.output.width, 1366);
    QCOMPARE(r.output.height, 768);
}

void MalloyModelTests::smartConfigWarnsRatherThanGuessing() {
    const OutputSettings current;

    // Unmeasured upload is unknown, not slow: the current bitrate is kept and
    // the user is told the figure is a starting point.
    {
        SystemProfile p = makeStrongProfile();
        p.uploadKbps = 0;
        OutputSettings mine;
        mine.bitrateKbps = 5200;
        const Recommendation r = SettingsRecommender::recommend(p, mine);
        QCOMPARE(r.output.bitrateKbps, 5200);
        QVERIFY(!r.warnings.filter(QStringLiteral("not measured")).isEmpty());
    }

    // No microphone, no hardware encoder and a nearly full disk are all things
    // to say out loud rather than silently encode around.
    {
        const Recommendation r = SettingsRecommender::recommend(makeWeakProfile(), current);
        QVERIFY(!r.warnings.filter(QStringLiteral("microphone")).isEmpty());
        QVERIFY(!r.warnings.filter(QStringLiteral("hardware encoder")).isEmpty());
        QVERIFY(!r.warnings.filter(QStringLiteral("free")).isEmpty());
    }

    // An unchecked machine must not be told it has no microphone.
    {
        SystemProfile p = makeWeakProfile();
        p.microphonesChecked = false;
        const Recommendation r = SettingsRecommender::recommend(p, current);
        QVERIFY(r.warnings.filter(QStringLiteral("microphone"), Qt::CaseInsensitive).isEmpty());
    }

    // Nor must an unchecked machine be told it has no camera. Camera
    // enumeration is slow and often has not run, and a cold cache is not
    // evidence of absence.
    {
        SystemProfile p = makeWeakProfile();
        p.camerasChecked = false;
        p.hasCamera = false;
        const Recommendation r = SettingsRecommender::recommend(p, current);
        QVERIFY(r.warnings.filter(QStringLiteral("camera"), Qt::CaseInsensitive).isEmpty());

        p.camerasChecked = true;   // now something looked, so saying so is fair
        const Recommendation r2 = SettingsRecommender::recommend(p, current);
        QCOMPARE(r2.warnings.filter(QStringLiteral("camera"), Qt::CaseInsensitive).size(), 1);
    }

    // A configured input whose device has gone away is a different problem
    // from having no input at all, and gets a different warning.
    {
        SystemProfile p = makeWeakProfile();
        p.microphonesChecked = true;
        p.microphoneName = QStringLiteral("Focusrite Input 1");
        p.microphoneConnected = false;
        const Recommendation r = SettingsRecommender::recommend(p, current);
        QCOMPARE(r.warnings.filter(QStringLiteral("not available")).size(), 1);
        QVERIFY(r.warnings.filter(QStringLiteral("no microphone is set up"),
                                  Qt::CaseInsensitive).isEmpty());
    }

    // The powered-off microphone on a live interface. Connected, enumerated,
    // and producing nothing. This is only sayable because something watched
    // the input; an unobserved peak must stay silent about it.
    {
        SystemProfile p = makeWeakProfile();
        p.microphonesChecked = true;
        p.microphoneName = QStringLiteral("Analogue 1 + 2");
        p.microphoneConnected = true;

        p.micPeakObserved = -1.0f;   // nobody listened
        const Recommendation unwatched = SettingsRecommender::recommend(p, current);
        QVERIFY(unwatched.warnings.filter(QStringLiteral("no sound arrived"),
                                          Qt::CaseInsensitive).isEmpty());

        p.micPeakObserved = 0.0f;    // listened, heard nothing
        const Recommendation silent = SettingsRecommender::recommend(p, current);
        QCOMPARE(silent.warnings.filter(QStringLiteral("no sound arrived"),
                                        Qt::CaseInsensitive).size(), 1);

        p.micPeakObserved = 0.4f;    // listened, heard something
        const Recommendation working = SettingsRecommender::recommend(p, current);
        QVERIFY(working.warnings.filter(QStringLiteral("no sound arrived"),
                                        Qt::CaseInsensitive).isEmpty());
    }

    // A machine with no encoders at all must not propose an empty codec.
    {
        SystemProfile p;
        p.cpuThreads = 8;
        OutputSettings mine;
        mine.videoCodec = QStringLiteral("libx264");
        const Recommendation r = SettingsRecommender::recommend(p, mine);
        QCOMPARE(r.output.videoCodec, QStringLiteral("libx264"));
    }

    // Profile helpers agree with the encoder list they are given.
    {
        QVERIFY(makeStrongProfile().hasHardwareEncoder());
        QCOMPARE(makeStrongProfile().preferredEncoderId(), QStringLiteral("h264_nvenc"));
        QVERIFY(!makeWeakProfile().hasHardwareEncoder());
        QCOMPARE(makeWeakProfile().preferredEncoderId(), QStringLiteral("libx264"));
        QVERIFY(SystemProfile{}.preferredEncoderId().isEmpty());
    }
}

void MalloyModelTests::captureBackendPreferenceParses() {
    using CaptureBackend::Kind;
    QCOMPARE(CaptureBackend::parse(QStringLiteral("wgc")), Kind::Wgc);
    QCOMPARE(CaptureBackend::parse(QStringLiteral("WGC")), Kind::Wgc);
    QCOMPARE(CaptureBackend::parse(QStringLiteral("dxgi")), Kind::Dxgi);

    // An unset or unreadable value is the backend that works everywhere, not
    // an error and not the newer one.
    QCOMPARE(CaptureBackend::parse(QString()), Kind::Dxgi);
    QCOMPARE(CaptureBackend::parse(QStringLiteral("something else")), Kind::Dxgi);

    // What is written is what is read back.
    QCOMPARE(CaptureBackend::parse(CaptureBackend::name(Kind::Wgc)), Kind::Wgc);
    QCOMPARE(CaptureBackend::parse(CaptureBackend::name(Kind::Dxgi)), Kind::Dxgi);
}

void MalloyModelTests::wgcBackendTimestampsAreCheckedNotTrusted() {
    using namespace std::chrono;
    const auto now = steady_clock::time_point(seconds(300000));
    steady_clock::time_point out{};

    // A stamp on the same clock base is used as it stands, converted from
    // 100 ns units.
    const qint64 sameInstant = 300000LL * 10000000LL;
    QVERIFY(WgcCapture::normaliseBackendTime(sameInstant, now, &out));
    QCOMPARE(duration_cast<milliseconds>(out - now).count(), 0LL);

    // A frame captured 40 ms ago keeps its own time rather than being restamped
    // with the moment it happened to be handled.
    QVERIFY(WgcCapture::normaliseBackendTime(sameInstant - 400000LL, now, &out));
    QCOMPARE(duration_cast<milliseconds>(now - out).count(), 40LL);

    // A stamp from some other clock is refused, and the arrival time stands in.
    QVERIFY(!WgcCapture::normaliseBackendTime(sameInstant + 60LL * 10000000LL, now, &out));
    QCOMPARE(out, now);
    QVERIFY(!WgcCapture::normaliseBackendTime(0, now, &out));
    QCOMPARE(out, now);
}

void MalloyModelTests::capturedFrameHoldsItsHandoffSlot() {
    auto inFlight = std::make_shared<std::atomic<int>>(0);
    auto claim = [inFlight] {
        inFlight->fetch_add(1);
        return std::shared_ptr<void>(nullptr, [inFlight](void*) { inFlight->fetch_sub(1); });
    };

    {
        CapturedFrame frame;
        frame.inFlightSlot = claim();
        QCOMPARE(inFlight->load(), 1);

        // Moving the frame is what delivery does, and it must not free the
        // slot early: the bound has to hold until the consumer has the frame.
        CapturedFrame delivered = std::move(frame);
        QCOMPARE(inFlight->load(), 1);

        // Taking a copy of the image out does not release the slot either.
        const QImage taken = delivered.image;
        QCOMPARE(inFlight->load(), 1);
    }
    // Letting the frame go is what frees the slot, whether it was consumed or
    // dropped undelivered.
    QCOMPARE(inFlight->load(), 0);
}

void MalloyModelTests::captureStatsAccumulateAcrossSessionChurn() {
    FakeCaptureSession::started.clear();
    FakeCaptureSession::stopped.clear();
    FakeCaptureSession::created.clear();

    SceneCollection scenes;
    QUndoStack undo;
    scenes.setUndoStack(&undo);
    scenes.addScene(QStringLiteral("Scene"));
    scenes.addNewSourceToCurrent(QStringLiteral("Display"), Source::Type::DisplayCapture,
                                 QString(), QColor(), 1, 2);

    CaptureController controller(
        &scenes,
        [](int adapterIndex, int outputIndex, QObject* parent) {
            return new FakeCaptureSession(adapterIndex, outputIndex, parent);
        });

    QCOMPARE(FakeCaptureSession::created.size(), 1);
    FakeCaptureSession::created.last()->setStats(100, 7);
    QCOMPARE(controller.captureStats().framesProduced, 100);
    QCOMPARE(controller.captureStats().framesDropped, 7);

    // Hiding the source stops the session. What it produced still happened.
    scenes.setCurrentItemVisible(0, false);
    QCOMPARE(controller.activeSessionCount(), 0);
    QCOMPARE(controller.captureStats().framesProduced, 100);
    QCOMPARE(controller.captureStats().framesDropped, 7);

    // A rebuilt session counts from zero, and the totals carry both.
    undo.undo();
    QCOMPARE(FakeCaptureSession::created.size(), 2);
    FakeCaptureSession::created.last()->setStats(20, 1);
    QCOMPARE(controller.captureStats().framesProduced, 120);
    QCOMPARE(controller.captureStats().framesDropped, 8);
}

void MalloyModelTests::rateControlFollowsWhereTheMediaIsGoing() {
    // Hardware encoders are only present when this machine has them, so the
    // test asserts on whichever it finds rather than requiring one.
    const EncoderRegistry::Encoder* hardware = nullptr;
    for (const auto& e : EncoderRegistry::available()) {
        if (e.isHardware) { hardware = &e; break; }
    }
    if (!hardware) {
        QSKIP("no hardware encoder on this machine");
    }

    OutputSettings s;
    s.bitrateKbps = 4500;
    s.crf = 23;

    const QStringList file = hardware->buildArgs(s, EncoderRegistry::Destination::File);
    const QStringList stream = hardware->buildArgs(s, EncoderRegistry::Destination::Stream);

    // A wire holds the bitrate it negotiated.
    QVERIFY(stream.contains(QStringLiteral("-b:v")));
    QVERIFY(stream.contains(QStringLiteral("4500k")));
    QVERIFY(stream.contains(QStringLiteral("-maxrate")));

    // A file holds quality instead, and asks for no bitrate at all. This is
    // the half that matters for the input rate: a rate target divides its
    // budget by the declared frame rate, and a quality target does not.
    QVERIFY(!file.contains(QStringLiteral("-b:v")));
    QVERIFY(!file.contains(QStringLiteral("-maxrate")));
    QVERIFY(file.contains(QStringLiteral("23")));

    // Both still name the codec and a pixel format.
    QVERIFY(file.contains(QStringLiteral("-c:v")));
    QVERIFY(file.contains(QStringLiteral("-pix_fmt")));
    QVERIFY(stream.contains(QStringLiteral("-c:v")));
}

void MalloyModelTests::inputDeclaresTheConfiguredFrameRate() {
    OutputSettings s;

    s.fps = 120;
    QCOMPARE(EncoderPipeline::declaredInputFrameRate(s), 120);
    s.fps = 60;
    QCOMPARE(EncoderPipeline::declaredInputFrameRate(s), 60);
    s.fps = 24;
    QCOMPARE(EncoderPipeline::declaredInputFrameRate(s), 24);

    // Never zero or negative: that is not a legal time base, and a recording
    // that refuses to start is a worse answer than one that runs at a sane
    // rate.
    s.fps = 0;
    QCOMPARE(EncoderPipeline::declaredInputFrameRate(s), 1);
    s.fps = -30;
    QCOMPARE(EncoderPipeline::declaredInputFrameRate(s), 1);

    // And bounded above, because this number is also what the stream path's
    // rate control divides its budget by.
    s.fps = 100000;
    QCOMPARE(EncoderPipeline::declaredInputFrameRate(s), 1000);
}

void MalloyModelTests::frameProfileSummarisesADistribution() {
    using namespace FrameProfile;

    // Off by default, and recording while off must store nothing. That is what
    // lets the instrumentation stay in the frame path without taxing it.
    setEnabled(false);
    reset();
    for (int i = 0; i < 100; ++i) record(Stage::Composition, 5000);
    QVERIFY(!report().contains(QStringLiteral("COMPOSITION")));

    setEnabled(true);
    reset();

    // Bucket edges never go backwards, which is what makes walking them for a
    // percentile meaningful.
    qint64 previous = -1;
    for (int i = 0; i < detail::kBuckets; ++i) {
        const qint64 edge = detail::bucketFloor(i);
        QVERIFY(edge >= previous);
        previous = edge;
    }

    // A sample always lands in a bucket whose floor it has reached.
    for (qint64 v : {qint64(1), qint64(7), qint64(999), qint64(1000),
                     qint64(1500000), qint64(40000000)}) {
        QVERIFY(detail::bucketFloor(detail::bucketFor(v)) <= v);
    }

    // Ninety samples at about 1 ms and ten at about 40 ms: the shape a stage
    // has when it is usually free and occasionally stalls. The mean alone
    // would report about 4.9 ms and describe neither population.
    for (int i = 0; i < 90; ++i) record(Stage::EncoderWrite, 1000000);
    for (int i = 0; i < 10; ++i) record(Stage::EncoderWrite, 40000000);

    const detail::Stat& write = detail::cumulative()[size_t(Stage::EncoderWrite)];
    QCOMPARE(write.count.load(), quint64(100));
    QCOMPARE(write.max.load(), qint64(40000000));

    // The median sits with the cheap population and the tail with the
    // expensive one, each within a bucket width of the truth.
    const double p50 = double(write.percentile(0.50));
    const double p95 = double(write.percentile(0.95));
    QVERIFY2(p50 > 800000 && p50 <= 1000000, qPrintable(QString::number(p50)));
    QVERIFY2(p95 > 32000000 && p95 <= 40000000, qPrintable(QString::number(p95)));

    // The mean lies between them and equals neither, which is the point of
    // keeping a distribution rather than an average.
    QVERIFY(write.mean() > p50 && write.mean() < p95);

    // The report names only the stages it has samples for.
    const QString text = report();
    QVERIFY(text.contains(QStringLiteral("ENCODER HANDOFF")));
    QVERIFY(!text.contains(QStringLiteral("WIDGET BLIT")));

    // Reading the window empties it, so a series line describes one second
    // rather than the run so far.
    record(Stage::WidgetBlit, 2000000);
    QVERIFY(seriesLine().contains(QStringLiteral("WIDGET BLIT")));
    QVERIFY(!seriesLine().contains(QStringLiteral("WIDGET BLIT")));

    setEnabled(false);
    reset();
}

void MalloyModelTests::compositionFollowsConsumersNotPaintEvents() {
    // A recording must produce media whether or not anyone can see the window.
    // This is the rule whose absence produced a valid 184 second file holding
    // a still picture for 143 of them, with the duration correct and no error
    // reported anywhere.
    QVERIFY(PreviewWidget::compositionRequired(/*recording=*/true, /*streaming=*/false,
                                               /*previewVisible=*/false,
                                               /*contentAdvanced=*/true));
    QVERIFY(PreviewWidget::compositionRequired(false, /*streaming=*/true, false, true));

    // Nobody recording, nobody streaming, nobody looking: composing would be
    // work for no consumer. One measured run threw away 36000 readbacks for
    // want of this half of the rule.
    QVERIFY(!PreviewWidget::compositionRequired(false, false, false, true));

    // Visible with nothing recording is still a consumer: the preview itself.
    QVERIFY(PreviewWidget::compositionRequired(false, false, /*previewVisible=*/true, true));

    // A picture that has not moved is never composed again, whoever is
    // watching. This is what keeps a repaint from being mistaken for new media.
    QVERIFY(!PreviewWidget::compositionRequired(true, true, true, /*contentAdvanced=*/false));
    QVERIFY(!PreviewWidget::compositionRequired(false, false, true, false));
}

void MalloyModelTests::rawVideoDeclarationCoversSizeNotOnlyFormat() {
    // What the arguments promise ffmpeg: this format, at this resolution.
    QImage good(int(MalloyCanvas::Width), int(MalloyCanvas::Height),
                EncoderPipeline::rawVideoFormat());
    QVERIFY(EncoderPipeline::conformsToPipeDeclaration(good));

    // The format alone is not the contract. rawvideo has no framing, so the
    // far end counts bytes: a frame of the right format and the wrong size is
    // accepted silently and shifts every frame after it for the rest of the
    // recording. This is the case the priming path got wrong by fixing up the
    // format and not the size.
    QImage tooSmall(1280, 720, EncoderPipeline::rawVideoFormat());
    QVERIFY2(!EncoderPipeline::conformsToPipeDeclaration(tooSmall),
             "a frame of the declared format but the wrong size must be "
             "refused: it displaces every frame after it");

    QImage tooWide(int(MalloyCanvas::Width) + 1, int(MalloyCanvas::Height),
                   EncoderPipeline::rawVideoFormat());
    QVERIFY(!EncoderPipeline::conformsToPipeDeclaration(tooWide));

    // Nor is the size alone the contract. Premultiplied alpha is the trap
    // here, because it is what the compositor paints onto and it differs from
    // the declared format only in how the samples are interpreted.
    QImage premultiplied(int(MalloyCanvas::Width), int(MalloyCanvas::Height),
                         QImage::Format_ARGB32_Premultiplied);
    QVERIFY2(!EncoderPipeline::conformsToPipeDeclaration(premultiplied),
             "the compositor's own format is not the declared one, and would "
             "darken every pixel with alpha below full");

    // A null frame declares nothing and conforms to nothing.
    QVERIFY(!EncoderPipeline::conformsToPipeDeclaration(QImage()));
}

void MalloyModelTests::rawVideoNeedsTightlyPackedRows() {
    // An ordinary four byte frame is naturally tight: 1920 pixels at 4 bytes
    // is already aligned, which is why this has never gone wrong in practice.
    QImage ordinary(1920, 1080, QImage::Format_ARGB32);
    QVERIFY(EncoderPipeline::isTightlyPacked(ordinary));
    QCOMPARE(ordinary.bytesPerLine(), 1920 * 4);

    // An odd width in the same format is still tight, because four byte pixels
    // never need padding to reach a four byte boundary.
    QImage odd(1919, 1080, QImage::Format_ARGB32);
    QVERIFY(EncoderPipeline::isTightlyPacked(odd));

    // A padded image is what the row by row path exists for. Built over a
    // buffer with a deliberately wider stride, since Qt will not produce one
    // for this format on its own.
    const int width = 640, height = 8;
    const int paddedStride = width * 4 + 64;
    QByteArray storage(paddedStride * height, 0);
    QImage padded(reinterpret_cast<uchar*>(storage.data()), width, height,
                  paddedStride, QImage::Format_ARGB32);
    QVERIFY(!padded.isNull());
    QCOMPARE(padded.bytesPerLine(), paddedStride);
    QVERIFY2(!EncoderPipeline::isTightlyPacked(padded),
             "a padded frame must not be written as one block: ffmpeg would "
             "read the padding as picture and shear the image");

    // A three byte format pads to a four byte boundary at odd widths, which is
    // the case that would arise if the canvas ever stopped being 32 bit.
    QImage rgb888(1919, 4, QImage::Format_RGB888);
    QCOMPARE(rgb888.bytesPerLine() % 4, 0);
    QVERIFY(!EncoderPipeline::isTightlyPacked(rgb888));

    QVERIFY(!EncoderPipeline::isTightlyPacked(QImage()));

    // Layout and format are independent contracts, and passing one says
    // nothing about the other. The padded RGB888 frame above exercises the row
    // by row path; it is still not a frame this pipe would accept, because
    // ffmpeg was told the bytes would be BGRA.
    QCOMPARE(EncoderPipeline::rawVideoFormat(), QImage::Format_ARGB32);
    QVERIFY(rgb888.format() != EncoderPipeline::rawVideoFormat());
    QVERIFY(ordinary.format() == EncoderPipeline::rawVideoFormat());

    // And a premultiplied frame, which is what the compositor works in, is the
    // near miss worth naming: same depth, same stride, wrong meaning for the
    // alpha channel, so it would encode as subtly wrong colour rather than
    // failing.
    QImage premultiplied(64, 64, QImage::Format_ARGB32_Premultiplied);
    QVERIFY(EncoderPipeline::isTightlyPacked(premultiplied));
    QVERIFY(premultiplied.format() != EncoderPipeline::rawVideoFormat());
}

void MalloyModelTests::frameRateChoicesAlwaysIncludeTheConfiguredRate() {
    // The case that corrupted a project: 120 was stored, the list offered
    // 60, 30 and 24, the page showed 60 and wrote 60 back on Apply.
    const QStringList at120 = OutputSettings::frameRateChoices(120);
    QVERIFY(at120.contains(QStringLiteral("120")));

    // An ordinary rate is offered once, not twice.
    const QStringList at60 = OutputSettings::frameRateChoices(60);
    QCOMPARE(at60.count(QStringLiteral("60")), 1);

    // A rate nobody anticipated is still offered, because the field is an
    // arbitrary integer everywhere else, including the rate declared to
    // ffmpeg.
    const QStringList odd = OutputSettings::frameRateChoices(37);
    QVERIFY(odd.contains(QStringLiteral("37")));
    QVERIFY(odd.contains(QStringLiteral("60")));

    // Highest first, and every entry a number.
    int previous = 1 << 30;
    for (const QString& entry : odd) {
        bool ok = false;
        const int value = entry.toInt(&ok);
        QVERIFY(ok);
        QVERIFY(value < previous);
        previous = value;
    }

    // An unset or nonsensical stored rate does not add an entry, and the list
    // is still usable.
    const QStringList none = OutputSettings::frameRateChoices(0);
    QVERIFY(!none.contains(QStringLiteral("0")));
    QVERIFY(none.contains(QStringLiteral("60")));
    QVERIFY(!OutputSettings::frameRateChoices(-5).contains(QStringLiteral("-5")));
}

void MalloyModelTests::captureDemandFollowsConsumers() {
    SceneCollection scenes;
    PreviewWidget preview(&scenes, PreviewWidget::Role::Program);

    // Never shown, nothing recording: nobody wants frames, so the capture
    // backends should not be reading any back. This is the state that cost a
    // fifth of a core producing pictures for no one.
    QVERIFY(!preview.consumersPresent());

    QSignalSpy demand(&preview, &PreviewWidget::consumerDemandChanged);

    // A recording is a consumer whether or not anything is on screen. This is
    // the same guarantee #38 turned on, reaching one stage further upstream.
    preview.setRecordingActive(true);
    QVERIFY(preview.consumersPresent());
    QCOMPARE(demand.count(), 1);
    QCOMPARE(demand.takeFirst().at(0).toBool(), true);

    // Streaming counts too, and demand does not drop while it holds.
    preview.setStreamingActive(true);
    preview.setRecordingActive(false);
    QVERIFY(preview.consumersPresent());

    // Only when the last consumer goes does demand fall, and it is reported
    // once rather than on every change that leaves the answer the same.
    preview.setStreamingActive(false);
    QVERIFY(!preview.consumersPresent());
    QCOMPARE(demand.count(), 1);
    QCOMPARE(demand.takeFirst().at(0).toBool(), false);
}

void MalloyModelTests::aWorkerThatWillNotStopIsCutLooseNotDestroyed() {
    // A worker that stops in time is simply deleted, and the last look at it
    // happens first, while it is still there to be read.
    {
        auto* quick = new StuckWorker;
        quick->release();
        quick->start();
        QPointer<StuckWorker> gone(quick);
        int lookedAt = 0;
        QVERIFY(retireWorker(quick, 5000, [&](const StuckWorker&) { ++lookedAt; }));
        QVERIFY(!gone);
        QCOMPARE(lookedAt, 1);
    }

    // One that does not stop must not be destroyed: Qt aborts the whole
    // process when a running QThread is deleted, which is what removing a
    // hung window's source used to do.
    QObject owner;
    auto* stuck = new StuckWorker;
    stuck->setParent(&owner);
    QPointer<StuckWorker> alive(stuck);
    const QMetaObject::Connection listener =
        QObject::connect(stuck, &QThread::started, &owner, [] {});
    stuck->start();
    const auto letGo = qScopeGuard([&] { if (alive) alive->release(); });

    int lookedAt = 0;
    QVERIFY(!retireWorker(stuck, 50, [&](const StuckWorker&) { ++lookedAt; }));
    QCOMPARE(lookedAt, 1);
    QVERIFY(alive);
    QVERIFY(alive->isRunning());
    QCOMPARE(alive->stopRequests.load(), 1);

    // Nothing it does from here reaches its old listeners, and its old owner
    // no longer owns it, so the owner going away cannot delete it either.
    // disconnect() reports false for a connection that is already gone.
    QVERIFY(!QObject::disconnect(listener));
    QVERIFY(alive->parent() == nullptr);

    // When its thread finally ends it deletes itself.
    alive->release();
    QTRY_VERIFY_WITH_TIMEOUT(!alive, 5000);
}

void MalloyModelTests::aStopRequestedBeforeTheWorkerRunsIsKept() {
    // A stop that lands between start() and the thread's first instruction
    // used to be overwritten when run() set the running flag, so the worker
    // went on to do its work and the stop waited out its timeout. A null
    // window is used because the worker notices at once that it is gone and
    // says so: if the stop was lost, it reports a closed window; if kept, it
    // does nothing at all.
    WindowCapture worker(0);
    std::atomic<int> closed{0};
    QObject::connect(&worker, &WindowCapture::windowClosed, &worker,
                     [&] { closed.fetch_add(1); }, Qt::DirectConnection);
    worker.requestStop();
    worker.start();
    QVERIFY(worker.wait(5000));
    QCOMPARE(closed.load(), 0);
}

void MalloyModelTests::aSignOutDuringARefreshStaysSignedOut() {
    const auto cleanup = qScopeGuard([] { CredentialStore::erase(kTestTwitchCredential); });
    seedTwitchTokens(/*expired*/ true);

    FakeTwitch twitch;
    twitch.hold();
    twitch.answer(200, kFreshTokens);
    TwitchAuth auth(kTestTwitchCredential, nullptr);
    auth.setClientId(QStringLiteral("test-client"));
    auth.setAuthBase(twitch.base());
    QVERIFY(auth.isConnected());

    int calls = 0;
    bool ok = true;
    auth.withAccessToken([&](bool success, const QString&) { ++calls; ok = success; });
    QTRY_COMPARE(twitch.waiting(), 1);

    // The user signs out while the refresh is out. When Twitch answers, the
    // new tokens must not be stored: that signed the user back in, and wrote
    // the tokens back to the credential store, behind a UI saying otherwise.
    auth.signOut();
    twitch.release();
    QTRY_COMPARE(calls, 1);
    QVERIFY(!ok);
    QVERIFY(!auth.isConnected());
    QVERIFY(CredentialStore::load(kTestTwitchCredential).isEmpty());
}

void MalloyModelTests::aRefreshTwitchDidNotAnswerKeepsTheAccount() {
    const auto cleanup = qScopeGuard([] { CredentialStore::erase(kTestTwitchCredential); });
    seedTwitchTokens(/*expired*/ true);

    // A port nobody is listening on: the request never reaches anyone, as
    // when offline.
    quint16 deadPort = 0;
    {
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost));
        deadPort = probe.serverPort();
    }
    TwitchAuth auth(kTestTwitchCredential, nullptr);
    auth.setClientId(QStringLiteral("test-client"));
    auth.setAuthBase(QStringLiteral("http://127.0.0.1:%1").arg(deadPort));
    QSignalSpy signedOut(&auth, &TwitchAuth::signedOut);

    int calls = 0;
    bool ok = true;
    const auto ask = [&] {
        auth.withAccessToken([&](bool success, const QString&) { ++calls; ok = success; });
    };

    ask();
    QTRY_COMPARE_WITH_TIMEOUT(calls, 1, 15000);
    QVERIFY(!ok);
    QVERIFY(auth.isConnected());
    QCOMPARE(signedOut.count(), 0);
    QVERIFY(!CredentialStore::load(kTestTwitchCredential).isEmpty());

    // Twitch answering that it is unavailable says nothing about the token
    // either.
    FakeTwitch twitch;
    auth.setAuthBase(twitch.base());
    twitch.answer(503, "<html>Service Unavailable</html>");
    ask();
    QTRY_COMPARE(calls, 2);
    QVERIFY(!ok);
    QVERIFY(auth.isConnected());
    QCOMPARE(signedOut.count(), 0);

    // Twitch refusing the token does disconnect the account.
    twitch.answer(400, R"({"status":400,"message":"Invalid refresh token"})");
    ask();
    QTRY_COMPARE(calls, 3);
    QVERIFY(!ok);
    QVERIFY(!auth.isConnected());
    QCOMPARE(signedOut.count(), 1);
    QVERIFY(CredentialStore::load(kTestTwitchCredential).isEmpty());
}

void MalloyModelTests::aCancelledSignInIgnoresALateCode() {
    const auto cleanup = qScopeGuard([] { CredentialStore::erase(kTestTwitchCredential); });
    CredentialStore::erase(kTestTwitchCredential);

    FakeTwitch twitch;
    twitch.hold();
    twitch.answer(200, kDeviceCode);
    TwitchAuth auth(kTestTwitchCredential, nullptr);
    auth.setClientId(QStringLiteral("test-client"));
    auth.setAuthBase(twitch.base());
    QSignalSpy shown(&auth, &TwitchAuth::deviceCodeReady);

    auth.beginDeviceFlow(TwitchApi::requiredScopes());
    QTRY_COMPARE(twitch.waiting(), 1);

    // Cancel while the code is still being fetched. The answer arriving
    // afterwards used to be shown and polled with, invisibly, once a second.
    auth.cancelDeviceFlow();
    twitch.release();
    QTest::qWait(1500);   // longer than the one second polling interval
    QCOMPARE(shown.count(), 0);
    QCOMPARE(twitch.requests.size(), 1);
}

void MalloyModelTests::aNetworkBlipWhilePollingDoesNotEndTheSignIn() {
    const auto cleanup = qScopeGuard([] { CredentialStore::erase(kTestTwitchCredential); });
    CredentialStore::erase(kTestTwitchCredential);

    FakeTwitch twitch;
    twitch.answer(200, kDeviceCode);
    twitch.answer(503, {});           // one poll lands on an unavailable Twitch
    twitch.answer(200, kFreshTokens); // and the next finds the approval
    TwitchAuth auth(kTestTwitchCredential, nullptr);
    auth.setClientId(QStringLiteral("test-client"));
    auth.setAuthBase(twitch.base());
    QSignalSpy failed(&auth, &TwitchAuth::failed);
    QSignalSpy connected(&auth, &TwitchAuth::connected);

    auth.beginDeviceFlow(TwitchApi::requiredScopes());
    QTRY_COMPARE_WITH_TIMEOUT(connected.count(), 1, 10000);
    QCOMPARE(failed.count(), 0);
    QVERIFY(auth.isConnected());
}

void MalloyModelTests::theChannelIdIsForgottenWhenTheAccountChanges() {
    const auto cleanup = qScopeGuard([] { CredentialStore::erase(kTestTwitchCredential); });
    seedTwitchTokens(/*expired*/ false);

    FakeTwitch helix;
    helix.answer(200, R"({"data":[{"id":"111","login":"first"}]})");
    TwitchAuth auth(kTestTwitchCredential, nullptr);
    auth.setClientId(QStringLiteral("test-client"));
    TwitchApi api(&auth);
    api.setApiBase(helix.base());

    int calls = 0;
    bool ok = false;
    QString id;
    const auto fetch = [&] {
        api.fetchUserId([&](bool success, const QString& value) { ++calls; ok = success; id = value; });
    };

    fetch();
    QTRY_COMPARE(calls, 1);
    QVERIFY(ok);
    QCOMPARE(id, QStringLiteral("111"));

    // Signed out, the cached id must not be handed back as if an account were
    // still connected.
    auth.signOut();
    fetch();
    QTRY_COMPARE(calls, 2);
    QVERIFY(!ok);

    // A lookup still in flight when someone signs in, perhaps as a different
    // account, must not cache the id it was answering for.
    seedTwitchTokens(/*expired*/ false);
    TwitchAuth second(kTestTwitchCredential, nullptr);
    second.setClientId(QStringLiteral("test-client"));
    TwitchApi api2(&second);
    api2.setApiBase(helix.base());
    helix.hold();
    helix.answer(200, R"({"data":[{"id":"111","login":"first"}]})");
    helix.answer(200, R"({"data":[{"id":"222","login":"second"}]})");
    api2.fetchUserId([&](bool success, const QString& value) { ++calls; ok = success; id = value; });
    QTRY_COMPARE(helix.waiting(), 1);
    emit second.connected();   // stands in for a sign-in completing meanwhile
    helix.release();
    QTRY_COMPARE(calls, 3);
    api2.fetchUserId([&](bool success, const QString& value) { ++calls; ok = success; id = value; });
    QTRY_COMPARE(calls, 4);
    QVERIFY(ok);
    QCOMPARE(id, QStringLiteral("222"));
}

void MalloyModelTests::replayAudioWaitsForTheEncoder() {
    QQueue<TimedPcm> chunks;
    for (int i = 0; i < 3; ++i)
        chunks.enqueue(TimedPcm{QByteArray(3840, char('a' + i)), i * 20000});
    RingTimedPcmSource source(chunks);
    QSignalSpy finished(&source, &RingTimedPcmSource::finished);

    // Started, as saving a replay does, well before the encoder subscribes.
    source.start();
    QTest::qWait(150);

    QList<QByteArray> received;
    QObject::connect(&source, &TimedPcmSource::pcmReady, &source,
                     [&received](const QByteArray& pcm) { received.append(pcm); });
    QTRY_COMPARE(finished.count(), 1);

    // Every chunk, from the first, reaches the late subscriber in order.
    QCOMPARE(received.size(), 3);
    for (int i = 0; i < 3; ++i) QCOMPARE(received[i].at(0), char('a' + i));
}

void MalloyModelTests::closingDuringAReplaySaveFinishesItFirst() {
    RecordingTestFrames frames;
    RecordingTestAudio audio;
    auto* controller = new MediaController(&frames, &audio);
    const auto cleanup = qScopeGuard([&controller] { delete controller; });
    if (!controller->ffmpegAvailable()) QSKIP("Real encoder lifecycle requires ffmpeg in PATH");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // Three seconds of replay, five frames a second, with its audio.
    QImage img(320, 180, QImage::Format_RGB32);
    img.fill(QColor(40, 110, 180));
    QByteArray jpeg;
    QBuffer buf(&jpeg);
    buf.open(QIODevice::WriteOnly);
    QVERIFY(img.save(&buf, "JPEG"));
    QQueue<ReplayFrame> ring;
    for (int i = 0; i < 15; ++i) ring.enqueue(ReplayFrame{jpeg, i * 200000});
    QQueue<TimedPcm> pcm;
    for (int i = 0; i < 150; ++i) pcm.enqueue(TimedPcm{QByteArray(3840, '\0'), i * 20000});

    const QString path = dir.filePath(QStringLiteral("replay.mp4"));
    QString error;
    QVERIFY2(controller->saveReplay(path, recordingTestSettings(), ring, 320, 180, pcm, &error),
             qPrintable(error));

    EncoderPipeline* replay = nullptr;
    for (EncoderPipeline* p : controller->findChildren<EncoderPipeline*>())
        if (p->isRunning()) replay = p;
    QVERIFY(replay);
    const QString ffmpeg = replay->ffmpegPath();
    const QList<RingTimedPcmSource*> sources = controller->findChildren<RingTimedPcmSource*>();
    QCOMPARE(sources.size(), 1);

    QStringList order;
    QObject::connect(replay, &EncoderPipeline::finished, replay,
                     [&order] { order << QStringLiteral("replay finished"); });
    QObject::connect(sources.first(), &QObject::destroyed, sources.first(),
                     [&order] { order << QStringLiteral("audio source gone"); });

    // The app closes part way through the save.
    QTest::qWait(700);
    delete std::exchange(controller, nullptr);

    // The replay is finished while its audio source still exists, not after:
    // its teardown disconnects from that source.
    QCOMPARE(order, QStringList({QStringLiteral("replay finished"),
                                 QStringLiteral("audio source gone")}));
    QVERIFY(recordingDecodes(ffmpeg, path));
}

namespace {
QJsonObject projectWithSource(int sourceId) {
    QJsonObject source{{QStringLiteral("id"), sourceId},
                       {QStringLiteral("type"), QStringLiteral("text")},
                       {QStringLiteral("name"), QStringLiteral("Title")}};
    QJsonObject item{{QStringLiteral("sourceId"), sourceId}};
    QJsonObject scene{{QStringLiteral("name"), QStringLiteral("Scene 1")},
                      {QStringLiteral("items"), QJsonArray{item}}};
    return QJsonObject{{QStringLiteral("app"), QStringLiteral("MalloyStudio")},
                       {QStringLiteral("version"), 2},
                       {QStringLiteral("currentScene"), 0},
                       {QStringLiteral("sources"), QJsonArray{source}},
                       {QStringLiteral("scenes"), QJsonArray{scene}}};
}
}

void MalloyModelTests::sourceIdsNearTheTopDoNotOverflow() {
    // INT_MAX is refused outright: the counter raised past it would wrap to a
    // negative id, which the next load refuses, so the saved project would no
    // longer open.
    SceneCollection scenes;
    QString error;
    QVERIFY(!scenes.loadFromJson(projectWithSource(std::numeric_limits<int>::max()), &error));
    QVERIFY(error.contains(QStringLiteral("id")));

    // The largest id allowed loads, and a source added after it still gets a
    // positive id that is not taken, and the result saves and opens again.
    QVERIFY2(scenes.loadFromJson(projectWithSource(Source::MaxId), &error), qPrintable(error));
    Source* added = scenes.sourceForItem(scenes.addNewSourceToCurrent(
        QStringLiteral("Second"), Source::Type::Text, QStringLiteral("two")));
    QVERIFY(added);
    QVERIFY(added->id() > Source::InvalidId);
    QVERIFY(added->id() != Source::MaxId);
    Source* third = scenes.sourceForItem(scenes.addNewSourceToCurrent(
        QStringLiteral("Third"), Source::Type::Text, QStringLiteral("three")));
    QVERIFY(third);
    QVERIFY(third->id() > Source::InvalidId);
    QVERIFY(third->id() != added->id());

    SceneCollection reopened;
    QVERIFY2(reopened.loadFromJson(scenes.toJson(), &error), qPrintable(error));
    QCOMPARE(reopened.sources().size(), 3);
}

void MalloyModelTests::anEditSessionEndsWithTheStateItBeganIn() {
    SceneCollection scenes;
    QUndoStack undo;
    scenes.setUndoStack(&undo);
    QString error;
    QVERIFY(scenes.loadFromJson(projectWithSource(1), &error));
    scenes.selectCurrentItemAt(0);
    undo.clear();

    // A slider moved with the wheel opens a session that nothing commits.
    scenes.beginEditSession();
    scenes.setCurrentSourceText(0, QStringLiteral("changed"), false);

    // Another project is opened. The session belonged to the old one.
    QJsonObject other = projectWithSource(7);
    QVERIFY(scenes.loadFromJson(other, &error, SceneCollection::LoadOrigin::File));
    QVERIFY(!scenes.editSessionActive());

    // A drag in the new project begins and commits its own edit. Before, the
    // begin was a no-op on the stale session, so the step recorded carried the
    // old project as its "before", and undoing it brought that project back.
    scenes.selectCurrentItemAt(0);
    scenes.beginEditSession();
    scenes.setCurrentItemTransform(0, QRectF(10, 10, 200, 100), false);
    scenes.commitEditSession(QStringLiteral("Transform Layer"));
    QCOMPARE(undo.count(), 1);
    undo.undo();
    QCOMPARE(scenes.sources().size(), 1);
    QCOMPARE(scenes.sources().first()->id(), 7);

    // Undo and redo end a session too.
    scenes.beginEditSession();
    undo.redo();
    QVERIFY(!scenes.editSessionActive());
}

void MalloyModelTests::aCommandDuringAnEditSessionKeepsItsOwnUndoStep() {
    SceneCollection scenes;
    QUndoStack undo;
    scenes.setUndoStack(&undo);
    QString error;
    QVERIFY(scenes.loadFromJson(projectWithSource(1), &error));
    scenes.selectCurrentItemAt(0);
    undo.clear();

    scenes.beginEditSession();
    scenes.setCurrentSourceText(0, QStringLiteral("a"), false);
    // A discrete, recorded command lands while the session is open.
    scenes.setCurrentItemVisible(0, false);
    scenes.setCurrentSourceText(0, QStringLiteral("b"), false);
    scenes.commitEditSession(QStringLiteral("Edit Text"));

    // Undoing the text edit takes back the text, not the hiding as well.
    undo.undo();
    QVERIFY(!scenes.currentScene()->itemAt(0)->isVisible());
    QCOMPARE(scenes.sources().first()->text(), QStringLiteral("a"));
}

void MalloyModelTests::aSlowUplinkBacksUpIntoFfmpegNotMemory() {
    // An rtmps ingest that accepts the connection and never answers the TLS
    // handshake, so the upstream never becomes ready and everything the relay
    // reads from ffmpeg waits inside the relay. On loopback the kernel will
    // buffer tens of megabytes of a plain connection, which would hide the
    // relay's own behaviour; this cannot.
    QTcpServer ingest;
    QVERIFY(ingest.listen(QHostAddress::LocalHost));
    QTcpSocket* silent = nullptr;
    QObject::connect(&ingest, &QTcpServer::newConnection, &ingest,
                     [&] { silent = ingest.nextPendingConnection(); });

    RtmpKeyRelay relay;
    QString error;
    const QString local = relay.start(
        QStringLiteral("rtmps://127.0.0.1:%1/app/live_secret_key").arg(ingest.serverPort()), &error);
    QVERIFY2(!local.isEmpty(), qPrintable(error));

    const QUrl url(local);
    QTcpSocket publisher;
    publisher.connectToHost(url.host(), quint16(url.port()));
    QVERIFY(publisher.waitForConnected(5000));
    QTRY_VERIFY(silent != nullptr);

    // ffmpeg publishing far more than can go anywhere.
    constexpr qint64 kSent = 16 * 1024 * 1024;
    publisher.write(QByteArray(kSent, '\0'));
    QTest::qWait(1500);

    // The relay filled to its bound and stopped reading. Before, it read all
    // of it into memory, and on a slow uplink kept doing so without limit.
    QVERIFY2(relay.backlog() >= RtmpKeyRelay::kMaxBacklog,
             qPrintable(QString::number(relay.backlog())));
    QVERIFY2(relay.backlog() <= RtmpKeyRelay::kMaxBacklog + 1024 * 1024,
             qPrintable(QString::number(relay.backlog())));
    relay.stop();
    publisher.abort();
}

void MalloyModelTests::aRelayThatHeldBackStillDeliversEverything() {
    // An ingest that stalls, long enough for the relay to reach its bound and
    // stop reading from ffmpeg, and then catches up. Holding off must not
    // strand anything: once the uplink drains, reading resumes and every byte
    // arrives. The stalled side keeps a small window and reads nothing, so
    // the kernel cannot soak up the excess the way loopback otherwise does.
    QTcpServer ingest;
    QVERIFY(ingest.listen(QHostAddress::LocalHost));
    QTcpSocket* upstream = nullptr;
    qint64 received = 0;
    QObject::connect(&ingest, &QTcpServer::newConnection, &ingest, [&] {
        upstream = ingest.nextPendingConnection();
        upstream->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 8192);
        upstream->setReadBufferSize(1);
    });

    RtmpKeyRelay relay;
    QString error;
    const QString local = relay.start(
        QStringLiteral("rtmp://127.0.0.1:%1/app/live_secret_key").arg(ingest.serverPort()), &error);
    QVERIFY2(!local.isEmpty(), qPrintable(error));

    const QUrl url(local);
    QTcpSocket publisher;
    publisher.connectToHost(url.host(), quint16(url.port()));
    QVERIFY(publisher.waitForConnected(5000));
    QTRY_VERIFY(upstream != nullptr);

    constexpr qint64 kSent = 6 * 1024 * 1024;
    publisher.write(QByteArray(kSent, '\0'));
    QTRY_VERIFY_WITH_TIMEOUT(relay.backlog() >= RtmpKeyRelay::kMaxBacklog, 10000);
    // Long enough for the relay's read buffer on the ffmpeg side to fill, after
    // which ffmpeg's socket reports nothing new, and only progress upstream
    // can start the relay reading again.
    QTest::qWait(1000);
    QVERIFY(relay.backlog() <= RtmpKeyRelay::kMaxBacklog + 1024 * 1024);

    // The uplink recovers.
    QObject::connect(upstream, &QTcpSocket::readyRead, upstream,
                     [&] { received += upstream->readAll().size(); });
    upstream->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 1024 * 1024);
    upstream->setReadBufferSize(0);
    received += upstream->readAll().size();
    QTRY_COMPARE_WITH_TIMEOUT(received, kSent, 30000);
    relay.stop();
    publisher.abort();
}

QTEST_MAIN(MalloyModelTests)
#include "model_tests.moc"
