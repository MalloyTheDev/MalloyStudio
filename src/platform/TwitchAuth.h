#pragma once

// Twitch sign-in for a desktop app, using the Device Code Grant Flow.
//
// Twitch does not support PKCE, so the flows a desktop app can actually use
// are: the implicit flow, which returns no refresh token and would make the
// user sign in constantly, and the device code flow, which works for a public
// client with no secret and does return one. Hence this.
//
// The user is shown a short code and a URL, approves in a browser, and this
// polls until Twitch hands over the tokens.
//
// Refresh tokens are ONE TIME USE: refreshing invalidates the token that was
// used, so the replacement must be stored before it is needed again or the
// account is effectively signed out. They also expire after 30 days of
// inactivity. Tokens live in the Windows Credential Manager next to the stream
// key, with the same caveat: code running as this user can read them.

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

struct TwitchTokens {
    QString   accessToken;
    QString   refreshToken;
    QDateTime expiresAt;
    QStringList scopes;

    bool isValid() const { return !accessToken.isEmpty(); }
    // True when the token has expired or is about to, so callers refresh
    // before a request rather than after a 401.
    bool needsRefresh(int marginSecs = 300) const;

    QString toJson() const;
    static TwitchTokens fromJson(const QString& json);
};

// What the user has to type in, returned when the flow starts.
struct TwitchDeviceCode {
    QString deviceCode;       // secret; polled with, never shown
    QString userCode;         // short code the user enters
    QString verificationUri;  // where they enter it
    int     expiresInSecs = 0;
    int     intervalSecs = 5; // minimum polling interval Twitch asks for
};

class TwitchAuth : public QObject {
    Q_OBJECT
public:
    explicit TwitchAuth(QObject* parent = nullptr);
    // Keeps its tokens under another credential name. For tests, so they
    // never read or disturb the user's own sign-in.
    TwitchAuth(const QString& credentialTarget, QObject* parent);
    ~TwitchAuth() override;

    // The client id of a Twitch application registered by the user. It is not a
    // secret (public clients cannot keep one), but it is per-installation
    // configuration rather than something to bake into the binary.
    void setClientId(const QString& clientId);
    QString clientId() const { return m_clientId; }

    // Overrides the id.twitch.tv base, so the flow can be exercised against a
    // local stand-in without a real Twitch application.
    void setAuthBase(const QString& baseUrl);

    bool isConnected() const;          // a refresh token is stored
    QString accessToken() const { return m_tokens.accessToken; }
    QStringList grantedScopes() const { return m_tokens.scopes; }

    // Starts the flow. deviceCodeReady() carries what to show the user.
    void beginDeviceFlow(const QStringList& scopes);
    void cancelDeviceFlow();

    // Forgets the tokens locally. Twitch keeps the authorization until the user
    // removes it in their account settings, which the UI should say. A refresh
    // or sign-in still in flight is disowned: what it brings back is dropped.
    void signOut();

    // Calls back with a usable access token, refreshing first if needed. A
    // refresh that gets no answer from Twitch keeps the account; only a
    // refresh Twitch refuses signs it out.
    void withAccessToken(std::function<void(bool ok, QString tokenOrError)> done);

    // ---- Pure protocol helpers, exposed for testing -----------------------

    static QByteArray buildDeviceCodeBody(const QString& clientId, const QStringList& scopes);
    static QByteArray buildDeviceTokenBody(const QString& clientId, const QStringList& scopes,
                                           const QString& deviceCode);
    static QByteArray buildRefreshBody(const QString& clientId, const QString& refreshToken);

    static bool parseDeviceCode(const QByteArray& json, TwitchDeviceCode* out, QString* error);
    // Parses a token response. On a pending or transient condition, sets
    // *pendingReason ("authorization_pending" or "slow_down") and returns false
    // without setting *error, so the caller knows to keep polling.
    static bool parseTokens(const QByteArray& json, TwitchTokens* out,
                            QString* pendingReason, QString* error);

signals:
    void deviceCodeReady(QString userCode, QString verificationUri, int expiresInSecs);
    void connected();
    void signedOut();
    void failed(QString message);

private slots:
    void pollOnce();

private:
    void storeTokens(const TwitchTokens& tokens);
    void loadTokens();
    void forgetTokens();
    void refresh(std::function<void(bool ok, QString tokenOrError)> done);
    void finishWithError(const QString& message);

    QNetworkAccessManager* m_net = nullptr;
    QTimer*  m_pollTimer = nullptr;
    QString  m_credentialTarget;
    QString  m_clientId;
    QString  m_authBase;
    QStringList m_requestedScopes;
    TwitchDeviceCode m_device;
    QDateTime m_deviceExpiresAt;
    TwitchTokens m_tokens;
    bool m_refreshing = false;

    // Replies carry the generation they were sent in and are dropped if it
    // has moved on. m_session moves when the tokens are forgotten, so a
    // refresh answered after a sign-out cannot store tokens again; m_flow
    // moves when a sign-in is cancelled or restarted, so a late device code
    // or poll answer cannot resume it.
    quint64 m_session = 0;
    quint64 m_flow = 0;
};
