#include "platform/TwitchApi.h"
#include "platform/TwitchAuth.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>

namespace {
const QString kDefaultApiBase = QStringLiteral("https://api.twitch.tv");

QJsonObject firstDataObject(const QByteArray& json, bool* ok) {
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    const QJsonArray data = root.value(QStringLiteral("data")).toArray();
    if (data.isEmpty()) {
        if (ok) *ok = false;
        return root;   // handed back so the caller can look for an error message
    }
    if (ok) *ok = true;
    return data.first().toObject();
}
}  // namespace

QStringList TwitchApi::requiredScopes() {
    return {QStringLiteral("channel:read:stream_key"),
            QStringLiteral("channel:manage:broadcast")};
}

TwitchApi::TwitchApi(TwitchAuth* auth, QObject* parent)
    : QObject(parent), m_auth(auth), m_apiBase(kDefaultApiBase) {
    m_net = new QNetworkAccessManager(this);
}

void TwitchApi::setApiBase(const QString& baseUrl) {
    m_apiBase = baseUrl.isEmpty() ? kDefaultApiBase : baseUrl;
    while (m_apiBase.endsWith(QLatin1Char('/'))) m_apiBase.chop(1);
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

QString TwitchApi::parseHelixError(const QByteArray& json) {
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    const QString message = o.value(QStringLiteral("message")).toString().trimmed();
    const QString error   = o.value(QStringLiteral("error")).toString().trimmed();
    if (!message.isEmpty() && !error.isEmpty())
        return QStringLiteral("%1: %2").arg(error, message);
    if (!message.isEmpty()) return message;
    return error;
}

QString TwitchApi::parseUserId(const QByteArray& json, QString* error) {
    bool ok = false;
    const QJsonObject first = firstDataObject(json, &ok);
    if (!ok) {
        if (error) {
            const QString apiError = parseHelixError(json);
            *error = apiError.isEmpty() ? QObject::tr("Twitch returned no user.") : apiError;
        }
        return QString();
    }
    const QString id = first.value(QStringLiteral("id")).toString();
    if (id.isEmpty() && error) *error = QObject::tr("Twitch returned a user without an id.");
    return id;
}

QString TwitchApi::parseStreamKey(const QByteArray& json, QString* error) {
    bool ok = false;
    const QJsonObject first = firstDataObject(json, &ok);
    if (!ok) {
        if (error) {
            const QString apiError = parseHelixError(json);
            *error = apiError.isEmpty() ? QObject::tr("Twitch returned no stream key.") : apiError;
        }
        return QString();
    }
    const QString key = first.value(QStringLiteral("stream_key")).toString();
    if (key.isEmpty() && error) *error = QObject::tr("Twitch returned an empty stream key.");
    return key;
}

QString TwitchApi::parseGameId(const QByteArray& json, QString* error) {
    bool ok = false;
    const QJsonObject first = firstDataObject(json, &ok);
    if (!ok) {
        // No match is not an error: the title still gets set, the category is
        // just left alone.
        if (error) error->clear();
        return QString();
    }
    return first.value(QStringLiteral("id")).toString();
}

// ---------------------------------------------------------------------------
// Calls
// ---------------------------------------------------------------------------

void TwitchApi::fetchUserId(std::function<void(bool, QString)> done) {
    if (!done) return;
    if (!m_userId.isEmpty()) {
        done(true, m_userId);
        return;
    }
    if (!m_auth) {
        done(false, tr("Twitch is not configured."));
        return;
    }

    m_auth->withAccessToken([this, done](bool ok, const QString& tokenOrError) {
        if (!ok) { done(false, tokenOrError); return; }

        QNetworkRequest req{QUrl(m_apiBase + QStringLiteral("/helix/users"))};
        req.setRawHeader("Client-Id", m_auth->clientId().toUtf8());
        req.setRawHeader("Authorization", QByteArray("Bearer ") + tokenOrError.toUtf8());

        QNetworkReply* reply = m_net->get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply, done] {
            reply->deleteLater();
            QString error;
            const QString id = parseUserId(reply->readAll(), &error);
            if (id.isEmpty()) {
                done(false, error.isEmpty() ? reply->errorString() : error);
                return;
            }
            m_userId = id;
            done(true, id);
        });
    });
}

