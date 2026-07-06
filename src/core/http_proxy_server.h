#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QSet>
#include <QtCore/QStringList>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QTcpServer>

namespace net_tunnel {

struct ProxySettings {
    QString listenHost;
    int listenPort;
    QString upstreamBaseUrl;
    QString proxyPrefix;
    QString upstreamApiKey;
    QString upstreamUserAgent;
    bool forwardUserAgent;
    QString upstreamProxy;
    QString upstreamHttpProxy;
    QString upstreamHttpsProxy;
    QString upstreamSocksProxy;
    int upstreamTimeoutSec;
    int bufferTimeoutSec;
    qint64 requestBodyLimitBytes;
    qint64 responseBufferLimitBytes;
    int reasoning516RetryCount;
    QString interceptRuleMode;
    QStringList reasoningEquals;
    int guardRetryAttempts;
    bool retryUpstreamCapacityErrors;
    QStringList guardEndpoints;
    bool interceptStreaming;
    bool interceptNonStreaming;
    int nonStreamStatusCode;
    QString streamAction;

    ProxySettings();
};

class HttpProxyServer : public QObject {
    Q_OBJECT

public:
    explicit HttpProxyServer(QObject *parent = 0);
    ~HttpProxyServer();

    bool start(const ProxySettings &settings, QString *error = 0);
    void stop();
    bool isRunning() const;
    QString listenUrl() const;
    ProxySettings settings() const;
    QJsonObject statusPayload() const;

    void recordRequest(const QString &kind);
    void recordResult(const QString &kind,
                      const QString &method,
                      const QString &path,
                      int statusCode,
                      double latencyMs,
                      const QString &errorType = QString(),
                      const QString &errorMessage = QString(),
                      int reasoningTokens = -1,
                      const QString &requestKind = QString(),
                      const QString &interceptExemptReason = QString());
    void recordReasoningGuardRetry(const QString &method,
                                   const QString &path,
                                   int attempt,
                                   int maxRetries,
                                   int reasoningTokens,
                                   const QString &requestKind = QString(),
                                   const QString &interceptExemptReason = QString());
    void recordInspectedResponse(int reasoningTokens,
                                 bool matched,
                                 const QString &streamKind);
    void recordBlockedResponse(const QString &streamKind);
    void recordUpstreamAttempt();
    void recordBypassedProxyRequest();
    void recordTransferDiagnostics(const QJsonObject &diagnostics);

signals:
    void logLine(const QString &line);
    void started(const QString &url);
    void stopped();
    void statsChanged();

private slots:
    void acceptConnection();

private:
    Q_DISABLE_COPY(HttpProxyServer)

    QJsonObject healthPayload() const;
    QJsonObject runtimePayload() const;
    void configureUpstreamProxy();

    QTcpServer server_;
    QNetworkAccessManager manager_;
    ProxySettings settings_;
    QUrl upstreamBase_;
    QString upstreamBasePath_;
    QString proxyPrefix_;

    qint64 startedAtMs_;
    qint64 requestsTotal_;
    qint64 controlRequestsTotal_;
    qint64 healthRequestsTotal_;
    qint64 statusRequestsTotal_;
    qint64 interceptedRequestsTotal_;
    qint64 successfulRequestsTotal_;
    qint64 failedRequestsTotal_;
    qint64 proxyErrorTotal_;
    qint64 upstreamHttpErrorTotal_;
    qint64 clientConnectionErrorTotal_;
    qint64 bufferTimeoutTotal_;
    qint64 upstreamTimeoutTotal_;
    qint64 localProxyErrorTotal_;
    qint64 reasoningTokens516Total_;
    qint64 reasoningTokens516RetryTotal_;
    qint64 inspectedResponseCount_;
    qint64 bypassedProxyRequestCount_;
    qint64 matchedResponseCount_;
    qint64 matchedStreamingCount_;
    qint64 matchedNonStreamingCount_;
    qint64 blockedResponseCount_;
    qint64 blockedStreamingCount_;
    qint64 blockedNonStreamingCount_;
    qint64 guardRetryTotal_;
    qint64 upstreamAttemptsTotal_;
    QJsonObject observedReasoningCounts_;
    qint64 consecutiveFailures_;
    QJsonObject statusCodeCounts_;
    QJsonObject lastResult_;
    QJsonObject lastFailure_;
    QJsonObject lastTransferDiagnostics_;
    double lastLatencyMs_;
    double latencyTotalMs_;
    qint64 latencySamples_;
};

QString normalizePathPrefix(const QString &prefix);
QString joinPaths(const QString &basePath, const QString &suffix);
QStringList defaultReasoningEquals();
QStringList defaultGuardEndpoints();
qint64 defaultRequestBodyLimitBytes();
qint64 defaultResponseBufferLimitBytes();
QStringList normalizeIntegerList(const QString &values, const QStringList &fallback);
QStringList normalizePathList(const QString &values, const QStringList &fallback);

} // namespace net_tunnel
