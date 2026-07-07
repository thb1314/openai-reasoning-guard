#include "core/http_proxy_server.h"

#include "core/json_utils.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QMap>
#include <QtCore/QRegExp>
#include <QtCore/QTimer>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QTcpSocket>

namespace net_tunnel {

static bool isJsonContent(const QString &contentType)
{
    return contentType.toLower().contains("json");
}

static bool isEventStream(const QString &contentType)
{
    return contentType.toLower().contains("text/event-stream");
}

static QByteArray reasonPhrase(int statusCode)
{
    switch (statusCode) {
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "Status";
    }
}

static void rememberTail(QByteArray *tail, const QByteArray &chunk)
{
    if (!tail || chunk.isEmpty()) {
        return;
    }
    static const int limit = 128;
    if (chunk.size() >= limit) {
        *tail = chunk.right(limit);
        return;
    }
    tail->append(chunk);
    if (tail->size() > limit) {
        *tail = tail->right(limit);
    }
}

static QString tailHashText(const QByteArray &tail)
{
    if (tail.isEmpty()) {
        return QString();
    }
    return QString::fromLatin1(QCryptographicHash::hash(tail, QCryptographicHash::Sha256).toHex().left(16));
}

static QString headerValue(const QMap<QString, QByteArray> &headers,
                           const QString &name,
                           const QString &fallback = QString())
{
    const QByteArray value = headers.value(name.toLower());
    if (value.isEmpty()) {
        return fallback;
    }
    return QString::fromLatin1(value);
}

static QStringList hopByHopHeaders()
{
        return QStringList()
            << "connection"
            << "content-encoding"
            << "keep-alive"
        << "proxy-authenticate"
        << "proxy-authorization"
        << "te"
        << "trailer"
        << "transfer-encoding"
        << "upgrade";
}

QString normalizePathPrefix(const QString &prefix)
{
    QString value = prefix.trimmed();
    if (value.isEmpty() || value == "/") {
        return QString();
    }
    if (!value.startsWith('/')) {
        value.prepend('/');
    }
    while (value.endsWith('/') && value.size() > 1) {
        value.chop(1);
    }
    return value;
}

QString joinPaths(const QString &basePath, const QString &suffix)
{
    QString base = basePath.trimmed();
    QString tail = suffix.trimmed();
    if (tail.isEmpty()) {
        tail = "/";
    }
    if (!tail.startsWith('/')) {
        tail.prepend('/');
    }
    if (base.isEmpty() || base == "/") {
        return tail;
    }
    while (base.endsWith('/') && base.size() > 1) {
        base.chop(1);
    }
    if (tail == "/") {
        return base;
    }
    return base + tail;
}

QStringList defaultReasoningEquals()
{
    return QStringList() << "516" << "1034" << "1552";
}

QStringList defaultGuardEndpoints()
{
    return QStringList()
        << "/responses"
        << "/chat/completions"
        << "/v1/responses"
        << "/v1/chat/completions";
}

qint64 defaultRequestBodyLimitBytes()
{
    return 100LL * 1024LL * 1024LL;
}

qint64 defaultResponseBufferLimitBytes()
{
    return 100LL * 1024LL * 1024LL;
}

static QString interceptRuleModeReasoningTokens()
{
    return "reasoning_tokens";
}

static QString interceptRuleModeFinalAnswerOnlyHighXhigh()
{
    return "final_answer_only_high_xhigh";
}

static QString streamActionStrict502()
{
    return "strict_502";
}

static QString streamActionDisconnect()
{
    return "disconnect";
}

static QString normalizeInterceptRuleMode(const QString &mode)
{
    const QString normalized = mode.trimmed().toLower();
    return normalized == interceptRuleModeFinalAnswerOnlyHighXhigh()
        ? interceptRuleModeFinalAnswerOnlyHighXhigh()
        : interceptRuleModeReasoningTokens();
}

static QString normalizeStreamAction(const QString &action)
{
    const QString normalized = action.trimmed().toLower();
    return normalized == streamActionDisconnect()
        ? streamActionDisconnect()
        : streamActionStrict502();
}

static QString requestKindNormal()
{
    return "normal";
}

static QString requestKindContextCompaction()
{
    return "context_compaction";
}

QStringList normalizeIntegerList(const QString &values, const QStringList &fallback)
{
    const QString source = values.trimmed().isEmpty() ? fallback.join(",") : values;
    const QStringList parts = source.split(QRegExp("[\\s,]+"), QString::SkipEmptyParts);
    QStringList result;
    for (int i = 0; i < parts.size(); ++i) {
        bool ok = false;
        const int parsed = parts.at(i).trimmed().toInt(&ok);
        if (ok) {
            const QString item = QString::number(parsed);
            if (!result.contains(item)) {
                result.append(item);
            }
        }
    }
    return result.isEmpty() ? fallback : result;
}

QStringList normalizePathList(const QString &values, const QStringList &fallback)
{
    const QString source = values.trimmed().isEmpty() ? fallback.join(",") : values;
    const QStringList parts = source.split(QRegExp("[\\s,]+"), QString::SkipEmptyParts);
    QStringList result;
    for (int i = 0; i < parts.size(); ++i) {
        QString item = normalizePathPrefix(parts.at(i));
        if (item.isEmpty()) {
            item = "/";
        }
        if (!result.contains(item)) {
            result.append(item);
        }
    }
    return result.isEmpty() ? fallback : result;
}

static QJsonArray stringListToJsonArray(const QStringList &values)
{
    QJsonArray array;
    for (int i = 0; i < values.size(); ++i) {
        bool ok = false;
        const int parsed = values.at(i).toInt(&ok);
        if (ok) {
            array.append(parsed);
        } else {
            array.append(values.at(i));
        }
    }
    return array;
}

static bool reasoningMatched(const ProxySettings &settings, int reasoningTokens)
{
    return reasoningTokens >= 0 && settings.reasoningEquals.contains(QString::number(reasoningTokens));
}

static int maxReasoningEquals(const ProxySettings &settings)
{
    int maximum = -1;
    for (int i = 0; i < settings.reasoningEquals.size(); ++i) {
        bool ok = false;
        const int parsed = settings.reasoningEquals.at(i).toInt(&ok);
        if (ok && parsed > maximum) {
            maximum = parsed;
        }
    }
    return maximum;
}

static bool includesContextCompactionMarker(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    return !normalized.isEmpty() &&
        (normalized.contains("remote_compaction") || normalized.contains("context_compaction"));
}

static QString jsonValueText(const QJsonValue &value)
{
    if (value.isString()) {
        return value.toString();
    }
    if (value.isBool()) {
        return value.toBool() ? QString("true") : QString("false");
    }
    if (value.isDouble()) {
        return QString::number(value.toDouble(), 'f', 0);
    }
    if (value.isObject()) {
        return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    }
    if (value.isArray()) {
        return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    }
    return QString();
}

static QString detectRequestKind(const QMap<QString, QByteArray> &headers, const QJsonObject &requestJson)
{
    const QString headerSignals = QString("%1 %2 %3")
        .arg(headerValue(headers, "x-codex-request-kind"))
        .arg(headerValue(headers, "x-codex-purpose"))
        .arg(headerValue(headers, "x-codex-turn-metadata"));
    if (includesContextCompactionMarker(headerSignals)) {
        return requestKindContextCompaction();
    }

    const QString metadataSignals = QString("%1 %2 %3 %4")
        .arg(jsonValueText(requestJson.value("metadata")))
        .arg(jsonValueText(requestJson.value("codex_request_kind")))
        .arg(jsonValueText(requestJson.value("request_kind")))
        .arg(jsonValueText(requestJson.value("purpose")));
    return includesContextCompactionMarker(metadataSignals) ? requestKindContextCompaction() : requestKindNormal();
}

static QString normalizeReasoningEffort(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    return normalized == "minimal" || normalized == "low" || normalized == "medium" ||
           normalized == "high" || normalized == "xhigh"
        ? normalized
        : QString();
}

static QString requestReasoningEffort(const QJsonObject &requestJson)
{
    const QJsonObject reasoning = requestJson.value("reasoning").toObject();
    return normalizeReasoningEffort(reasoning.value("effort").toString());
}

struct StructureSignals {
    bool hasCommentary;
    bool hasFinalAnswer;
    bool hasToolCall;
    bool hasOutputText;
    bool hasReasoningItem;

    StructureSignals()
        : hasCommentary(false),
          hasFinalAnswer(false),
          hasToolCall(false),
          hasOutputText(false),
          hasReasoningItem(false)
    {
    }
};

static QString normalizedStringValue(const QJsonValue &value)
{
    return value.isString() ? value.toString().trimmed() : QString();
}

static void markVisibleContent(StructureSignals *structure)
{
    structure->hasFinalAnswer = true;
    structure->hasOutputText = true;
}

static void inspectContentEntryForStructure(const QJsonObject &entry, StructureSignals *structure)
{
    const QString contentType = normalizedStringValue(entry.value("type"));
    if (!contentType.isEmpty()) {
        if (contentType.contains("commentary")) {
            structure->hasCommentary = true;
        }
        if (contentType.contains("tool_call") || contentType.contains("function_call")) {
            structure->hasToolCall = true;
        }
        if (contentType.contains("output_text") || contentType.contains("text")) {
            QString textValue = normalizedStringValue(entry.value("text"));
            if (textValue.isEmpty()) {
                textValue = normalizedStringValue(entry.value("output_text"));
            }
            if (textValue.isEmpty()) {
                textValue = normalizedStringValue(entry.value("content"));
            }
            if (!textValue.isEmpty()) {
                markVisibleContent(structure);
            }
        }
    }
    if (!normalizedStringValue(entry.value("text")).isEmpty() ||
        !normalizedStringValue(entry.value("output_text")).isEmpty()) {
        markVisibleContent(structure);
    }
}

static void inspectOutputItemForStructure(const QJsonObject &item, StructureSignals *structure)
{
    QString itemType = normalizedStringValue(item.value("type"));
    if (itemType.isEmpty()) {
        itemType = "unknown";
    }
    if (itemType.contains("reasoning")) {
        structure->hasReasoningItem = true;
    }
    if (itemType.contains("commentary")) {
        structure->hasCommentary = true;
    }
    if (itemType.contains("tool_call") || itemType.contains("function_call") || itemType.contains("tool")) {
        structure->hasToolCall = true;
    }
    if (!normalizedStringValue(item.value("text")).isEmpty() ||
        !normalizedStringValue(item.value("output_text")).isEmpty()) {
        markVisibleContent(structure);
    }
    const QJsonArray content = item.value("content").toArray();
    for (int i = 0; i < content.size(); ++i) {
        if (content.at(i).isObject()) {
            inspectContentEntryForStructure(content.at(i).toObject(), structure);
        }
    }
}

static void applyStructureSignalsFromPayload(const QJsonValue &payload, StructureSignals *structure)
{
    if (!payload.isObject()) {
        return;
    }
    const QJsonObject object = payload.toObject();
    const QString eventType = normalizedStringValue(object.value("type"));
    if (eventType.contains("commentary")) {
        structure->hasCommentary = true;
    }
    if (eventType.contains("tool_call") || eventType.contains("function_call")) {
        structure->hasToolCall = true;
    }
    if (eventType.contains("output_text.delta") ||
        eventType.contains("message.delta") ||
        eventType.contains("content.delta")) {
        QString deltaText = normalizedStringValue(object.value("delta"));
        if (deltaText.isEmpty()) {
            deltaText = normalizedStringValue(object.value("text"));
        }
        if (deltaText.isEmpty()) {
            deltaText = normalizedStringValue(object.value("content"));
        }
        if (deltaText.isEmpty()) {
            const QJsonArray choices = object.value("choices").toArray();
            for (int i = 0; i < choices.size(); ++i) {
                const QJsonObject choice = choices.at(i).toObject();
                deltaText += normalizedStringValue(choice.value("delta").toObject().value("content"));
            }
        }
        if (!deltaText.isEmpty()) {
            markVisibleContent(structure);
        }
    }
    const QJsonArray choices = object.value("choices").toArray();
    for (int i = 0; i < choices.size(); ++i) {
        const QJsonObject choice = choices.at(i).toObject();
        if (!normalizedStringValue(choice.value("delta").toObject().value("content")).isEmpty() ||
            !normalizedStringValue(choice.value("message").toObject().value("content")).isEmpty()) {
            markVisibleContent(structure);
        }
    }
    if (!normalizedStringValue(object.value("output_text")).isEmpty()) {
        markVisibleContent(structure);
    }
    QList<QJsonArray> outputCollections;
    outputCollections.append(object.value("output").toArray());
    outputCollections.append(object.value("response").toObject().value("output").toArray());
    for (int collectionIndex = 0; collectionIndex < outputCollections.size(); ++collectionIndex) {
        const QJsonArray outputItems = outputCollections.at(collectionIndex);
        for (int i = 0; i < outputItems.size(); ++i) {
            if (outputItems.at(i).isObject()) {
                inspectOutputItemForStructure(outputItems.at(i).toObject(), structure);
            }
        }
    }
}

static bool finalAnswerOnly(const StructureSignals &structure)
{
    return structure.hasFinalAnswer && !structure.hasCommentary && !structure.hasToolCall && !structure.hasReasoningItem;
}

struct RuleMatch {
    QString mode;
    bool matched;
    QString reasonForLog;
    int blockedReasoning;
    QString exemptReason;

