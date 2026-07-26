#pragma once

#include <QtCore/QString>

namespace net_tunnel {

// Returns the Qt library directory only when it carries the matching OpenSSL 1.1 runtime.
QString bundledOpenSslLibraryDirectory();

// Re-execs a Linux process early enough for Qt to prefer its bundled OpenSSL runtime.
bool ensureBundledOpenSslRuntime(int argc, char **argv, QString *error = 0);

} // namespace net_tunnel
