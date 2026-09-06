#include "recording/RtmpKeyRelay.h"

#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>

#include <cstring>

namespace {
// Characters that are unambiguous in an RTMP path and in AMF strings.
constexpr char kPlaceholderChars[] =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
constexpr int kPlaceholderCharCount = sizeof(kPlaceholderChars) - 1;

// A key shorter than this is not worth relaying: the placeholder would be short
// enough to plausibly occur in the media payload by chance.
constexpr int kMinKeyLength = 8;

constexpr quint16 kDefaultRtmpPort = 1935;
}  // namespace

RtmpKeyRelay::RtmpKeyRelay(QObject* parent) : QObject(parent) {}

RtmpKeyRelay::~RtmpKeyRelay() {
    stop();
}

QString RtmpKeyRelay::makePlaceholder(int length) {
    QString out;
    out.reserve(length);
    for (int i = 0; i < length; ++i) {
        const quint32 pick = QRandomGenerator::system()->bounded(kPlaceholderCharCount);
        out.append(QLatin1Char(kPlaceholderChars[pick]));
    }
    return out;
}

int RtmpKeyRelay::substituteAll(QByteArray& data, const QByteArray& from, const QByteArray& to) {
    if (from.isEmpty() || from.size() != to.size()) return 0;
    int count = 0;
    int at = 0;
    while ((at = data.indexOf(from, at)) >= 0) {
        data.replace(at, from.size(), to);
        at += to.size();
        ++count;
    }
    return count;
}

int RtmpKeyRelay::holdBackLength(const QByteArray& data, const QByteArray& needle) {
    if (needle.size() < 2 || data.isEmpty()) return 0;
    const int maxLen = qMin(static_cast<int>(data.size()), static_cast<int>(needle.size()) - 1);
    for (int len = maxLen; len > 0; --len) {
        if (std::memcmp(data.constData() + data.size() - len, needle.constData(),
                        static_cast<size_t>(len)) == 0)
            return len;
    }
    return 0;
}

QString RtmpKeyRelay::start(const QString& realUrl, QString* error) {
    auto bail = [error](const QString& message) {
        if (error) *error = message;
        return QString();
    };

    stop();

    const QUrl url(realUrl);
    if (!url.isValid() || url.host().isEmpty())
        return bail(tr("The stream URL could not be parsed."));

    const QString path = url.path();
    const int lastSlash = path.lastIndexOf(QLatin1Char('/'));
    if (lastSlash < 0 || lastSlash + 1 >= path.size())
        return bail(tr("The stream URL has no key segment to protect."));

    const QString key = path.mid(lastSlash + 1);
    if (key.size() < kMinKeyLength)
        return bail(tr("The stream key is too short to relay safely."));

    m_secret = key.toUtf8();
    m_placeholder = makePlaceholder(m_secret.size()).toUtf8();
    // Byte lengths must match exactly, or every AMF string length and RTMP
    // message length downstream of the substitution would be wrong.
    if (m_placeholder.size() != m_secret.size())
        return bail(tr("Could not build a placeholder for the stream key."));

    m_substitutions = 0;
    m_upstreamHost = url.host();
    m_upstreamPort = url.port(kDefaultRtmpPort) > 0
                         ? static_cast<quint16>(url.port(kDefaultRtmpPort))
                         : kDefaultRtmpPort;

    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &RtmpKeyRelay::onIncomingConnection);
    // Loopback only: nothing outside this machine may publish through the relay.
    if (!m_server->listen(QHostAddress::LocalHost, 0)) {
        const QString why = m_server->errorString();
        stop();
        return bail(tr("Could not open the local relay: %1").arg(why));
    }

    // The path keeps its app segment, because the upstream server routes on it
    // and the bytes are forwarded verbatim. Only the key segment changes.
    QString relayPath = path;
    relayPath.replace(lastSlash + 1, key.size(), QString::fromUtf8(m_placeholder));

    QUrl relayUrl;
    relayUrl.setScheme(QStringLiteral("rtmp"));
    relayUrl.setHost(QStringLiteral("127.0.0.1"));
    relayUrl.setPort(m_server->serverPort());
    relayUrl.setPath(relayPath);
    if (error) error->clear();
    return relayUrl.toString();
}

