#include "platform/TwitchAuth.h"
#include "platform/CredentialStore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrlQuery>

namespace {
const QString kDefaultAuthBase = QStringLiteral("https://id.twitch.tv");
const QString kTokenCredential = QStringLiteral("MalloyStudio_TwitchTokens");

// Twitch asks for a minimum interval and can ask for a slower one mid-flow.
constexpr int kMinPollSecs = 1;
constexpr int kSlowDownStepSecs = 5;

QByteArray formEncode(const QList<QPair<QString, QString>>& fields) {
    QUrlQuery q;
    for (const auto& f : fields) q.addQueryItem(f.first, f.second);
    return q.toString(QUrl::FullyEncoded).toUtf8();
}
}  // namespace

// ---------------------------------------------------------------------------
// TwitchTokens
// ---------------------------------------------------------------------------

bool TwitchTokens::needsRefresh(int marginSecs) const {
    if (accessToken.isEmpty()) return true;
    if (!expiresAt.isValid()) return true;
    return QDateTime::currentDateTimeUtc().addSecs(marginSecs) >= expiresAt;
}

QString TwitchTokens::toJson() const {
    QJsonObject o;
    o.insert(QStringLiteral("access"), accessToken);
    o.insert(QStringLiteral("refresh"), refreshToken);
    o.insert(QStringLiteral("expiresAt"), expiresAt.toUTC().toString(Qt::ISODate));
    o.insert(QStringLiteral("scopes"), QJsonArray::fromStringList(scopes));
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

TwitchTokens TwitchTokens::fromJson(const QString& json) {
    TwitchTokens t;
    const QJsonObject o = QJsonDocument::fromJson(json.toUtf8()).object();
    t.accessToken  = o.value(QStringLiteral("access")).toString();
    t.refreshToken = o.value(QStringLiteral("refresh")).toString();
    t.expiresAt    = QDateTime::fromString(o.value(QStringLiteral("expiresAt")).toString(),
                                           Qt::ISODate);
    for (const auto& v : o.value(QStringLiteral("scopes")).toArray())
        t.scopes << v.toString();
    return t;
}

// ---------------------------------------------------------------------------
// Pure protocol helpers
// ---------------------------------------------------------------------------

QByteArray TwitchAuth::buildDeviceCodeBody(const QString& clientId, const QStringList& scopes) {
    return formEncode({{QStringLiteral("client_id"), clientId},
                       {QStringLiteral("scopes"), scopes.join(QLatin1Char(' '))}});
}

QByteArray TwitchAuth::buildDeviceTokenBody(const QString& clientId, const QStringList& scopes,
                                            const QString& deviceCode) {
    return formEncode({
        {QStringLiteral("client_id"), clientId},
        {QStringLiteral("scopes"), scopes.join(QLatin1Char(' '))},
        {QStringLiteral("device_code"), deviceCode},
        {QStringLiteral("grant_type"),
         QStringLiteral("urn:ietf:params:oauth:grant-type:device_code")},
    });
}

QByteArray TwitchAuth::buildRefreshBody(const QString& clientId, const QString& refreshToken) {
    return formEncode({
        {QStringLiteral("client_id"), clientId},
        {QStringLiteral("refresh_token"), refreshToken},
        {QStringLiteral("grant_type"), QStringLiteral("refresh_token")},
    });
}

bool TwitchAuth::parseDeviceCode(const QByteArray& json, TwitchDeviceCode* out, QString* error) {
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    if (o.isEmpty()) {
        if (error) *error = QObject::tr("Twitch returned an unreadable response.");
        return false;
    }
    if (o.contains(QStringLiteral("message")) && !o.contains(QStringLiteral("device_code"))) {
        if (error) *error = o.value(QStringLiteral("message")).toString();
        return false;
    }

    TwitchDeviceCode d;
    d.deviceCode      = o.value(QStringLiteral("device_code")).toString();
    d.userCode        = o.value(QStringLiteral("user_code")).toString();
    d.verificationUri = o.value(QStringLiteral("verification_uri")).toString();
    if (d.verificationUri.isEmpty())
        d.verificationUri = o.value(QStringLiteral("verification_uri_complete")).toString();
    d.expiresInSecs   = o.value(QStringLiteral("expires_in")).toInt(1800);
    d.intervalSecs    = qMax(kMinPollSecs, o.value(QStringLiteral("interval")).toInt(5));

    if (d.deviceCode.isEmpty() || d.userCode.isEmpty()) {
        if (error) *error = QObject::tr("Twitch did not return a device code.");
        return false;
    }
    if (out) *out = d;
    return true;
}

bool TwitchAuth::parseTokens(const QByteArray& json, TwitchTokens* out,
                             QString* pendingReason, QString* error) {
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    if (o.isEmpty()) {
        if (error) *error = QObject::tr("Twitch returned an unreadable response.");
        return false;
    }

    const QString access = o.value(QStringLiteral("access_token")).toString();
    if (access.isEmpty()) {
        // Twitch reports the waiting states in "message", with the OAuth error
        // name as the text.
        const QString message = o.value(QStringLiteral("message")).toString().trimmed();
        const QString errorName = o.value(QStringLiteral("error")).toString().trimmed();
        const QString reason = message.isEmpty() ? errorName : message;

        if (reason.compare(QStringLiteral("authorization_pending"), Qt::CaseInsensitive) == 0
            || reason.compare(QStringLiteral("slow_down"), Qt::CaseInsensitive) == 0) {
            if (pendingReason) *pendingReason = reason.toLower();
            return false;
        }
        if (error)
            *error = reason.isEmpty() ? QObject::tr("Twitch did not return an access token.")
                                      : reason;
        return false;
    }

    TwitchTokens t;
    t.accessToken  = access;
    t.refreshToken = o.value(QStringLiteral("refresh_token")).toString();
    const int expiresIn = o.value(QStringLiteral("expires_in")).toInt(0);
    t.expiresAt = expiresIn > 0 ? QDateTime::currentDateTimeUtc().addSecs(expiresIn)
                                : QDateTime::currentDateTimeUtc().addSecs(3600);

    // "scope" is an array in the device flow response.
    const QJsonValue scope = o.value(QStringLiteral("scope"));
    if (scope.isArray()) {
        for (const auto& v : scope.toArray()) t.scopes << v.toString();
    } else if (scope.isString()) {
        t.scopes = scope.toString().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    }

    if (out) *out = t;
    return true;
}

// ---------------------------------------------------------------------------
// TwitchAuth
// ---------------------------------------------------------------------------

TwitchAuth::TwitchAuth(QObject* parent)
    : QObject(parent), m_authBase(kDefaultAuthBase) {
    m_net = new QNetworkAccessManager(this);
    m_pollTimer = new QTimer(this);
    m_pollTimer->setSingleShot(true);
    connect(m_pollTimer, &QTimer::timeout, this, &TwitchAuth::pollOnce);
    loadTokens();
}

TwitchAuth::~TwitchAuth() = default;

void TwitchAuth::setClientId(const QString& clientId) { m_clientId = clientId.trimmed(); }

void TwitchAuth::setAuthBase(const QString& baseUrl) {
    m_authBase = baseUrl.isEmpty() ? kDefaultAuthBase : baseUrl;
    while (m_authBase.endsWith(QLatin1Char('/'))) m_authBase.chop(1);
}

bool TwitchAuth::isConnected() const { return !m_tokens.refreshToken.isEmpty(); }

void TwitchAuth::loadTokens() {
    m_tokens = TwitchTokens::fromJson(CredentialStore::load(kTokenCredential));
}

void TwitchAuth::storeTokens(const TwitchTokens& tokens) {
    m_tokens = tokens;
    CredentialStore::save(kTokenCredential, tokens.toJson());
}

void TwitchAuth::signOut() {
    cancelDeviceFlow();
    m_tokens = TwitchTokens{};
    CredentialStore::erase(kTokenCredential);
    emit signedOut();
}

void TwitchAuth::cancelDeviceFlow() {
    m_pollTimer->stop();
    m_device = TwitchDeviceCode{};
    m_deviceExpiresAt = QDateTime();
}

void TwitchAuth::finishWithError(const QString& message) {
    cancelDeviceFlow();
    emit failed(message);
}

void TwitchAuth::beginDeviceFlow(const QStringList& scopes) {
    if (m_clientId.isEmpty()) {
        emit failed(tr("No Twitch application client ID is configured."));
        return;
    }
    cancelDeviceFlow();
    m_requestedScopes = scopes;

    QNetworkRequest req{QUrl(m_authBase + QStringLiteral("/oauth2/device"))};
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/x-www-form-urlencoded"));

    QNetworkReply* reply = m_net->post(req, buildDeviceCodeBody(m_clientId, scopes));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const QByteArray body = reply->readAll();
        QString error;
        TwitchDeviceCode device;
        if (!parseDeviceCode(body, &device, &error)) {
            finishWithError(error.isEmpty() ? reply->errorString() : error);
            return;
        }
        m_device = device;
        m_deviceExpiresAt = QDateTime::currentDateTimeUtc().addSecs(device.expiresInSecs);
        emit deviceCodeReady(device.userCode, device.verificationUri, device.expiresInSecs);
        m_pollTimer->start(device.intervalSecs * 1000);
    });
}