    RuleMatch()
        : matched(false),
          blockedReasoning(-1)
    {
    }
};

static RuleMatch buildRuleMatch(const ProxySettings &settings,
                                int reasoningTokens,
                                const QString &requestKind,
                                const QString &reasoningEffort,
                                const StructureSignals &structure)
{
    RuleMatch match;
    match.mode = normalizeInterceptRuleMode(settings.interceptRuleMode);
    match.blockedReasoning = reasoningTokens;
    if (requestKind == requestKindContextCompaction() && reasoningTokens == 0) {
        match.reasonForLog = QString("request_kind=%1 intercept_exempt_reason=%2 reasoning_tokens=%3")
            .arg(requestKindContextCompaction())
            .arg(requestKindContextCompaction())
            .arg(reasoningTokens);
        match.exemptReason = requestKindContextCompaction();
        return match;
    }
    if (match.mode == interceptRuleModeFinalAnswerOnlyHighXhigh()) {
        const QString effort = normalizeReasoningEffort(reasoningEffort);
        const bool isFinalOnly = finalAnswerOnly(structure);
        const bool reasoningAllowed = reasoningTokens != 0;
        match.matched = isFinalOnly && reasoningAllowed && (effort == "high" || effort == "xhigh");
        match.reasonForLog = QString("final_answer_only=%1 effort=%2 reasoning_tokens=%3 zero_reasoning_excluded=%4")
            .arg(isFinalOnly ? QString("true") : QString("false"))
            .arg(effort.isEmpty() ? QString("unknown") : effort)
            .arg(reasoningTokens >= 0 ? QString::number(reasoningTokens) : QString("null"))
            .arg(reasoningTokens == 0 ? QString("true") : QString("false"));
        return match;
    }
    match.matched = reasoningMatched(settings, reasoningTokens);
    match.reasonForLog = QString("reasoning_tokens=%1")
        .arg(reasoningTokens >= 0 ? QString::number(reasoningTokens) : QString("null"));
    return match;
}

static QJsonValue parseJsonBody(const QByteArray &body)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError) {
        return QJsonValue();
    }
    if (document.isObject()) {
        return document.object();
    }
    if (document.isArray()) {
        return document.array();
    }
    return QJsonValue();
}

static QList<QJsonValue> parseSsePayloads(const QByteArray &body, bool *doneSeen = 0)
{
    QList<QJsonValue> payloads;
    if (doneSeen) {
        *doneSeen = false;
    }
    const QString text = QString::fromUtf8(body);
    const QStringList events = text.split(QRegExp("\\r?\\n\\r?\\n"), QString::SkipEmptyParts);
    for (int i = 0; i < events.size(); ++i) {
        const QStringList lines = events.at(i).split(QRegExp("\\r?\\n"), QString::SkipEmptyParts);
        QStringList dataLines;
        for (int j = 0; j < lines.size(); ++j) {
            const QString line = lines.at(j);
            if (line.startsWith("data:")) {
                QString dataLine = line.mid(5);
                if (dataLine.startsWith(' ')) {
                    dataLine.remove(0, 1);
                }
                dataLines.append(dataLine);
            }
        }
        const QString data = dataLines.join("\n").trimmed();
        if (data.isEmpty()) {
            continue;
        }
        if (data == "[DONE]") {
            if (doneSeen) {
                *doneSeen = true;
            }
            continue;
        }
        const QJsonValue payload = parseJsonBody(data.toUtf8());
        if (!payload.isUndefined() && !payload.isNull()) {
            payloads.append(payload);
        }
    }
    return payloads;
}

static bool isTerminalSsePayload(const QJsonValue &payload)
{
    if (!payload.isObject()) {
        return false;
    }
    const QString type = normalizedStringValue(payload.toObject().value("type"));
    return type == "response.completed" ||
        type == "response.done" ||
        type == "done";
}

static bool isFailureSsePayload(const QJsonValue &payload)
{
    if (!payload.isObject()) {
        return false;
    }
    const QJsonObject object = payload.toObject();
    const QString type = normalizedStringValue(object.value("type"));
    return type == "response.failed" ||
        type == "response.incomplete" ||
        type == "error" ||
        object.value("error").isObject();
}

static bool payloadHasUsage(const QJsonValue &payload)
{
    if (!payload.isObject()) {
        return false;
    }
    const QJsonObject object = payload.toObject();
    if (object.value("usage").isObject()) {
        return true;
    }
    return object.value("response").toObject().value("usage").isObject();
}

static QString guardedStreamAnomalyType(int statusCode,
                                        bool failureSeen,
                                        bool terminalSeen,
                                        bool usageSeen)
{
    if (statusCode < 200 || statusCode >= 300) {
        return QString();
    }
    if (failureSeen) {
        return "stream_failed_event";
    }
    if (!terminalSeen) {
        return "stream_incomplete_response";
    }
    if (!usageSeen) {
        return "stream_missing_usage";
    }
    return QString();
}

static QList<QJsonValue> takeCompleteSsePayloads(QByteArray *buffer,
                                                 bool *terminalSeen,
                                                 bool *doneSeen = 0)
{
    QList<QJsonValue> payloads;
    if (!buffer) {
        return payloads;
    }
    if (terminalSeen) {
        *terminalSeen = false;
    }
    if (doneSeen) {
        *doneSeen = false;
    }

    while (true) {
        const int crlfEnd = buffer->indexOf("\r\n\r\n");
        const int lfEnd = buffer->indexOf("\n\n");
        int eventEnd = -1;
        int terminatorLength = 0;
        if (crlfEnd >= 0 && (lfEnd < 0 || crlfEnd <= lfEnd)) {
            eventEnd = crlfEnd;
            terminatorLength = 4;
        } else if (lfEnd >= 0) {
            eventEnd = lfEnd;
            terminatorLength = 2;
        }
        if (eventEnd < 0) {
            break;
        }

        const QByteArray event = buffer->left(eventEnd);
        buffer->remove(0, eventEnd + terminatorLength);

        const QList<QByteArray> lines = event.split('\n');
        QByteArray data;
        for (int i = 0; i < lines.size(); ++i) {
            QByteArray line = lines.at(i);
            if (line.endsWith('\r')) {
                line.chop(1);
            }
            if (!line.startsWith("data:")) {
                continue;
            }
            QByteArray dataLine = line.mid(5);
            if (dataLine.startsWith(' ')) {
                dataLine.remove(0, 1);
            }
            if (!data.isEmpty()) {
                data.append('\n');
            }
            data.append(dataLine);
        }

        data = data.trimmed();
        if (data.isEmpty()) {
            continue;
        }
        if (data == "[DONE]") {
            if (terminalSeen) {
                *terminalSeen = true;
            }
            if (doneSeen) {
                *doneSeen = true;
            }
            continue;
        }
        const QJsonValue payload = parseJsonBody(data);
        if (!payload.isUndefined() && !payload.isNull()) {
            payloads.append(payload);
            if (terminalSeen && isTerminalSsePayload(payload)) {
                *terminalSeen = true;
            }
        }
    }

    return payloads;
}

static QString jsonValueCompactText(const QJsonValue &payload)
{
    if (payload.isObject()) {
        return QString::fromUtf8(QJsonDocument(payload.toObject()).toJson(QJsonDocument::Compact));
    }
    if (payload.isArray()) {
        return QString::fromUtf8(QJsonDocument(payload.toArray()).toJson(QJsonDocument::Compact));
    }
    return QString();
}

static bool upstreamCapacityErrorMatched(int statusCode, const QJsonValue &payload, const QByteArray &body)
{
    if (statusCode < 400) {
        return false;
    }
    QString text = jsonValueCompactText(payload);
    if (!body.isEmpty()) {
        text += QString::fromUtf8(body);
    }
    text = text.toLower();
    return text.contains("selected model is at capacity. please try a different model.") ||
        (text.contains("selected model is at capacity") && text.contains("try a different model"));
}

static bool shouldInspectPath(const ProxySettings &settings, const QString &path)
{
    QString normalized = normalizePathPrefix(path);
    if (normalized.isEmpty()) {
        normalized = "/";
    }
    return settings.guardEndpoints.contains(normalized);
}

static QString pathWithoutProxyPrefix(const QString &path, const QString &proxyPrefix)
{
    QString normalizedPath = normalizePathPrefix(path);
    if (normalizedPath.isEmpty()) {
        normalizedPath = "/";
    }

    const QString normalizedPrefix = normalizePathPrefix(proxyPrefix);
    if (normalizedPrefix.isEmpty()) {
        return normalizedPath;
    }
    if (normalizedPath == normalizedPrefix) {
        return "/";
    }
    if (normalizedPath.startsWith(normalizedPrefix + "/")) {
        const QString suffix = normalizedPath.mid(normalizedPrefix.size());
        return suffix.isEmpty() ? QString("/") : suffix;
    }
    return normalizedPath;
}

static bool shouldInspectPathWithProxyPrefix(const ProxySettings &settings,
                                             const QString &path,
                                             const QString &proxyPrefix)
{
    if (shouldInspectPath(settings, path)) {
        return true;
    }
    const QString suffix = pathWithoutProxyPrefix(path, proxyPrefix);
    return shouldInspectPath(settings, suffix);
}

static QString proxyUrlText(const QString &text, bool socksFallback)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || trimmed.contains("://")) {
        return trimmed;
    }
    return QString("%1://%2").arg(socksFallback ? QString("socks5") : QString("http"), trimmed);
}

static QJsonObject blockedReasoningBody(const QString &path, int reasoningTokens, int statusCode)
{
    QJsonObject error;
    error.insert("message", QString("codex retry gateway blocked suspicious reasoning response on %1").arg(path));
    error.insert("type", "codex_retry_gateway");
    error.insert("code", "reasoning_guard_triggered");
    error.insert("reasoning_tokens", reasoningTokens >= 0 ? QJsonValue(reasoningTokens) : QJsonValue());
    error.insert("status_code", statusCode);
    return QJsonObject{{"error", error}};
}

ProxySettings::ProxySettings()
    : listenHost("127.0.0.1"),
      listenPort(8010),
      upstreamBaseUrl(""),
      proxyPrefix("/v1"),
      upstreamUserAgent("curl/8.7.1"),
      forwardUserAgent(false),
      upstreamTimeoutSec(1800),
      bufferTimeoutSec(180),
      requestBodyLimitBytes(defaultRequestBodyLimitBytes()),
      responseBufferLimitBytes(defaultResponseBufferLimitBytes()),
      reasoning516RetryCount(3),
      interceptRuleMode(interceptRuleModeReasoningTokens()),
      reasoningEquals(defaultReasoningEquals()),
      guardRetryAttempts(3),
      retryUpstreamCapacityErrors(true),
      guardEndpoints(defaultGuardEndpoints()),
      interceptStreaming(true),
      interceptNonStreaming(true),
      nonStreamStatusCode(502),
      streamAction("strict_502")
{
}

