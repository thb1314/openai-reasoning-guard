#include "core/json_utils.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegExp>
#include <QtCore/QStringList>

#include <cmath>

namespace net_tunnel {

static QStringList reasoningPointers()
{
    return QStringList()
        << "/usage/output_tokens_details/reasoning_tokens"
        << "/usage/completion_tokens_details/reasoning_tokens"
        << "/response/usage/output_tokens_details/reasoning_tokens"
        << "/response/usage/completion_tokens_details/reasoning_tokens";
}

static QJsonValue jsonPointerGet(const QJsonValue &value, const QString &pointer)
{
    if (!pointer.startsWith('/')) {
        return QJsonValue();
    }
    QJsonValue current = value;
    const QStringList segments = pointer.mid(1).split('/');
    for (int i = 0; i < segments.size(); ++i) {
        QString key = segments.at(i);
        key.replace("~1", "/");
        key.replace("~0", "~");
        if (current.isObject()) {
            const QJsonObject object = current.toObject();
            if (!object.contains(key)) {
                return QJsonValue();
            }
            current = object.value(key);
        } else if (current.isArray()) {
            bool ok = false;
            const int index = key.toInt(&ok);
            const QJsonArray array = current.toArray();
            if (!ok || index < 0 || index >= array.size()) {
                return QJsonValue();
            }
            current = array.at(index);
        } else {
            return QJsonValue();
        }
    }
    return current;
}

static int coerceJsonInteger(const QJsonValue &value)
{
    if (!value.isDouble()) {
        return -1;
    }
    const double number = value.toDouble();
    const int parsed = value.toInt();
    if (std::fabs(number - double(parsed)) > 0.000001) {
        return -1;
    }
    return parsed;
}

int findReasoningTokens(const QJsonValue &value)
{
    const QStringList pointers = reasoningPointers();
    for (int i = 0; i < pointers.size(); ++i) {
        const int tokens = coerceJsonInteger(jsonPointerGet(value, pointers.at(i)));
        if (tokens >= 0) {
            return tokens;
        }
    }
    return -1;
}

int findReasoningTokensInJsonBody(const QByteArray &body)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError) {
        return -1;
    }
    if (document.isObject()) {
        return findReasoningTokens(document.object());
    }
    if (document.isArray()) {
        return findReasoningTokens(document.array());
    }
    return -1;
}

int findReasoningTokensInSseBody(const QByteArray &body)
{
    const QString text = QString::fromUtf8(body);
    const QStringList events = text.split(QRegExp("\\r?\\n\\r?\\n"), QString::SkipEmptyParts);
    int reasoningTokens = -1;

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
        if (data.isEmpty() || data == "[DONE]") {
            continue;
        }
        const int token = findReasoningTokensInJsonBody(data.toUtf8());
        if (token >= 0) {
            reasoningTokens = token;
        }
    }

    return reasoningTokens;
}

} // namespace net_tunnel
