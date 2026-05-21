#include "audio/AudioController.h"
#include "capture/CaptureController.h"
#include "input/HotkeyManager.h"
#include "model/Canvas.h"
#include "model/FilterEffect.h"
#include "model/Scene.h"
#include "model/SceneCollection.h"
#include "model/SceneItem.h"
#include "model/Source.h"
#include "project/ProjectDocument.h"
#include "project/ClipsRegistry.h"
#include "project/ProjectRegistry.h"
#include "project/MediaRegistry.h"
#include "recording/RenderQueue.h"
#include "recording/OutputSettings.h"
#include "recording/EncoderPipeline.h"
#include "recording/RingTimedPcmSource.h"
#include "recording/StreamSettings.h"
#include "recording/StreamingPipeline.h"
#include "recording/EncoderRegistry.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonObject>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest/QtTest>
#include <QUndoStack>

#include <algorithm>
#include <cstdint>

class FakeCaptureSession final : public CaptureSession {
public:
    FakeCaptureSession(int adapterIndex, int outputIndex, QObject* parent = nullptr)
        : CaptureSession(parent), m_key(CaptureController::keyFor(adapterIndex, outputIndex))
    {}

    void startCapture() override { started.append(m_key); }
    void stopCapture() override { stopped.append(m_key); }

    static QStringList started;
    static QStringList stopped;

private:
    QString m_key;
};

QStringList FakeCaptureSession::started;
QStringList FakeCaptureSession::stopped;

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
    void addingAudioInputTriggersAudioInputsChanged();
    void togglingAudioInputVisibilityChangesGatherList();
    // v7 Tier 3: per-filter visibility toggle and the ffmpeg progress parser.
    // The filter-enabled flag is a small surface but easy to regress on
    // JSON round-trip, so we lock the schema with a test. The progress-line
    // parser anchors the streaming stats display.
    void filterEnabledFlagRoundtripsJson();
    void streamProgressLineParsesBitrateAndDrops();
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
};

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
    const QStringList args = x264->buildArgs(OutputSettings{});
    QVERIFY(args.contains(QStringLiteral("libx264")));
    QVERIFY(args.contains(QStringLiteral("-crf")));
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
    // HotkeyManager::setBinding() must persist to QSettings so that a fresh
    // instance can retrieve the same binding via loadBindings().
    // We bind F12 to kRecordToggle; RegisterHotKey may fail in CI (no user
    // session) but the QSettings round-trip must succeed regardless.
    const QKeySequence seq(Qt::Key_F12);
    const QLatin1String action(HotkeyManager::kRecordToggle);

    {
        HotkeyManager mgr;
        mgr.setBinding(action, seq);
        QCOMPARE(mgr.binding(action), seq);
    }

    // Verify QSettings was written.
    {
        const QSettings s;
        const QString stored =
            s.value(QStringLiteral("hotkeys/") + action).toString();
        QCOMPARE(QKeySequence(stored), seq);
    }

    // A fresh manager loading persisted bindings must reproduce the same key.
    {
        HotkeyManager mgr;
        mgr.loadBindings();
        QCOMPARE(mgr.binding(action), seq);
    }

    // Cleanup: clear so subsequent runs start fresh.
    HotkeyManager mgr;
    mgr.setBinding(action, QKeySequence());
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
    // In the test process WasapiCapture workers typically don't deliver any
    // chunks (no real default endpoint), so every ring stays empty. That is
    // exactly the broken case the bug used to fail in.

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
    //     hevc_nvenc is selected. NVENC uses `-preset p4 -rc cbr -b:v`. The
    //     registry's NVENC lambda hard-codes "p4", so even when the saved
    //     OutputSettings.preset is "faster" the emitted args never contain
    //     "-preset faster". (Test only runs when NVENC is available — most
    //     CI machines won't have it, in which case we skip silently.)
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
        QVERIFY(args.contains(QStringLiteral("cbr")));
        QVERIFY(args.contains(QStringLiteral("-b:v")));
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
    int kbps = -1, drops = -1;

    // Canonical line: bitrate + drop together.
    {
        const QString line = QStringLiteral(
            "frame= 1234 fps= 60 q=23.0 size= 4096kB time=00:00:20.00 "
            "bitrate=1700.6kbits/s drop=3 speed=1.0x");
        QVERIFY(EncoderPipeline::tryParseProgressLine(line, &kbps, &drops));
        QCOMPARE(kbps, 1701);
        QCOMPARE(drops, 3);
    }

    // Line without `drop=` (first second of a stream): drops defaults to 0.
    {
        kbps = drops = -1;
        const QString line = QStringLiteral(
            "frame= 30 fps=30.0 q=18.0 size= 128kB time=00:00:01.00 "
            "bitrate=1024.0kbits/s speed=1.0x");
        QVERIFY(EncoderPipeline::tryParseProgressLine(line, &kbps, &drops));
        QCOMPARE(kbps, 1024);
        QCOMPARE(drops, 0);
    }

    // Line with no `bitrate=` token (compile/info noise): parser must reject.
    {
        kbps = 99; drops = 99;
        const QString line = QStringLiteral("hevc_nvenc: GPU encoding session opened");
        QVERIFY(!EncoderPipeline::tryParseProgressLine(line, &kbps, &drops));
        // Out-params unchanged on failure.
        QCOMPARE(kbps, 99);
        QCOMPARE(drops, 99);
    }
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

    q.enqueue(QStringLiteral("a.mp4"), QStringLiteral("1080p60"), QStringLiteral("Proj"));
    q.enqueue(QStringLiteral("b.mp4"), QStringLiteral("1080p60"), QStringLiteral("Proj"));
    QCOMPARE(q.jobs().size(), 2);
    QCOMPARE(q.countOfState(RenderJob::Active), 1);    // first promoted immediately
    QCOMPARE(q.countOfState(RenderJob::Pending), 1);

    for (int i = 0; i < 100 && q.countOfState(RenderJob::Completed) == 0; ++i)
        q.advanceForTest();
    QCOMPARE(q.countOfState(RenderJob::Completed), 1);
    QCOMPARE(q.countOfState(RenderJob::Active), 1);     // second promoted

    // Reload from the same store: completed history survives; active requeues.
    RenderQueue q2;
    q2.setStorePath(store);
    QCOMPARE(q2.jobs().size(), 2);
    QCOMPARE(q2.countOfState(RenderJob::Completed), 1);
}

QTEST_MAIN(MalloyModelTests)
#include "model_tests.moc"