class ProxyConnection : public QObject {
    Q_OBJECT

public:
    ProxyConnection(HttpProxyServer *server,
                    QNetworkAccessManager *manager,
                    const QUrl &upstreamBase,
                    const QString &upstreamBasePath,
                    const QString &proxyPrefix,
                    QTcpSocket *socket)
        : QObject(server),
          server_(server),
          manager_(manager),
          upstreamBase_(upstreamBase),
          upstreamBasePath_(upstreamBasePath),
          proxyPrefix_(proxyPrefix),
          socket_(socket),
          currentReply_(0),
          upstreamResponseStatus_(0),
          upstreamResponseStateReady_(false),
          upstreamIsJson_(false),
          upstreamIsStream_(false),
          upstreamShouldInspect_(false),
          upstreamPassThrough_(false),
          streamInspectedRecorded_(false),
          streamObservedReasoning_(-1),
          streamCompletedSeen_(false),
          streamTerminalSeen_(false),
          streamUsageSeen_(false),
          streamFailureSeen_(false),
          streamingWroteAnyBody_(false),
          requestUsesChunkedEncoding_(false),
          contentLength_(-1),
          headerEnd_(-1),
          headerTerminatorLength_(0),
          retryAttempt_(0),
          requestIsStream_(false),
          proxyRequestRecorded_(false),
          responseWritten_(false),
          clientCloseRecorded_(false),
          responseTimedOut_(false),
          responseLimitExceeded_(false),
          upstreamTimedOut_(false),
          upstreamBytesRead_(0),
          downstreamBytesQueued_(0),
          downstreamBytesWritten_(0),
          streamDoneSeen_(false),
          diagnosticsLogged_(false),
          diagnosticClientClosedFirst_(false),
          diagnosticStatusCode_(0)
    {
        socket_->setParent(this);
        connect(socket_, SIGNAL(readyRead()), this, SLOT(readClient()));
        connect(socket_, SIGNAL(bytesWritten(qint64)), this, SLOT(clientBytesWritten(qint64)));
        connect(socket_, SIGNAL(disconnected()), this, SLOT(clientDisconnected()));
        connect(socket_, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(clientSocketError(QAbstractSocket::SocketError)));
        requestBufferTimer_.setSingleShot(true);
        requestBufferTimer_.setParent(this);
        responseBufferTimer_.setSingleShot(true);
        responseBufferTimer_.setParent(this);
        connect(&requestBufferTimer_, SIGNAL(timeout()), this, SLOT(requestBufferTimedOut()));
        connect(&responseBufferTimer_, SIGNAL(timeout()), this, SLOT(responseBufferTimedOut()));
        startRequestBufferTimer();
    }

    ~ProxyConnection()
    {
        abortCurrentReply();
    }

private slots:
    void readClient()
    {
        buffer_.append(socket_->readAll());
        if (headerEnd_ < 0) {
            headerEnd_ = buffer_.indexOf("\r\n\r\n");
            if (headerEnd_ < 0) {
                headerEnd_ = buffer_.indexOf("\n\n");
                if (headerEnd_ >= 0) {
                    headerTerminatorLength_ = 2;
                }
            } else {
                headerTerminatorLength_ = 4;
            }
            if (headerEnd_ < 0) {
                return;
            }
            if (!parseHeaders()) {
                writeJson(QJsonObject{{"error", QJsonObject{{"message", "invalid HTTP request"}, {"type", "bad_request"}}}}, 400);
                return;
            }
            parseTargetPath();
            if (!requestUsesChunkedEncoding_ && contentLength_ < 0) {
                writeJson(QJsonObject{{"error", QJsonObject{{"message", "invalid Content-Length"}, {"type", "bad_request"}}}}, 400);
                return;
            }
            if (!requestUsesChunkedEncoding_ &&
                server_->settings().requestBodyLimitBytes > 0 &&
                contentLength_ > server_->settings().requestBodyLimitBytes) {
                rejectRequestBodyLimitExceeded();
                return;
            }
        }

        if (requestUsesChunkedEncoding_) {
            const QByteArray encodedBody = buffer_.mid(headerEnd_ + headerTerminatorLength_);
            QByteArray decodedBody;
            bool complete = false;
            bool limitExceeded = false;
            QString errorMessage;
            if (!decodeChunkedBody(encodedBody, &decodedBody, &complete, &limitExceeded, &errorMessage)) {
                if (limitExceeded) {
                    rejectRequestBodyLimitExceeded();
                } else {
                    rejectBadRequest(errorMessage.isEmpty() ? QString("invalid chunked request body") : errorMessage);
                }
                return;
            }
            if (!complete) {
                return;
            }
            requestBufferTimer_.stop();
            forwardOrHandleControl(decodedBody);
            return;
        }

        if (contentLength_ > 0 && buffer_.size() < headerEnd_ + headerTerminatorLength_ + contentLength_) {
            return;
        }

        requestBufferTimer_.stop();
        const QByteArray body = contentLength_ > 0
            ? buffer_.mid(headerEnd_ + headerTerminatorLength_, int(contentLength_))
            : QByteArray();
        forwardOrHandleControl(body);
    }

    void upstreamReadyRead()
    {
        QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
        if (!reply || reply != currentReply_) {
            return;
        }

        const QByteArray chunk = reply->readAll();
        if (chunk.isEmpty()) {
            return;
        }
        noteUpstreamBytes(chunk);
        if (method_.toUpper() == "HEAD") {
            return;
        }

        if (upstreamPassThrough_) {
            if (canUseStreamingEarlyDecision() && inspectStreamingChunk(reply, chunk, false)) {
                return;
            }
            writeClient(chunk);
            streamingWroteAnyBody_ = true;
            socket_->flush();
            return;
        }

        const qint64 responseLimit = server_->settings().responseBufferLimitBytes;
        if (responseLimit > 0 &&
            qint64(upstreamBodyBuffer_.size()) + qint64(chunk.size()) > responseLimit) {
            responseLimitExceeded_ = true;
            reply->abort();
            return;
        }
        upstreamBodyBuffer_.append(chunk);
        if (!ensureUpstreamResponseState(reply) || !canUseStreamingEarlyDecision()) {
            return;
        }

        if (inspectStreamingChunk(reply, chunk, true)) {
            return;
        }
        if (normalizeStreamAction(server_->settings().streamAction) == streamActionDisconnect() &&
            !shouldDelayStreamingPassThroughForRetry()) {
            beginUpstreamPassThrough(reply);
        }
    }

    void upstreamFinished()
    {
        QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
        if (!reply) {
            return;
        }
        if (reply != currentReply_) {
            reply->deleteLater();
            return;
        }
        currentReply_ = 0;
        responseBufferTimer_.stop();
        reply->deleteLater();

        if (responseTimedOut_) {
            const int responseStatus = 502;
            const QString errorType = "buffer_timeout";
            const QString errorMessage = QString("upstream response buffering timed out after %1 seconds")
                .arg(server_->settings().bufferTimeoutSec);
            writeJson(QJsonObject{{"error", QJsonObject{{"message", errorMessage}, {"type", errorType}}}}, responseStatus);
            record(responseStatus, errorType, errorMessage);
            return;
        }
        if (responseLimitExceeded_) {
            const int responseStatus = 502;
            const QString errorType = "response_buffer_limit_exceeded";
            const QString errorMessage = QString("upstream response exceeded buffer limit: %1 bytes")
                .arg(server_->settings().responseBufferLimitBytes);
            writeJson(QJsonObject{{"error", QJsonObject{{"message", errorMessage}, {"type", errorType}}}}, responseStatus);
            record(responseStatus, errorType, errorMessage);
            return;
        }
        if (upstreamTimedOut_) {
            const int responseStatus = 504;
            const QString errorType = "upstream_timeout";
            const QString errorMessage = QString("upstream request timed out after %1 seconds")
                .arg(server_->settings().upstreamTimeoutSec);
            writeJson(QJsonObject{{"error", QJsonObject{{"message", errorMessage}, {"type", errorType}}}}, responseStatus);
            record(responseStatus, errorType, errorMessage);
            return;
        }

        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        int responseStatus = statusCode > 0 ? statusCode : 502;
        QString errorType;
        QString errorMessage;
        if (statusCode <= 0 || reply->error() != QNetworkReply::NoError) {
            if (statusCode <= 0) {
                errorType = "proxy_error";
                errorMessage = reply->errorString();
                writeJson(QJsonObject{{"error", QJsonObject{{"message", errorMessage}, {"type", errorType}}}}, 502);
                record(responseStatus, errorType, errorMessage);
                return;
            }
            if (statusCode >= 400) {
                errorType = "upstream_http_error";
            }
        } else if (statusCode >= 400) {
            errorType = "upstream_http_error";
        }

        const QByteArray remainingBody = method_.toUpper() == "HEAD" ? QByteArray() : reply->readAll();
        if (!remainingBody.isEmpty()) {
            noteUpstreamBytes(remainingBody);
        }
        const qint64 responseLimit = server_->settings().responseBufferLimitBytes;
        if (!upstreamPassThrough_ && responseLimit > 0 &&
            qint64(upstreamBodyBuffer_.size()) + qint64(remainingBody.size()) > responseLimit) {
            const int limitStatus = 502;
            const QString limitType = "response_buffer_limit_exceeded";
            const QString limitMessage = QString("upstream response exceeded buffer limit: %1 bytes")
                .arg(responseLimit);
            writeJson(QJsonObject{{"error", QJsonObject{{"message", limitMessage}, {"type", limitType}}}}, limitStatus);
            record(limitStatus, limitType, limitMessage);
            return;
        }
        if (upstreamPassThrough_) {
            if (!remainingBody.isEmpty()) {
                writeClient(remainingBody);
                streamingWroteAnyBody_ = true;
            }
            socket_->flush();
            responseWritten_ = true;
            socket_->disconnectFromHost();
            if (upstreamIsStream_ && upstreamShouldInspect_ && !streamInspectedRecorded_) {
                server_->recordInspectedResponse(streamObservedReasoning_, false, "stream");
                streamInspectedRecorded_ = true;
            }
            record(responseStatus, errorType, errorMessage, streamObservedReasoning_);
            return;
        }

        const QByteArray responseBody = method_.toUpper() == "HEAD"
            ? QByteArray()
            : upstreamBodyBuffer_ + remainingBody;
        upstreamBodyBuffer_.clear();
        const QString contentType = QString::fromLatin1(reply->rawHeader("Content-Type"));
        int reasoningTokens = -1;
        const bool isJson = isJsonContent(contentType);
        const bool isStream = isEventStream(contentType) || (requestIsStream_ && !isJson);
        const bool shouldInspect = shouldInspectCurrentRequest();
        if (!shouldInspect) {
            server_->recordBypassedProxyRequest();
        }
        if (shouldInspect && method_.toUpper() != "HEAD") {
            QJsonValue responsePayload;
            StructureSignals structure;
            bool streamTerminalSeen = false;
            bool streamUsageSeen = false;
            bool streamFailureSeen = false;
            if (isJson || isStream) {
                if (isStream) {
                    bool doneSeen = false;
                    const QList<QJsonValue> payloads = parseSsePayloads(responseBody, &doneSeen);
                    if (doneSeen) {
                        streamDoneSeen_ = true;
                        streamTerminalSeen = true;
                        streamTerminalSeen_ = true;
                    }
                    for (int i = 0; i < payloads.size(); ++i) {
                        if (isTerminalSsePayload(payloads.at(i))) {
                            streamTerminalSeen = true;
                            streamCompletedSeen_ = true;
                            streamTerminalSeen_ = true;
                        }
                        if (payloadHasUsage(payloads.at(i))) {
                            streamUsageSeen = true;
                        }
                        if (isFailureSsePayload(payloads.at(i))) {
                            streamFailureSeen = true;
                        }
                        const int extracted = findReasoningTokens(payloads.at(i));
                        if (extracted >= 0) {
                            reasoningTokens = extracted;
                        }
                        applyStructureSignalsFromPayload(payloads.at(i), &structure);
                    }
                } else {
                    responsePayload = parseJsonBody(responseBody);
                    if (!responsePayload.isUndefined() && !responsePayload.isNull()) {
                        reasoningTokens = findReasoningTokens(responsePayload);
                        applyStructureSignalsFromPayload(responsePayload, &structure);
                    }
                }
            }
            const ProxySettings settings = server_->settings();
            const RuleMatch ruleMatch = buildRuleMatch(settings, reasoningTokens, requestKind_, requestReasoningEffort_, structure);
            interceptExemptReason_ = ruleMatch.exemptReason;
            const bool matched = ruleMatch.matched;
            const QString streamKind = isStream ? QString("stream") : QString("non-stream");
            if (!streamInspectedRecorded_) {
                server_->recordInspectedResponse(reasoningTokens, matched, streamKind);
                streamInspectedRecorded_ = isStream;
            }
            if (!ruleMatch.exemptReason.isEmpty()) {
                emit server_->logLine(QString("[pass] %1 path=%2 %3 action=intercept_exempt mode=%4")
                    .arg(streamKind)
                    .arg(path_)
                    .arg(ruleMatch.reasonForLog)
                    .arg(ruleMatch.mode));
            }
            const bool capacityRetryMatched = settings.retryUpstreamCapacityErrors &&
                !isStream &&
                upstreamCapacityErrorMatched(responseStatus, responsePayload, responseBody);
            if (capacityRetryMatched) {
                const bool canGuardRetry = retryAttempt_ < settings.guardRetryAttempts;
                const QString action = canGuardRetry
                    ? QString("internal_retry remaining=%1").arg(settings.guardRetryAttempts - retryAttempt_)
                    : QString("pass_through");
                emit server_->logLine(QString("[upstream-capacity] non-stream path=%1 status=%2 action=%3")
                    .arg(path_)
                    .arg(responseStatus)
                    .arg(action));
                if (canGuardRetry) {
                    ++retryAttempt_;
                    server_->recordReasoningGuardRetry(method_, path_, retryAttempt_, settings.guardRetryAttempts, reasoningTokens, requestKind_, interceptExemptReason_);
                    startUpstreamRequest();
                    return;
                }
            }
            if (matched) {
                const bool shouldIntercept = isStream ? settings.interceptStreaming : settings.interceptNonStreaming;
                const bool canGuardRetry = shouldIntercept && retryAttempt_ < settings.guardRetryAttempts;
                const QString action = !shouldIntercept
                    ? QString("observe_only")
                    : (canGuardRetry
                        ? QString("internal_retry remaining=%1").arg(settings.guardRetryAttempts - retryAttempt_)
                        : QString("return_status_%1").arg(settings.nonStreamStatusCode));
                emit server_->logLine(QString("[match] %1 path=%2 %3 action=%4 mode=%5")
                    .arg(streamKind)
                    .arg(path_)
                    .arg(ruleMatch.reasonForLog)
                    .arg(action)
                    .arg(ruleMatch.mode));
                if (canGuardRetry) {
                    ++retryAttempt_;
                    server_->recordReasoningGuardRetry(method_, path_, retryAttempt_, settings.guardRetryAttempts, reasoningTokens, requestKind_, interceptExemptReason_);
                    startUpstreamRequest();
                    return;
                }
                if (shouldIntercept) {
                    server_->recordBlockedResponse(streamKind);
                    responseStatus = settings.nonStreamStatusCode;
                    errorType = "reasoning_guard_triggered";
                    errorMessage = QString("response matched suspicious reasoning_tokens=%1 after %2 retries")
                        .arg(reasoningTokens)
                        .arg(settings.guardRetryAttempts);
                    QList<QPair<QByteArray, QByteArray> > extraHeaders;
                    extraHeaders.append(qMakePair(QByteArray("x-codex-retry-gateway-reason"), QByteArray("reasoning-guard-triggered")));
                    writeJson(blockedReasoningBody(path_, reasoningTokens, responseStatus), responseStatus, extraHeaders);
                    record(responseStatus, errorType, errorMessage, reasoningTokens);
                    return;
                }
            }
            if (isStream && settings.interceptStreaming) {
                const QString anomalyType = guardedStreamAnomalyType(responseStatus,
                                                                     streamFailureSeen,
                                                                     streamTerminalSeen,
                                                                     streamUsageSeen);
                if (!anomalyType.isEmpty() && handleStreamingAnomaly(0, anomalyType)) {
                    return;
                }
            }
        }

        const QList<QPair<QByteArray, QByteArray> > headers = filteredReplyHeaders(reply);
        writeResponse(responseStatus, headers, responseBody);
        record(responseStatus, errorType, errorMessage, reasoningTokens);
    }