void TwitchApi::fetchStreamKey(std::function<void(bool, QString)> done) {
    if (!done) return;
    fetchUserId([this, done](bool ok, const QString& idOrError) {
        if (!ok) { done(false, idOrError); return; }
        const QString userId = idOrError;

        m_auth->withAccessToken([this, done, userId](bool tokenOk, const QString& tokenOrError) {
            if (!tokenOk) { done(false, tokenOrError); return; }

            QUrl url(m_apiBase + QStringLiteral("/helix/streams/key"));
            QUrlQuery query;
            query.addQueryItem(QStringLiteral("broadcaster_id"), userId);
            url.setQuery(query);

            QNetworkRequest req{url};
            req.setRawHeader("Client-Id", m_auth->clientId().toUtf8());
            req.setRawHeader("Authorization", QByteArray("Bearer ") + tokenOrError.toUtf8());

            QNetworkReply* reply = m_net->get(req);
            connect(reply, &QNetworkReply::finished, this, [this, reply, done] {
                reply->deleteLater();
                QString error;
                const QString key = parseStreamKey(reply->readAll(), &error);
                if (key.isEmpty()) {
                    done(false, error.isEmpty() ? reply->errorString() : error);
                    return;
                }
                done(true, key);
            });
        });
    });
}

void TwitchApi::resolveGameId(const QString& name, std::function<void(QString)> done) {
    if (!done) return;
    if (name.trimmed().isEmpty()) { done(QString()); return; }

    m_auth->withAccessToken([this, name, done](bool ok, const QString& tokenOrError) {
        if (!ok) { done(QString()); return; }

        QUrl url(m_apiBase + QStringLiteral("/helix/games"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("name"), name.trimmed());
        url.setQuery(query);

        QNetworkRequest req{url};
        req.setRawHeader("Client-Id", m_auth->clientId().toUtf8());
        req.setRawHeader("Authorization", QByteArray("Bearer ") + tokenOrError.toUtf8());

        QNetworkReply* reply = m_net->get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply, done] {
            reply->deleteLater();
            QString error;
            done(parseGameId(reply->readAll(), &error));
        });
    });
}

void TwitchApi::updateChannel(const QString& title, const QString& categoryName,
                              std::function<void(bool, QString)> done) {
    if (!done) return;
    fetchUserId([this, title, categoryName, done](bool ok, const QString& idOrError) {
        if (!ok) { done(false, idOrError); return; }
        const QString userId = idOrError;

        // An unknown category must not cost the user their title, so the game
        // lookup failing simply leaves the category unchanged.
        resolveGameId(categoryName, [this, title, userId, done](const QString& gameId) {
            m_auth->withAccessToken(
                [this, title, userId, gameId, done](bool tokenOk, const QString& tokenOrError) {
                if (!tokenOk) { done(false, tokenOrError); return; }

                QUrl url(m_apiBase + QStringLiteral("/helix/channels"));
                QUrlQuery query;
                query.addQueryItem(QStringLiteral("broadcaster_id"), userId);
                url.setQuery(query);

                QJsonObject body;
                if (!title.trimmed().isEmpty())
                    body.insert(QStringLiteral("title"), title.trimmed());
                if (!gameId.isEmpty())
                    body.insert(QStringLiteral("game_id"), gameId);
                if (body.isEmpty()) { done(true, QString()); return; }

                QNetworkRequest req{url};
                req.setRawHeader("Client-Id", m_auth->clientId().toUtf8());
                req.setRawHeader("Authorization", QByteArray("Bearer ") + tokenOrError.toUtf8());
                req.setHeader(QNetworkRequest::ContentTypeHeader,
                              QStringLiteral("application/json"));

                QNetworkReply* reply = m_net->sendCustomRequest(
                    req, "PATCH", QJsonDocument(body).toJson(QJsonDocument::Compact));
                connect(reply, &QNetworkReply::finished, this, [reply, done] {
                    reply->deleteLater();
                    const QByteArray payload = reply->readAll();
                    const int status =
                        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    // 204 No Content is the success case here.
                    if (status >= 200 && status < 300) { done(true, QString()); return; }
                    const QString apiError = parseHelixError(payload);
                    done(false, apiError.isEmpty() ? reply->errorString() : apiError);
                });
            });
        });
    });
}
