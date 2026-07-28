#include "core/app_config.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>

using namespace net_tunnel;

class AppConfigTest : public QObject {
    Q_OBJECT

private:
    QTemporaryDir homeDirectory_;
    QString legacyPath_;
    QByteArray originalConfigPath_;
    QByteArray originalHome_;
    bool configPathWasSet_ = false;
    bool homeWasSet_ = false;

private slots:
    void initTestCase()
    {
        QVERIFY(homeDirectory_.isValid());
        originalHome_ = qgetenv("HOME");
        homeWasSet_ = !originalHome_.isNull();
        originalConfigPath_ = qgetenv("NET_TUNNEL_CONFIG");
        configPathWasSet_ = !originalConfigPath_.isNull();
        qputenv("HOME", homeDirectory_.path().toLocal8Bit());
        qunsetenv("NET_TUNNEL_CONFIG");
    }

    void defaultConfigPathUsesUserDirectoryOnMacOS()
    {
#if defined(Q_OS_MACOS)
        const QString expectedPath = QDir(QDir::homePath()).filePath(
            "Library/Application Support/OpenAI Reasoning Guard/config.json");
        QCOMPARE(defaultConfigPath(), expectedPath);
#else
        QSKIP("This regression only applies to macOS.");
#endif
    }

    void configPathOverrideTakesPrecedence()
    {
        const QByteArray expectedPath("/tmp/openai-reasoning-guard-test-config.json");
        qputenv("NET_TUNNEL_CONFIG", expectedPath);

        QCOMPARE(defaultConfigPath(), QString::fromLocal8Bit(expectedPath));
        qunsetenv("NET_TUNNEL_CONFIG");
    }

    void defaultConfigPathMigratesLegacyMacOSConfig()
    {
#if defined(Q_OS_MACOS)
        const QString defaultPath = QDir(QDir::homePath()).filePath(
            "Library/Application Support/OpenAI Reasoning Guard/config.json");
        QVERIFY2(!QFile::exists(defaultPath), "The temporary HOME unexpectedly contains config.json.");

        const QString legacyPath = QDir(QCoreApplication::applicationDirPath()).filePath("config.json");
        if (QFile::exists(legacyPath)) {
            QSKIP("The test binary directory already contains config.json.");
        }

        QFile legacyFile(legacyPath);
        legacyPath_ = legacyPath;
        QVERIFY(legacyFile.open(QIODevice::WriteOnly));
        QJsonObject legacyConfig;
        legacyConfig.insert("proxy_port", "8123");
        const QByteArray contents = QJsonDocument(legacyConfig).toJson();
        QCOMPARE(legacyFile.write(contents), qint64(contents.size()));
        legacyFile.close();

        QCOMPARE(defaultConfigPath(), defaultPath);
        QVERIFY(QFile::exists(defaultPath));
        const AppConfig config = loadConfig(defaultPath);
        QCOMPARE(config.proxyPort, 8123);
#else
        QSKIP("This regression only applies to macOS.");
#endif
    }

    void saveConfigCreatesPrivateMacOSFiles()
    {
#if defined(Q_OS_MACOS)
        const QString configPath = defaultConfigPath();
        const QString configDirectory = QFileInfo(configPath).absolutePath();

        AppConfig config;
        config.upstreamApiKey = "test-api-key";
        QString error;
        QVERIFY2(saveConfig(config, configPath, &error), qPrintable(error));

        const QFileDevice::Permissions exposed = QFileDevice::ReadGroup |
            QFileDevice::WriteGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther |
            QFileDevice::WriteOther | QFileDevice::ExeOther;
        QCOMPARE(QFileInfo(configDirectory).permissions() & exposed, QFileDevice::Permissions());
        QCOMPARE(QFileInfo(configPath).permissions() & exposed, QFileDevice::Permissions());
#else
        QSKIP("This regression only applies to macOS.");
#endif
    }

    void saveConfigPreservesExplicitExistingDirectoryPermissions()
    {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        const QFileDevice::Permissions sharedPermissions = QFileDevice::ReadOwner |
            QFileDevice::WriteOwner | QFileDevice::ExeOwner | QFileDevice::ReadGroup |
            QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::ExeOther;
        QVERIFY(QFile::setPermissions(temporaryDirectory.path(), sharedPermissions));
        const QFileDevice::Permissions beforePermissions = QFileInfo(temporaryDirectory.path()).permissions();

        AppConfig config;
        QString error;
        const QString explicitPath = temporaryDirectory.filePath("config.json");
        QVERIFY2(saveConfig(config, explicitPath, &error), qPrintable(error));

        QCOMPARE(QFileInfo(temporaryDirectory.path()).permissions(), beforePermissions);
    }

    void cleanup()
    {
        qunsetenv("NET_TUNNEL_CONFIG");
        if (!legacyPath_.isEmpty()) {
            QFile::remove(legacyPath_);
            legacyPath_.clear();
        }
    }

    void cleanupTestCase()
    {
        if (homeWasSet_) {
            qputenv("HOME", originalHome_);
        } else {
            qunsetenv("HOME");
        }
        if (configPathWasSet_) {
            qputenv("NET_TUNNEL_CONFIG", originalConfigPath_);
        } else {
            qunsetenv("NET_TUNNEL_CONFIG");
        }
    }
};

QTEST_MAIN(AppConfigTest)

#include "app_config_test.moc"
