#include "core/openssl_runtime.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QTextStream>
#include <QtTest/QTest>

using namespace net_tunnel;

namespace {

const char kRuntimePreparedMarker[] = "OPENAI_REASONING_GUARD_OPENSSL_RUNTIME_READY";

class OpenSslRuntimeTest : public QObject {
    Q_OBJECT

private slots:
    void directLinuxProcessUsesBundledOpenSslPath()
    {
#if defined(Q_OS_LINUX)
        const QString libraryDirectory = bundledOpenSslLibraryDirectory();
        if (libraryDirectory.isEmpty()) {
            QSKIP("This Qt runtime does not bundle OpenSSL 1.1.");
        }

        QProcess process;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.remove("LD_LIBRARY_PATH");
        environment.remove(QString::fromLatin1(kRuntimePreparedMarker));
        process.setProcessEnvironment(environment);
        process.start(QCoreApplication::applicationFilePath(),
                      QStringList() << "--openssl-runtime-probe");
        QVERIFY2(process.waitForFinished(10000), qPrintable(process.errorString()));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 0);

        const QByteArray preparedPath = process.readAllStandardOutput().trimmed();
        QVERIFY2(preparedPath.startsWith(QFile::encodeName(libraryDirectory)), preparedPath.constData());
#else
        QSKIP("Bundled OpenSSL runtime selection is only required on Linux.");
#endif
    }
};

} // namespace

int main(int argc, char **argv)
{
    QString runtimeError;
    if (!ensureBundledOpenSslRuntime(argc, argv, &runtimeError)) {
        QTextStream(stderr) << runtimeError << "\n";
        return 127;
    }

    QCoreApplication app(argc, argv);
    if (app.arguments().contains("--openssl-runtime-probe")) {
        QTextStream(stdout) << qgetenv("LD_LIBRARY_PATH") << "\n";
        return 0;
    }

    OpenSslRuntimeTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "openssl_runtime_test.moc"
