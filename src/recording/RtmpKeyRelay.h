#pragma once

// Keeps the stream key out of ffmpeg's command line.
//
// ffmpeg takes its publish destination as a positional argument, and there is
// no way to hand it the key by any other route: file-sourced options cannot
// reach the rtmp_* AVOptions, -fpre parses them and then discards them, and
// there is no response-file or environment expansion. Any process running as
// this user can read another process's command line, so the key is visible to
// passive process listings, telemetry agents and crash dumps.
//
// So ffmpeg is given a placeholder instead. It publishes to a loopback listener
// here, and this relay substitutes the placeholder for the real key on the way
// upstream. The placeholder is generated with exactly the byte length of the
// key, which is what makes the substitution safe: no AMF string length changes,
// no RTMP message length changes, no re-chunking. The key exists only in this
// process's memory.
//
// KNOWN LIMITATION, and why this is opt-in: ffmpeg derives the `tcUrl` it sends
// in the RTMP `connect` command from the URL it was given, so the upstream
// server receives a tcUrl naming the loopback address rather than its own
// hostname. Servers generally route on `app` and ignore tcUrl, and this has been
// verified end to end against a local RTMP server, but it has NOT been verified
// against Twitch or YouTube. Rewriting tcUrl would change an AMF string length
// and require re-chunking the connect message, which is a much larger change.

#include <QByteArray>
#include <QObject>
#include <QString>

class QTcpServer;
class QTcpSocket;

class RtmpKeyRelay : public QObject {
    Q_OBJECT
public:
    explicit RtmpKeyRelay(QObject* parent = nullptr);
    ~RtmpKeyRelay() override;

    // Starts a single-use loopback listener for `realUrl` (which contains the
    // key) and returns the URL ffmpeg should publish to, carrying a placeholder
    // in place of the key. Returns an empty string and sets *error on failure.
    QString start(const QString& realUrl, QString* error = nullptr);

    void stop();
    bool isListening() const;
    bool isConnected() const { return m_upstreamReady; }

    // How many placeholder occurrences have been rewritten. A publishing
    // session substitutes at least three (releaseStream, FCPublish, publish);
    // zero by the time media flows means the relay is not doing its job.
    int substitutions() const { return m_substitutions; }

    // ---- Pure helpers, exposed for testing -------------------------------

    // A random placeholder of exactly `length` bytes, from a character set that
    // is safe in an RTMP path and will not collide with structural bytes.
    static QString makePlaceholder(int length);

    // Replaces every occurrence of `from` with `to` (which must be the same
    // length) and returns how many were replaced.
    static int substituteAll(QByteArray& data, const QByteArray& from, const QByteArray& to);

    // Number of trailing bytes of `data` that form a PROPER prefix of `needle`,
    // and so might be the start of an occurrence split across two reads. Those
    // bytes must be held back rather than forwarded.
    //
    // The "proper" part matters: holding back a fixed `needle.size() - 1` bytes
    // instead deadlocks the RTMP handshake, because the 1537-byte handshake
    // carries no placeholder and the withheld tail is never completed.
    static int holdBackLength(const QByteArray& data, const QByteArray& needle);

signals:
    // The relay could not carry the stream. The caller should surface this and
    // stop streaming: continuing would publish under the placeholder.
    void failed(QString message);

private slots:
    void onIncomingConnection();
    void onClientReadable();
    void onUpstreamReadable();
    void onUpstreamConnected();
    void onSocketError();

private:
    void fail(const QString& message);

    QTcpServer* m_server = nullptr;
    QTcpSocket* m_client = nullptr;     // ffmpeg
    QTcpSocket* m_upstream = nullptr;   // the real ingest

    QString    m_upstreamHost;
    quint16    m_upstreamPort = 1935;
    QByteArray m_placeholder;
    QByteArray m_secret;

    QByteArray m_pending;        // trailing bytes withheld pending a split match
    QByteArray m_outboundQueue;  // written before the upstream connection was up
    bool       m_upstreamReady = false;
    int        m_substitutions = 0;
};
