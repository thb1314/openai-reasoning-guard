#include "core/app_config.h"

#include "core/http_proxy_server.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegExp>
#include <QtCore/QSaveFile>

namespace net_tunnel {

static QString readString(const QJsonObject &object, const QString &key, const QString &fallback)
{
    const QJsonValue value = object.value(key);
    if (value.isString()) {
        return value.toString();
    }
    if (value.isDouble()) {
        return QString::number(value.toInt());
    }
    return fallback;
}

static QString normalizeUpstreamBaseUrl(const QString &value)
{
    return value.trimmed();
}

static int readInt(const QJsonObject &object, const QString &key, int fallback)
{
    const QJsonValue value = object.value(key);
    if (value.isDouble()) {
        return value.toInt();
    }
    if (value.isString()) {
        bool ok = false;
        const int parsed = value.toString().trimmed().toInt(&ok);
        if (ok) {
            return parsed;
        }
    }
    return fallback;
}

static qint64 readInt64(const QJsonObject &object, const QString &key, qint64 fallback)
{
    const QJsonValue value = object.value(key);
    if (value.isDouble()) {
        return qint64(value.toDouble());
    }
    if (value.isString()) {
        bool ok = false;
        const qint64 parsed = value.toString().trimmed().toLongLong(&ok);
        if (ok) {
            return parsed;
        }
    }
    return fallback;
}

static bool readBool(const QJsonObject &object, const QString &key, bool fallback)
{
    const QJsonValue value = object.value(key);
    if (value.isBool()) {
        return value.toBool();
    }
    if (value.isString()) {
        const QString text = value.toString().trimmed().toLower();
        if (text == "1" || text == "true" || text == "yes" || text == "on") {
            return true;
        }
        if (text == "0" || text == "false" || text == "no" || text == "off") {
            return false;
        }
    }
    return fallback;
}

static QString readListText(const QJsonObject &object, const QString &key, const QString &fallback)
{
    const QJsonValue value = object.value(key);
    if (value.isString()) {
        return value.toString();
    }
    if (value.isArray()) {
        QStringList items;
        const QJsonArray array = value.toArray();
        for (int i = 0; i < array.size(); ++i) {
            const QJsonValue item = array.at(i);
            if (item.isString()) {
                items.append(item.toString());
            } else if (item.isDouble()) {
                items.append(QString::number(item.toInt()));
            }
        }
        if (!items.isEmpty()) {
            return items.join(",");
        }
    }
    return fallback;
}

static QJsonArray integerTextToJsonArray(const QString &text)
{
    QJsonArray array;
    const QStringList values = normalizeIntegerList(text, defaultReasoningEquals());
    for (int i = 0; i < values.size(); ++i) {
        bool ok = false;
        const int parsed = values.at(i).toInt(&ok);
        if (ok) {
            array.append(parsed);
        }
    }
    return array;
}

static QJsonArray pathTextToJsonArray(const QString &text)
{
    QJsonArray array;
    const QStringList values = normalizePathList(text, defaultGuardEndpoints());
    for (int i = 0; i < values.size(); ++i) {
        array.append(values.at(i));
    }
    return array;
}

static QString proxyTextWithDefaultScheme(const QString &text, bool socksFallback)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || trimmed.contains("://")) {
        return trimmed;
    }
    return QString("%1://%2").arg(socksFallback ? QString("socks5") : QString("http"), trimmed);
}

static QString userConfigDirectory()
{
#if defined(Q_OS_WIN)
    QString basePath = QString::fromLocal8Bit(qgetenv("APPDATA")).trimmed();
    if (basePath.isEmpty()) {
        basePath = QDir(QDir::homePath()).filePath("AppData/Roaming");
    }
#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    const QString basePath = QDir(QDir::homePath()).filePath("Library/Application Support");
#else
    QString basePath = QString::fromLocal8Bit(qgetenv("XDG_CONFIG_HOME")).trimmed();
    if (basePath.isEmpty()) {
        basePath = QDir(QDir::homePath()).filePath(".config");
    }
#endif
    return QDir(basePath).filePath("OpenAI Reasoning Guard");
}

