#include <QtTest>

#include "recording/OutputSettings.h"
#include "recording/StreamSettings.h"

#include <QJsonArray>
#include <QSettings>
#include <QTemporaryDir>

#include <limits>

class OutputSettingsTests : public QObject {
    Q_OBJECT

private slots:
    void emptyStoreUsesDefaults();
    void outputNumericBounds_data();
    void outputNumericBounds();
    void malformedNumbersUseDefaults();
    void codecPresets_data();
    void codecPresets();
    void unknownOutputStrings();
    void jsonUsesTheSameContract();
    void customFrameRatesSurvive();
    void streamingValuesAreBounded();
    void customStreamUrls_data();
    void customStreamUrls();
};

void OutputSettingsTests::emptyStoreUsesDefaults() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    QCOMPARE(OutputSettings::load(settings).toJson(), OutputSettings{}.toJson());
    const StreamSettings stream = StreamSettings::load(settings);
    QCOMPARE(stream.bitrateKbps, 4500);
    QCOMPARE(stream.keyframeSec, 2);
    QCOMPARE(stream.service, StreamSettings::Service::Twitch);
    QVERIFY(stream.streamKey.isEmpty());
    QVERIFY(settings.allKeys().isEmpty());
}

void OutputSettingsTests::outputNumericBounds_data() {
    QTest::addColumn<QString>("field");
    QTest::addColumn<QVariant>("stored");
    QTest::addColumn<int>("expected");
    struct Bound { const char* field; int minimum; int maximum; };
    const Bound bounds[]{
        {"width", 320, 7680}, {"height", 180, 4320}, {"fps", 1, 1000},
        {"crf", 0, 51}, {"audioBitratekbps", 64, 320},
        {"bitrateKbps", 500, 51000}, {"replayBufferSeconds", 0, 300},
        {"keyframeSec", 0, 10}
    };
    for (const auto& bound : bounds) {
        const QString field = QString::fromLatin1(bound.field);
        QTest::newRow(qPrintable(field + QStringLiteral("-below")))
            << field << QVariant(bound.minimum - 1) << bound.minimum;
        QTest::newRow(qPrintable(field + QStringLiteral("-above")))
            << field << QVariant(bound.maximum + 1) << bound.maximum;
        QTest::newRow(qPrintable(field + QStringLiteral("-uint64")))
            << field << QVariant::fromValue(std::numeric_limits<qulonglong>::max()) << bound.maximum;
        QTest::newRow(qPrintable(field + QStringLiteral("-int64")))
            << field << QVariant::fromValue(std::numeric_limits<qlonglong>::min()) << bound.minimum;
        QTest::newRow(qPrintable(field + QStringLiteral("-huge-double")))
            << field << QVariant(1e300) << bound.maximum;
        QTest::newRow(qPrintable(field + QStringLiteral("-huge-string")))
            << field << QVariant(QStringLiteral("-1e300")) << bound.minimum;
    }
}

void OutputSettingsTests::outputNumericBounds() {
    QFETCH(QString, field);
    QFETCH(QVariant, stored);
    QFETCH(int, expected);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString filename = directory.filePath(QStringLiteral("settings.ini"));
    const QString key = QStringLiteral("output/") + field;
    {
        QSettings writer(filename, QSettings::IniFormat);
        writer.setValue(key, stored);
        writer.sync();
        QCOMPARE(writer.status(), QSettings::NoError);
    }
    QSettings settings(filename, QSettings::IniFormat);
    const QVariant before = settings.value(key);
    const OutputSettings output = OutputSettings::load(settings);
    const int actual = field == QLatin1String("replayBufferSeconds")
        ? output.replayBufferSeconds : output.toJson().value(field).toInt();
    QCOMPARE(actual, expected);
    QCOMPARE(settings.value(key), before);
    QCOMPARE(settings.allKeys(), QStringList{key});
}

void OutputSettingsTests::malformedNumbersUseDefaults() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    const QList<QVariant> malformed{
        QStringLiteral("not a number"), QStringLiteral(""), QVariant(true),
        QVariant(QStringList{QStringLiteral("4500")}), QVariant(12.5),
        QVariant(std::numeric_limits<double>::infinity()),
        QVariant(-std::numeric_limits<double>::infinity()),
        QVariant(std::numeric_limits<double>::quiet_NaN())
    };
    for (const QVariant& value : malformed) {
        settings.setValue(QStringLiteral("output/bitrateKbps"), value);
        settings.setValue(QStringLiteral("output/width"), value);
        settings.setValue(QStringLiteral("stream/bitrateKbps"), value);
        settings.setValue(QStringLiteral("stream/keyframeSec"), value);
        const OutputSettings output = OutputSettings::load(settings);
        const StreamSettings stream = StreamSettings::load(settings);
        QCOMPARE(output.bitrateKbps, 4500);
        QCOMPARE(output.width, 1920);
        QCOMPARE(stream.bitrateKbps, 4500);
        QCOMPARE(stream.keyframeSec, 2);
    }
}