void RtmpKeyRelay::stop() {
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
    if (m_client) {
        m_client->disconnect(this);
        m_client->abort();
        m_client->deleteLater();
        m_client = nullptr;
    }
    if (m_upstream) {
        m_upstream->disconnect(this);
        m_upstream->abort();
        m_upstream->deleteLater();
        m_upstream = nullptr;
    }
    m_pending.clear();
    m_outboundQueue.clear();
    m_upstreamReady = false;
    // The substitution count deliberately survives stop(): it is the evidence
    // that the relay did its job, and it is read after a session ends. start()
    // resets it for the next one.
    // The key does not outlive the session.
    m_secret.fill('\0');
    m_secret.clear();
    m_placeholder.clear();
}

bool RtmpKeyRelay::isListening() const {
    return m_server && m_server->isListening();
}

void RtmpKeyRelay::onIncomingConnection() {
    if (!m_server) return;
    QTcpSocket* incoming = m_server->nextPendingConnection();
    if (!incoming) return;

    // Exactly one publisher, then the door closes. This is what stops another
    // local process from publishing through the relay under the real key.
    if (m_client) {
        incoming->abort();
        incoming->deleteLater();
        return;
    }
    m_client = incoming;
    m_server->close();

    connect(m_client, &QTcpSocket::readyRead, this, &RtmpKeyRelay::onClientReadable);
    connect(m_client, &QTcpSocket::disconnected, this, &RtmpKeyRelay::onSocketError);
    connect(m_client, &QTcpSocket::errorOccurred, this, &RtmpKeyRelay::onSocketError);

    m_upstream = new QTcpSocket(this);
    connect(m_upstream, &QTcpSocket::connected, this, &RtmpKeyRelay::onUpstreamConnected);
    connect(m_upstream, &QTcpSocket::readyRead, this, &RtmpKeyRelay::onUpstreamReadable);
    connect(m_upstream, &QTcpSocket::disconnected, this, &RtmpKeyRelay::onSocketError);
    connect(m_upstream, &QTcpSocket::errorOccurred, this, &RtmpKeyRelay::onSocketError);
    m_upstream->connectToHost(m_upstreamHost, m_upstreamPort);
}

void RtmpKeyRelay::onUpstreamConnected() {
    m_upstreamReady = true;
    if (!m_outboundQueue.isEmpty() && m_upstream) {
        m_upstream->write(m_outboundQueue);
        m_outboundQueue.clear();
    }
}

void RtmpKeyRelay::onClientReadable() {
    if (!m_client) return;
    m_pending += m_client->readAll();

    // Hold back only what could be the beginning of a split placeholder. A
    // fixed-size hold-back stalls the handshake, which contains no placeholder
    // at all and so would never release the withheld bytes.
    const int hold = holdBackLength(m_pending, m_placeholder);
    QByteArray forward = m_pending.left(m_pending.size() - hold);
    m_pending = m_pending.right(hold);

    m_substitutions += substituteAll(forward, m_placeholder, m_secret);

    if (forward.isEmpty()) return;
    if (m_upstreamReady && m_upstream)
        m_upstream->write(forward);
    else
        m_outboundQueue += forward;
}

void RtmpKeyRelay::onUpstreamReadable() {
    if (!m_upstream || !m_client) return;
    // Nothing to rewrite on the way back: the key only travels outbound.
    m_client->write(m_upstream->readAll());
}

void RtmpKeyRelay::onSocketError() {
    // Either side going away ends the session. Streaming stops on its own when
    // ffmpeg's connection drops; this makes sure the other socket does too.
    const bool clientGone = !m_client || m_client->state() == QAbstractSocket::UnconnectedState;
    const bool upstreamGone = !m_upstream || m_upstream->state() == QAbstractSocket::UnconnectedState;
    if (!clientGone && !upstreamGone) return;

    if (m_upstreamReady || clientGone) {
        // A normal end of stream: ffmpeg closed, or the ingest did.
        stop();
        return;
    }
    fail(tr("The relay could not reach %1:%2.").arg(m_upstreamHost).arg(m_upstreamPort));
}

void RtmpKeyRelay::fail(const QString& message) {
    stop();
    emit failed(message);
}
