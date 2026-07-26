#include "core/upstream_profile.h"

#include <QtCore/QFile>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlQuery>
#include <QtTest/QtTest>

using namespace net_tunnel;

namespace {

UpstreamProfile makeProfile(const QString &name, const QString &url)
{
    UpstreamProfile profile;
    profile.displayName = name;
    profile.baseUrl = url;
    return profile;
}

QJsonObject readObject(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QJsonObject();
    return QJsonDocument::fromJson(file.readAll()).object();
}

bool writeObject(const QString &path, const QJsonObject &object)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return file.write(QJsonDocument(object).toJson()) > 0;
}

} // namespace

class UpstreamProfileStoreTest : public QObject {
    Q_OBJECT

private slots:
    void defaultsAndValidation()
    {
        UpstreamProfile profile;
        QCOMPARE(profile.userAgent, QString("curl/8.7.1"));
        QCOMPARE(profile.upstreamTimeoutSec, 1800);
        QCOMPARE(profile.firstTokenTimeoutSec, 30);

        QString field;
        QString error;
        QVERIFY(!validateUpstreamProfile(profile, &field, &error));
        QCOMPARE(field, QString("display_name"));
        profile.displayName = "unsafe\nname";
        QVERIFY(!validateUpstreamProfile(profile, &field, &error));
        QCOMPARE(field, QString("display_name"));
        profile.displayName = QString(513, QLatin1Char('n'));
        QVERIFY(!validateUpstreamProfile(profile, &field, &error));
        QCOMPARE(field, QString("display_name"));
        profile.displayName = "primary";
        profile.baseUrl = "ftp://example.com";
        QVERIFY(!validateUpstreamProfile(profile, &field, &error));
        QCOMPARE(field, QString("base_url"));
        profile.baseUrl = "https://example.com/v1?bad=1";
        QVERIFY(!validateUpstreamProfile(profile, &field, &error));
        profile.baseUrl = "https://example.com/v1";
        profile.baseUrl = "https://user:secret@example.com/v1";
        QVERIFY(!validateUpstreamProfile(profile, &field, &error));
        QCOMPARE(field, QString("base_url"));
        profile.baseUrl = "https://example.com/v1";
        profile.upstreamProxy = "invalid://localhost:1";
        QVERIFY(!validateUpstreamProfile(profile, &field, &error));
        QCOMPARE(field, QString("upstream_proxy"));
        profile.upstreamProxy = "https://localhost:7890";
        QVERIFY(!validateUpstreamProfile(profile, &field, &error));
        profile.upstreamProxy = "socks4://localhost:1080";
        QVERIFY(!validateUpstreamProfile(profile, &field, &error));
        profile.upstreamProxy = "http://user:secret@localhost:7890";
        QVERIFY(!validateUpstreamProfile(profile, &field, &error));
        profile.upstreamProxy = "http://localhost:7890/proxy-path";
        QVERIFY(!validateUpstreamProfile(profile, &field, &error));
        profile.upstreamProxy = "http://localhost:7890?mode=tunnel";
        QVERIFY(!validateUpstreamProfile(profile, &field, &error));
        profile.upstreamProxy = "http://localhost:7890#fragment";
        QVERIFY(!validateUpstreamProfile(profile, &field, &error));
        profile.upstreamProxy.clear();
        profile.apiKey = "safe\r\nX-Injected: yes";
        QVERIFY(!validateUpstreamProfile(profile, &field, &error));
        QCOMPARE(field, QString("api_key"));
        profile.apiKey.clear();
        profile.userAgent = "safe\nX-Injected: yes";
        QVERIFY(!validateUpstreamProfile(profile, &field, &error));
        QCOMPARE(field, QString("user_agent"));
        profile.userAgent = "curl/8.7.1";
        profile.apiKey = QString(8193, QLatin1Char('x'));
        QVERIFY(!validateUpstreamProfile(profile, &field, &error));
        QCOMPARE(field, QString("api_key"));
        profile.apiKey.clear();
        profile.upstreamProxy = "http://127.0.0.1:7890/";
        QVERIFY2(validateUpstreamProfile(profile, &field, &error), qPrintable(error));
    }

    void encodedBaseUrlPathRoundTrip()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        UpstreamProfileStore store(dir.filePath("profiles.sqlite3"));
        QString error;
        QVERIFY2(store.open(&error), qPrintable(error));

