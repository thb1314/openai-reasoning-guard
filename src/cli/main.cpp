#include "core/app_config.h"
#include "core/http_proxy_server.h"

#include <QtCore/QCommandLineOption>
#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QTextStream>
#include <QtCore/QTimer>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

using namespace net_tunnel;

static void logLine(const QString &message)
{
    QTextStream(stdout) << "[" << QTime::currentTime().toString("HH:mm:ss") << "] "
                        << message << "\n";
    QTextStream(stdout).flush();
}

static int optionInt(const QCommandLineParser &parser,
                     const QCommandLineOption &option,
                     int fallback)
{
    if (!parser.isSet(option)) {
        return fallback;
    }
    bool ok = false;
    const int value = parser.value(option).toInt(&ok);
    return ok ? value : fallback;
}

static qint64 optionInt64(const QCommandLineParser &parser,
                          const QCommandLineOption &option,
                          qint64 fallback)
{
    if (!parser.isSet(option)) {
        return fallback;
    }
    bool ok = false;
    const qint64 value = parser.value(option).toLongLong(&ok);
    return ok ? value : fallback;
}

static QString optionString(const QCommandLineParser &parser,
                            const QCommandLineOption &option,
                            const QString &fallback)
{
    return parser.isSet(option) ? parser.value(option) : fallback;
}

static QString proxyTextWithDefaultScheme(const QString &text, bool socksFallback)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || trimmed.contains("://")) {
        return trimmed;
    }
    return QString("%1://%2").arg(socksFallback ? QString("socks5") : QString("http"), trimmed);
}