    void upstreamTimedOut()
    {
        if (!currentReply_) {
            return;
        }
        upstreamTimedOut_ = true;
        currentReply_->abort();
    }

    void requestBufferTimedOut()
    {
        if (responseWritten_ || proxyRequestRecorded_) {
            return;
        }
        parseTargetPath();
        ensureProxyRequestRecorded();
        const int responseStatus = 408;
        const QString errorType = "buffer_timeout";
        const QString errorMessage = QString("request buffering timed out after %1 seconds")
            .arg(server_->settings().bufferTimeoutSec);
        writeJson(QJsonObject{{"error", QJsonObject{{"message", errorMessage}, {"type", errorType}}}}, responseStatus);
        record(responseStatus, errorType, errorMessage);
    }

    void responseBufferTimedOut()
    {
        if (!currentReply_) {
            return;
        }
        responseTimedOut_ = true;
        currentReply_->abort();
    }

    void clientDisconnected()
    {
        const QString message = responseWritten_
            ? QString("client disconnected after response")
            : QString("client disconnected before upstream response completed");
        handleClientClosed(message);
        emitTransferDiagnostics(message);
        deleteLater();
    }

    void clientSocketError(QAbstractSocket::SocketError)
    {
        handleClientClosed(socket_ ? socket_->errorString() : QString("client socket error"));
        emitTransferDiagnostics(socket_ ? socket_->errorString() : QString("client socket error"));
        deleteLater();
    }

    void clientBytesWritten(qint64 bytes)
    {
        if (bytes > 0) {
            downstreamBytesWritten_ += bytes;
        }
        if (diagnosticsLogged_) {
            emitTransferDiagnostics();
        }
    }

private:
    bool parseHeaders()
    {
        const QByteArray headerBytes = buffer_.left(headerEnd_);
        QList<QByteArray> lines = headerBytes.split('\n');
        if (lines.isEmpty()) {
            return false;
        }

        QByteArray first = lines.takeFirst().trimmed();
        QList<QByteArray> parts = first.split(' ');
        if (parts.size() < 2) {
            return false;
        }
        method_ = QString::fromLatin1(parts.at(0)).trimmed();
        target_ = QString::fromLatin1(parts.at(1)).trimmed();
        headers_.clear();
        for (int i = 0; i < lines.size(); ++i) {
            const QByteArray line = lines.at(i).trimmed();
            const int colon = line.indexOf(':');
            if (colon <= 0) {
                continue;
            }
            const QString key = QString::fromLatin1(line.left(colon)).trimmed().toLower();
            const QByteArray value = line.mid(colon + 1).trimmed();
            headers_.insert(key, value);
        }
        requestUsesChunkedEncoding_ = headerValue(headers_, "transfer-encoding").toLower().contains("chunked");
        bool ok = false;
        const QByteArray contentLengthHeader = headers_.value("content-length");
        contentLength_ = QString::fromLatin1(contentLengthHeader).toLongLong(&ok);
        if (!contentLengthHeader.isEmpty() && !ok && !requestUsesChunkedEncoding_) {
            contentLength_ = -1;
        } else if (!ok) {
            contentLength_ = 0;
        }
        return true;
    }

    void parseTargetPath()
    {
        if (!path_.isEmpty() || target_.isEmpty()) {
            return;
        }
        QUrl requestUrl(target_);
        query_.clear();
        if (requestUrl.isValid() && !requestUrl.isRelative()) {
            path_ = requestUrl.path().isEmpty() ? "/" : requestUrl.path();
            query_ = requestUrl.query(QUrl::FullyEncoded);
        } else {
            const int question = target_.indexOf('?');
            path_ = question >= 0 ? target_.left(question) : target_;
            query_ = question >= 0 ? target_.mid(question + 1) : QString();
            if (path_.isEmpty()) {
                path_ = "/";
            }
        }
    }

    void ensureProxyRequestRecorded()
    {
        if (proxyRequestRecorded_) {
            return;
        }
        server_->recordRequest("proxy");
        proxyRequestRecorded_ = true;
        if (!elapsed_.isValid()) {
            elapsed_.start();
        }
    }

    void rejectRequestBodyLimitExceeded()
    {
        requestBufferTimer_.stop();
        parseTargetPath();
        ensureProxyRequestRecorded();
        const int responseStatus = 413;
        const QString errorType = "request_body_limit_exceeded";
        const QString errorMessage = QString("request body exceeded limit: %1 bytes")
            .arg(server_->settings().requestBodyLimitBytes);
        writeJson(QJsonObject{{"error", QJsonObject{{"message", errorMessage}, {"type", errorType}}}}, responseStatus);
        record(responseStatus, errorType, errorMessage);
    }

    void rejectBadRequest(const QString &message)
    {
        requestBufferTimer_.stop();
        parseTargetPath();
        ensureProxyRequestRecorded();
        const int responseStatus = 400;
        const QString errorType = "bad_request";
        const QString errorMessage = message.isEmpty() ? QString("invalid HTTP request") : message;
        writeJson(QJsonObject{{"error", QJsonObject{{"message", errorMessage}, {"type", errorType}}}}, responseStatus);
        record(responseStatus, errorType, errorMessage);
    }

    bool decodeChunkedBody(const QByteArray &encoded,
                           QByteArray *decoded,
                           bool *complete,
                           bool *limitExceeded,
                           QString *errorMessage) const
    {
        if (decoded) {
            decoded->clear();
        }
        if (complete) {
            *complete = false;
        }
        if (limitExceeded) {
            *limitExceeded = false;
        }

        int pos = 0;
        while (true) {
            const int crlfEnd = encoded.indexOf("\r\n", pos);
            const int lfEnd = encoded.indexOf('\n', pos);
            int lineEnd = -1;
            int terminatorLength = 0;
            if (crlfEnd >= 0 && (lfEnd < 0 || crlfEnd <= lfEnd)) {
                lineEnd = crlfEnd;
                terminatorLength = 2;
            } else if (lfEnd >= 0) {
                lineEnd = lfEnd;
                terminatorLength = 1;
            }
            if (lineEnd < 0) {
                return true;
            }

            QByteArray sizeLine = encoded.mid(pos, lineEnd - pos).trimmed();
            const int extension = sizeLine.indexOf(';');
            if (extension >= 0) {
                sizeLine = sizeLine.left(extension).trimmed();
            }
            bool ok = false;
            const qint64 chunkSize = sizeLine.toLongLong(&ok, 16);
            if (!ok || chunkSize < 0) {
                if (errorMessage) {
                    *errorMessage = "invalid chunk size";
                }
                return false;
            }
            if (chunkSize > qint64(0x7fffffff)) {
                if (errorMessage) {
                    *errorMessage = "chunk size is too large";
                }
                return false;
            }

            pos = lineEnd + terminatorLength;
            if (chunkSize == 0) {
                while (true) {
                    const int trailerCrlfEnd = encoded.indexOf("\r\n", pos);
                    const int trailerLfEnd = encoded.indexOf('\n', pos);
                    int trailerEnd = -1;
                    int trailerTerminatorLength = 0;
                    if (trailerCrlfEnd >= 0 && (trailerLfEnd < 0 || trailerCrlfEnd <= trailerLfEnd)) {
                        trailerEnd = trailerCrlfEnd;
                        trailerTerminatorLength = 2;
                    } else if (trailerLfEnd >= 0) {
                        trailerEnd = trailerLfEnd;
                        trailerTerminatorLength = 1;
                    }
                    if (trailerEnd < 0) {
                        return true;
                    }
                    const QByteArray trailerLine = encoded.mid(pos, trailerEnd - pos).trimmed();
                    pos = trailerEnd + trailerTerminatorLength;
                    if (trailerLine.isEmpty()) {
                        if (complete) {
                            *complete = true;
                        }
                        return true;
                    }
                }
            }

            if (encoded.size() < pos + int(chunkSize)) {
                return true;
            }
            const qint64 requestLimit = server_->settings().requestBodyLimitBytes;
            if (requestLimit > 0 && decoded &&
                qint64(decoded->size()) + chunkSize > requestLimit) {
                if (limitExceeded) {
                    *limitExceeded = true;
                }
                return false;
            }
            if (decoded) {
                decoded->append(encoded.mid(pos, int(chunkSize)));
            }
            pos += int(chunkSize);

            if (encoded.size() < pos + 1) {
                return true;
            }
            if (encoded.mid(pos, 2) == "\r\n") {
                pos += 2;
            } else if (encoded.at(pos) == '\n') {
                ++pos;
            } else {
                if (errorMessage) {
                    *errorMessage = "chunk data missing line terminator";
                }
                return false;
            }
        }
    }