void OutputSettingsTests::codecPresets_data() {
    QTest::addColumn<QString>("codec");
    QTest::addColumn<QString>("preset");
    QTest::addColumn<QString>("expectedCodec");
    QTest::addColumn<QString>("expectedPreset");
    const auto row = [](const char* name, const char* codec, const char* preset,
                        const char* expectedCodec, const char* expectedPreset) {
        QTest::newRow(name) << QString::fromLatin1(codec) << QString::fromLatin1(preset)
            << QString::fromLatin1(expectedCodec) << QString::fromLatin1(expectedPreset);
    };
    row("x264-kept", "libx264", "ultrafast", "libx264", "ultrafast");
    row("x265-kept", "libx265", "veryslow", "libx265", "veryslow");
    row("x264-wrong-family", "libx264", "p5", "libx264", "veryfast");
    row("nvenc-kept", "h264_nvenc", "p5", "h264_nvenc", "p5");
    row("nvenc-default", "hevc_nvenc", "veryfast", "hevc_nvenc", "p4");
    row("qsv-kept", "h264_qsv", "slow", "h264_qsv", "slow");
    row("qsv-default", "hevc_qsv", "ultrafast", "hevc_qsv", "medium");
    row("amf-kept", "h264_amf", "quality", "h264_amf", "quality");
    row("amf-default", "hevc_amf", "bad", "hevc_amf", "balanced");
    row("unknown", "unknown_encoder", "p5", "libx264", "veryfast");
    row("option-like", "-some-option", "-some-option", "libx264", "veryfast");
}

void OutputSettingsTests::codecPresets() {
    QFETCH(QString, codec);
    QFETCH(QString, preset);
    QFETCH(QString, expectedCodec);
    QFETCH(QString, expectedPreset);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    settings.setValue(QStringLiteral("output/videoCodec"), codec);
    settings.setValue(QStringLiteral("output/preset"), preset);
    const OutputSettings output = OutputSettings::load(settings);
    QCOMPARE(output.videoCodec, expectedCodec);
    QCOMPARE(output.preset, expectedPreset);
    const OutputSettings json = OutputSettings::fromJson({{QStringLiteral("videoCodec"), codec},
                                                        {QStringLiteral("preset"), preset}});
    QCOMPARE(json.videoCodec, expectedCodec);
    QCOMPARE(json.preset, expectedPreset);
}

void OutputSettingsTests::unknownOutputStrings() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    settings.setValue(QStringLiteral("output/audioCodec"), QStringLiteral("arbitrary"));
    settings.setValue(QStringLiteral("output/container"), QStringLiteral("../other"));
    const OutputSettings output = OutputSettings::load(settings);
    QCOMPARE(output.audioCodec, QStringLiteral("aac"));
    QCOMPARE(output.container, QStringLiteral("mp4"));
    for (const QString& codec : {QStringLiteral("aac"), QStringLiteral("opus"), QStringLiteral("libopus")}) {
        settings.setValue(QStringLiteral("output/audioCodec"), codec);
        settings.setValue(QStringLiteral("output/container"), QStringLiteral("mkv"));
        const OutputSettings supported = OutputSettings::load(settings);
        QCOMPARE(supported.audioCodec, codec);
        QCOMPARE(supported.container, QStringLiteral("mkv"));
    }
}