static bool ensureConfigDirectory(const QString &filePath, QString *error)
{
    const QFileInfo fileInfo(filePath);
    QDir dir = fileInfo.dir();
    const bool existed = dir.exists();
    if (!existed && !dir.mkpath(".")) {
        if (error) {
            *error = QString("failed to create config directory: %1").arg(dir.absolutePath());
        }
        return false;
    }

    if (!existed) {
        const QFileDevice::Permissions permissions = QFileDevice::ReadOwner |
            QFileDevice::WriteOwner | QFileDevice::ExeOwner;
        if (!QFile::setPermissions(dir.absolutePath(), permissions)) {
            if (error) {
                *error = QString("failed to restrict config directory permissions: %1")
                             .arg(dir.absolutePath());
            }
            return false;
        }
#if !defined(Q_OS_WIN)
        const QFileDevice::Permissions actual = QFileInfo(dir.absolutePath()).permissions();
        const QFileDevice::Permissions exposed = QFileDevice::ReadGroup |
            QFileDevice::WriteGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther |
            QFileDevice::WriteOther | QFileDevice::ExeOther;
        if (actual & exposed) {
            if (error) {
                *error = QString("owner-only config directory permissions are not enforceable: %1")
                             .arg(dir.absolutePath());
            }
            return false;
        }
#endif
    }
    return true;
}

static bool writePrivateFileAtomically(const QString &path,
                                       const QByteArray &contents,
                                       QString *error)
{
    if (!ensureConfigDirectory(path, error)) {
        return false;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }

    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        if (error) {
            *error = QString("failed to restrict config permissions: %1").arg(path);
        }
        file.cancelWriting();
        return false;
    }

    if (file.write(contents) != contents.size()) {
        if (error) {
            *error = file.errorString();
        }
        file.cancelWriting();
        return false;
    }

    if (!file.commit()) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    return true;
}

static void copyLegacyConfigIfNeeded(const QString &targetPath)
{
    if (QFileInfo::exists(targetPath)) {
        return;
    }

    const QString legacyPath = QDir(QCoreApplication::applicationDirPath()).filePath("config.json");
    if (QDir::cleanPath(QFileInfo(legacyPath).absoluteFilePath()) ==
            QDir::cleanPath(QFileInfo(targetPath).absoluteFilePath()) ||
        !QFileInfo::exists(legacyPath)) {
        return;
    }

    QFile legacyFile(legacyPath);
    if (!legacyFile.open(QIODevice::ReadOnly)) {
        return;
    }
    const QByteArray contents = legacyFile.readAll();
    if (legacyFile.error() != QFileDevice::NoError) {
        return;
    }

    writePrivateFileAtomically(targetPath, contents, 0);
}

AppConfig::AppConfig()
    : lang("zh"),
      proxyHost("127.0.0.1"),
      proxyPort(8010),
      proxyPrefix("/v1"),
      upstreamBaseUrl(""),
      upstreamUserAgent("curl/8.7.1"),
      forwardUserAgent(false),
      upstreamTimeoutSec(1800),
      firstTokenTimeoutSec(30),
      bufferTimeoutSec(180),
      requestBodyLimitBytes(defaultRequestBodyLimitBytes()),
      responseBufferLimitBytes(defaultResponseBufferLimitBytes()),
      reasoning516RetryCount(3),
      interceptRuleMode("reasoning_tokens"),
      reasoningEquals(defaultReasoningEquals().join(",")),
      guardRetryAttempts(3),
      retryUpstreamCapacityErrors(true),
      guardEndpoints(defaultGuardEndpoints().join(",")),
      interceptStreaming(true),
      interceptNonStreaming(true),
      nonStreamStatusCode(502),
      streamAction("strict_502")
{
}

QString defaultConfigPath()
{
    const QString envPath = QString::fromLocal8Bit(qgetenv("NET_TUNNEL_CONFIG")).trimmed();
    if (!envPath.isEmpty()) {
        return envPath;
    }

    const QString path = QDir(userConfigDirectory()).filePath("config.json");
    copyLegacyConfigIfNeeded(path);
    return path;
}