    void forwardOrHandleControl(const QByteArray &body)
    {
        parseTargetPath();

        const bool rootProxy = proxyPrefix_.isEmpty();
        if (path_ == "/healthz" || (!rootProxy && path_ == "/")) {
            server_->recordRequest("health");
            writeJson(server_->statusPayload().value("health").toObject());
            return;
        }
        if (path_ == "/version" || path_ == "/v1/version") {
            server_->recordRequest("version");
            writeJson(versionPayload());
            return;
        }
        if (path_ == "/props" || path_ == "/v1/props") {
            server_->recordRequest("version");
            writeJson(propsPayload());
            return;
        }
        if (path_ == "/status" || path_ == "/metrics") {
            server_->recordRequest("status");
            writeJson(server_->statusPayload());
            return;
        }

        ensureProxyRequestRecorded();

        requestBody_ = body;
        const QJsonValue requestPayload = parseJsonBody(requestBody_);
        requestJson_ = requestPayload.isObject() ? requestPayload.toObject() : QJsonObject();
        requestKind_ = detectRequestKind(headers_, requestJson_);
        requestReasoningEffort_ = requestReasoningEffort(requestJson_);
        requestIsStream_ = requestJson_.value("stream").toBool(false);
        startUpstreamRequest();
    }

    void startRequestBufferTimer()
    {
        const int seconds = server_->settings().bufferTimeoutSec;
        if (seconds > 0) {
            requestBufferTimer_.start(seconds * 1000);
        }
    }

    void startResponseBufferTimer()
    {
        const int seconds = server_->settings().bufferTimeoutSec;
        if (seconds > 0) {
            responseBufferTimer_.start(seconds * 1000);
        }
    }

    void startUpstreamRequest()
    {
        resetUpstreamResponseState();
        server_->recordUpstreamAttempt();
        const QString suffix = pathWithoutProxyPrefix(path_, proxyPrefix_);

        QUrl upstream = upstreamBase_;
        upstream.setPath(joinPaths(upstreamBasePath_, suffix));
        upstream.setQuery(query_);

        QNetworkRequest request(upstream);
        request.setRawHeader("Accept", headerValue(headers_, "accept", "*/*").toLatin1());
        request.setRawHeader("Accept-Encoding", "identity");
        request.setRawHeader("Content-Type", headerValue(headers_, "content-type", "application/json").toLatin1());
        const ProxySettings settings = server_->settings();
        const QString userAgent = settings.forwardUserAgent
            ? headerValue(headers_, "user-agent", settings.upstreamUserAgent)
            : settings.upstreamUserAgent;
        request.setRawHeader("User-Agent", userAgent.toLatin1());

        if (!settings.upstreamApiKey.trimmed().isEmpty()) {
            request.setRawHeader("Authorization", QString("Bearer %1").arg(settings.upstreamApiKey.trimmed()).toLatin1());
        } else {
            const QString auth = headerValue(headers_, "authorization");
            if (!auth.isEmpty()) {
                request.setRawHeader("Authorization", auth.toLatin1());
            }
        }

        const QStringList passthroughHeaders = QStringList()
            << "openai-organization"
            << "openai-project"
            << "openai-beta"
            << "idempotency-key";
        for (int i = 0; i < passthroughHeaders.size(); ++i) {
            const QString key = passthroughHeaders.at(i);
            const QByteArray value = headers_.value(key);
            if (!value.isEmpty()) {
                request.setRawHeader(key.toLatin1(), value);
            }
        }

        currentReply_ = manager_->sendCustomRequest(request, method_.toLatin1(), requestBody_);
        connect(currentReply_, SIGNAL(readyRead()), this, SLOT(upstreamReadyRead()));
        connect(currentReply_, SIGNAL(finished()), this, SLOT(upstreamFinished()));
        startResponseBufferTimer();
        if (settings.upstreamTimeoutSec > 0) {
            QTimer *timer = new QTimer(currentReply_);
            timer->setSingleShot(true);
            connect(timer, SIGNAL(timeout()), this, SLOT(upstreamTimedOut()));
            timer->start(settings.upstreamTimeoutSec * 1000);
        }
    }

    void resetUpstreamResponseState()
    {
        upstreamBodyBuffer_.clear();
        sseScanBuffer_.clear();
        streamStructure_ = StructureSignals();
        upstreamResponseStatus_ = 0;
        upstreamResponseStateReady_ = false;
        upstreamIsJson_ = false;
        upstreamIsStream_ = false;
        upstreamShouldInspect_ = false;
        upstreamPassThrough_ = false;
        streamInspectedRecorded_ = false;
        streamObservedReasoning_ = -1;
        streamCompletedSeen_ = false;
        streamTerminalSeen_ = false;
        streamUsageSeen_ = false;
        streamFailureSeen_ = false;
        streamingWroteAnyBody_ = false;
        responseTimedOut_ = false;
        responseLimitExceeded_ = false;
        upstreamTimedOut_ = false;
        streamDoneSeen_ = false;
        interceptExemptReason_.clear();
    }

    bool ensureUpstreamResponseState(QNetworkReply *reply)
    {
        if (upstreamResponseStateReady_) {
            return true;
        }
        if (!reply) {
            return false;
        }
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (statusCode <= 0) {
            return false;
        }
        const QString contentType = QString::fromLatin1(reply->rawHeader("Content-Type"));
        upstreamResponseStatus_ = statusCode;
        upstreamIsJson_ = isJsonContent(contentType);
        upstreamIsStream_ = isEventStream(contentType) || (requestIsStream_ && !upstreamIsJson_);
        upstreamShouldInspect_ = shouldInspectCurrentRequest();
        upstreamResponseStateReady_ = true;
        return true;
    }

    bool shouldInspectCurrentRequest() const
    {
        return shouldInspectPathWithProxyPrefix(server_->settings(), path_, proxyPrefix_);
    }

    bool canUseStreamingEarlyDecision() const
    {
        const ProxySettings settings = server_->settings();
        return upstreamShouldInspect_ &&
            upstreamIsStream_ &&
            method_.toUpper() != "HEAD" &&
            normalizeInterceptRuleMode(settings.interceptRuleMode) == interceptRuleModeReasoningTokens();
    }

    bool shouldDelayStreamingPassThroughForRetry() const
    {
        const ProxySettings settings = server_->settings();
        return settings.interceptStreaming && retryAttempt_ < settings.guardRetryAttempts;
    }

    QList<QPair<QByteArray, QByteArray> > filteredReplyHeaders(QNetworkReply *reply) const
    {
        QList<QPair<QByteArray, QByteArray> > headers;
        if (!reply) {
            return headers;
        }
        const QList<QNetworkReply::RawHeaderPair> pairs = reply->rawHeaderPairs();
        const QStringList hopHeaders = hopByHopHeaders();
        for (int i = 0; i < pairs.size(); ++i) {
            const QByteArray name = pairs.at(i).first;
            const QString lower = QString::fromLatin1(name).toLower();
            if (hopHeaders.contains(lower) || lower == "content-length") {
                continue;
            }
            headers.append(qMakePair(name, pairs.at(i).second));
        }
        return headers;
    }

    void writeStreamingResponseHead(int statusCode,
                                    const QList<QPair<QByteArray, QByteArray> > &headers)
    {
        QByteArray response;
        response += "HTTP/1.1 " + QByteArray::number(statusCode) + " " + reasonPhrase(statusCode) + "\r\n";
        for (int i = 0; i < headers.size(); ++i) {
            response += headers.at(i).first + ": " + headers.at(i).second + "\r\n";
        }
        response += "Connection: close\r\n\r\n";
        writeClient(response);
    }

    void beginUpstreamPassThrough(QNetworkReply *reply)
    {
        if (upstreamPassThrough_) {
            return;
        }
        upstreamPassThrough_ = true;
        responseBufferTimer_.stop();
        const int statusCode = upstreamResponseStatus_ > 0 ? upstreamResponseStatus_ : 200;
        writeStreamingResponseHead(statusCode, filteredReplyHeaders(reply));
        if (!upstreamBodyBuffer_.isEmpty()) {
            writeClient(upstreamBodyBuffer_);
            streamingWroteAnyBody_ = true;
            upstreamBodyBuffer_.clear();
        }
        socket_->flush();
    }

    bool inspectStreamingChunk(QNetworkReply *reply, const QByteArray &chunk, bool allowTerminalHandling)
    {
        sseScanBuffer_.append(chunk);
        bool terminalSeen = false;
        bool doneSeen = false;
        const QList<QJsonValue> payloads = takeCompleteSsePayloads(&sseScanBuffer_, &terminalSeen, &doneSeen);
        for (int i = 0; i < payloads.size(); ++i) {
            const QJsonValue payload = payloads.at(i);
            if (isTerminalSsePayload(payload)) {
                streamCompletedSeen_ = true;
                streamTerminalSeen_ = true;
            }
            if (payloadHasUsage(payload)) {
                streamUsageSeen_ = true;
            }
            if (isFailureSsePayload(payload)) {
                streamFailureSeen_ = true;
            }
            const int extracted = findReasoningTokens(payload);
            if (extracted >= 0) {
                streamObservedReasoning_ = extracted;
            }
            applyStructureSignalsFromPayload(payload, &streamStructure_);
            if (extracted >= 0 && handleStreamingReasoningObservation(reply, extracted)) {
                return true;
            }
        }
        if (terminalSeen) {
            streamTerminalSeen_ = true;
        }
        if (doneSeen) {
            streamDoneSeen_ = true;
        }
        if (allowTerminalHandling && terminalSeen && handleStreamingTerminalObservation(reply)) {
            return true;
        }
        return false;
    }

    void abortCurrentReply(QNetworkReply *reply)
    {
        if (reply && reply == currentReply_) {
            currentReply_ = 0;
            responseBufferTimer_.stop();
            reply->abort();
            reply->deleteLater();
        }
    }

    void abortCurrentReply()
    {
        if (!currentReply_) {
            return;
        }
        QNetworkReply *reply = currentReply_;
        currentReply_ = 0;
        responseBufferTimer_.stop();
        disconnect(reply, 0, this, 0);
        reply->abort();
        reply->deleteLater();
    }

    void handleClientClosed(const QString &message)
    {
        requestBufferTimer_.stop();
        responseBufferTimer_.stop();
        if (!responseWritten_ && !clientCloseRecorded_ && (currentReply_ || proxyRequestRecorded_)) {
            diagnosticClientClosedFirst_ = true;
            clientCloseRecorded_ = true;
            record(499, "client_connection_error", message);
        }
        abortCurrentReply();
    }