void TwitchAuth::pollOnce() {
    if (m_device.deviceCode.isEmpty()) return;
    if (m_deviceExpiresAt.isValid() && QDateTime::currentDateTimeUtc() > m_deviceExpiresAt) {
        finishWithError(tr("The Twitch sign-in code expired before it was approved."));
        return;
    }

    QNetworkRequest req{QUrl(m_authBase + QStringLiteral("/oauth2/token"))};
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/x-www-form-urlencoded"));

    QNetworkReply* reply = m_net->post(
        req, buildDeviceTokenBody(m_clientId, m_requestedScopes, m_device.deviceCode));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const QByteArray body = reply->readAll();

        QString pending, error;
        TwitchTokens tokens;
        if (parseTokens(body, &tokens, &pending, &error)) {
            cancelDeviceFlow();
            storeTokens(tokens);
            emit connected();
            return;
        }
        if (!pending.isEmpty()) {
            // slow_down means Twitch wants a longer gap, and it stays longer.
            if (pending == QLatin1String("slow_down"))
                m_device.intervalSecs += kSlowDownStepSecs;
            m_pollTimer->start(m_device.intervalSecs * 1000);
            return;
        }
        finishWithError(error.isEmpty() ? reply->errorString() : error);
    });
}

void TwitchAuth::withAccessToken(std::function<void(bool, QString)> done) {
    if (!done) return;
    if (!isConnected()) {
        done(false, tr("No Twitch account is connected."));
        return;
    }
    if (!m_tokens.needsRefresh()) {
        done(true, m_tokens.accessToken);
        return;
    }
    refresh(std::move(done));
}

