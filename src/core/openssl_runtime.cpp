#include "core/openssl_runtime.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QList>
#include <QtCore/QLibraryInfo>

#if defined(Q_OS_LINUX)
#include <cerrno>
#include <cstring>
#include <unistd.h>
#endif

namespace net_tunnel {

namespace {

const char kRuntimePreparedMarker[] = "OPENAI_REASONING_GUARD_OPENSSL_RUNTIME_READY";

bool pathListContains(const QByteArray &pathList, const QByteArray &directory)
{
    const QList<QByteArray> entries = pathList.split(':');
    for (int i = 0; i < entries.size(); ++i) {
        if (entries.at(i) == directory) {
            return true;
        }
    }
    return false;
}

#if defined(Q_OS_LINUX)
QString currentExecutablePath(const char *argv0)
{
    char path[4096];
    const ssize_t size = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (size > 0) {
        path[size] = '\0';
        return QString::fromLocal8Bit(path);
    }
    return QFileInfo(QString::fromLocal8Bit(argv0 ? argv0 : "")).absoluteFilePath();
}
#endif

} // namespace

QString bundledOpenSslLibraryDirectory()
{
#if defined(Q_OS_LINUX)
    const QString directory = QLibraryInfo::location(QLibraryInfo::LibrariesPath);
    const QDir libraryDirectory(directory);
    if (QFileInfo(libraryDirectory.filePath("libssl.so.1.1")).isFile()
        && QFileInfo(libraryDirectory.filePath("libcrypto.so.1.1")).isFile()) {
        return libraryDirectory.absolutePath();
    }
#endif
    return QString();
}

bool ensureBundledOpenSslRuntime(int argc, char **argv, QString *error)
{
    Q_UNUSED(argc)
    if (error) {
        error->clear();
    }

#if defined(Q_OS_LINUX)
    if (qgetenv(kRuntimePreparedMarker) == "1") {
        return true;
    }

    const QString libraryDirectory = bundledOpenSslLibraryDirectory();
    if (libraryDirectory.isEmpty()) {
        return true;
    }

    const QByteArray encodedDirectory = QFile::encodeName(libraryDirectory);
    const QByteArray currentLibraryPath = qgetenv("LD_LIBRARY_PATH");
    if (pathListContains(currentLibraryPath, encodedDirectory)) {
        return true;
    }

    QByteArray nextLibraryPath = encodedDirectory;
    if (!currentLibraryPath.isEmpty()) {
        nextLibraryPath.append(':');
        nextLibraryPath.append(currentLibraryPath);
    }
    if (setenv("LD_LIBRARY_PATH", nextLibraryPath.constData(), 1) != 0
        || setenv(kRuntimePreparedMarker, "1", 1) != 0) {
        if (error) {
            *error = QString("failed to prepare bundled OpenSSL runtime: %1")
                .arg(QString::fromLocal8Bit(strerror(errno)));
        }
        return false;
    }

    const QString executablePath = currentExecutablePath(argv ? argv[0] : 0);
    const QByteArray encodedExecutablePath = QFile::encodeName(executablePath);
    if (encodedExecutablePath.isEmpty() || !QFileInfo(executablePath).isExecutable()
        || execv(encodedExecutablePath.constData(), argv) != 0) {
        if (error) {
            *error = QString("failed to restart with bundled OpenSSL runtime: %1")
                .arg(QString::fromLocal8Bit(strerror(errno)));
        }
        return false;
    }
#endif

    return true;
}

} // namespace net_tunnel