    bool handleStreamingReasoningObservation(QNetworkReply *reply, int reasoningTokens)
    {
        const ProxySettings settings = server_->settings();
        const int maxGuard = maxReasoningEquals(settings);
        const bool matched = reasoningMatched(settings, reasoningTokens);
        if (!matched && (maxGuard < 0 || reasoningTokens <= maxGuard)) {
            return false;
        }

        if (!streamInspectedRecorded_) {
            server_->recordInspectedResponse(reasoningTokens, matched, "stream");
            streamInspectedRecorded_ = true;
        }

        if (!matched) {
            emit server_->logLine(QString("[pass] stream path=%1 reasoning_tokens=%2 action=early_pass_through max_guard=%3")
                .arg(path_)
                .arg(reasoningTokens)
                .arg(maxGuard));
            if (!upstreamPassThrough_) {
                beginUpstreamPassThrough(reply);
                return true;
            }
            return false;
        }

        const bool shouldIntercept = settings.interceptStreaming;
        const bool disconnectMode = normalizeStreamAction(settings.streamAction) == streamActionDisconnect();
        const bool clientResponseStarted = upstreamPassThrough_ || streamingWroteAnyBody_;
        const bool canGuardRetry = shouldIntercept && !clientResponseStarted && retryAttempt_ < settings.guardRetryAttempts;
        const bool canReturnBlockedStatus = shouldIntercept && !disconnectMode && !clientResponseStarted;
        const QString action = !shouldIntercept
            ? QString("observe_only")
            : (canGuardRetry
                ? QString("internal_retry remaining=%1").arg(settings.guardRetryAttempts - retryAttempt_)
                : (canReturnBlockedStatus
                    ? QString("return_status_%1").arg(settings.nonStreamStatusCode)
                    : QString("disconnect")));
        emit server_->logLine(QString("[match] stream path=%1 reasoning_tokens=%2 action=%3 mode=%4")
            .arg(path_)
            .arg(reasoningTokens)
            .arg(action)
            .arg(interceptRuleModeReasoningTokens()));

        if (!shouldIntercept) {
            if (!upstreamPassThrough_) {
                beginUpstreamPassThrough(reply);
                return true;
            }
            return false;
        }

        abortCurrentReply(reply);
        if (canGuardRetry) {
            ++retryAttempt_;
            server_->recordReasoningGuardRetry(method_, path_, retryAttempt_, settings.guardRetryAttempts, reasoningTokens, requestKind_, interceptExemptReason_);
            startUpstreamRequest();
            return true;
        }
        if (canReturnBlockedStatus) {
            server_->recordBlockedResponse("stream");
            const int responseStatus = settings.nonStreamStatusCode;
            const QString errorMessage = QString("response matched suspicious reasoning_tokens=%1 after %2 retries")
                .arg(reasoningTokens)
                .arg(settings.guardRetryAttempts);
            QList<QPair<QByteArray, QByteArray> > extraHeaders;
            extraHeaders.append(qMakePair(QByteArray("x-codex-retry-gateway-reason"), QByteArray("reasoning-guard-triggered")));
            writeJson(blockedReasoningBody(path_, reasoningTokens, responseStatus), responseStatus, extraHeaders);
            record(responseStatus, "reasoning_guard_triggered", errorMessage, reasoningTokens);
            return true;
        }

        server_->recordBlockedResponse("stream");
        const QString errorMessage = QString("response matched suspicious reasoning_tokens=%1 after %2 retries")
            .arg(reasoningTokens)
            .arg(settings.guardRetryAttempts);
        clientCloseRecorded_ = true;
        record(499, "reasoning_guard_triggered", errorMessage, reasoningTokens);
        if (socket_) {
            socket_->abort();
        }
        return true;
    }

    bool handleStreamingTerminalObservation(QNetworkReply *reply)
    {
        const ProxySettings settings = server_->settings();
        const QString anomalyType = guardedStreamAnomalyType(upstreamResponseStatus_,
                                                             streamFailureSeen_,
                                                             streamTerminalSeen_,
                                                             streamUsageSeen_);
        if (!anomalyType.isEmpty() && handleStreamingAnomaly(reply, anomalyType)) {
            return true;
        }

        const RuleMatch ruleMatch = buildRuleMatch(settings,
                                                   streamObservedReasoning_,
                                                   requestKind_,
                                                   requestReasoningEffort_,
                                                   streamStructure_);
        interceptExemptReason_ = ruleMatch.exemptReason;
        if (ruleMatch.matched && streamObservedReasoning_ >= 0) {
            return handleStreamingReasoningObservation(reply, streamObservedReasoning_);
        }

        if (!streamInspectedRecorded_) {
            server_->recordInspectedResponse(streamObservedReasoning_, false, "stream");
            streamInspectedRecorded_ = true;
        }

        if (!ruleMatch.exemptReason.isEmpty()) {
            emit server_->logLine(QString("[pass] stream path=%1 %2 action=intercept_exempt mode=%3")
                .arg(path_)
                .arg(ruleMatch.reasonForLog)
                .arg(ruleMatch.mode));
        }
        emit server_->logLine(QString("[pass] stream path=%1 reasoning_tokens=%2 action=terminal_pass_through")
            .arg(path_)
            .arg(streamObservedReasoning_ >= 0 ? QString::number(streamObservedReasoning_) : QString("null")));
        beginUpstreamPassThrough(reply);
        return true;
    }

    bool handleStreamingAnomaly(QNetworkReply *reply, const QString &errorType)
    {
        if (errorType.isEmpty()) {
            return false;
        }

        const ProxySettings settings = server_->settings();
        if (!settings.interceptStreaming) {
            return false;
        }

        const bool canReturnBlockedStatus = !upstreamPassThrough_ && !streamingWroteAnyBody_;
        if (!canReturnBlockedStatus) {
            return false;
        }

        const bool canGuardRetry = retryAttempt_ < settings.guardRetryAttempts;
        const QString action = canGuardRetry
            ? QString("internal_retry remaining=%1").arg(settings.guardRetryAttempts - retryAttempt_)
            : QString("return_status_%1").arg(settings.nonStreamStatusCode);
        emit server_->logLine(QString("[stream-anomaly] path=%1 error_type=%2 action=%3")
            .arg(path_)
            .arg(errorType)
            .arg(action));

        if (reply && reply == currentReply_) {
            abortCurrentReply(reply);
        }

        if (canGuardRetry) {
            ++retryAttempt_;
            server_->recordReasoningGuardRetry(method_, path_, retryAttempt_, settings.guardRetryAttempts, streamObservedReasoning_, requestKind_, interceptExemptReason_);
            startUpstreamRequest();
            return true;
        }

        server_->recordBlockedResponse("stream");
        const int responseStatus = settings.nonStreamStatusCode;
        const QString errorMessage = QString("guarded stream response was not usable: %1").arg(errorType);
        writeJson(QJsonObject{{"error", QJsonObject{{"message", errorMessage}, {"type", errorType}}}}, responseStatus);
        record(responseStatus, errorType, errorMessage, streamObservedReasoning_);
        return true;
    }

    QJsonObject versionPayload() const
    {
        const ProxySettings settings = server_->settings();
        return QJsonObject{
            {"ok", true},
            {"name", "openai-reasoning-guard-api-proxy"},
            {"version", "local"},
            {"mode", "openai-proxy"},
            {"upstream_base_url", settings.upstreamBaseUrl},
            {"proxy_prefix", proxyPrefix_.isEmpty() ? QString("/") : proxyPrefix_},
            {"upstream_timeout_sec", settings.upstreamTimeoutSec},
            {"buffer_timeout_sec", settings.bufferTimeoutSec},
            {"request_body_limit_bytes", double(settings.requestBodyLimitBytes)},
            {"response_buffer_limit_bytes", double(settings.responseBufferLimitBytes)},
            {"intercept_rule_mode", normalizeInterceptRuleMode(settings.interceptRuleMode)},
            {"reasoning_equals", stringListToJsonArray(settings.reasoningEquals)},
            {"guard_retry_attempts", settings.guardRetryAttempts},
            {"retry_upstream_capacity_errors", settings.retryUpstreamCapacityErrors},
            {"intercept_streaming", settings.interceptStreaming},
            {"intercept_non_streaming", settings.interceptNonStreaming},
            {"non_stream_status_code", settings.nonStreamStatusCode},
            {"stream_action", settings.streamAction},
            {"guard_endpoints", stringListToJsonArray(settings.guardEndpoints)},
            {"reasoning_516_retry_count", settings.guardRetryAttempts},
            {"forward_user_agent", settings.forwardUserAgent},
            {"upstream_proxy", settings.upstreamProxy},
            {"upstream_http_proxy", settings.upstreamHttpProxy},
            {"upstream_https_proxy", settings.upstreamHttpsProxy},
            {"upstream_socks_proxy", settings.upstreamSocksProxy}
        };
    }

    QJsonObject propsPayload() const
    {
        const ProxySettings settings = server_->settings();
        return QJsonObject{
            {"ok", true},
            {"name", "openai-reasoning-guard-api-proxy"},
            {"mode", "openai-proxy"},
            {"upstream_base_url", settings.upstreamBaseUrl},
            {"proxy_prefix", proxyPrefix_.isEmpty() ? QString("/") : proxyPrefix_},
            {"endpoints", QJsonObject{
                {"responses", true},
                {"chat_completions", true},
                {"models", true}
            }},
            {"features", QJsonObject{
                {"buffers_responses_for_reasoning_guard", true},
                {"request_body_limit_bytes", double(settings.requestBodyLimitBytes)},
                {"response_buffer_limit_bytes", double(settings.responseBufferLimitBytes)},
                {"intercept_rule_mode", normalizeInterceptRuleMode(settings.interceptRuleMode)},
                {"reasoning_equals", stringListToJsonArray(settings.reasoningEquals)},
                {"guard_retry_attempts", settings.guardRetryAttempts},
                {"retry_upstream_capacity_errors", settings.retryUpstreamCapacityErrors},
                {"intercept_streaming", settings.interceptStreaming},
                {"intercept_non_streaming", settings.interceptNonStreaming},
                {"non_stream_status_code", settings.nonStreamStatusCode},
                {"stream_action", settings.streamAction},
                {"retries_reasoning_guard_before_502", settings.guardRetryAttempts},
                {"returns_502_on_reasoning_guard_after_retries", settings.nonStreamStatusCode == 502},
                {"buffers_responses_for_reasoning_tokens_516", true},
                {"retries_reasoning_tokens_516_before_502", settings.reasoningEquals.contains("516") ? settings.guardRetryAttempts : 0},
                {"returns_502_on_reasoning_tokens_516_after_retries", settings.reasoningEquals.contains("516") && settings.nonStreamStatusCode == 502},
                {"forwards_user_agent", settings.forwardUserAgent},
                {"uses_explicit_upstream_proxy", !settings.upstreamProxy.isEmpty() || !settings.upstreamHttpProxy.isEmpty() || !settings.upstreamHttpsProxy.isEmpty() || !settings.upstreamSocksProxy.isEmpty()}
            }}
        };
    }

    void writeJson(const QJsonObject &payload,
                   int statusCode = 200,
                   const QList<QPair<QByteArray, QByteArray> > &extraHeaders = QList<QPair<QByteArray, QByteArray> >())
    {
        const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
        QList<QPair<QByteArray, QByteArray> > headers;
        headers.append(qMakePair(QByteArray("Content-Type"), QByteArray("application/json; charset=utf-8")));
        headers.append(extraHeaders);
        writeResponse(statusCode, headers, method_.toUpper() == "HEAD" ? QByteArray() : body);
    }

    void writeResponse(int statusCode,
                       const QList<QPair<QByteArray, QByteArray> > &headers,
                       const QByteArray &body)
    {
        QByteArray response;
        response += "HTTP/1.1 " + QByteArray::number(statusCode) + " " + reasonPhrase(statusCode) + "\r\n";
        for (int i = 0; i < headers.size(); ++i) {
            response += headers.at(i).first + ": " + headers.at(i).second + "\r\n";
        }
        response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
        response += "Connection: close\r\n\r\n";
        if (method_.toUpper() != "HEAD") {
            response += body;
        }
        responseWritten_ = true;
        writeClient(response);
        socket_->flush();
        socket_->disconnectFromHost();
    }

    void noteUpstreamBytes(const QByteArray &chunk)
    {
        upstreamBytesRead_ += chunk.size();
        rememberTail(&upstreamTail_, chunk);
    }

    qint64 writeClient(const QByteArray &chunk)
    {
        if (!socket_ || chunk.isEmpty()) {
            return 0;
        }
        const qint64 written = socket_->write(chunk);
        if (written > 0) {
            downstreamBytesQueued_ += written;
            rememberTail(&downstreamTail_, chunk.left(int(qMin(written, qint64(chunk.size())))));
        }
        return written;
    }

