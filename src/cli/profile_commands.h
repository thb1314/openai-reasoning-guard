#pragma once

#include <QtCore/QStringList>

namespace net_tunnel {

bool isProfileCommand(const QStringList &arguments);
int runProfileCommand(const QStringList &arguments);

} // namespace net_tunnel
