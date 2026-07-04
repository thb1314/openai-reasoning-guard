#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QJsonValue>

namespace net_tunnel {

int findReasoningTokens(const QJsonValue &value);
int findReasoningTokensInJsonBody(const QByteArray &body);
int findReasoningTokensInSseBody(const QByteArray &body);

} // namespace net_tunnel