    void emitTransferDiagnostics(const QString &closeReason = QString())
    {
        if (!proxyRequestRecorded_) {
            return;
        }
        const bool shouldLog = !diagnosticsLogged_;
        diagnosticsLogged_ = true;
        if (!closeReason.isEmpty()) {
            diagnosticCloseReason_ = closeReason;
        }

        QJsonObject diagnostics;
        diagnostics.insert("method", method_);
        diagnostics.insert("path", path_);
        diagnostics.insert("status_code", diagnosticStatusCode_ > 0 ? QJsonValue(diagnosticStatusCode_) : QJsonValue());
        diagnostics.insert("request_kind", requestKind_.isEmpty() ? QJsonValue() : QJsonValue(requestKind_));
        diagnostics.insert("intercept_exempt_reason", interceptExemptReason_.isEmpty() ? QJsonValue() : QJsonValue(interceptExemptReason_));
        diagnostics.insert("upstream_bytes_read", double(upstreamBytesRead_));
        diagnostics.insert("downstream_bytes_queued", double(downstreamBytesQueued_));
        diagnostics.insert("downstream_bytes_written", double(downstreamBytesWritten_));
        diagnostics.insert("downstream_bytes_pending", double(downstreamBytesQueued_ - downstreamBytesWritten_));
        diagnostics.insert("upstream_tail_sha256_16", upstreamTail_.isEmpty() ? QJsonValue() : QJsonValue(tailHashText(upstreamTail_)));
        diagnostics.insert("downstream_tail_sha256_16", downstreamTail_.isEmpty() ? QJsonValue() : QJsonValue(tailHashText(downstreamTail_)));
        diagnostics.insert("stream_completed_seen", streamCompletedSeen_);
        diagnostics.insert("stream_terminal_seen", streamTerminalSeen_);
        diagnostics.insert("stream_done_seen", streamDoneSeen_);
        diagnostics.insert("client_closed_first", diagnosticClientClosedFirst_);
        diagnostics.insert("close_reason", diagnosticCloseReason_.isEmpty() ? QJsonValue() : QJsonValue(diagnosticCloseReason_));
        diagnostics.insert("at", QDateTime::currentMSecsSinceEpoch() / 1000.0);
        server_->recordTransferDiagnostics(diagnostics);
        if (shouldLog) {
            emit server_->logLine(QString("[transfer] path=%1 status=%2 upstream_bytes=%3 downstream_queued=%4 downstream_written=%5 stream_completed=%6 stream_done=%7 client_closed_first=%8")
                .arg(path_)
                .arg(diagnosticStatusCode_ > 0 ? QString::number(diagnosticStatusCode_) : QString("null"))
                .arg(upstreamBytesRead_)
                .arg(downstreamBytesQueued_)
                .arg(downstreamBytesWritten_)
                .arg(streamCompletedSeen_ ? "true" : "false")
                .arg(streamDoneSeen_ ? "true" : "false")
                .arg(diagnosticClientClosedFirst_ ? "true" : "false"));
        }
    }

    void record(int statusCode,
                const QString &errorType,
                const QString &errorMessage,
                int reasoningTokens = -1)
    {
        diagnosticStatusCode_ = statusCode;
        if (!errorType.isEmpty()) {
            diagnosticCloseReason_ = errorType;
        }
        server_->recordResult("proxy",
                              method_,
                              path_,
                              statusCode,
                              elapsed_.isValid() ? elapsed_.elapsed() : 0.0,
                              errorType,
                              errorMessage,
                              reasoningTokens,
                              requestKind_,
                              interceptExemptReason_);
        emitTransferDiagnostics();
    }

    HttpProxyServer *server_;
    QNetworkAccessManager *manager_;
    QUrl upstreamBase_;
    QString upstreamBasePath_;
    QString proxyPrefix_;
    QTcpSocket *socket_;
    QNetworkReply *currentReply_;
    QByteArray upstreamBodyBuffer_;
    QByteArray sseScanBuffer_;
    StructureSignals streamStructure_;
    int upstreamResponseStatus_;
    bool upstreamResponseStateReady_;
    bool upstreamIsJson_;
    bool upstreamIsStream_;
    bool upstreamShouldInspect_;
    bool upstreamPassThrough_;
    bool streamInspectedRecorded_;
    int streamObservedReasoning_;
    bool streamCompletedSeen_;
    bool streamTerminalSeen_;
    bool streamUsageSeen_;
    bool streamFailureSeen_;
    bool streamingWroteAnyBody_;
    bool requestUsesChunkedEncoding_;
    QByteArray buffer_;
    QByteArray requestBody_;
    QMap<QString, QByteArray> headers_;
    QString method_;
    QString target_;
    QString path_;
    QString query_;
    qint64 contentLength_;
    int headerEnd_;
    int headerTerminatorLength_;
    int retryAttempt_;
    QJsonObject requestJson_;
    QString requestKind_;
    QString requestReasoningEffort_;
    QString interceptExemptReason_;
    bool requestIsStream_;
    bool proxyRequestRecorded_;
    bool responseWritten_;
    bool clientCloseRecorded_;
    bool responseTimedOut_;
    bool responseLimitExceeded_;
    bool upstreamTimedOut_;
    qint64 upstreamBytesRead_;
    qint64 downstreamBytesQueued_;
    qint64 downstreamBytesWritten_;
    QByteArray upstreamTail_;
    QByteArray downstreamTail_;
    bool streamDoneSeen_;
    bool diagnosticsLogged_;
    bool diagnosticClientClosedFirst_;
    int diagnosticStatusCode_;
    QString diagnosticCloseReason_;
    QTimer requestBufferTimer_;
    QTimer responseBufferTimer_;
    QElapsedTimer elapsed_;
};

HttpProxyServer::HttpProxyServer(QObject *parent)
    : QObject(parent),
      startedAtMs_(0),
      requestsTotal_(0),
      controlRequestsTotal_(0),
      healthRequestsTotal_(0),
      statusRequestsTotal_(0),
      interceptedRequestsTotal_(0),
      successfulRequestsTotal_(0),
      failedRequestsTotal_(0),
      proxyErrorTotal_(0),
      upstreamHttpErrorTotal_(0),
      clientConnectionErrorTotal_(0),
      bufferTimeoutTotal_(0),
      upstreamTimeoutTotal_(0),
      localProxyErrorTotal_(0),
      reasoningTokens516Total_(0),
      reasoningTokens516RetryTotal_(0),
      inspectedResponseCount_(0),
      bypassedProxyRequestCount_(0),
      matchedResponseCount_(0),
      matchedStreamingCount_(0),
      matchedNonStreamingCount_(0),
      blockedResponseCount_(0),
      blockedStreamingCount_(0),
      blockedNonStreamingCount_(0),
      guardRetryTotal_(0),
      upstreamAttemptsTotal_(0),
      consecutiveFailures_(0),
      lastLatencyMs_(-1.0),
      latencyTotalMs_(0.0),
      latencySamples_(0)
{
    connect(&server_, SIGNAL(newConnection()), this, SLOT(acceptConnection()));
}

HttpProxyServer::~HttpProxyServer()
{
    stop();
}

bool HttpProxyServer::start(const ProxySettings &settings, QString *error)
{
    if (server_.isListening()) {
        if (error) {
            *error = "proxy server already running";
        }
        return false;
    }

    const QUrl upstream(settings.upstreamBaseUrl);
    if (!upstream.isValid() || upstream.scheme().isEmpty() || upstream.host().isEmpty()
        || (upstream.scheme() != "http" && upstream.scheme() != "https")) {
        if (error) {
            *error = QString("unsupported upstream base url: %1").arg(settings.upstreamBaseUrl);
        }
        return false;
    }

    settings_ = settings;
    if (settings_.listenHost.trimmed().isEmpty()) {
        settings_.listenHost = "127.0.0.1";
    }
    if (settings_.listenPort <= 0 || settings_.listenPort > 65535) {
        if (error) {
            *error = "invalid listen port";
        }
        return false;
    }
    if (settings_.upstreamUserAgent.trimmed().isEmpty()) {
        settings_.upstreamUserAgent = "curl/8.7.1";
    }
    if (settings_.upstreamTimeoutSec <= 0) {
        settings_.upstreamTimeoutSec = 1800;
    }
    if (settings_.bufferTimeoutSec <= 0) {
        settings_.bufferTimeoutSec = 180;
    }
    if (settings_.requestBodyLimitBytes <= 0) {
        settings_.requestBodyLimitBytes = defaultRequestBodyLimitBytes();
    }
    if (settings_.responseBufferLimitBytes <= 0) {
        settings_.responseBufferLimitBytes = defaultResponseBufferLimitBytes();
    }
    const qint64 maxQtByteArraySize = qint64(0x7fffffff);
    if (settings_.requestBodyLimitBytes > maxQtByteArraySize) {
        settings_.requestBodyLimitBytes = maxQtByteArraySize;
    }
    if (settings_.responseBufferLimitBytes > maxQtByteArraySize) {
        settings_.responseBufferLimitBytes = maxQtByteArraySize;
    }
    settings_.interceptRuleMode = normalizeInterceptRuleMode(settings_.interceptRuleMode);
    settings_.reasoningEquals = normalizeIntegerList(settings_.reasoningEquals.join(","), defaultReasoningEquals());
    settings_.guardEndpoints = normalizePathList(settings_.guardEndpoints.join(","), defaultGuardEndpoints());
    if (settings_.guardRetryAttempts < 0) {
        settings_.guardRetryAttempts = 0;
    }
    settings_.reasoning516RetryCount = settings_.guardRetryAttempts;
    if (!settings_.interceptStreaming && !settings_.interceptNonStreaming) {
        if (error) {
            *error = "intercept_streaming and intercept_non_streaming cannot both be disabled";
        }
        return false;
    }
    if (settings_.nonStreamStatusCode <= 0) {
        settings_.nonStreamStatusCode = 502;
    }
    settings_.streamAction = normalizeStreamAction(settings_.streamAction);

    upstreamBase_ = upstream;
    upstreamBasePath_ = upstream.path().isEmpty() ? QString("/") : upstream.path();
    proxyPrefix_ = normalizePathPrefix(settings_.proxyPrefix);
    configureUpstreamProxy();

    startedAtMs_ = QDateTime::currentMSecsSinceEpoch();

    QHostAddress address(settings_.listenHost);
    if (settings_.listenHost == "localhost") {
        address = QHostAddress(QHostAddress::LocalHost);
    } else if (settings_.listenHost == "0.0.0.0" || settings_.listenHost == "::") {
        address = QHostAddress(QHostAddress::Any);
    }
    if (!server_.listen(address, quint16(settings_.listenPort))) {
        if (error) {
            *error = server_.errorString();
        }
        return false;
    }

    emit started(listenUrl());
    emit logLine(QString("openai-proxy listening on %1").arg(listenUrl()));
    emit logLine(QString("status endpoint=http://%1:%2/status").arg(settings_.listenHost).arg(settings_.listenPort));
    emit logLine(QString("upstream=%1").arg(settings_.upstreamBaseUrl));
    emit logLine(QString("intercept_rule_mode=%1").arg(settings_.interceptRuleMode));
    emit logLine(QString("reasoning_equals=%1").arg(settings_.reasoningEquals.join(",")));
    emit logLine(QString("guard_retry_attempts=%1").arg(settings_.guardRetryAttempts));
    emit logLine(QString("retry_upstream_capacity_errors=%1").arg(settings_.retryUpstreamCapacityErrors ? "true" : "false"));
    return true;
}

void HttpProxyServer::stop()
{
    if (!server_.isListening()) {
        return;
    }
    server_.close();
    emit logLine("proxy stopped");
    emit stopped();
}

bool HttpProxyServer::isRunning() const
{
    return server_.isListening();
}

QString HttpProxyServer::listenUrl() const
{
    const QString prefix = proxyPrefix_.isEmpty() ? QString("/") : proxyPrefix_;
    return QString("http://%1:%2%3").arg(settings_.listenHost).arg(settings_.listenPort).arg(prefix);
}

ProxySettings HttpProxyServer::settings() const
{
    return settings_;
}

QJsonObject HttpProxyServer::healthPayload() const
{
    return QJsonObject{
        {"ok", true},
        {"mode", "openai-proxy"},
        {"upstream_base_url", settings_.upstreamBaseUrl},
            {"proxy_prefix", proxyPrefix_.isEmpty() ? QString("/") : proxyPrefix_},
            {"upstream_timeout_sec", settings_.upstreamTimeoutSec},
            {"buffer_timeout_sec", settings_.bufferTimeoutSec},
            {"request_body_limit_bytes", double(settings_.requestBodyLimitBytes)},
            {"response_buffer_limit_bytes", double(settings_.responseBufferLimitBytes)},
            {"intercept_rule_mode", normalizeInterceptRuleMode(settings_.interceptRuleMode)},
            {"reasoning_equals", stringListToJsonArray(settings_.reasoningEquals)},
            {"guard_retry_attempts", settings_.guardRetryAttempts},
            {"retry_upstream_capacity_errors", settings_.retryUpstreamCapacityErrors},
            {"intercept_streaming", settings_.interceptStreaming},
            {"intercept_non_streaming", settings_.interceptNonStreaming},
            {"non_stream_status_code", settings_.nonStreamStatusCode},
            {"stream_action", settings_.streamAction},
            {"guard_endpoints", stringListToJsonArray(settings_.guardEndpoints)},
            {"reasoning_516_retry_count", settings_.guardRetryAttempts},
            {"forward_user_agent", settings_.forwardUserAgent},
            {"upstream_proxy", settings_.upstreamProxy},
            {"upstream_http_proxy", settings_.upstreamHttpProxy},
            {"upstream_https_proxy", settings_.upstreamHttpsProxy},
            {"upstream_socks_proxy", settings_.upstreamSocksProxy}
    };
}