AppConfig appConfigFromJsonObject(const QJsonObject &object)
{
    AppConfig config;
    config.lang = readString(object, "lang", config.lang);
    config.proxyHost = readString(object, "proxy_host", config.proxyHost);
    config.proxyPort = readInt(object, "proxy_port", config.proxyPort);
    config.proxyPrefix = readString(object, "proxy_prefix", config.proxyPrefix);
    config.upstreamBaseUrl = normalizeUpstreamBaseUrl(readString(object, "upstream_base_url", config.upstreamBaseUrl));
    config.upstreamApiKey = readString(object, "upstream_api_key", config.upstreamApiKey);
    config.upstreamUserAgent = readString(object, "upstream_user_agent", config.upstreamUserAgent);
    config.forwardUserAgent = readBool(object, "forward_user_agent", config.forwardUserAgent);
    config.upstreamProxy = readString(object, "upstream_proxy", config.upstreamProxy);
    config.upstreamHttpProxy = readString(object, "upstream_http_proxy", config.upstreamHttpProxy);
    config.upstreamHttpsProxy = readString(object, "upstream_https_proxy", config.upstreamHttpsProxy);
    config.upstreamSocksProxy = readString(object, "upstream_socks_proxy", config.upstreamSocksProxy);
    if (!config.upstreamProxy.trimmed().isEmpty()) {
        config.upstreamProxy = proxyTextWithDefaultScheme(config.upstreamProxy, false);
    }
    config.upstreamHttpProxy = proxyTextWithDefaultScheme(config.upstreamHttpProxy, false);
    config.upstreamHttpsProxy = proxyTextWithDefaultScheme(config.upstreamHttpsProxy, false);
    config.upstreamSocksProxy = proxyTextWithDefaultScheme(config.upstreamSocksProxy, true);
    config.upstreamTimeoutSec = readInt(object, "upstream_timeout_sec", config.upstreamTimeoutSec);
    config.firstTokenTimeoutSec = readInt(object, "first_token_timeout_sec",
        readInt(object, "upstream_first_byte_timeout_seconds", config.firstTokenTimeoutSec));
    config.bufferTimeoutSec = readInt(object, "buffer_timeout_sec", config.bufferTimeoutSec);
    config.requestBodyLimitBytes = readInt64(object, "request_body_limit_bytes", config.requestBodyLimitBytes);
    config.responseBufferLimitBytes = readInt64(object, "response_buffer_limit_bytes", config.responseBufferLimitBytes);
    config.interceptRuleMode = readString(object, "intercept_rule_mode", config.interceptRuleMode);
    config.reasoningEquals = readListText(object, "reasoning_equals", config.reasoningEquals);
    config.guardRetryAttempts = readInt(object, "guard_retry_attempts",
        readInt(object, "reasoning_516_retry_count", config.guardRetryAttempts));
    config.reasoning516RetryCount = config.guardRetryAttempts;
    config.retryUpstreamCapacityErrors = readBool(object, "retry_upstream_capacity_errors", config.retryUpstreamCapacityErrors);
    config.guardEndpoints = readListText(object, "guard_endpoints", config.guardEndpoints);
    config.interceptStreaming = readBool(object, "intercept_streaming", config.interceptStreaming);
    config.interceptNonStreaming = readBool(object, "intercept_non_streaming", config.interceptNonStreaming);
    config.nonStreamStatusCode = readInt(object, "non_stream_status_code", config.nonStreamStatusCode);
    config.streamAction = readString(object, "stream_action", config.streamAction);
    return config;
}

AppConfig loadConfig(const QString &path)
{
    const QString resolvedPath = path.isEmpty() ? defaultConfigPath() : path;
    QFile file(resolvedPath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return AppConfig();
    }

    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        return AppConfig();
    }
    return appConfigFromJsonObject(document.object());
}

bool saveConfig(const AppConfig &config, const QString &path, QString *error)
{
    if (error) {
        error->clear();
    }
    const QString resolvedPath = path.isEmpty() ? defaultConfigPath() : path;
    QJsonObject object;
    object.insert("lang", config.lang);
    object.insert("proxy_host", config.proxyHost);
    object.insert("proxy_port", QString::number(config.proxyPort));
    object.insert("proxy_prefix", config.proxyPrefix);
    object.insert("buffer_timeout_sec", config.bufferTimeoutSec);
    object.insert("request_body_limit_bytes", double(config.requestBodyLimitBytes));
    object.insert("response_buffer_limit_bytes", double(config.responseBufferLimitBytes));
    object.insert("intercept_rule_mode", config.interceptRuleMode);
    object.insert("reasoning_equals", integerTextToJsonArray(config.reasoningEquals));
    object.insert("guard_retry_attempts", config.guardRetryAttempts);
    object.insert("reasoning_516_retry_count", config.guardRetryAttempts);
    object.insert("retry_upstream_capacity_errors", config.retryUpstreamCapacityErrors);
    object.insert("guard_endpoints", pathTextToJsonArray(config.guardEndpoints));
    object.insert("intercept_streaming", config.interceptStreaming);
    object.insert("intercept_non_streaming", config.interceptNonStreaming);
    object.insert("non_stream_status_code", config.nonStreamStatusCode);
    object.insert("stream_action", config.streamAction);

    return writePrivateFileAtomically(
        resolvedPath, QJsonDocument(object).toJson(QJsonDocument::Indented), error);
}

} // namespace net_tunnel
