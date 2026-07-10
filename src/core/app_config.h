#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

namespace net_tunnel {

struct AppConfig {
    QString lang;

    QString proxyHost;
    int proxyPort;
    QString proxyPrefix;
    QString upstreamBaseUrl;
    QString upstreamApiKey;
    QString upstreamUserAgent;
    bool forwardUserAgent;
    QString upstreamProxy;
    QString upstreamHttpProxy;
    QString upstreamHttpsProxy;
    QString upstreamSocksProxy;
    int upstreamTimeoutSec;
    int firstTokenTimeoutSec;
    int bufferTimeoutSec;
    qint64 requestBodyLimitBytes;
    qint64 responseBufferLimitBytes;
    int reasoning516RetryCount;
    QString interceptRuleMode;
    QString reasoningEquals;
    int guardRetryAttempts;
    bool retryUpstreamCapacityErrors;
    QString guardEndpoints;
    bool interceptStreaming;
    bool interceptNonStreaming;
    int nonStreamStatusCode;
    QString streamAction;

    AppConfig();
};

QString defaultConfigPath();
AppConfig loadConfig(const QString &path = QString());
bool saveConfig(const AppConfig &config, const QString &path = QString(), QString *error = 0);

} // namespace net_tunnel