static int queryStatus(const QUrl &url)
{
    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    QNetworkReply *reply = manager.get(request);
    QEventLoop loop;
    QObject::connect(reply, SIGNAL(finished()), &loop, SLOT(quit()));
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, SIGNAL(timeout()), &loop, SLOT(quit()));
    timer.start(5000);
    loop.exec();

    if (timer.isActive()) {
        timer.stop();
    } else if (reply->isRunning()) {
        reply->abort();
    }

    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();
    if (reply->error() != QNetworkReply::NoError || statusCode >= 400 || statusCode <= 0) {
        QTextStream(stderr) << "query failed: " << reply->errorString() << "\n";
        if (!body.isEmpty()) {
            QTextStream(stderr) << body << "\n";
        }
        reply->deleteLater();
        return 2;
    }

    QTextStream(stdout) << body;
    if (!body.endsWith('\n')) {
        QTextStream(stdout) << "\n";
    }
    reply->deleteLater();
    return 0;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("openai-reasoning-guard-cli");
    QCoreApplication::setApplicationVersion("0.1.0");

    QCommandLineParser parser;
    parser.setApplicationDescription("OpenAI reasoning degradation guard proxy CLI");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption configOpt(QStringList() << "c" << "config", "Path to config.json", "path");
    QCommandLineOption apiProxyOpt("api-proxy", "Compatibility flag; the CLI always runs proxy mode");
    QCommandLineOption proxyHostOpt("proxy-host", "Proxy bind host", "host", "127.0.0.1");
    QCommandLineOption proxyPortOpt("proxy-port", "Proxy bind port", "port", "8010");
    QCommandLineOption proxyPrefixOpt("proxy-prefix", "Proxy request path prefix", "prefix");
    QCommandLineOption upstreamBaseUrlOpt("upstream-base-url", "Upstream OpenAI-compatible base URL", "url", "https://ai.input.im/v1");
    QCommandLineOption upstreamApiKeyOpt("upstream-api-key", "Upstream bearer token override; empty forwards incoming Authorization", "token");
    QCommandLineOption upstreamUserAgentOpt("upstream-user-agent", "User-Agent sent upstream", "ua", "curl/8.7.1");
    QCommandLineOption forwardUserAgentOpt("forward-user-agent", "Forward incoming User-Agent upstream");
    QCommandLineOption upstreamProxyOpt("upstream-proxy", "Proxy for upstream API requests, e.g. http://127.0.0.1:7890 or socks5://127.0.0.1:7890", "url");
    QCommandLineOption upstreamHttpProxyOpt("upstream-http-proxy", "Compatibility alias for --upstream-proxy", "url");
    QCommandLineOption upstreamHttpsProxyOpt("upstream-https-proxy", "Compatibility alias for --upstream-proxy", "url");
    QCommandLineOption upstreamSocksProxyOpt("upstream-socks-proxy", "Compatibility alias for --upstream-proxy", "url");
    QCommandLineOption upstreamTimeoutOpt("upstream-timeout", "Upstream timeout in seconds", "seconds", "1800");
    QCommandLineOption bufferTimeoutOpt("buffer-timeout", "JSON/SSE buffer timeout in seconds", "seconds", "180");
    QCommandLineOption requestBodyLimitOpt("request-body-limit-bytes", "Maximum buffered client request body bytes", "bytes", QString::number(defaultRequestBodyLimitBytes()));
    QCommandLineOption responseBufferLimitOpt("response-buffer-limit-bytes", "Maximum buffered upstream response bytes", "bytes", QString::number(defaultResponseBufferLimitBytes()));
    QCommandLineOption reasoningEqualsOpt("reasoning-equals", "Comma/space separated reasoning_tokens values guarded by the proxy", "values", defaultReasoningEquals().join(","));
    QCommandLineOption interceptRuleModeOpt("intercept-rule-mode", "Intercept rule mode: reasoning_tokens or final_answer_only_high_xhigh", "mode");
    QCommandLineOption guardRetryAttemptsOpt("guard-retry-attempts", "Internal upstream retries after a reasoning guard match", "count", "3");
    QCommandLineOption retryCapacityOpt("retry-upstream-capacity-errors", "Retry selected upstream capacity errors inside the gateway");
    QCommandLineOption noRetryCapacityOpt("no-retry-upstream-capacity-errors", "Pass selected upstream capacity errors through without gateway retries");
    QCommandLineOption reasoning516RetriesOpt("reasoning-516-retries", "Compatibility alias for --guard-retry-attempts", "count");
    QCommandLineOption guardEndpointsOpt("guard-endpoints", "Comma/space separated paths inspected by the reasoning guard", "paths", defaultGuardEndpoints().join(","));
    QCommandLineOption noInterceptStreamingOpt("no-intercept-streaming", "Observe streaming reasoning guard matches without blocking");
    QCommandLineOption noInterceptNonStreamingOpt("no-intercept-non-streaming", "Observe non-streaming reasoning guard matches without blocking");
    QCommandLineOption nonStreamStatusCodeOpt("non-stream-status-code", "HTTP status returned after reasoning guard retries are exhausted", "status", "502");
    QCommandLineOption streamActionOpt("stream-action", "Streaming guard action name, kept aligned with codex-retry-gateway", "action", "strict_502");
    QCommandLineOption statusJsonOpt("status-json", "Print proxy status JSON once after startup");
    QCommandLineOption queryStatusOpt("query-status", "Query /status from an already running proxy and exit");
    QCommandLineOption queryUrlOpt("query-url", "Full status URL for --query-status", "url");

    QCommandLineOption keepConfigOpt("keep-config", "Persist selected settings to config.json");

    parser.addOptions(QList<QCommandLineOption>()
        << configOpt
        << apiProxyOpt
        << proxyHostOpt
        << proxyPortOpt
        << proxyPrefixOpt
        << upstreamBaseUrlOpt
        << upstreamApiKeyOpt
        << upstreamUserAgentOpt
        << forwardUserAgentOpt
        << upstreamProxyOpt
        << upstreamHttpProxyOpt
        << upstreamHttpsProxyOpt
        << upstreamSocksProxyOpt
        << upstreamTimeoutOpt
        << bufferTimeoutOpt
        << requestBodyLimitOpt
        << responseBufferLimitOpt
        << reasoningEqualsOpt
        << interceptRuleModeOpt
        << guardRetryAttemptsOpt
        << retryCapacityOpt
        << noRetryCapacityOpt
        << reasoning516RetriesOpt
        << guardEndpointsOpt
        << noInterceptStreamingOpt
        << noInterceptNonStreamingOpt
        << nonStreamStatusCodeOpt
        << streamActionOpt
        << statusJsonOpt
        << queryStatusOpt
        << queryUrlOpt
        << keepConfigOpt);
    parser.process(app);

    const QString configPath = parser.isSet(configOpt) ? parser.value(configOpt) : defaultConfigPath();
    AppConfig config = loadConfig(configPath);

    Q_UNUSED(apiProxyOpt)

    ProxySettings settings;
    settings.listenHost = optionString(parser, proxyHostOpt, config.proxyHost);
    settings.listenPort = optionInt(parser, proxyPortOpt, config.proxyPort);
    settings.proxyPrefix = parser.isSet(proxyPrefixOpt) ? parser.value(proxyPrefixOpt) : config.proxyPrefix;
    settings.upstreamBaseUrl = optionString(parser, upstreamBaseUrlOpt, config.upstreamBaseUrl);
    settings.upstreamApiKey = parser.isSet(upstreamApiKeyOpt) ? parser.value(upstreamApiKeyOpt) : config.upstreamApiKey;
    settings.upstreamUserAgent = optionString(parser, upstreamUserAgentOpt, config.upstreamUserAgent);
    settings.forwardUserAgent = parser.isSet(forwardUserAgentOpt) || config.forwardUserAgent;
    settings.upstreamProxy = parser.isSet(upstreamProxyOpt) ? parser.value(upstreamProxyOpt) : config.upstreamProxy;
    settings.upstreamHttpProxy = parser.isSet(upstreamHttpProxyOpt) ? parser.value(upstreamHttpProxyOpt) : config.upstreamHttpProxy;
    settings.upstreamHttpsProxy = parser.isSet(upstreamHttpsProxyOpt) ? parser.value(upstreamHttpsProxyOpt) : config.upstreamHttpsProxy;
    settings.upstreamSocksProxy = parser.isSet(upstreamSocksProxyOpt) ? parser.value(upstreamSocksProxyOpt) : config.upstreamSocksProxy;
    if (!settings.upstreamProxy.trimmed().isEmpty()) {
        settings.upstreamProxy = proxyTextWithDefaultScheme(settings.upstreamProxy, false);
    }
    settings.upstreamHttpProxy = proxyTextWithDefaultScheme(settings.upstreamHttpProxy, false);
    settings.upstreamHttpsProxy = proxyTextWithDefaultScheme(settings.upstreamHttpsProxy, false);
    settings.upstreamSocksProxy = proxyTextWithDefaultScheme(settings.upstreamSocksProxy, true);
    settings.upstreamTimeoutSec = optionInt(parser, upstreamTimeoutOpt, config.upstreamTimeoutSec);
    settings.bufferTimeoutSec = optionInt(parser, bufferTimeoutOpt, config.bufferTimeoutSec);
    settings.requestBodyLimitBytes = optionInt64(parser, requestBodyLimitOpt, config.requestBodyLimitBytes);
    settings.responseBufferLimitBytes = optionInt64(parser, responseBufferLimitOpt, config.responseBufferLimitBytes);
    settings.interceptRuleMode = parser.isSet(interceptRuleModeOpt) ? parser.value(interceptRuleModeOpt) : config.interceptRuleMode;
    settings.reasoningEquals = normalizeIntegerList(
        parser.isSet(reasoningEqualsOpt) ? parser.value(reasoningEqualsOpt) : config.reasoningEquals,
        defaultReasoningEquals());
    settings.guardRetryAttempts = parser.isSet(reasoning516RetriesOpt)
        ? optionInt(parser, reasoning516RetriesOpt, config.guardRetryAttempts)
        : optionInt(parser, guardRetryAttemptsOpt, config.guardRetryAttempts);
    settings.reasoning516RetryCount = settings.guardRetryAttempts;
    settings.retryUpstreamCapacityErrors = parser.isSet(noRetryCapacityOpt)
        ? false
        : (parser.isSet(retryCapacityOpt) ? true : config.retryUpstreamCapacityErrors);
    settings.guardEndpoints = normalizePathList(
        parser.isSet(guardEndpointsOpt) ? parser.value(guardEndpointsOpt) : config.guardEndpoints,
        defaultGuardEndpoints());
    settings.interceptStreaming = config.interceptStreaming && !parser.isSet(noInterceptStreamingOpt);
    settings.interceptNonStreaming = config.interceptNonStreaming && !parser.isSet(noInterceptNonStreamingOpt);
    settings.nonStreamStatusCode = optionInt(parser, nonStreamStatusCodeOpt, config.nonStreamStatusCode);
    settings.streamAction = parser.isSet(streamActionOpt) ? parser.value(streamActionOpt) : config.streamAction;

    if (parser.isSet(queryStatusOpt)) {
        const QString url = parser.isSet(queryUrlOpt)
            ? parser.value(queryUrlOpt)
            : QString("http://%1:%2/status").arg(settings.listenHost).arg(settings.listenPort);
        return queryStatus(QUrl(url));
    }

    if (parser.isSet(keepConfigOpt)) {
        config.proxyHost = settings.listenHost;
        config.proxyPort = settings.listenPort;
        config.proxyPrefix = settings.proxyPrefix;
        config.upstreamBaseUrl = settings.upstreamBaseUrl;
        config.upstreamApiKey = settings.upstreamApiKey;
        config.upstreamUserAgent = settings.upstreamUserAgent;
        config.forwardUserAgent = settings.forwardUserAgent;
        config.upstreamProxy = settings.upstreamProxy;
        config.upstreamHttpProxy = settings.upstreamHttpProxy;
        config.upstreamHttpsProxy = settings.upstreamHttpsProxy;
        config.upstreamSocksProxy = settings.upstreamSocksProxy;
        config.upstreamTimeoutSec = settings.upstreamTimeoutSec;
        config.bufferTimeoutSec = settings.bufferTimeoutSec;
        config.requestBodyLimitBytes = settings.requestBodyLimitBytes;
        config.responseBufferLimitBytes = settings.responseBufferLimitBytes;
        config.interceptRuleMode = settings.interceptRuleMode;
        config.reasoningEquals = settings.reasoningEquals.join(",");
        config.guardRetryAttempts = settings.guardRetryAttempts;
        config.reasoning516RetryCount = settings.guardRetryAttempts;
        config.retryUpstreamCapacityErrors = settings.retryUpstreamCapacityErrors;
        config.guardEndpoints = settings.guardEndpoints.join(",");
        config.interceptStreaming = settings.interceptStreaming;
        config.interceptNonStreaming = settings.interceptNonStreaming;
        config.nonStreamStatusCode = settings.nonStreamStatusCode;
        config.streamAction = settings.streamAction;
        QString error;
        if (!saveConfig(config, configPath, &error)) {
            logLine(QString("save config failed: %1").arg(error));
        }
    }

    HttpProxyServer server;
    if (!parser.isSet(statusJsonOpt)) {
        QObject::connect(&server, &HttpProxyServer::logLine, logLine);
    }

    QString error;
    if (!server.start(settings, &error)) {
        logLine(QString("proxy start failed: %1").arg(error));
        return 2;
    }
    if (parser.isSet(statusJsonOpt)) {
        QTextStream(stdout) << QJsonDocument(server.statusPayload()).toJson(QJsonDocument::Indented);
        return 0;
    }
    return app.exec();
}