QJsonObject HttpProxyServer::runtimePayload() const
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const double uptime = startedAtMs_ > 0 ? (now - startedAtMs_) / 1000.0 : 0.0;
    const double avgLatency = latencySamples_ > 0 ? latencyTotalMs_ / latencySamples_ : -1.0;
    QJsonObject runtime;
    runtime.insert("started_at", startedAtMs_ / 1000.0);
    runtime.insert("uptime_sec", uptime);
    runtime.insert("requests_total", double(requestsTotal_));
    runtime.insert("control_requests_total", double(controlRequestsTotal_));
    runtime.insert("health_requests_total", double(healthRequestsTotal_));
    runtime.insert("status_requests_total", double(statusRequestsTotal_));
    runtime.insert("intercepted_requests_total", double(interceptedRequestsTotal_));
    runtime.insert("successful_requests_total", double(successfulRequestsTotal_));
    runtime.insert("failed_requests_total", double(failedRequestsTotal_));
    runtime.insert("proxy_error_total", double(proxyErrorTotal_));
    runtime.insert("upstream_http_error_total", double(upstreamHttpErrorTotal_));
    runtime.insert("client_connection_error_total", double(clientConnectionErrorTotal_));
    runtime.insert("buffer_timeout_total", double(bufferTimeoutTotal_));
    runtime.insert("upstream_timeout_total", double(upstreamTimeoutTotal_));
    runtime.insert("local_proxy_error_total", double(localProxyErrorTotal_));
    runtime.insert("reasoning_tokens_516_total", double(reasoningTokens516Total_));
    runtime.insert("reasoning_tokens_516_retry_total", double(reasoningTokens516RetryTotal_));
    runtime.insert("total_proxy_request_count", double(upstreamAttemptsTotal_));
    runtime.insert("upstream_attempts_total", double(upstreamAttemptsTotal_));
    runtime.insert("inspected_response_count", double(inspectedResponseCount_));
    runtime.insert("bypassed_proxy_request_count", double(bypassedProxyRequestCount_));
    runtime.insert("matched_response_count", double(matchedResponseCount_));
    runtime.insert("matched_streaming_count", double(matchedStreamingCount_));
    runtime.insert("matched_non_streaming_count", double(matchedNonStreamingCount_));
    runtime.insert("blocked_response_count", double(blockedResponseCount_));
    runtime.insert("blocked_streaming_count", double(blockedStreamingCount_));
    runtime.insert("blocked_non_streaming_count", double(blockedNonStreamingCount_));
    runtime.insert("guard_retry_total", double(guardRetryTotal_));
    const int reasoning516Count = observedReasoningCounts_.value("516").toInt();
    runtime.insert("reasoning_516_count", double(reasoning516Count));
    runtime.insert("reasoning_516_ratio", inspectedResponseCount_ == 0 ? 0.0 : double(reasoning516Count) / double(inspectedResponseCount_));
    runtime.insert("observed_reasoning_counts", observedReasoningCounts_);
    runtime.insert("consecutive_failures", double(consecutiveFailures_));
    if (lastLatencyMs_ >= 0.0) {
        runtime.insert("last_latency_ms", lastLatencyMs_);
    } else {
        runtime.insert("last_latency_ms", QJsonValue());
    }
    if (avgLatency >= 0.0) {
        runtime.insert("avg_latency_ms", avgLatency);
    } else {
        runtime.insert("avg_latency_ms", QJsonValue());
    }
    runtime.insert("last_result", lastResult_.isEmpty() ? QJsonValue() : QJsonValue(lastResult_));
    runtime.insert("last_failure", lastFailure_.isEmpty() ? QJsonValue() : QJsonValue(lastFailure_));
    runtime.insert("last_transfer_diagnostics", lastTransferDiagnostics_.isEmpty() ? QJsonValue() : QJsonValue(lastTransferDiagnostics_));
    runtime.insert("status_code_counts", statusCodeCounts_);
    return runtime;
}

QJsonObject HttpProxyServer::statusPayload() const
{
    QJsonObject payload = healthPayload();
    payload.insert("health", healthPayload());
    payload.insert("runtime", runtimePayload());
    return payload;
}

void HttpProxyServer::recordRequest(const QString &kind)
{
    ++requestsTotal_;
    if (kind == "health") {
        ++controlRequestsTotal_;
        ++healthRequestsTotal_;
    } else if (kind == "status" || kind == "version") {
        ++controlRequestsTotal_;
        ++statusRequestsTotal_;
    } else {
        ++interceptedRequestsTotal_;
    }
    emit statsChanged();
}

void HttpProxyServer::recordResult(const QString &kind,
                                   const QString &method,
                                   const QString &path,
                                   int statusCode,
                                   double latencyMs,
                                   const QString &errorType,
                                   const QString &errorMessage,
                                   int reasoningTokens,
                                   const QString &requestKind,
                                   const QString &interceptExemptReason)
{
    QJsonObject result;
    result.insert("kind", kind);
    result.insert("method", method);
    result.insert("path", path);
    result.insert("status_code", statusCode);
    result.insert("latency_ms", latencyMs);
    result.insert("error_type", errorType.isEmpty() ? QJsonValue() : QJsonValue(errorType));
    result.insert("error_message", errorMessage.isEmpty() ? QJsonValue() : QJsonValue(errorMessage));
    result.insert("at", QDateTime::currentMSecsSinceEpoch() / 1000.0);
    if (reasoningTokens >= 0) {
        result.insert("reasoning_tokens", reasoningTokens);
    }
    if (!requestKind.isEmpty()) {
        result.insert("request_kind", requestKind);
    }
    if (!interceptExemptReason.isEmpty()) {
        result.insert("intercept_exempt_reason", interceptExemptReason);
    }

    lastResult_ = result;
    lastLatencyMs_ = latencyMs;
    latencyTotalMs_ += latencyMs;
    ++latencySamples_;

    if (kind == "proxy") {
        const QString codeKey = QString::number(statusCode);
        statusCodeCounts_.insert(codeKey, statusCodeCounts_.value(codeKey).toInt() + 1);
        if (statusCode >= 400 || !errorType.isEmpty()) {
            ++failedRequestsTotal_;
            ++consecutiveFailures_;
            if (errorType == "proxy_error") {
                ++proxyErrorTotal_;
            } else if (errorType == "client_connection_error") {
                ++clientConnectionErrorTotal_;
            } else if (errorType == "buffer_timeout") {
                ++bufferTimeoutTotal_;
            } else if (errorType == "upstream_timeout") {
                ++upstreamTimeoutTotal_;
            } else if (errorType == "request_body_limit_exceeded" ||
                       errorType == "response_buffer_limit_exceeded" ||
                       errorType == "bad_request" ||
                       errorType == "stream_failed_event" ||
                       errorType == "stream_incomplete_response" ||
                       errorType == "stream_missing_usage") {
                ++localProxyErrorTotal_;
            } else if (errorType == "reasoning_tokens_516" || errorType == "reasoning_guard_triggered") {
                if (reasoningTokens == 516) {
                    ++reasoningTokens516Total_;
                }
            } else {
                ++upstreamHttpErrorTotal_;
            }
            lastFailure_ = result;
        } else {
            ++successfulRequestsTotal_;
            consecutiveFailures_ = 0;
        }
    }

    emit statsChanged();
}

void HttpProxyServer::recordReasoningGuardRetry(const QString &method,
                                                const QString &path,
                                                int attempt,
                                                int maxRetries,
                                                int reasoningTokens,
                                                const QString &requestKind,
                                                const QString &interceptExemptReason)
{
    QJsonObject result;
    result.insert("kind", "retry");
    result.insert("method", method);
    result.insert("path", path);
    result.insert("status_code", 200);
    result.insert("error_type", "reasoning_guard_retry");
    result.insert("error_message", QString("retrying reasoning guard matched response"));
    result.insert("reasoning_tokens", reasoningTokens);
    result.insert("retry_attempt", attempt);
    result.insert("retry_limit", maxRetries);
    result.insert("at", QDateTime::currentMSecsSinceEpoch() / 1000.0);
    if (!requestKind.isEmpty()) {
        result.insert("request_kind", requestKind);
    }
    if (!interceptExemptReason.isEmpty()) {
        result.insert("intercept_exempt_reason", interceptExemptReason);
    }

    ++guardRetryTotal_;
    if (reasoningTokens == 516) {
        ++reasoningTokens516RetryTotal_;
    }
    lastResult_ = result;
    emit statsChanged();
}

void HttpProxyServer::recordInspectedResponse(int reasoningTokens,
                                              bool matched,
                                              const QString &streamKind)
{
    ++inspectedResponseCount_;
    if (reasoningTokens >= 0) {
        const QString key = QString::number(reasoningTokens);
        observedReasoningCounts_.insert(key, observedReasoningCounts_.value(key).toInt() + 1);
    }
    if (matched) {
        ++matchedResponseCount_;
        if (streamKind == "stream") {
            ++matchedStreamingCount_;
        } else if (streamKind == "non-stream") {
            ++matchedNonStreamingCount_;
        }
    }
    emit statsChanged();
}

void HttpProxyServer::recordBlockedResponse(const QString &streamKind)
{
    ++blockedResponseCount_;
    if (streamKind == "stream") {
        ++blockedStreamingCount_;
    } else if (streamKind == "non-stream") {
        ++blockedNonStreamingCount_;
    }
    emit statsChanged();
}

void HttpProxyServer::recordUpstreamAttempt()
{
    ++upstreamAttemptsTotal_;
    emit statsChanged();
}

void HttpProxyServer::recordBypassedProxyRequest()
{
    ++bypassedProxyRequestCount_;
    emit statsChanged();
}

void HttpProxyServer::recordTransferDiagnostics(const QJsonObject &diagnostics)
{
    lastTransferDiagnostics_ = diagnostics;
    emit statsChanged();
}

void HttpProxyServer::acceptConnection()
{
    while (server_.hasPendingConnections()) {
        QTcpSocket *socket = server_.nextPendingConnection();
        new ProxyConnection(this, &manager_, upstreamBase_, upstreamBasePath_, proxyPrefix_, socket);
    }
}

void HttpProxyServer::configureUpstreamProxy()
{
    QString proxyText = settings_.upstreamProxy;
    bool socksFallback = false;
    if (proxyText.trimmed().isEmpty() && upstreamBase_.scheme() == "https") {
        if (!settings_.upstreamHttpsProxy.trimmed().isEmpty()) {
            proxyText = settings_.upstreamHttpsProxy;
        } else if (!settings_.upstreamHttpProxy.trimmed().isEmpty()) {
            proxyText = settings_.upstreamHttpProxy;
        } else {
            proxyText = settings_.upstreamSocksProxy;
            socksFallback = true;
        }
    } else if (proxyText.trimmed().isEmpty() && !settings_.upstreamHttpProxy.trimmed().isEmpty()) {
        proxyText = settings_.upstreamHttpProxy;
    } else if (proxyText.trimmed().isEmpty()) {
        proxyText = settings_.upstreamSocksProxy;
        socksFallback = true;
    }
    proxyText = proxyUrlText(proxyText, socksFallback);
    if (proxyText.isEmpty()) {
        manager_.setProxy(QNetworkProxy::NoProxy);
        return;
    }

    QUrl proxyUrl(proxyText);
    if (!proxyUrl.isValid() || proxyUrl.host().isEmpty()) {
        manager_.setProxy(QNetworkProxy::NoProxy);
        return;
    }
    const QString scheme = proxyUrl.scheme().trimmed().toLower();
    const bool socksProxy = scheme == "socks" || scheme == "socks5" || scheme == "socks5h" || scheme == "socks4";
    manager_.setProxy(QNetworkProxy(socksProxy ? QNetworkProxy::Socks5Proxy : QNetworkProxy::HttpProxy,
                                    proxyUrl.host(),
                                    quint16(proxyUrl.port(socksProxy ? 1080 : 8080)),
                                    proxyUrl.userName(),
                                    proxyUrl.password()));
}

} // namespace net_tunnel

#include "http_proxy_server.moc"