        const QStringList urls = QStringList()
            << "https://example.com/v1%2Ftenant/"
            << "https://example.com/v1%3Ftenant/"
            << "https://example.com/%E4%B8%AD%E6%96%87/";
        for (int i = 0; i < urls.size(); ++i) {
            UpstreamProfile profile = makeProfile(QString("Encoded %1").arg(i), urls.at(i));
            QVERIFY2(store.addProfile(&profile, &error), qPrintable(error));
            QCOMPARE(profile.baseUrl, urls.at(i).left(urls.at(i).size() - 1));
        }
    }

    void crudSelectionAndUnicodeNameUniqueness()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        UpstreamProfileStore store(dir.filePath("profiles.sqlite3"));
        QString error;
        QVERIFY2(store.open(&error), qPrintable(error));

        UpstreamProfile first = makeProfile("  Main Line  ", "https://example.com/v1///");
        first.apiKey = "  secret-with-spaces  ";
        first.upstreamProxy = "127.0.0.1:7890";
        QVERIFY2(store.addProfile(&first, &error), qPrintable(error));
        QVERIFY(!first.id.isEmpty());
        QCOMPARE(first.displayName, QString("Main Line"));
        QCOMPARE(first.baseUrl, QString("https://example.com/v1"));
        QCOMPARE(first.apiKey, QString("secret-with-spaces"));
        QCOMPARE(first.upstreamProxy, QString("http://127.0.0.1:7890"));
        QCOMPARE(store.selectedProfileId(&error), first.id);

        UpstreamProfile fetched;
        QVERIFY2(store.profileByName(" main line ", &fetched, &error), qPrintable(error));
        QCOMPARE(fetched.apiKey, first.apiKey);
        UpstreamProfile duplicate = makeProfile("MAIN LINE", "https://two.example/v1");
        QVERIFY(!store.addProfile(&duplicate, &error));

        UpstreamProfile unicode = makeProfile(QString::fromUtf8("Äpfel"), "https://three.example/v1");
        QVERIFY2(store.addProfile(&unicode, &error), qPrintable(error));
        UpstreamProfile unicodeDuplicate = makeProfile(QString::fromUtf8("ÄPFEL"), "https://four.example/v1");
        QVERIFY(!store.addProfile(&unicodeDuplicate, &error));

        first.displayName = "Renamed";
        first.apiKey = "changed";
        QVERIFY2(store.updateProfile(first, &error), qPrintable(error));
        QVERIFY2(store.profileById(first.id, &fetched, &error), qPrintable(error));
        QCOMPARE(fetched.displayName, QString("Renamed"));
        QCOMPARE(fetched.apiKey, QString("changed"));

        QVERIFY2(store.removeProfile(first.id, &error), qPrintable(error));
        QCOMPARE(store.selectedProfileId(&error), unicode.id);
        QVERIFY2(store.removeProfile(unicode.id, &error), qPrintable(error));
        QVERIFY(store.selectedProfileId(&error).isEmpty());
        QVERIFY(error.isEmpty());
    }

    void paginationSearchAndSort()
    {
        QTemporaryDir dir;
        UpstreamProfileStore store(dir.filePath("profiles.sqlite3"));
        QString error;
        QVERIFY2(store.open(&error), qPrintable(error));
        const QStringList names = QStringList() << "Bravo" << "Alpha%" << "Charlie" << "Delta_";
        for (int i = 0; i < names.size(); ++i) {
            UpstreamProfile profile = makeProfile(names.at(i), QString("https://host%1.example/v1").arg(i));
            QVERIFY2(store.addProfile(&profile, &error), qPrintable(error));
        }
        UpstreamProfilePage page;
        QVERIFY2(store.listProfiles(QString(), 1, 2, SortByDisplayName,
                                    Qt::AscendingOrder, &page, &error), qPrintable(error));
        QCOMPARE(page.totalItems, 4);
        QCOMPARE(page.totalPages, 2);
        QCOMPARE(page.items.size(), 2);
        QCOMPARE(page.items.at(0).displayName, QString("Alpha%"));
        QVERIFY2(store.listProfiles("%", 1, 20, SortByUpdatedAt,
                                    Qt::DescendingOrder, &page, &error), qPrintable(error));
        QCOMPARE(page.totalItems, 1);
        QCOMPARE(page.items.first().displayName, QString("Alpha%"));
        QVERIFY2(store.listProfiles("_", 1, 20, SortByUpdatedAt,
                                    Qt::DescendingOrder, &page, &error), qPrintable(error));
        QCOMPARE(page.totalItems, 1);
        QCOMPARE(page.items.first().displayName, QString("Delta_"));
        QVERIFY(!store.listProfiles(QString(), 0, 20, SortByUpdatedAt,
                                    Qt::DescendingOrder, &page, &error));
        QVERIFY(!store.listProfiles(QString(), 1, 101, SortByUpdatedAt,
                                    Qt::DescendingOrder, &page, &error));
    }

    void rejectsNewerSchemaAndCleansFailedConnection()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("future.sqlite3");
        const QString connection = "future_schema_test";
        {
            QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connection);
            database.setDatabaseName(path);
            QVERIFY(database.open());
            QSqlQuery query(database);
            QVERIFY(query.exec("PRAGMA user_version=99"));
            database.close();
        }
        QSqlDatabase::removeDatabase(connection);

        UpstreamProfileStore store(path);
        QString error;
        QVERIFY(!store.open(&error));
        QVERIFY(error.contains("newer"));
        QVERIFY(!store.isOpen());
        error.clear();
        QVERIFY(!store.open(&error));
        QVERIFY(error.contains("newer"));
        QVERIFY(!store.isOpen());
    }

    void existingCustomDirectoryPermissionsRemainUnchanged()
    {
#if defined(Q_OS_WIN)
        QSKIP("POSIX directory permission semantics are not available on Windows");
#else
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString customDirectory = dir.filePath("shared-config");
        QVERIFY(QDir().mkpath(customDirectory));
        const QFileDevice::Permissions sharedPermissions =
            QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
            QFileDevice::ReadGroup | QFileDevice::ExeGroup |
            QFileDevice::ReadOther | QFileDevice::ExeOther;
        QVERIFY(QFile::setPermissions(customDirectory, sharedPermissions));
        const QFileDevice::Permissions directoryPermissions = QFileInfo(customDirectory).permissions();

        const QString databasePath = QDir(customDirectory).filePath("profiles.sqlite3");
        UpstreamProfileStore store(databasePath);
        QString error;
        QVERIFY2(store.open(&error), qPrintable(error));
        UpstreamProfilePage page;
        QVERIFY2(store.listProfiles(QString(), 1, 20, SortByUpdatedAt,
                                    Qt::DescendingOrder, &page, &error), qPrintable(error));
        QCOMPARE(QFileInfo(customDirectory).permissions(), directoryPermissions);
        const QFileDevice::Permissions databasePermissions = QFileInfo(databasePath).permissions();
        QVERIFY(databasePermissions.testFlag(QFileDevice::ReadOwner));
        QVERIFY(databasePermissions.testFlag(QFileDevice::WriteOwner));
        QVERIFY(!(databasePermissions & (QFileDevice::ReadGroup | QFileDevice::WriteGroup |
                                         QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                         QFileDevice::WriteOther | QFileDevice::ExeOther)));

        const QString configPath = QDir(customDirectory).filePath("config.json");
        AppConfig config;
        QVERIFY2(saveConfig(config, configPath, &error), qPrintable(error));
        QCOMPARE(QFileInfo(customDirectory).permissions(), directoryPermissions);
        const QFileDevice::Permissions configPermissions = QFileInfo(configPath).permissions();
        QVERIFY(configPermissions.testFlag(QFileDevice::ReadOwner));
        QVERIFY(configPermissions.testFlag(QFileDevice::WriteOwner));
        QVERIFY(!(configPermissions & (QFileDevice::ReadGroup | QFileDevice::WriteGroup |
                                       QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                       QFileDevice::WriteOther | QFileDevice::ExeOther)));
#endif
    }

    void exportImportRedactsAndPreservesSecrets()
    {
        QTemporaryDir dir;
        QString error;
        UpstreamProfileStore source(dir.filePath("source.sqlite3"));
        QVERIFY2(source.open(&error), qPrintable(error));
        UpstreamProfile profile = makeProfile("Primary", "https://example.com/v1");
        profile.apiKey = "  sk-secret  ";
        profile.forwardUserAgent = true;
        QVERIFY2(source.addProfile(&profile, &error), qPrintable(error));

        const QString redactedPath = dir.filePath("redacted.json");
        QVERIFY2(source.exportJson(redactedPath, false, &error), qPrintable(error));
        QJsonObject exported = readObject(redactedPath);
        QJsonObject exportedProfile = exported.value("profiles").toArray().first().toObject();
        QVERIFY(!exportedProfile.contains("api_key"));
        QCOMPARE(exportedProfile.value("api_key_configured").toBool(), true);

        UpstreamProfileStore target(dir.filePath("target.sqlite3"));
        QVERIFY2(target.open(&error), qPrintable(error));
        UpstreamProfile existing = profile;
        existing.apiKey = "local-secret";
        QVERIFY2(target.addProfile(&existing, &error), qPrintable(error));
        UpstreamProfileImportResult imported;
        QVERIFY2(target.importJson(redactedPath, OverwriteImportConflicts, &imported, &error), qPrintable(error));
        QCOMPARE(imported.updated, 1);
        UpstreamProfile fetched;
        QVERIFY2(target.profileById(profile.id, &fetched, &error), qPrintable(error));
        QCOMPARE(fetched.apiKey, QString("local-secret"));

        UpstreamProfileStore emptyTarget(dir.filePath("empty-target.sqlite3"));
        QVERIFY2(emptyTarget.open(&error), qPrintable(error));
        QVERIFY2(emptyTarget.importJson(redactedPath, OverwriteImportConflicts, &imported, &error),
                 qPrintable(error));
        QCOMPARE(imported.added, 1);
        QVERIFY2(emptyTarget.profileById(profile.id, &fetched, &error), qPrintable(error));
        QCOMPARE(fetched.apiKey, QString(""));

        const QString secretPath = dir.filePath("with-secrets.json");
        QVERIFY2(source.exportJson(secretPath, true, &error), qPrintable(error));
        const QJsonObject secretProfile =
            readObject(secretPath).value("profiles").toArray().first().toObject();
        QVERIFY(secretProfile.contains("api_key"));
        QCOMPARE(secretProfile.value("api_key").toString(), QString("sk-secret"));
        QVERIFY2(target.importJson(secretPath, OverwriteImportConflicts, &imported, &error), qPrintable(error));
        QVERIFY2(target.profileById(profile.id, &fetched, &error), qPrintable(error));
        QCOMPARE(fetched.apiKey, QString("sk-secret"));
    }

    void nameConflictOverwritePreservesStableIdAndExplicitEmptyClearsSecret()
    {
        QTemporaryDir dir;
        QString error;
        UpstreamProfileStore source(dir.filePath("source.sqlite3"));
        UpstreamProfileStore target(dir.filePath("target.sqlite3"));
        QVERIFY2(source.open(&error), qPrintable(error));
        QVERIFY2(target.open(&error), qPrintable(error));

        UpstreamProfile incoming = makeProfile("Shared Name", "https://new.example/v1");
        incoming.apiKey.clear();
        QVERIFY2(source.addProfile(&incoming, &error), qPrintable(error));
        UpstreamProfile existing = makeProfile("shared name", "https://old.example/v1");
        existing.apiKey = "keep-until-explicitly-cleared";
        QVERIFY2(target.addProfile(&existing, &error), qPrintable(error));
        QVERIFY(incoming.id != existing.id);

        const QString path = dir.filePath("profiles.json");
        QVERIFY2(source.exportJson(path, true, &error), qPrintable(error));
        UpstreamProfileImportResult result;
        QVERIFY2(target.importJson(path, OverwriteImportConflicts, &result, &error), qPrintable(error));
        QCOMPARE(result.updated, 1);
        UpstreamProfile fetched;
        QVERIFY2(target.profileById(existing.id, &fetched, &error), qPrintable(error));
        QCOMPARE(fetched.id, existing.id);
        QCOMPARE(fetched.baseUrl, QString("https://new.example/v1"));
        QVERIFY(fetched.apiKey.isEmpty());
        QVERIFY(!target.profileById(incoming.id, &fetched, &error));
        QVERIFY(error.isEmpty());
    }

    void importIsAtomicAndHonorsLocks()
    {
        QTemporaryDir dir;
        QString error;
        const QString dbPath = dir.filePath("profiles.sqlite3");
        UpstreamProfileStore store(dbPath);
        QVERIFY2(store.open(&error), qPrintable(error));
        UpstreamProfile profile = makeProfile("Locked", "https://example.com/v1");
        QVERIFY2(store.addProfile(&profile, &error), qPrintable(error));
        const QString exportPath = dir.filePath("profiles.json");
        QVERIFY2(store.exportJson(exportPath, true, &error), qPrintable(error));

        UpstreamProfileRunLock runLock(dbPath);
        QVERIFY2(runLock.tryLock(profile.id, &error), qPrintable(error));
        UpstreamProfileImportResult result;
        QVERIFY(!store.importJson(exportPath, OverwriteImportConflicts, &result, &error));
        QVERIFY(store.importJson(exportPath, SkipImportConflicts, &result, &error));
        QCOMPARE(result.skipped, 1);
        QVERIFY(!store.updateProfile(profile, &error));
        QVERIFY(!store.removeProfile(profile.id, &error));
        QVERIFY(!store.setSelectedProfileId(profile.id, &error));
        QVERIFY(!store.clearSelectedProfile(&error));
        runLock.unlock();
        QVERIFY2(store.updateProfile(profile, &error), qPrintable(error));
    }

    void selectionOnlyRunLockDoesNotSelectConcurrentFirstAdd()
    {
        QTemporaryDir dir;
        QString error;
        const QString dbPath = dir.filePath("profiles.sqlite3");
        UpstreamProfileStore store(dbPath);
        QVERIFY2(store.open(&error), qPrintable(error));
        UpstreamProfileRunLock runLock(dbPath);
        QVERIFY2(runLock.tryLock(QString(), &error), qPrintable(error));
        UpstreamProfile profile = makeProfile("Added", "https://example.com/v1");
        QVERIFY2(store.addProfile(&profile, &error), qPrintable(error));
        QVERIFY(store.selectedProfileId(&error).isEmpty());
        QVERIFY(error.isEmpty());
        runLock.unlock();
        QCOMPARE(store.selectedProfileId(&error), profile.id);
    }

    void runLocksAreVisibleAcrossStoreInstances()
    {
        QTemporaryDir dir;
        QString error;
        const QString dbPath = dir.filePath("profiles.sqlite3");
        UpstreamProfileStore firstStore(dbPath);
        UpstreamProfileStore secondStore(dbPath);
        QVERIFY2(firstStore.open(&error), qPrintable(error));
        QVERIFY2(secondStore.open(&error), qPrintable(error));
        UpstreamProfile first = makeProfile("First", "https://one.example/v1");
        UpstreamProfile second = makeProfile("Second", "https://two.example/v1");
        QVERIFY2(firstStore.addProfile(&first, &error), qPrintable(error));
        QVERIFY2(firstStore.addProfile(&second, &error), qPrintable(error));

        UpstreamProfileRunLock runLock(dbPath);
        QVERIFY2(runLock.tryLock(first.id, &error), qPrintable(error));
        QVERIFY(secondStore.isSelectionLocked());
        QVERIFY(secondStore.isProfileLocked(first.id));
        QVERIFY(!secondStore.isProfileLocked(second.id));
        UpstreamProfileRunLock competing(dbPath);
        QVERIFY(!competing.tryLock(second.id, &error));

        second.displayName = "Second Updated";
        QVERIFY2(secondStore.updateProfile(second, &error), qPrintable(error));
        UpstreamProfile third = makeProfile("Third", "https://three.example/v1");
        QVERIFY2(secondStore.addProfile(&third, &error), qPrintable(error));
        runLock.unlock();
        QVERIFY2(competing.tryLock(second.id, &error), qPrintable(error));
    }

    void liveRunLocksDoNotExpireByAge()
    {
        QTemporaryDir dir;
        QString error;
        const QString dbPath = dir.filePath("profiles.sqlite3");
        UpstreamProfileStore store(dbPath);
        QVERIFY2(store.open(&error), qPrintable(error));
        UpstreamProfile profile = makeProfile("Long Running", "https://example.com/v1");
        QVERIFY2(store.addProfile(&profile, &error), qPrintable(error));

        UpstreamProfileRunLock runLock(dbPath);
        QVERIFY2(runLock.tryLock(profile.id, &error), qPrintable(error));
        const QByteArray digest = QCryptographicHash::hash(profile.id.toUtf8(),
                                                            QCryptographicHash::Sha256).toHex();
        const QStringList lockPaths = QStringList()
            << dbPath + ".selection.lock"
            << dbPath + ".profile-" + QString::fromLatin1(digest) + ".lock";
        const QDateTime oldTime = QDateTime::currentDateTime().addSecs(-60);
        for (const QString &path : lockPaths) {
            QFile lockFile(path);
            QVERIFY2(lockFile.open(QIODevice::ReadOnly), qPrintable(lockFile.errorString()));
            QVERIFY2(lockFile.setFileTime(oldTime, QFileDevice::FileModificationTime),
                     qPrintable(lockFile.errorString()));
        }

        QVERIFY(store.isSelectionLocked());
        QVERIFY(store.isProfileLocked(profile.id));
        UpstreamProfileRunLock competing(dbPath);
        QVERIFY(!competing.tryLock(profile.id, &error));
        profile.baseUrl = "https://changed.example.com/v1";
        QVERIFY(!store.updateProfile(profile, &error));
        QVERIFY(!store.removeProfile(profile.id, &error));
        QVERIFY(!store.setSelectedProfileId(profile.id, &error));
        runLock.unlock();
    }

    void migratesLegacyConfigAndEffectiveProxy()
    {
        QTemporaryDir dir;
        const QString configPath = dir.filePath("config.json");
        QJsonObject legacy;
        legacy.insert("lang", "zh");
        legacy.insert("unknown_setting", 42);
        legacy.insert("upstream_base_url", "https://example.com/v1/");
        legacy.insert("upstream_api_key", "sk-old");
        legacy.insert("upstream_user_agent", "legacy-agent");
        legacy.insert("forward_user_agent", true);
        legacy.insert("upstream_http_proxy", "127.0.0.1:7890");
        legacy.insert("upstream_https_proxy", "http://127.0.0.1:7891");
        legacy.insert("upstream_socks_proxy", "127.0.0.1:1080");
        legacy.insert("upstream_timeout_sec", 600);
        legacy.insert("first_token_timeout_sec", 10);
        QVERIFY(writeObject(configPath, legacy));
        AppConfig config = loadConfig(configPath);
        UpstreamProfileStore store(upstreamProfileDatabasePath(configPath));
        QString error;
        QVERIFY2(store.open(&error), qPrintable(error));
        bool migrated = false;
        QVERIFY2(migrateLegacyUpstreamConfig(configPath, &config, &store, &migrated, &error), qPrintable(error));
        QVERIFY(migrated);
        QVERIFY(QFile::exists(configPath + ".pre-upstream-profiles.bak"));
        UpstreamProfile profile;
        QVERIFY2(store.selectedProfile(&profile, &error), qPrintable(error));
        QCOMPARE(profile.baseUrl, QString("https://example.com/v1"));
        QCOMPARE(profile.apiKey, QString("sk-old"));
        QCOMPARE(profile.upstreamProxy, QString("http://127.0.0.1:7891"));
        const QJsonObject stripped = readObject(configPath);
        QVERIFY(!stripped.contains("upstream_base_url"));
        QVERIFY(!stripped.contains("upstream_https_proxy"));
        QCOMPARE(stripped.value("unknown_setting").toInt(), 42);

        migrated = true;
        QVERIFY2(migrateLegacyUpstreamConfig(configPath, &config, &store, &migrated, &error), qPrintable(error));
        QVERIFY(!migrated);
    }

    void migrationUsesCurrentJsonSnapshotInsteadOfStaleConfig()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString configPath = dir.filePath("config.json");

        QJsonObject staleJson;
        staleJson.insert("lang", "zh");
        staleJson.insert("proxy_host", "127.0.0.2");
        staleJson.insert("upstream_base_url", "https://stale.example/v1");
        staleJson.insert("upstream_api_key", "stale-secret");
        staleJson.insert("upstream_user_agent", "stale-agent");
        staleJson.insert("upstream_timeout_sec", 300);
        QVERIFY(writeObject(configPath, staleJson));
        AppConfig config = loadConfig(configPath);
        QCOMPARE(config.upstreamBaseUrl, QString("https://stale.example/v1"));

        QJsonObject currentJson;
        currentJson.insert("lang", "en");
        currentJson.insert("proxy_host", "127.0.0.9");
        currentJson.insert("upstream_base_url", "https://current.example/v1/");
        currentJson.insert("upstream_api_key", "current-secret");
        currentJson.insert("upstream_user_agent", "current-agent");
        currentJson.insert("forward_user_agent", true);
        currentJson.insert("upstream_https_proxy", "http://127.0.0.1:7891");
        currentJson.insert("upstream_timeout_sec", 900);
        currentJson.insert("first_token_timeout_sec", 15);
        QVERIFY(writeObject(configPath, currentJson));

        UpstreamProfileStore store(upstreamProfileDatabasePath(configPath));
        QString error;
        QVERIFY2(store.open(&error), qPrintable(error));
        bool migrated = false;
        QVERIFY2(migrateLegacyUpstreamConfig(configPath, &config, &store, &migrated, &error),
                 qPrintable(error));
        QVERIFY(migrated);

        UpstreamProfile profile;
        QVERIFY2(store.selectedProfile(&profile, &error), qPrintable(error));
        QCOMPARE(profile.baseUrl, QString("https://current.example/v1"));
        QCOMPARE(profile.apiKey, QString("current-secret"));
        QCOMPARE(profile.userAgent, QString("current-agent"));
        QVERIFY(profile.forwardUserAgent);
        QCOMPARE(profile.upstreamProxy, QString("http://127.0.0.1:7891"));
        QCOMPARE(profile.upstreamTimeoutSec, 900);
        QCOMPARE(profile.firstTokenTimeoutSec, 15);

        QCOMPARE(config.lang, QString("en"));
        QCOMPARE(config.proxyHost, QString("127.0.0.9"));
        QVERIFY(config.upstreamBaseUrl.isEmpty());
        QCOMPARE(readObject(configPath).value("lang").toString(), QString("en"));
        QCOMPARE(readObject(configPath).value("proxy_host").toString(), QString("127.0.0.9"));
        QCOMPARE(readObject(configPath + ".pre-upstream-profiles.bak"), currentJson);
    }

    void cleansDefaultLegacyPlaceholdersWithoutCreatingProfile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString configPath = dir.filePath("config.json");
        QJsonObject legacy;
        legacy.insert("lang", "zh");
        legacy.insert("upstream_base_url", "");
        legacy.insert("upstream_api_key", "");
        legacy.insert("upstream_user_agent", "curl/8.7.1");
        legacy.insert("forward_user_agent", false);
        legacy.insert("upstream_proxy", "");
        legacy.insert("upstream_http_proxy", "");
        legacy.insert("upstream_https_proxy", "");
        legacy.insert("upstream_socks_proxy", "");
        legacy.insert("upstream_timeout_sec", 1800);
        legacy.insert("first_token_timeout_sec", 30);
        QVERIFY(writeObject(configPath, legacy));

        AppConfig config = loadConfig(configPath);
        UpstreamProfileStore store(upstreamProfileDatabasePath(configPath));
        QString error;
        QVERIFY2(store.open(&error), qPrintable(error));
        bool migrated = false;
        QVERIFY2(migrateLegacyUpstreamConfig(configPath, &config, &store, &migrated, &error),
                 qPrintable(error));
        QVERIFY(migrated);
        QCOMPARE(readObject(configPath).value("lang").toString(), QString("zh"));
        QVERIFY(!readObject(configPath).contains("upstream_base_url"));
        QVERIFY(QFile::exists(configPath + ".pre-upstream-profiles.bak"));
        UpstreamProfilePage page;
        QVERIFY2(store.listProfiles(QString(), 1, 20, SortByUpdatedAt,
                                    Qt::DescendingOrder, &page, &error), qPrintable(error));
        QCOMPARE(page.totalItems, 0);
    }

    void preservesMeaningfulLegacyFieldsWithoutBaseUrl()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString configPath = dir.filePath("config.json");
        QJsonObject legacy;
        legacy.insert("lang", "en");
        legacy.insert("upstream_base_url", "");
        legacy.insert("upstream_api_key", "orphan-secret");
        legacy.insert("upstream_proxy", "http://127.0.0.1:7890");
        QVERIFY(writeObject(configPath, legacy));

        AppConfig config = loadConfig(configPath);
        UpstreamProfileStore store(upstreamProfileDatabasePath(configPath));
        QString error;
        QVERIFY2(store.open(&error), qPrintable(error));
        bool migrated = false;
        QVERIFY(!migrateLegacyUpstreamConfig(configPath, &config, &store, &migrated, &error));
        QVERIFY(error.contains("no Base URL"));
        QVERIFY(!migrated);
        QCOMPARE(readObject(configPath), legacy);
        QVERIFY(!QFile::exists(configPath + ".pre-upstream-profiles.bak"));
    }

    void resumesCommittedLegacyMigrationAfterInterruptedJsonCleanup()
    {
        QTemporaryDir dir;
        const QString configPath = dir.filePath("config.json");
        QJsonObject legacy;
        legacy.insert("lang", "en");
        legacy.insert("upstream_base_url", "https://resume.example/v1");
        legacy.insert("upstream_api_key", "resume-secret");
        legacy.insert("upstream_timeout_sec", 500);
        QVERIFY(writeObject(configPath, legacy));

        QString error;
        const QString dbPath = upstreamProfileDatabasePath(configPath);
        UpstreamProfileStore store(dbPath);
        QVERIFY2(store.open(&error), qPrintable(error));
        UpstreamProfile profile = makeProfile("Migrated", "https://resume.example/v1");
        profile.apiKey = "resume-secret";
        profile.upstreamTimeoutSec = 500;
        QVERIFY2(store.addProfile(&profile, &error), qPrintable(error));

        QJsonObject legacyValues;
        legacyValues.insert("upstream_base_url", legacy.value("upstream_base_url"));
        legacyValues.insert("upstream_api_key", legacy.value("upstream_api_key"));
        legacyValues.insert("upstream_timeout_sec", legacy.value("upstream_timeout_sec"));
        const QString fingerprint = QString::fromLatin1(QCryptographicHash::hash(
            QJsonDocument(legacyValues).toJson(QJsonDocument::Compact),
            QCryptographicHash::Sha256).toHex());
        QJsonObject marker;
        marker.insert("profile_id", profile.id);
        marker.insert("legacy_fingerprint", fingerprint);

        const QString connection = "pending_migration_test";
        {
            QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connection);
            database.setDatabaseName(dbPath);
            QVERIFY(database.open());
            QSqlQuery query(database);
            query.prepare("INSERT OR REPLACE INTO app_meta (key, value) VALUES (?, ?)");
            query.addBindValue("legacy_upstream_migration_pending_v1");
            query.addBindValue(QString::fromUtf8(QJsonDocument(marker).toJson(QJsonDocument::Compact)));
            QVERIFY(query.exec());
            database.close();
        }
        QSqlDatabase::removeDatabase(connection);

        AppConfig config = loadConfig(configPath);
        bool migrated = false;
        QVERIFY2(migrateLegacyUpstreamConfig(configPath, &config, &store, &migrated, &error), qPrintable(error));
        QVERIFY(migrated);
        const QJsonObject recovered = readObject(configPath);
        QCOMPARE(recovered.value("lang").toString(), QString("en"));
        QVERIFY(!recovered.contains("upstream_base_url"));
        QVERIFY(!recovered.contains("upstream_api_key"));
        QVERIFY(!recovered.contains("upstream_timeout_sec"));
        QVERIFY(QFile::exists(configPath + ".pre-upstream-profiles.bak"));

        QString markerValue;
        {
            QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connection);
            database.setDatabaseName(dbPath);
            QVERIFY(database.open());
            QSqlQuery query(database);
            query.prepare("SELECT value FROM app_meta WHERE key=?");
            query.addBindValue("legacy_upstream_migration_pending_v1");
            QVERIFY(query.exec());
            QVERIFY(!query.next());
            database.close();
        }
        Q_UNUSED(markerValue)
        QSqlDatabase::removeDatabase(connection);
    }

    void preservesLegacyFieldsWhenProfileDatabaseIsAlreadyPopulated()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString configPath = dir.filePath("config.json");
        QJsonObject legacy;
        legacy.insert("lang", "en");
        legacy.insert("upstream_base_url", "https://legacy.example/v1");
        legacy.insert("upstream_api_key", "legacy-secret");
        QVERIFY(writeObject(configPath, legacy));

        QString error;
        UpstreamProfileStore store(upstreamProfileDatabasePath(configPath));
        QVERIFY2(store.open(&error), qPrintable(error));
        UpstreamProfile existing = makeProfile("Existing", "https://existing.example/v1");
        QVERIFY2(store.addProfile(&existing, &error), qPrintable(error));

        AppConfig config = loadConfig(configPath);
        bool migrated = false;
        QVERIFY(!migrateLegacyUpstreamConfig(configPath, &config, &store, &migrated, &error));
        QVERIFY(error.contains("profile database is not empty"));
        QVERIFY(!migrated);
        QCOMPARE(readObject(configPath), legacy);
        UpstreamProfilePage page;
        QVERIFY2(store.listProfiles(QString(), 1, 20, SortByUpdatedAt,
                                    Qt::DescendingOrder, &page, &error), qPrintable(error));
        QCOMPARE(page.totalItems, 1);
    }

    void preservesLegacyFieldsWhenPendingMigrationFingerprintChanged()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString configPath = dir.filePath("config.json");
        QJsonObject legacy;
        legacy.insert("upstream_base_url", "https://changed.example/v1");
        legacy.insert("upstream_api_key", "changed-secret");
        QVERIFY(writeObject(configPath, legacy));

        QString error;
        const QString dbPath = upstreamProfileDatabasePath(configPath);
        UpstreamProfileStore store(dbPath);
        QVERIFY2(store.open(&error), qPrintable(error));
        UpstreamProfile pending = makeProfile("Pending", "https://original.example/v1");
        QVERIFY2(store.addProfile(&pending, &error), qPrintable(error));

        QJsonObject marker;
        marker.insert("profile_id", pending.id);
        marker.insert("legacy_fingerprint", QString(64, QLatin1Char('0')));
        const QString connection = "pending_migration_mismatch_test";
        {
            QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connection);
            database.setDatabaseName(dbPath);
            QVERIFY(database.open());
            QSqlQuery query(database);
            query.prepare("INSERT OR REPLACE INTO app_meta (key, value) VALUES (?, ?)");
            query.addBindValue("legacy_upstream_migration_pending_v1");
            query.addBindValue(QString::fromUtf8(QJsonDocument(marker).toJson(QJsonDocument::Compact)));
            QVERIFY(query.exec());
            database.close();
        }
        QSqlDatabase::removeDatabase(connection);

        AppConfig config = loadConfig(configPath);
        bool migrated = false;
        QVERIFY(!migrateLegacyUpstreamConfig(configPath, &config, &store, &migrated, &error));
        QVERIFY(error.contains("no longer matches"));
        QVERIFY(!migrated);
        QCOMPARE(readObject(configPath), legacy);

        {
            QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connection);
            database.setDatabaseName(dbPath);
            QVERIFY(database.open());
            QSqlQuery query(database);
            query.prepare("SELECT value FROM app_meta WHERE key=?");
            query.addBindValue("legacy_upstream_migration_pending_v1");
            QVERIFY(query.exec());
            QVERIFY(!query.next());
            database.close();
        }
        QSqlDatabase::removeDatabase(connection);
    }
};

QTEST_MAIN(UpstreamProfileStoreTest)

#include "upstream_profile_store_test.moc"