void OutputSettingsTests::jsonUsesTheSameContract() {
    const OutputSettings output = OutputSettings::fromJson({
        {QStringLiteral("width"), 1e300}, {QStringLiteral("height"), -1e300},
        {QStringLiteral("fps"), 2e9}, {QStringLiteral("bitrateKbps"), 2e9},
        {QStringLiteral("crf"), QJsonArray{}}, {QStringLiteral("audioBitratekbps"), true},
        {QStringLiteral("keyframeSec"), -10}, {QStringLiteral("audioCodec"), QStringLiteral("unknown")},
        {QStringLiteral("container"), QStringLiteral("unknown")}
    });
    QCOMPARE(output.width, 7680);
    QCOMPARE(output.height, 180);
    QCOMPARE(output.fps, 1000);
    QCOMPARE(output.bitrateKbps, 51000);
    QCOMPARE(output.bitrateKbps * 2, 102000);
    QCOMPARE(output.fps * 10, 10000);
    QCOMPARE(output.crf, 23);
    QCOMPARE(output.audioBitratekbps, 192);
    QCOMPARE(output.keyframeSec, 0);
    QCOMPARE(output.audioCodec, QStringLiteral("aac"));
    QCOMPARE(output.container, QStringLiteral("mp4"));
    QCOMPARE(output.replayBufferSeconds, 0);

    const OutputSettings odd = OutputSettings::fromJson({{QStringLiteral("width"), 1921},
                                                        {QStringLiteral("height"), 1081}});
    QCOMPARE(odd.width, 1920);
    QCOMPARE(odd.height, 1080);
}

void OutputSettingsTests::customFrameRatesSurvive() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    for (int fps : {37, 120, 144, 240, 1000}) {
        settings.setValue(QStringLiteral("output/fps"), fps);
        QCOMPARE(OutputSettings::load(settings).fps, fps);
        QCOMPARE(OutputSettings::fromJson({{QStringLiteral("fps"), fps}}).fps, fps);
    }
}

void OutputSettingsTests::streamingValuesAreBounded() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    settings.setValue(QStringLiteral("stream/service"), QStringLiteral("unknown"));
    settings.setValue(QStringLiteral("stream/bitrateKbps"), QVariant::fromValue(std::numeric_limits<qulonglong>::max()));
    settings.setValue(QStringLiteral("stream/keyframeSec"), 1e300);
    settings.setValue(QStringLiteral("stream/title"), QStringLiteral("Existing title"));
    settings.setValue(QStringLiteral("stream/streamKey"), QStringLiteral("must-not-be-read"));
    const StreamSettings upper = StreamSettings::load(settings);
    QCOMPARE(upper.service, StreamSettings::Service::Twitch);
    QCOMPARE(upper.bitrateKbps, 51000);
    QCOMPARE(upper.keyframeSec, 10);
    QCOMPARE(upper.title, QStringLiteral("Existing title"));
    QVERIFY(upper.streamKey.isEmpty());
    settings.setValue(QStringLiteral("stream/service"), QStringLiteral("youtube"));
    settings.setValue(QStringLiteral("stream/bitrateKbps"), -1e300);
    settings.setValue(QStringLiteral("stream/keyframeSec"), -100);
    const StreamSettings lower = StreamSettings::load(settings);
    QCOMPARE(lower.service, StreamSettings::Service::YouTube);
    QCOMPARE(lower.bitrateKbps, 500);
    QCOMPARE(lower.keyframeSec, 1);
}

void OutputSettingsTests::customStreamUrls_data() {
    QTest::addColumn<QString>("url");
    QTest::addColumn<bool>("accepted");
    QTest::newRow("rtmp") << QStringLiteral("rtmp://localhost:1935/live/{key}") << true;
    QTest::newRow("rtmps") << QStringLiteral("rtmps://example.com/live/{key}") << true;
    QTest::newRow("no-key-placeholder") << QStringLiteral("rtmp://example.com/live/channel") << true;
    QTest::newRow("local-file") << QStringLiteral("C:/recordings/replace.flv") << false;
    QTest::newRow("file-url") << QStringLiteral("file:///C:/recordings/replace.flv") << false;
    QTest::newRow("http") << QStringLiteral("https://example.com/live") << false;
    QTest::newRow("missing-host") << QStringLiteral("rtmp:///live/{key}") << false;
    QTest::newRow("newline") << QStringLiteral("rtmp://example.com/live/\n{key}") << false;
}

void OutputSettingsTests::customStreamUrls() {
    QFETCH(QString, url);
    QFETCH(bool, accepted);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    settings.setValue(QStringLiteral("stream/service"), QStringLiteral("custom"));
    settings.setValue(QStringLiteral("stream/customUrl"), url);
    const StreamSettings output = StreamSettings::load(settings);
    QCOMPARE(output.service, StreamSettings::Service::Custom);
    QCOMPARE(output.customUrl, accepted ? url : StreamSettings::templateFor(StreamSettings::Service::Custom));
    QCOMPARE(settings.value(QStringLiteral("stream/customUrl")).toString(), url);
}

QTEST_GUILESS_MAIN(OutputSettingsTests)
#include "output_settings_tests.moc"