void TwitchAuth::refresh(std::function<void(bool, QString)> done) {
    if (m_clientId.isEmpty()) {
        done(false, tr("No Twitch application client ID is configured."));
        return;
    }
    if (m_refreshing) {
        done(false, tr("A token refresh is already in progress."));
        return;
    }
    m_refreshing = true;

    QNetworkRequest req{QUrl(m_authBase + QStringLiteral("/oauth2/token"))};
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/x-www-form-urlencoded"));

    QNetworkReply* reply = m_net->post(req, buildRefreshBody(m_clientId, m_tokens.refreshToken));
    connect(reply, &QNetworkReply::finished, this, [this, reply, done] {
        reply->deleteLater();
        m_refreshing = false;

        QString pending, error;
        TwitchTokens tokens;
        if (!parseTokens(reply->readAll(), &tokens, &pending, &error)) {
            // The refresh token is spent whether or not this succeeded, so a
            // failure here means the account is disconnected rather than
            // retryable. Say so instead of leaving a token that cannot work.
            m_tokens = TwitchTokens{};
            CredentialStore::erase(kTokenCredential);
            emit signedOut();
            done(false, error.isEmpty() ? tr("Twitch sign-in expired.") : error);
            return;
        }
        // Refresh tokens are one time use: persist the replacement immediately,
        // before anything can fail, or the account is locked out.
        if (tokens.refreshToken.isEmpty()) tokens.refreshToken = m_tokens.refreshToken;
        if (tokens.scopes.isEmpty()) tokens.scopes = m_tokens.scopes;
        storeTokens(tokens);
        done(true, tokens.accessToken);
    });
}
