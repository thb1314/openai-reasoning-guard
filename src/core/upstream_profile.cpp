#include "core/upstream_profile.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QLockFile>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtCore/QStandardPaths>
#include <QtCore/QUuid>
#include <QtCore/QUrl>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlError>
#include <QtSql/QSqlQuery>
#include <QtSql/QSqlRecord>

namespace net_tunnel {

namespace {

const int kSchemaVersion = 1;
const int kBusyTimeoutMs = 5000;
const char kSelectedProfileKey[] = "selected_profile_id";
const char kPendingLegacyMigrationKey[] = "legacy_upstream_migration_pending_v1";

bool fail(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
    return false;
}

void clearError(QString *error)
{
    if (error) {
        error->clear();
    }
}

QString sqlErrorText(const QString &operation, const QSqlQuery &query)
{
    return QString("%1: %2").arg(operation, query.lastError().text());
}

QString sqlErrorText(const QString &operation, const QSqlDatabase &database)
{
    return QString("%1: %2").arg(operation, database.lastError().text());
}

QString newUuid()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
}

QString profileNameKey(const QString &displayName)
{
    return displayName.trimmed().toCaseFolded();
}

bool canonicalUuid(const QString &value, QString *result)
{
    const QString trimmed = value.trimmed();
    const QUuid uuid(trimmed);
    if (uuid.isNull()) {
        return false;
    }
    const QString canonical = uuid.toString(QUuid::WithoutBraces).toLower();
    QString input = trimmed.toLower();
    if (input.startsWith('{') && input.endsWith('}')) {
        input = input.mid(1, input.size() - 2);
    }
    if (input != canonical) {
        return false;
    }
    if (result) {
        *result = canonical;
    }
    return true;
}

QString dateTimeText(const QDateTime &value)
{
    const QDateTime normalized = value.isValid() ? value.toUTC() : QDateTime::currentDateTimeUtc();
    return normalized.toString(Qt::ISODateWithMs);
}

QDateTime parseDateTime(const QVariant &value)
{
    QDateTime parsed = QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
    if (!parsed.isValid()) {
        parsed = QDateTime::fromString(value.toString(), Qt::ISODate);
    }
    return parsed.isValid() ? parsed.toUTC() : QDateTime();
}

QString normalizedBaseUrl(const QString &value)
{
    QUrl url(value.trimmed(), QUrl::StrictMode);
    QString path = url.path(QUrl::FullyEncoded);
    while (path.endsWith('/')) {
        path.chop(1);
    }
    url.setScheme(url.scheme().toLower());
    url.setPath(path, QUrl::StrictMode);
    return url.toString(QUrl::FullyEncoded);
}

QString normalizedProxy(const QString &value)
{
    QString text = value.trimmed();
    if (text.isEmpty()) return QString("");
    if (!text.isEmpty() && !text.contains("://")) {
        text.prepend("http://");
    }
    return text;
}

bool validateUpstreamProxyImpl(const QString &value, QString *message)
{
    const QString text = normalizedProxy(value);
    if (text.isEmpty()) {
        return true;
    }
    const QUrl url(text, QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();
    const QString path = url.path(QUrl::FullyEncoded);
    const bool supported = scheme == "http" || scheme == "socks" ||
                           scheme == "socks5" || scheme == "socks5h";
    if (!url.isValid() || !supported || url.host().isEmpty() || url.port(-1) > 65535 ||
        !url.userName().isEmpty() || !url.password().isEmpty() ||
        (!path.isEmpty() && path != "/") || url.hasQuery() || url.hasFragment()) {
        if (message) {
            *message = "upstream proxy must use http, socks, socks5, or socks5h, include a host, "
                       "and omit credentials, path, query, and fragment";
        }
        return false;
    }
    return true;
}

bool containsControlCharacters(const QString &value)
{
    for (int i = 0; i < value.size(); ++i) {
        const ushort code = value.at(i).unicode();
        if (code < 0x20 || code == 0x7f) return true;
    }
    return false;
}

bool validateOutboundHeaderValueImpl(const QString &value, QString *error)
{
    clearError(error);
    if (value.toUtf8().size() > 8192) {
        return fail(error, "header value must not exceed 8192 UTF-8 bytes");
    }
    if (containsControlCharacters(value)) {
        return fail(error, "header value must not contain control characters");
    }
    return true;
}

void normalizeProfile(UpstreamProfile *profile)
{
    profile->displayName = profile->displayName.trimmed();
    profile->baseUrl = normalizedBaseUrl(profile->baseUrl);
    profile->apiKey = profile->apiKey.trimmed();
    if (profile->apiKey.isNull()) profile->apiKey = QString("");
    profile->userAgent = profile->userAgent.trimmed();
    if (profile->userAgent.isNull()) profile->userAgent = QString("");
    profile->upstreamProxy = normalizedProxy(profile->upstreamProxy);
}

QString profileLockPath(const QString &databasePath, const QString &profileId)
{
    const QByteArray digest = QCryptographicHash::hash(profileId.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QFileInfo(databasePath).absoluteFilePath() + ".profile-" + QString::fromLatin1(digest) + ".lock";
}

QString selectionLockPath(const QString &databasePath)
{
    return QFileInfo(databasePath).absoluteFilePath() + ".selection.lock";
}

QLockFile *newLock(const QString &path)
{
    QLockFile *lock = new QLockFile(path);
    lock->setStaleLockTime(0);
    return lock;
}

bool lockNow(QLockFile *lock, const QString &description, QString *error)
{
    if (lock->tryLock(0)) {
        return true;
    }
    return fail(error, QString("%1 is locked by another running process").arg(description));
}

bool execSql(QSqlDatabase database, const QString &sql, QString *error)
{
    QSqlQuery query(database);
    if (!query.exec(sql)) {
        return fail(error, sqlErrorText(QString("SQL failed (%1)").arg(sql), query));
    }
    return true;
}

bool beginImmediate(QSqlDatabase database, QString *error)
{
    return execSql(database, "BEGIN IMMEDIATE TRANSACTION", error);
}

void rollback(QSqlDatabase database)
{
    QSqlQuery query(database);
    query.exec("ROLLBACK");
}

bool commit(QSqlDatabase database, QString *error)
{
    return execSql(database, "COMMIT", error);
}

UpstreamProfile profileFromQuery(const QSqlQuery &query)
{
    UpstreamProfile profile;
    profile.id = query.value(0).toString();
    profile.displayName = query.value(1).toString();
    profile.baseUrl = query.value(2).toString();
    profile.apiKey = query.value(3).toString();
    profile.userAgent = query.value(4).toString();
    profile.forwardUserAgent = query.value(5).toInt() != 0;
    profile.upstreamProxy = query.value(6).toString();
    profile.upstreamTimeoutSec = query.value(7).toInt();
    profile.firstTokenTimeoutSec = query.value(8).toInt();
    profile.createdAtUtc = parseDateTime(query.value(9));
    profile.updatedAtUtc = parseDateTime(query.value(10));
    return profile;
}

QString profileColumns()
{
    return "id, display_name, base_url, api_key, user_agent, forward_user_agent, "
           "upstream_proxy, upstream_timeout_sec, first_token_timeout_sec, "
           "created_at_utc, updated_at_utc";
}

bool bindAndInsert(QSqlDatabase database, const UpstreamProfile &profile, QString *error)
{
    QSqlQuery query(database);
    query.prepare("INSERT INTO upstream_profiles (id, display_name, display_name_key, base_url, api_key, "
                  "user_agent, forward_user_agent, upstream_proxy, upstream_timeout_sec, "
                  "first_token_timeout_sec, created_at_utc, updated_at_utc) "
                  "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    query.addBindValue(profile.id);
    query.addBindValue(profile.displayName);
    query.addBindValue(profileNameKey(profile.displayName));
    query.addBindValue(profile.baseUrl);
    query.addBindValue(profile.apiKey);
    query.addBindValue(profile.userAgent);
    query.addBindValue(profile.forwardUserAgent ? 1 : 0);
    query.addBindValue(profile.upstreamProxy);
    query.addBindValue(profile.upstreamTimeoutSec);
    query.addBindValue(profile.firstTokenTimeoutSec);
    query.addBindValue(dateTimeText(profile.createdAtUtc));
    query.addBindValue(dateTimeText(profile.updatedAtUtc));
    if (!query.exec()) {
        return fail(error, sqlErrorText("failed to add upstream profile", query));
    }
    return true;
}

bool bindAndUpdate(QSqlDatabase database, const UpstreamProfile &profile, QString *error)
{
    QSqlQuery query(database);
    query.prepare("UPDATE upstream_profiles SET display_name=?, display_name_key=?, base_url=?, api_key=?, user_agent=?, "
                  "forward_user_agent=?, upstream_proxy=?, upstream_timeout_sec=?, "
                  "first_token_timeout_sec=?, updated_at_utc=? WHERE id=?");
    query.addBindValue(profile.displayName);
    query.addBindValue(profileNameKey(profile.displayName));
    query.addBindValue(profile.baseUrl);
    query.addBindValue(profile.apiKey);
    query.addBindValue(profile.userAgent);
    query.addBindValue(profile.forwardUserAgent ? 1 : 0);
    query.addBindValue(profile.upstreamProxy);
    query.addBindValue(profile.upstreamTimeoutSec);
    query.addBindValue(profile.firstTokenTimeoutSec);
    query.addBindValue(dateTimeText(profile.updatedAtUtc));
    query.addBindValue(profile.id);
    if (!query.exec()) {
        return fail(error, sqlErrorText("failed to update upstream profile", query));
    }
    if (query.numRowsAffected() != 1) {
        return fail(error, "upstream profile does not exist");
    }
    return true;
}

bool queryProfile(QSqlDatabase database,
                  const QString &column,
                  const QString &value,
                  UpstreamProfile *profile,
                  QString *error)
{
    QSqlQuery query(database);
    query.prepare("SELECT " + profileColumns() + " FROM upstream_profiles WHERE " + column + "=?");
    query.addBindValue(value);
    if (!query.exec()) {
        return fail(error, sqlErrorText("failed to read upstream profile", query));
    }
    if (!query.next()) {
        clearError(error);
        return false;
    }
    if (profile) {
        *profile = profileFromQuery(query);
    }
    return true;
}

bool writeSelectedId(QSqlDatabase database, const QString &id, QString *error)
{
    QSqlQuery query(database);
    if (id.isEmpty()) {
        query.prepare("DELETE FROM app_meta WHERE key=?");
        query.addBindValue(QString::fromLatin1(kSelectedProfileKey));
    } else {
        query.prepare("INSERT OR REPLACE INTO app_meta (key, value) VALUES (?, ?)");
        query.addBindValue(QString::fromLatin1(kSelectedProfileKey));
        query.addBindValue(id);
    }
    if (!query.exec()) {
        return fail(error, sqlErrorText("failed to save selected upstream profile", query));
    }
    return true;
}

bool writeMetaValue(QSqlDatabase database, const QString &key, const QString &value, QString *error)
{
    QSqlQuery query(database);
    query.prepare("INSERT OR REPLACE INTO app_meta (key, value) VALUES (?, ?)");
    query.addBindValue(key);
    query.addBindValue(value);
    if (!query.exec()) return fail(error, sqlErrorText("failed to write profile metadata", query));
    return true;
}

bool deleteMetaValue(QSqlDatabase database, const QString &key, QString *error)
{
    QSqlQuery query(database);
    query.prepare("DELETE FROM app_meta WHERE key=?");
    query.addBindValue(key);
    if (!query.exec()) return fail(error, sqlErrorText("failed to remove profile metadata", query));
    return true;
}

bool readMetaValue(QSqlDatabase database, const QString &key, QString *value, QString *error)
{
    QSqlQuery query(database);
    query.prepare("SELECT value FROM app_meta WHERE key=?");
    query.addBindValue(key);
    if (!query.exec()) return fail(error, sqlErrorText("failed to read profile metadata", query));
    *value = query.next() ? query.value(0).toString() : QString();
    return true;
}

QString readSelectedId(QSqlDatabase database, QString *error)
{
    QSqlQuery query(database);
    query.prepare("SELECT value FROM app_meta WHERE key=?");
    query.addBindValue(QString::fromLatin1(kSelectedProfileKey));
    if (!query.exec()) {
        fail(error, sqlErrorText("failed to read selected upstream profile", query));
        return QString();
    }
    clearError(error);
    return query.next() ? query.value(0).toString() : QString();
}

QString firstProfileId(QSqlDatabase database, QString *error)
{
    QSqlQuery query(database);
    if (!query.exec("SELECT id FROM upstream_profiles ORDER BY updated_at_utc DESC, id ASC LIMIT 1")) {
        fail(error, sqlErrorText("failed to choose upstream profile", query));
        return QString();
    }
    clearError(error);
    return query.next() ? query.value(0).toString() : QString();
}

QString escapedLike(const QString &value)
{
    QString escaped = value;
    escaped.replace("\\", "\\\\");
    escaped.replace("%", "\\%");
    escaped.replace("_", "\\_");
    return "%" + escaped + "%";
}

bool jsonInteger(const QJsonObject &object, const QString &key, int fallback, int *value, QString *error)
{
    if (!object.contains(key)) {
        *value = fallback;
        return true;
    }
    const QJsonValue json = object.value(key);
    if (!json.isDouble()) {
        return fail(error, QString("%1 must be an integer").arg(key));
    }
    const double number = json.toDouble();
    const int integer = json.toInt();
    if (number != double(integer)) {
        return fail(error, QString("%1 must be an integer").arg(key));
    }
    *value = integer;
    return true;
}

bool jsonString(const QJsonObject &object,
                const QString &key,
                const QString &fallback,
                bool required,
                QString *value,
                QString *error)
{
    if (!object.contains(key)) {
        if (required) {
            return fail(error, QString("%1 is required").arg(key));
        }
        *value = fallback;
        return true;
    }
    if (!object.value(key).isString()) {
        return fail(error, QString("%1 must be a string").arg(key));
    }
    *value = object.value(key).toString();
    return true;
}

QJsonObject profileToJson(const UpstreamProfile &profile, bool includeSecrets)
{
    QJsonObject object;
    object.insert("id", profile.id);
    object.insert("display_name", profile.displayName);
    object.insert("base_url", profile.baseUrl);
    object.insert("user_agent", profile.userAgent);
    object.insert("forward_user_agent", profile.forwardUserAgent);
    object.insert("upstream_proxy", profile.upstreamProxy);
    object.insert("upstream_timeout_sec", profile.upstreamTimeoutSec);
    object.insert("first_token_timeout_sec", profile.firstTokenTimeoutSec);
    object.insert("created_at_utc", dateTimeText(profile.createdAtUtc));
    object.insert("updated_at_utc", dateTimeText(profile.updatedAtUtc));
    object.insert("api_key_configured", !profile.apiKey.isEmpty());
    if (includeSecrets) {
        object.insert("api_key", profile.apiKey);
    }
    return object;
}

bool enforceOwnerOnlyPermissions(const QString &path, bool directory, QString *error)
{
#if defined(Q_OS_WIN)
    Q_UNUSED(path)
    Q_UNUSED(directory)
    Q_UNUSED(error)
    return true;
#else
    QFileDevice::Permissions permissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if (directory) permissions |= QFileDevice::ExeOwner;
    if (!QFile::setPermissions(path, permissions)) {
        return fail(error, QString("failed to restrict permissions on %1").arg(path));
    }
    const QFileDevice::Permissions actual = QFileInfo(path).permissions();
    const QFileDevice::Permissions exposed = QFileDevice::ReadGroup | QFileDevice::WriteGroup |
        QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::WriteOther |
        QFileDevice::ExeOther;
    if ((actual & exposed) || !actual.testFlag(QFileDevice::ReadOwner) ||
        !actual.testFlag(QFileDevice::WriteOwner) ||
        (directory && !actual.testFlag(QFileDevice::ExeOwner))) {
        return fail(error, QString("owner-only permissions are not enforceable on %1").arg(path));
    }
    return true;
#endif
}

bool writeOwnerOnlyFile(const QString &path, const QByteArray &contents, QString *error)
{
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        return fail(error, QString("failed to open %1: %2").arg(path, file.errorString()));
    }
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        file.cancelWriting();
        return fail(error, QString("failed to restrict permissions on %1").arg(path));
    }
#if !defined(Q_OS_WIN)
    const QFileDevice::Permissions exposed = QFileDevice::ReadGroup | QFileDevice::WriteGroup |
        QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::WriteOther |
        QFileDevice::ExeOther;
    const QFileDevice::Permissions actual = file.permissions();
    if ((actual & exposed) || !actual.testFlag(QFileDevice::ReadOwner) ||
        !actual.testFlag(QFileDevice::WriteOwner)) {
        file.cancelWriting();
        return fail(error, QString("owner-only permissions are not enforceable on %1").arg(path));
    }
#endif
    if (file.write(contents) != contents.size()) {
        const QString message = file.errorString();
        file.cancelWriting();
        return fail(error, QString("failed to write %1: %2").arg(path, message));
    }
    if (!file.commit()) {
        return fail(error, QString("failed to commit %1: %2").arg(path, file.errorString()));
    }
    return true;
}

QStringList legacyUpstreamKeys()
{
    return QStringList()
        << "upstream_base_url" << "upstream_api_key" << "upstream_user_agent"
        << "forward_user_agent" << "upstream_proxy" << "upstream_http_proxy"
        << "upstream_https_proxy" << "upstream_socks_proxy" << "upstream_timeout_sec"
        << "first_token_timeout_sec" << "upstream_first_byte_timeout_seconds";
}

QJsonObject legacyUpstreamValues(const QJsonObject &configObject)
{
    QJsonObject values;
    const QStringList keys = legacyUpstreamKeys();
    for (int i = 0; i < keys.size(); ++i) {
        if (configObject.contains(keys.at(i))) values.insert(keys.at(i), configObject.value(keys.at(i)));
    }
    return values;
}

QJsonObject withoutLegacyUpstreamValues(const QJsonObject &configObject)
{
    QJsonObject stripped = configObject;
    const QStringList keys = legacyUpstreamKeys();
    for (int i = 0; i < keys.size(); ++i) stripped.remove(keys.at(i));
    return stripped;
}

QString legacyUpstreamFingerprint(const QJsonObject &configObject)
{
    const QByteArray json = QJsonDocument(legacyUpstreamValues(configObject)).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(json, QCryptographicHash::Sha256).toHex());
}

QString pendingMigrationMarker(const QString &profileId, const QString &fingerprint)
{
    QJsonObject marker;
    marker.insert("profile_id", profileId);
    marker.insert("legacy_fingerprint", fingerprint);
    return QString::fromUtf8(QJsonDocument(marker).toJson(QJsonDocument::Compact));
}

bool parsePendingMigrationMarker(const QString &text, QString *profileId, QString *fingerprint)
{
    const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8());
    if (!document.isObject()) return false;
    const QJsonObject object = document.object();
    if (!object.value("profile_id").isString() || !object.value("legacy_fingerprint").isString()) return false;
    QString canonical;
    if (!canonicalUuid(object.value("profile_id").toString(), &canonical)) return false;
    const QString hash = object.value("legacy_fingerprint").toString();
    if (hash.size() != 64) return false;
    if (profileId) *profileId = canonical;
    if (fingerprint) *fingerprint = hash;
    return true;
}

bool readPendingMigrationMarker(const QString &databasePath, QString *marker, QString *error)
{
    const QString connectionName = "upstream_profile_migration_read_" + newUuid();
    QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connectionName);
    database.setDatabaseName(databasePath);
    database.setConnectOptions(QString("QSQLITE_BUSY_TIMEOUT=%1").arg(kBusyTimeoutMs));
    if (!database.open()) {
        const QString message = sqlErrorText("failed to inspect pending legacy migration", database);
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName);
        return fail(error, message);
    }
    bool ok = false;
    QString localError;
    {
        ok = readMetaValue(database, QString::fromLatin1(kPendingLegacyMigrationKey), marker, &localError);
    }
    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
    return ok ? true : fail(error, localError);
}

bool clearPendingMigrationMarker(const QString &databasePath, QString *error)
{
    const QString connectionName = "upstream_profile_migration_clear_" + newUuid();
    QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connectionName);
    database.setDatabaseName(databasePath);
    database.setConnectOptions(QString("QSQLITE_BUSY_TIMEOUT=%1").arg(kBusyTimeoutMs));
    if (!database.open()) {
        const QString message = sqlErrorText("failed to open database to finish legacy migration", database);
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName);
        return fail(error, message);
    }
    bool ok = beginImmediate(database, error);
    if (ok) ok = deleteMetaValue(database, QString::fromLatin1(kPendingLegacyMigrationKey), error);
    if (ok) ok = commit(database, error);
    if (!ok) rollback(database);
    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
    return ok;
}

void clearLegacyProfileFields(AppConfig *config)
{
    config->upstreamBaseUrl.clear();
    config->upstreamApiKey.clear();
    config->upstreamUserAgent = "curl/8.7.1";
    config->forwardUserAgent = false;
    config->upstreamProxy.clear();
    config->upstreamHttpProxy.clear();
    config->upstreamHttpsProxy.clear();
    config->upstreamSocksProxy.clear();
    config->upstreamTimeoutSec = 1800;
    config->firstTokenTimeoutSec = 30;
}

bool hasMeaningfulLegacySettingsWithoutBaseUrl(const AppConfig &config)
{
    const QString userAgent = config.upstreamUserAgent.trimmed();
    return !config.upstreamApiKey.trimmed().isEmpty() ||
           !config.upstreamProxy.trimmed().isEmpty() ||
           !config.upstreamHttpProxy.trimmed().isEmpty() ||
           !config.upstreamHttpsProxy.trimmed().isEmpty() ||
           !config.upstreamSocksProxy.trimmed().isEmpty() ||
           (!userAgent.isEmpty() && userAgent != "curl/8.7.1") ||
           config.forwardUserAgent || config.upstreamTimeoutSec != 1800 ||
           config.firstTokenTimeoutSec != 30;
}

} // namespace

bool validateUpstreamProxy(const QString &value, QString *error)
{
    clearError(error);
    return validateUpstreamProxyImpl(value, error);
}

bool validateOutboundHeaderValue(const QString &value, QString *error)
{
    return validateOutboundHeaderValueImpl(value, error);
}

UpstreamProfile::UpstreamProfile()
    : userAgent("curl/8.7.1"),
      forwardUserAgent(false),
      upstreamTimeoutSec(1800),
      firstTokenTimeoutSec(30)
{
}

UpstreamProfilePage::UpstreamProfilePage()
    : page(1), pageSize(20), totalItems(0), totalPages(0)
{
}

UpstreamProfileImportResult::UpstreamProfileImportResult()
    : added(0), updated(0), skipped(0)
{
}

QString upstreamProfileDatabasePath(const QString &configPath)
{
    QString resolvedConfigPath = configPath.trimmed();
    if (resolvedConfigPath.isEmpty()) {
        const QString envPath = QString::fromLocal8Bit(qgetenv("NET_TUNNEL_CONFIG")).trimmed();
        if (!envPath.isEmpty()) {
            resolvedConfigPath = envPath;
        } else {
#if defined(Q_OS_WIN)
            QString base = QString::fromLocal8Bit(qgetenv("APPDATA"));
            if (base.isEmpty()) {
                base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
            }
            resolvedConfigPath = QDir(base).filePath("OpenAI Reasoning Guard/config.json");
#elif defined(Q_OS_MAC)
            resolvedConfigPath = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
                                     .filePath("OpenAI Reasoning Guard/config.json");
#else
            resolvedConfigPath = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
                                     .filePath("OpenAI Reasoning Guard/config.json");
#endif
        }
    }
    return QFileInfo(resolvedConfigPath).dir().filePath("upstream-profiles.sqlite3");
}

QString effectiveLegacyUpstreamProxy(const AppConfig &config)
{
    if (!config.upstreamProxy.trimmed().isEmpty()) {
        return normalizedProxy(config.upstreamProxy);
    }
    const bool https = QUrl(config.upstreamBaseUrl).scheme().compare("https", Qt::CaseInsensitive) == 0;
    if (https && !config.upstreamHttpsProxy.trimmed().isEmpty()) {
        return normalizedProxy(config.upstreamHttpsProxy);
    }
    if (!config.upstreamHttpProxy.trimmed().isEmpty()) {
        return normalizedProxy(config.upstreamHttpProxy);
    }
    const QString socks = config.upstreamSocksProxy.trimmed();
    if (socks.isEmpty() || socks.contains("://")) {
        return socks;
    }
    return "socks5://" + socks;
}

bool validateUpstreamProfile(const UpstreamProfile &profile, QString *field, QString *error)
{
    clearError(field);
    clearError(error);
    if (profile.displayName.trimmed().isEmpty()) {
        if (field) *field = "display_name";
        return fail(error, "display name is required");
    }
    if (profile.displayName.toUtf8().size() > 512 || containsControlCharacters(profile.displayName)) {
        if (field) *field = "display_name";
        return fail(error, "display name must not exceed 512 UTF-8 bytes or contain control characters");
    }
    const QString baseText = profile.baseUrl.trimmed();
    const QUrl baseUrl(baseText, QUrl::StrictMode);
    const QString scheme = baseUrl.scheme().toLower();
    if (!baseUrl.isValid() || (scheme != "http" && scheme != "https") || baseUrl.host().isEmpty() ||
        baseUrl.isRelative() || baseUrl.hasQuery() || baseUrl.hasFragment() ||
        !baseUrl.userName().isEmpty() || !baseUrl.password().isEmpty()) {
        if (field) *field = "base_url";
        return fail(error, "base URL must be a complete http or https URL with a host and no query, fragment, or credentials");
    }
    QString headerError;
    if (!validateOutboundHeaderValue(profile.apiKey, &headerError)) {
        if (field) *field = "api_key";
        return fail(error, QString("invalid API key: %1").arg(headerError));
    }
    if (!validateOutboundHeaderValue(profile.userAgent, &headerError)) {
        if (field) *field = "user_agent";
        return fail(error, QString("invalid User-Agent: %1").arg(headerError));
    }
    QString proxyError;
    if (!validateUpstreamProxy(profile.upstreamProxy, &proxyError)) {
        if (field) *field = "upstream_proxy";
        return fail(error, proxyError);
    }
    if (profile.upstreamTimeoutSec < 1 || profile.upstreamTimeoutSec > 86400) {
        if (field) *field = "upstream_timeout_sec";
        return fail(error, "upstream timeout must be between 1 and 86400 seconds");
    }
    if (profile.firstTokenTimeoutSec < 0 || profile.firstTokenTimeoutSec > 3600) {
        if (field) *field = "first_token_timeout_sec";
        return fail(error, "first-token timeout must be between 0 and 3600 seconds");
    }
    return true;
}

class UpstreamProfileStore::Private {
public:
    explicit Private(const QString &databasePath)
        : path(QFileInfo(databasePath).absoluteFilePath()),
          connectionName("upstream_profiles_" + newUuid())
    {
    }

    void closeAndRemove()
    {
        if (database.isValid()) {
            database.close();
            database = QSqlDatabase();
        }
        QSqlDatabase::removeDatabase(connectionName);
    }

    QString path;
    QString connectionName;
    QSqlDatabase database;
};

UpstreamProfileStore::UpstreamProfileStore(const QString &databasePath)
    : d(new Private(databasePath))
{
}

UpstreamProfileStore::~UpstreamProfileStore()
{
    d->closeAndRemove();
}

bool UpstreamProfileStore::open(QString *error)
{
    clearError(error);
    if (isOpen()) {
        return true;
    }
    if (d->path.trimmed().isEmpty()) {
        return fail(error, "upstream profile database path is empty");
    }
    const QFileInfo info(d->path);
    const bool databaseExisted = info.exists();
    QDir directory = info.dir();
    const bool directoryExisted = directory.exists();
    if (!directoryExisted && !directory.mkpath(".")) {
        return fail(error, QString("failed to create database directory: %1").arg(directory.absolutePath()));
    }
#if !defined(Q_OS_WIN)
    if (!directoryExisted && !enforceOwnerOnlyPermissions(directory.absolutePath(), true, error)) {
        return false;
    }
#endif
    if (!QSqlDatabase::isDriverAvailable("QSQLITE")) {
        return fail(error, "QSQLITE driver is not available");
    }
    d->database = QSqlDatabase::addDatabase("QSQLITE", d->connectionName);
    d->database.setDatabaseName(d->path);
    d->database.setConnectOptions(QString("QSQLITE_BUSY_TIMEOUT=%1").arg(kBusyTimeoutMs));
    if (!d->database.open()) {
        const QString message = sqlErrorText("failed to open upstream profile database", d->database);
        d->closeAndRemove();
        return fail(error, message);
    }
    if (!enforceOwnerOnlyPermissions(d->path, false, error)) {
        d->closeAndRemove();
        if (!databaseExisted) QFile::remove(d->path);
        return false;
    }
    if (!execSql(d->database, "PRAGMA foreign_keys=ON", error) ||
        !execSql(d->database, QString("PRAGMA busy_timeout=%1").arg(kBusyTimeoutMs), error)) {
        d->closeAndRemove();
        return false;
    }
    bool walEnabled = false;
    QString walError;
    {
        QSqlQuery query(d->database);
        walEnabled = query.exec("PRAGMA journal_mode=WAL") && query.next() &&
                     query.value(0).toString().compare("wal", Qt::CaseInsensitive) == 0;
        if (!walEnabled && query.lastError().isValid()) walError = query.lastError().text();
    }
    if (!walEnabled) {
        d->closeAndRemove();
        return fail(error, walError.isEmpty() ? "failed to enable SQLite WAL mode"
                                               : QString("failed to enable SQLite WAL mode: %1").arg(walError));
    }
    int version = 0;
    bool versionRead = false;
    QString versionError;
    {
        QSqlQuery query(d->database);
        versionRead = query.exec("PRAGMA user_version") && query.next();
        if (versionRead) version = query.value(0).toInt();
        else versionError = sqlErrorText("failed to read database schema version", query);
    }
    if (!versionRead) {
        d->closeAndRemove();
        return fail(error, versionError);
    }
    if (version > kSchemaVersion) {
        d->closeAndRemove();
        return fail(error, QString("database schema version %1 is newer than supported version %2")
                           .arg(version).arg(kSchemaVersion));
    }
    if (version == 0) {
        if (!beginImmediate(d->database, error)) {
            d->closeAndRemove();
            return false;
        }
        const bool created =
            execSql(d->database,
                    "CREATE TABLE IF NOT EXISTS upstream_profiles ("
                    "id TEXT PRIMARY KEY NOT NULL, "
                    "display_name TEXT NOT NULL, display_name_key TEXT NOT NULL UNIQUE, "
                    "base_url TEXT NOT NULL, api_key TEXT NOT NULL DEFAULT '', "
                    "user_agent TEXT NOT NULL DEFAULT 'curl/8.7.1', "
                    "forward_user_agent INTEGER NOT NULL DEFAULT 0 CHECK(forward_user_agent IN (0,1)), "
                    "upstream_proxy TEXT NOT NULL DEFAULT '', "
                    "upstream_timeout_sec INTEGER NOT NULL DEFAULT 1800 CHECK(upstream_timeout_sec BETWEEN 1 AND 86400), "
                    "first_token_timeout_sec INTEGER NOT NULL DEFAULT 30 CHECK(first_token_timeout_sec BETWEEN 0 AND 3600), "
                    "created_at_utc TEXT NOT NULL, updated_at_utc TEXT NOT NULL)", error) &&
            execSql(d->database,
                    "CREATE TABLE IF NOT EXISTS app_meta (key TEXT PRIMARY KEY NOT NULL, value TEXT NOT NULL)", error) &&
            execSql(d->database,
                    "CREATE INDEX IF NOT EXISTS upstream_profiles_updated_idx "
                    "ON upstream_profiles(updated_at_utc DESC, id ASC)", error) &&
            execSql(d->database, QString("PRAGMA user_version=%1").arg(kSchemaVersion), error);
        if (!created || !commit(d->database, error)) {
            rollback(d->database);
            d->closeAndRemove();
            return false;
        }
    }
    const QStringList sensitiveDatabaseFiles = QStringList()
        << d->path << d->path + "-wal" << d->path + "-shm";
    for (int i = 0; i < sensitiveDatabaseFiles.size(); ++i) {
        const QString path = sensitiveDatabaseFiles.at(i);
        if (QFile::exists(path) && !enforceOwnerOnlyPermissions(path, false, error)) {
            d->closeAndRemove();
            if (!databaseExisted) {
                QFile::remove(d->path);
                QFile::remove(d->path + "-wal");
                QFile::remove(d->path + "-shm");
            }
            return false;
        }
    }
    return true;
}

bool UpstreamProfileStore::isOpen() const
{
    return d->database.isValid() && d->database.isOpen();
}

QString UpstreamProfileStore::databasePath() const
{
    return d->path;
}

bool UpstreamProfileStore::addProfile(UpstreamProfile *profile, QString *error)
{
    clearError(error);
    if (!isOpen()) return fail(error, "upstream profile database is not open");
    if (!profile) return fail(error, "upstream profile is null");
    UpstreamProfile value = *profile;
    normalizeProfile(&value);
    if (!validateUpstreamProfile(value, 0, error)) return false;
    if (value.id.trimmed().isEmpty()) {
        value.id = newUuid();
    } else if (!canonicalUuid(value.id, &value.id)) {
        return fail(error, "upstream profile ID must be a canonical UUID");
    }
    QScopedPointer<QLockFile> selectionLock(newLock(selectionLockPath(d->path)));
    bool maySelect = false;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    value.createdAtUtc = now;
    value.updatedAtUtc = now;
    if (!beginImmediate(d->database, error)) return false;
    QScopedPointer<QLockFile> profileLock(newLock(profileLockPath(d->path, value.id)));
    if (!lockNow(profileLock.data(), "upstream profile", error)) {
        rollback(d->database);
        return false;
    }
    bool wasEmpty = false;
    {
        QSqlQuery count(d->database);
        if (!count.exec("SELECT COUNT(*) FROM upstream_profiles") || !count.next()) {
            rollback(d->database);
            return fail(error, sqlErrorText("failed to count upstream profiles", count));
        }
        wasEmpty = count.value(0).toInt() == 0;
    }
    if (wasEmpty) maySelect = selectionLock->tryLock(0);
    bool ok = bindAndInsert(d->database, value, error);
    if (ok && maySelect && wasEmpty) {
        ok = writeSelectedId(d->database, value.id, error);
    }
    if (!ok || !commit(d->database, error)) {
        rollback(d->database);
        return false;
    }
    *profile = value;
    return true;
}

bool UpstreamProfileStore::updateProfile(const UpstreamProfile &profile, QString *error)
{
    clearError(error);
    if (!isOpen()) return fail(error, "upstream profile database is not open");
    UpstreamProfile value = profile;
    if (!canonicalUuid(value.id, &value.id)) return fail(error, "upstream profile ID must be a canonical UUID");
    normalizeProfile(&value);
    if (!validateUpstreamProfile(value, 0, error)) return false;
    value.updatedAtUtc = QDateTime::currentDateTimeUtc();
    if (!beginImmediate(d->database, error)) return false;
    QScopedPointer<QLockFile> profileLock(newLock(profileLockPath(d->path, value.id)));
    if (!lockNow(profileLock.data(), "upstream profile", error)) {
        rollback(d->database);
        return false;
    }
    if (!bindAndUpdate(d->database, value, error) || !commit(d->database, error)) {
        rollback(d->database);
        return false;
    }
    return true;
}

bool UpstreamProfileStore::removeProfile(const QString &id, QString *error)
{
    clearError(error);
    if (!isOpen()) return fail(error, "upstream profile database is not open");
    QString canonical;
    if (!canonicalUuid(id, &canonical)) return fail(error, "upstream profile ID must be a canonical UUID");
    if (!beginImmediate(d->database, error)) return false;
    QScopedPointer<QLockFile> profileLock(newLock(profileLockPath(d->path, canonical)));
    if (!lockNow(profileLock.data(), "upstream profile", error)) {
        rollback(d->database);
        return false;
    }
    QString selectedError;
    const QString selected = readSelectedId(d->database, &selectedError);
    if (!selectedError.isEmpty()) {
        rollback(d->database);
        return fail(error, selectedError);
    }
    QScopedPointer<QLockFile> selectionLock;
    if (selected == canonical) {
        selectionLock.reset(newLock(selectionLockPath(d->path)));
        if (!lockNow(selectionLock.data(), "upstream profile selection", error)) {
            rollback(d->database);
            return false;
        }
    }
    QStringList orderedIds;
    {
        QSqlQuery list(d->database);
        if (!list.exec("SELECT id FROM upstream_profiles ORDER BY updated_at_utc DESC, id ASC")) {
            rollback(d->database);
            return fail(error, sqlErrorText("failed to enumerate upstream profiles", list));
        }
        while (list.next()) orderedIds.append(list.value(0).toString());
    }
    const int removedIndex = orderedIds.indexOf(canonical);
    if (removedIndex < 0) {
        rollback(d->database);
        return fail(error, "upstream profile does not exist");
    }
    QSqlQuery query(d->database);
    query.prepare("DELETE FROM upstream_profiles WHERE id=?");
    query.addBindValue(canonical);
    if (!query.exec() || query.numRowsAffected() != 1) {
        rollback(d->database);
        return fail(error, query.lastError().isValid()
                    ? sqlErrorText("failed to remove upstream profile", query)
                    : "upstream profile does not exist");
    }
    if (selected == canonical) {
        orderedIds.removeAt(removedIndex);
        QString replacement;
        if (removedIndex < orderedIds.size()) replacement = orderedIds.at(removedIndex);
        else if (!orderedIds.isEmpty()) replacement = orderedIds.last();
        if (!writeSelectedId(d->database, replacement, error)) {
            rollback(d->database);
            return false;
        }
    }
    if (!commit(d->database, error)) {
        rollback(d->database);
        return false;
    }
    return true;
}

bool UpstreamProfileStore::profileById(const QString &id, UpstreamProfile *profile, QString *error) const
{
    clearError(error);
    if (!isOpen()) return fail(error, "upstream profile database is not open");
    if (!profile) return fail(error, "output upstream profile is null");
    QString canonical;
    if (!canonicalUuid(id, &canonical)) return fail(error, "upstream profile ID must be a canonical UUID");
    return queryProfile(d->database, "id", canonical, profile, error);
}

bool UpstreamProfileStore::profileByName(const QString &displayName, UpstreamProfile *profile, QString *error) const
{
    clearError(error);
    if (!isOpen()) return fail(error, "upstream profile database is not open");
    if (!profile) return fail(error, "output upstream profile is null");
    const QString name = displayName.trimmed();
    if (name.isEmpty()) return fail(error, "display name is required");
    return queryProfile(d->database, "display_name_key", profileNameKey(name), profile, error);
}

bool UpstreamProfileStore::listProfiles(const QString &search,
                                        int page,
                                        int pageSize,
                                        UpstreamProfileSortField sortField,
                                        Qt::SortOrder sortOrder,
                                        UpstreamProfilePage *result,
                                        QString *error) const
{
    clearError(error);
    if (!isOpen()) return fail(error, "upstream profile database is not open");
    if (!result) return fail(error, "output page is null");
    if (page < 1) return fail(error, "page must be at least 1");
    if (pageSize < 1 || pageSize > 100) return fail(error, "page size must be between 1 and 100");
    QString sortColumn;
    switch (sortField) {
    case SortByDisplayName: sortColumn = "display_name_key"; break;
    case SortByBaseUrl: sortColumn = "base_url COLLATE NOCASE"; break;
    case SortByUpdatedAt: sortColumn = "updated_at_utc"; break;
    default: return fail(error, "invalid upstream profile sort field");
    }
    if (sortOrder != Qt::AscendingOrder && sortOrder != Qt::DescendingOrder) {
        return fail(error, "invalid upstream profile sort order");
    }
    const QString needle = search.trimmed();
    const QString where = needle.isEmpty()
        ? QString()
        : " WHERE (display_name_key LIKE ? ESCAPE '\\' OR base_url LIKE ? ESCAPE '\\')";
    const qint64 offset = qint64(page - 1) * qint64(pageSize);
    if (offset > 0x7fffffffLL) return fail(error, "profile page offset is too large");
    if (!execSql(d->database, "BEGIN TRANSACTION", error)) return false;
    int total = 0;
    {
        QSqlQuery count(d->database);
        count.prepare("SELECT COUNT(*) FROM upstream_profiles" + where);
        if (!needle.isEmpty()) {
            count.addBindValue(escapedLike(profileNameKey(needle)));
            count.addBindValue(escapedLike(needle));
        }
        if (!count.exec() || !count.next()) {
            rollback(d->database);
            return fail(error, sqlErrorText("failed to count upstream profiles", count));
        }
        total = count.value(0).toInt();
    }
    QSqlQuery query(d->database);
    const QString order = sortOrder == Qt::AscendingOrder ? "ASC" : "DESC";
    query.prepare("SELECT " + profileColumns() + " FROM upstream_profiles" + where +
                  " ORDER BY " + sortColumn + " " + order + ", id ASC LIMIT ? OFFSET ?");
    if (!needle.isEmpty()) {
        query.addBindValue(escapedLike(profileNameKey(needle)));
        query.addBindValue(escapedLike(needle));
    }
    query.addBindValue(pageSize);
    query.addBindValue(int(offset));
    if (!query.exec()) {
        rollback(d->database);
        return fail(error, sqlErrorText("failed to list upstream profiles", query));
    }
    UpstreamProfilePage output;
    output.page = page;
    output.pageSize = pageSize;
    output.totalItems = total;
    output.totalPages = total == 0 ? 0 : (total + pageSize - 1) / pageSize;
    while (query.next()) output.items.append(profileFromQuery(query));
    query.finish();
    if (!commit(d->database, error)) {
        rollback(d->database);
        return false;
    }
    *result = output;
    return true;
}

QString UpstreamProfileStore::selectedProfileId(QString *error) const
{
    clearError(error);
    if (!isOpen()) {
        fail(error, "upstream profile database is not open");
        return QString();
    }
    QString localError;
    QString selected = readSelectedId(d->database, &localError);
    if (!localError.isEmpty()) {
        fail(error, localError);
        return QString();
    }
    UpstreamProfile ignored;
    QString lookupError;
    if (!selected.isEmpty() && queryProfile(d->database, "id", selected, &ignored, &lookupError)) return selected;
    if (!lookupError.isEmpty()) {
        fail(error, lookupError);
        return QString();
    }
    QScopedPointer<QLockFile> selectionLock(newLock(selectionLockPath(d->path)));
    if (!selectionLock->tryLock(0)) return QString();
    QString writeError;
    if (!beginImmediate(d->database, &writeError)) {
        fail(error, writeError);
        return QString();
    }
    selected = readSelectedId(d->database, &writeError);
    if (!writeError.isEmpty()) {
        rollback(d->database);
        fail(error, writeError);
        return QString();
    }
    lookupError.clear();
    if (!selected.isEmpty() && queryProfile(d->database, "id", selected, &ignored, &lookupError)) {
        if (!commit(d->database, &writeError)) {
            rollback(d->database);
            fail(error, writeError);
            return QString();
        }
        return selected;
    }
    if (!lookupError.isEmpty()) {
        rollback(d->database);
        fail(error, lookupError);
        return QString();
    }
    const QString fallback = firstProfileId(d->database, &writeError);
    if (!writeError.isEmpty() || !writeSelectedId(d->database, fallback, &writeError) ||
        !commit(d->database, &writeError)) {
        rollback(d->database);
        fail(error, writeError);
        return QString();
    }
    return fallback;
}

bool UpstreamProfileStore::selectedProfile(UpstreamProfile *profile, QString *error) const
{
    clearError(error);
    if (!profile) return fail(error, "output upstream profile is null");
    const QString id = selectedProfileId(error);
    if (id.isEmpty()) return false;
    return profileById(id, profile, error);
}

bool UpstreamProfileStore::setSelectedProfileId(const QString &id, QString *error)
{
    clearError(error);
    if (!isOpen()) return fail(error, "upstream profile database is not open");
    QString canonical;
    if (!canonicalUuid(id, &canonical)) return fail(error, "upstream profile ID must be a canonical UUID");
    QScopedPointer<QLockFile> selectionLock(newLock(selectionLockPath(d->path)));
    if (!lockNow(selectionLock.data(), "upstream profile selection", error)) return false;
    if (!beginImmediate(d->database, error)) return false;
    UpstreamProfile ignored;
    QString lookupError;
    if (!queryProfile(d->database, "id", canonical, &ignored, &lookupError)) {
        rollback(d->database);
        return fail(error, lookupError.isEmpty() ? "upstream profile does not exist" : lookupError);
    }
    if (!writeSelectedId(d->database, canonical, error) || !commit(d->database, error)) {
        rollback(d->database);
        return false;
    }
    return true;
}

bool UpstreamProfileStore::clearSelectedProfile(QString *error)
{
    clearError(error);
    if (!isOpen()) return fail(error, "upstream profile database is not open");
    QScopedPointer<QLockFile> selectionLock(newLock(selectionLockPath(d->path)));
    if (!lockNow(selectionLock.data(), "upstream profile selection", error)) return false;
    if (!beginImmediate(d->database, error)) return false;
    if (!writeSelectedId(d->database, QString(), error) || !commit(d->database, error)) {
        rollback(d->database);
        return false;
    }
    return true;
}

bool UpstreamProfileStore::exportJson(const QString &path, bool includeSecrets, QString *error) const
{
    clearError(error);
    if (!isOpen()) return fail(error, "upstream profile database is not open");
    const QString outputPath = QFileInfo(path).absoluteFilePath();
    if (path.trimmed().isEmpty()) return fail(error, "export path is empty");
    if (outputPath == QFileInfo(d->path).absoluteFilePath()) {
        return fail(error, "export path must not overwrite the upstream profile database");
    }
    if (!execSql(d->database, "BEGIN TRANSACTION", error)) return false;
    QJsonArray profiles;
    {
        QSqlQuery query(d->database);
        if (!query.exec("SELECT " + profileColumns() +
                        " FROM upstream_profiles ORDER BY display_name_key ASC, id ASC")) {
            rollback(d->database);
            return fail(error, sqlErrorText("failed to export upstream profiles", query));
        }
        while (query.next()) profiles.append(profileToJson(profileFromQuery(query), includeSecrets));
    }
    if (!commit(d->database, error)) {
        rollback(d->database);
        return false;
    }
    QJsonObject root;
    root.insert("schema_version", 1);
    root.insert("exported_at_utc", dateTimeText(QDateTime::currentDateTimeUtc()));
    root.insert("secrets_included", includeSecrets);
    root.insert("profiles", profiles);
    const QFileInfo outputInfo(outputPath);
    QDir outputDirectory = outputInfo.dir();
    if (!outputDirectory.exists() && !outputDirectory.mkpath(".")) {
        return fail(error, QString("failed to create export directory: %1").arg(outputDirectory.absolutePath()));
    }
    return writeOwnerOnlyFile(outputPath, QJsonDocument(root).toJson(QJsonDocument::Indented), error);
}

namespace {

struct ImportedProfile {
    UpstreamProfile profile;
    bool apiKeyPresent;
};

bool parseImportedProfile(const QJsonValue &value, ImportedProfile *result, QString *error)
{
    if (!value.isObject()) return fail(error, "each imported profile must be an object");
    const QJsonObject object = value.toObject();
    ImportedProfile parsed;
    if (!jsonString(object, "display_name", QString(), true, &parsed.profile.displayName, error) ||
        !jsonString(object, "base_url", QString(), true, &parsed.profile.baseUrl, error) ||
        !jsonString(object, "user_agent", parsed.profile.userAgent, false, &parsed.profile.userAgent, error) ||
        !jsonString(object, "upstream_proxy", QString(), false, &parsed.profile.upstreamProxy, error) ||
        !jsonInteger(object, "upstream_timeout_sec", parsed.profile.upstreamTimeoutSec,
                     &parsed.profile.upstreamTimeoutSec, error) ||
        !jsonInteger(object, "first_token_timeout_sec", parsed.profile.firstTokenTimeoutSec,
                     &parsed.profile.firstTokenTimeoutSec, error)) {
        return false;
    }
    if (object.contains("id")) {
        QString rawId;
        if (!jsonString(object, "id", QString(), true, &rawId, error) ||
            !canonicalUuid(rawId, &parsed.profile.id)) {
            return fail(error, "imported profile ID must be a canonical UUID");
        }
    } else {
        parsed.profile.id = newUuid();
    }
    parsed.apiKeyPresent = object.contains("api_key");
    if (parsed.apiKeyPresent &&
        !jsonString(object, "api_key", QString(), true, &parsed.profile.apiKey, error)) {
        return false;
    }
    if (object.contains("forward_user_agent")) {
        if (!object.value("forward_user_agent").isBool()) {
            return fail(error, "forward_user_agent must be a boolean");
        }
        parsed.profile.forwardUserAgent = object.value("forward_user_agent").toBool();
    }
    const QDateTime now = QDateTime::currentDateTimeUtc();
    parsed.profile.createdAtUtc = now;
    parsed.profile.updatedAtUtc = now;
    const char *dateKeys[] = {"created_at_utc", "updated_at_utc"};
    for (int i = 0; i < 2; ++i) {
        const QString key = QString::fromLatin1(dateKeys[i]);
        if (!object.contains(key)) continue;
        if (!object.value(key).isString()) return fail(error, key + " must be a string");
        const QDateTime date = parseDateTime(object.value(key).toString());
        if (!date.isValid()) return fail(error, key + " must be an ISO-8601 date-time");
        if (i == 0) parsed.profile.createdAtUtc = date;
        else parsed.profile.updatedAtUtc = date;
    }
    normalizeProfile(&parsed.profile);
    if (!validateUpstreamProfile(parsed.profile, 0, error)) return false;
    *result = parsed;
    return true;
}

void releaseLocks(QList<QLockFile *> *locks)
{
    while (!locks->isEmpty()) delete locks->takeLast();
}

} // namespace

bool UpstreamProfileStore::importJson(const QString &path,
                                      UpstreamProfileImportConflictPolicy conflictPolicy,
                                      UpstreamProfileImportResult *result,
                                      QString *error)
{
    clearError(error);
    if (result) *result = UpstreamProfileImportResult();
    if (!isOpen()) return fail(error, "upstream profile database is not open");
    if (!result) return fail(error, "output import result is null");
    if (conflictPolicy != SkipImportConflicts && conflictPolicy != OverwriteImportConflicts) {
        return fail(error, "invalid import conflict policy");
    }
    const QString inputPath = QFileInfo(path).absoluteFilePath();
    if (path.trimmed().isEmpty()) return fail(error, "import path is empty");
    if (inputPath == QFileInfo(d->path).absoluteFilePath()) {
        return fail(error, "import path must not be the upstream profile database");
    }
    QFile file(inputPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(error, QString("failed to open import file: %1").arg(file.errorString()));
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return fail(error, QString("invalid upstream profile import JSON: %1").arg(parseError.errorString()));
    }
    const QJsonObject root = document.object();
    int schemaVersion = 0;
    if (!jsonInteger(root, "schema_version", 0, &schemaVersion, error) || schemaVersion != 1) {
        if (!error || error->isEmpty()) fail(error, "unsupported upstream profile import schema version");
        return false;
    }
    if (!root.value("profiles").isArray()) return fail(error, "profiles must be an array");
    QList<ImportedProfile> imported;
    QSet<QString> inputIds;
    QSet<QString> inputNames;
    const QJsonArray array = root.value("profiles").toArray();
    for (int i = 0; i < array.size(); ++i) {
        ImportedProfile parsed;
        QString itemError;
        if (!parseImportedProfile(array.at(i), &parsed, &itemError)) {
            return fail(error, QString("invalid profile at index %1: %2").arg(i).arg(itemError));
        }
        const QString nameKey = profileNameKey(parsed.profile.displayName);
        if (inputIds.contains(parsed.profile.id) || inputNames.contains(nameKey)) {
            return fail(error, QString("duplicate profile ID or display name at index %1").arg(i));
        }
        inputIds.insert(parsed.profile.id);
        inputNames.insert(nameKey);
        imported.append(parsed);
    }
    if (!beginImmediate(d->database, error)) return false;
    bool wasEmpty = false;
    {
        QSqlQuery count(d->database);
        if (!count.exec("SELECT COUNT(*) FROM upstream_profiles") || !count.next()) {
            rollback(d->database);
            return fail(error, sqlErrorText("failed to count upstream profiles", count));
        }
        wasEmpty = count.value(0).toInt() == 0;
    }
    QScopedPointer<QLockFile> selectionLock(newLock(selectionLockPath(d->path)));
    const bool maySelectFirst = wasEmpty && selectionLock->tryLock(0);
    QList<QLockFile *> acquiredLocks;
    UpstreamProfileImportResult output;
    QString firstAddedId;
    for (int i = 0; i < imported.size(); ++i) {
        ImportedProfile entry = imported.at(i);
        UpstreamProfile byId;
        UpstreamProfile byName;
        QString lookupError;
        const bool foundById = queryProfile(d->database, "id", entry.profile.id, &byId, &lookupError);
        if (!lookupError.isEmpty()) {
            rollback(d->database); releaseLocks(&acquiredLocks);
            return fail(error, lookupError);
        }
        const bool foundByName = queryProfile(d->database, "display_name_key",
                                              profileNameKey(entry.profile.displayName), &byName, &lookupError);
        if (!lookupError.isEmpty()) {
            rollback(d->database); releaseLocks(&acquiredLocks);
            return fail(error, lookupError);
        }
        if (foundById && foundByName && byId.id != byName.id) {
            rollback(d->database); releaseLocks(&acquiredLocks);
            return fail(error, QString("imported profile at index %1 matches different records by ID and name").arg(i));
        }
        const bool conflict = foundById || foundByName;
        if (conflict && conflictPolicy == SkipImportConflicts) {
            ++output.skipped;
            continue;
        }
        const UpstreamProfile target = foundById ? byId : byName;
        const QString targetId = conflict ? target.id : entry.profile.id;
        QLockFile *profileLock = newLock(profileLockPath(d->path, targetId));
        if (!lockNow(profileLock, "upstream profile", error)) {
            delete profileLock;
            rollback(d->database); releaseLocks(&acquiredLocks);
            return false;
        }
        acquiredLocks.append(profileLock);
        if (conflict) {
            entry.profile.id = target.id;
            entry.profile.createdAtUtc = target.createdAtUtc;
            if (!entry.apiKeyPresent) entry.profile.apiKey = target.apiKey;
            if (!bindAndUpdate(d->database, entry.profile, error)) {
                rollback(d->database); releaseLocks(&acquiredLocks);
                return false;
            }
            ++output.updated;
        } else {
            if (!entry.apiKeyPresent) entry.profile.apiKey = QString("");
            if (!bindAndInsert(d->database, entry.profile, error)) {
                rollback(d->database); releaseLocks(&acquiredLocks);
                return false;
            }
            if (firstAddedId.isEmpty()) firstAddedId = entry.profile.id;
            ++output.added;
        }
    }
    if (maySelectFirst && !firstAddedId.isEmpty() &&
        !writeSelectedId(d->database, firstAddedId, error)) {
        rollback(d->database); releaseLocks(&acquiredLocks);
        return false;
    }
    if (!commit(d->database, error)) {
        rollback(d->database); releaseLocks(&acquiredLocks);
        return false;
    }
    releaseLocks(&acquiredLocks);
    *result = output;
    return true;
}

bool UpstreamProfileStore::isProfileLocked(const QString &id) const
{
    QString canonical;
    if (!canonicalUuid(id, &canonical)) return false;
    QScopedPointer<QLockFile> lock(newLock(profileLockPath(d->path, canonical)));
    if (!lock->tryLock(0)) return true;
    lock->unlock();
    return false;
}

bool UpstreamProfileStore::isSelectionLocked() const
{
    QScopedPointer<QLockFile> lock(newLock(selectionLockPath(d->path)));
    if (!lock->tryLock(0)) return true;
    lock->unlock();
    return false;
}

UpstreamProfileRunLock::UpstreamProfileRunLock(const QString &databasePath)
    : databasePath_(QFileInfo(databasePath).absoluteFilePath())
{
}

UpstreamProfileRunLock::~UpstreamProfileRunLock()
{
    unlock();
}

bool UpstreamProfileRunLock::tryLock(const QString &profileId, QString *error)
{
    clearError(error);
    if (isLocked()) return fail(error, "upstream profile run lock is already held");
    QString canonical;
    if (!profileId.trimmed().isEmpty() && !canonicalUuid(profileId, &canonical)) {
        return fail(error, "upstream profile ID must be a canonical UUID");
    }
    selectionLock_.reset(newLock(selectionLockPath(databasePath_)));
    if (!lockNow(selectionLock_.data(), "upstream profile selection", error)) {
        selectionLock_.reset();
        return false;
    }
    if (!canonical.isEmpty()) {
        profileLock_.reset(newLock(profileLockPath(databasePath_, canonical)));
        if (!lockNow(profileLock_.data(), "upstream profile", error)) {
            profileLock_.reset();
            selectionLock_->unlock();
            selectionLock_.reset();
            return false;
        }
    }
    profileId_ = canonical;
    return true;
}

void UpstreamProfileRunLock::unlock()
{
    if (profileLock_) {
        profileLock_->unlock();
        profileLock_.reset();
    }
    if (selectionLock_) {
        selectionLock_->unlock();
        selectionLock_.reset();
    }
    profileId_.clear();
}

bool UpstreamProfileRunLock::isLocked() const
{
    return !selectionLock_.isNull() && selectionLock_->isLocked();
}

QString UpstreamProfileRunLock::profileId() const
{
    return profileId_;
}

bool migrateLegacyUpstreamConfig(const QString &configPath,
                                 AppConfig *config,
                                 UpstreamProfileStore *store,
                                 bool *migrated,
                                 QString *error)
{
    clearError(error);
    if (migrated) *migrated = false;
    if (!config) return fail(error, "application config is null");
    if (!store) return fail(error, "upstream profile store is null");
    if (!store->isOpen()) return fail(error, "upstream profile database is not open");
    const QString resolvedPath = configPath.trimmed().isEmpty() ? defaultConfigPath() : configPath;
    QFile source(resolvedPath);
    if (!source.exists()) return true;
    if (!source.open(QIODevice::ReadOnly)) {
        return fail(error, QString("failed to open legacy config: %1").arg(source.errorString()));
    }
    const QByteArray originalBytes = source.readAll();
    if (source.error() != QFileDevice::NoError) {
        return fail(error, QString("failed to read legacy config: %1").arg(source.errorString()));
    }
    source.close();
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(originalBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return fail(error, QString("invalid legacy config JSON: %1").arg(parseError.errorString()));
    }
    const QJsonObject configObject = document.object();
    *config = appConfigFromJsonObject(configObject);
    const QJsonObject legacyValues = legacyUpstreamValues(configObject);
    const QJsonObject stripped = withoutLegacyUpstreamValues(configObject);
    const QString legacyFingerprint = legacyUpstreamFingerprint(configObject);
    const QString backupPath = resolvedPath + ".pre-upstream-profiles.bak";
    UpstreamProfilePage existing;
    if (!store->listProfiles(QString(), 1, 1, SortByUpdatedAt, Qt::DescendingOrder, &existing, error)) return false;
    if (existing.totalItems != 0) {
        QString marker;
        if (!readPendingMigrationMarker(store->databasePath(), &marker, error)) return false;
        if (marker.isEmpty()) {
            if (legacyValues.isEmpty()) return true;
            return fail(error,
                        "legacy upstream settings remain, but the profile database is not empty; "
                        "remove the legacy fields only after importing or confirming them manually");
        }
        QScopedPointer<QLockFile> recoverySelectionLock(newLock(selectionLockPath(store->databasePath())));
        if (!lockNow(recoverySelectionLock.data(), "upstream profile selection", error)) return false;
        if (!readPendingMigrationMarker(store->databasePath(), &marker, error)) return false;
        if (marker.isEmpty()) {
            if (legacyValues.isEmpty()) return true;
            return fail(error,
                        "pending legacy upstream migration changed while recovery was starting; "
                        "the legacy fields were preserved");
        }
        QString pendingProfileId;
        QString pendingFingerprint;
        if (!parsePendingMigrationMarker(marker, &pendingProfileId, &pendingFingerprint)) {
            return fail(error, "pending legacy upstream migration marker is invalid");
        }
        QScopedPointer<QLockFile> recoveryProfileLock(
            newLock(profileLockPath(store->databasePath(), pendingProfileId)));
        if (!lockNow(recoveryProfileLock.data(), "upstream profile", error)) return false;
        UpstreamProfile pendingProfile;
        QString lookupError;
        const bool pendingProfileExists = store->profileById(pendingProfileId, &pendingProfile, &lookupError);
        if (!lookupError.isEmpty()) return fail(error, lookupError);
        const bool canFinish = pendingProfileExists && !legacyValues.isEmpty() &&
                               pendingFingerprint == legacyFingerprint;
        if (canFinish) {
            if (!QFile::exists(backupPath) && !writeOwnerOnlyFile(backupPath, originalBytes, error)) return false;
            if (!writeOwnerOnlyFile(resolvedPath, QJsonDocument(stripped).toJson(QJsonDocument::Indented), error)) {
                return false;
            }
        }
        if (!clearPendingMigrationMarker(store->databasePath(), error)) return false;
        if (canFinish) {
            clearLegacyProfileFields(config);
            if (migrated) *migrated = true;
        } else if (!legacyValues.isEmpty()) {
            return fail(error,
                        "pending legacy upstream migration no longer matches the JSON; "
                        "the legacy fields were preserved for manual recovery");
        }
        return true;
    }
    if (config->upstreamBaseUrl.trimmed().isEmpty()) {
        if (legacyValues.isEmpty()) return true;
        if (hasMeaningfulLegacySettingsWithoutBaseUrl(*config)) {
            return fail(error,
                        "legacy upstream settings contain credentials or non-default values but no Base URL; "
                        "the JSON was preserved and must be corrected manually");
        }
        if (!QFile::exists(backupPath) && !writeOwnerOnlyFile(backupPath, originalBytes, error)) return false;
        if (!writeOwnerOnlyFile(resolvedPath,
                                QJsonDocument(stripped).toJson(QJsonDocument::Indented),
                                error)) {
            return false;
        }
        clearLegacyProfileFields(config);
        if (migrated) *migrated = true;
        return true;
    }

    UpstreamProfile profile;
    profile.displayName = QString::fromUtf8("已迁移配置");
    profile.baseUrl = config->upstreamBaseUrl;
    profile.apiKey = config->upstreamApiKey;
    profile.userAgent = config->upstreamUserAgent;
    profile.forwardUserAgent = config->forwardUserAgent;
    profile.upstreamProxy = effectiveLegacyUpstreamProxy(*config);
    profile.upstreamTimeoutSec = config->upstreamTimeoutSec;
    profile.firstTokenTimeoutSec = config->firstTokenTimeoutSec;
    normalizeProfile(&profile);
    if (!validateUpstreamProfile(profile, 0, error)) return false;

    if (!writeOwnerOnlyFile(backupPath, originalBytes, error)) return false;

    profile.id = newUuid();
    const QDateTime now = QDateTime::currentDateTimeUtc();
    profile.createdAtUtc = now;
    profile.updatedAtUtc = now;

    QScopedPointer<QLockFile> selectionLock(newLock(selectionLockPath(store->databasePath())));
    if (!lockNow(selectionLock.data(), "upstream profile selection", error)) return false;
    QScopedPointer<QLockFile> profileLock(newLock(profileLockPath(store->databasePath(), profile.id)));
    if (!lockNow(profileLock.data(), "upstream profile", error)) return false;

    const QString migrationConnectionName = "upstream_profile_migration_" + newUuid();
    QSqlDatabase migrationDatabase = QSqlDatabase::addDatabase("QSQLITE", migrationConnectionName);
    migrationDatabase.setDatabaseName(store->databasePath());
    migrationDatabase.setConnectOptions(QString("QSQLITE_BUSY_TIMEOUT=%1").arg(kBusyTimeoutMs));
    if (!migrationDatabase.open()) {
        const QString message = sqlErrorText("failed to open database for legacy migration", migrationDatabase);
        migrationDatabase = QSqlDatabase();
        QSqlDatabase::removeDatabase(migrationConnectionName);
        return fail(error, message);
    }
    if (!beginImmediate(migrationDatabase, error)) {
        migrationDatabase.close();
        migrationDatabase = QSqlDatabase();
        QSqlDatabase::removeDatabase(migrationConnectionName);
        return false;
    }
    bool countOk = false;
    bool databaseStillEmpty = false;
    QString countError;
    {
        QSqlQuery count(migrationDatabase);
        countOk = count.exec("SELECT COUNT(*) FROM upstream_profiles") && count.next();
        if (countOk) databaseStillEmpty = count.value(0).toInt() == 0;
        else countError = sqlErrorText("failed to recheck upstream profiles before migration", count);
    }
    if (!countOk || !databaseStillEmpty) {
        rollback(migrationDatabase);
        migrationDatabase.close();
        migrationDatabase = QSqlDatabase();
        QSqlDatabase::removeDatabase(migrationConnectionName);
        return countOk
            ? fail(error,
                   "the profile database became non-empty while legacy settings were being migrated; "
                   "the legacy fields were preserved")
            : fail(error, countError);
    }
    if (!bindAndInsert(migrationDatabase, profile, error) ||
        !writeSelectedId(migrationDatabase, profile.id, error) ||
        !writeMetaValue(migrationDatabase,
                        QString::fromLatin1(kPendingLegacyMigrationKey),
                        pendingMigrationMarker(profile.id, legacyFingerprint),
                        error)) {
        rollback(migrationDatabase);
        migrationDatabase.close();
        migrationDatabase = QSqlDatabase();
        QSqlDatabase::removeDatabase(migrationConnectionName);
        return false;
    }
    if (!commit(migrationDatabase, error)) {
        rollback(migrationDatabase);
        migrationDatabase.close();
        migrationDatabase = QSqlDatabase();
        QSqlDatabase::removeDatabase(migrationConnectionName);
        return false;
    }

    if (!writeOwnerOnlyFile(resolvedPath, QJsonDocument(stripped).toJson(QJsonDocument::Indented), error)) {
        const QString writeError = error ? *error : QString("failed to rewrite legacy config");
        QString compensationError;
        if (beginImmediate(migrationDatabase, &compensationError)) {
            QSqlQuery remove(migrationDatabase);
            remove.prepare("DELETE FROM upstream_profiles WHERE id=?");
            remove.addBindValue(profile.id);
            if (!remove.exec()) compensationError = sqlErrorText("failed to roll back migrated profile", remove);
            if (compensationError.isEmpty()) {
                QSqlQuery clearSelection(migrationDatabase);
                clearSelection.prepare("DELETE FROM app_meta WHERE key=? AND value=?");
                clearSelection.addBindValue(QString::fromLatin1(kSelectedProfileKey));
                clearSelection.addBindValue(profile.id);
                if (!clearSelection.exec()) {
                    compensationError = sqlErrorText("failed to roll back migrated selection", clearSelection);
                }
            }
            if (compensationError.isEmpty() &&
                !deleteMetaValue(migrationDatabase,
                                 QString::fromLatin1(kPendingLegacyMigrationKey),
                                 &compensationError)) {
                // deleteMetaValue provides the diagnostic.
            }
            if (compensationError.isEmpty() && !commit(migrationDatabase, &compensationError)) {
                rollback(migrationDatabase);
            } else if (!compensationError.isEmpty()) {
                rollback(migrationDatabase);
            }
        }
        migrationDatabase.close();
        migrationDatabase = QSqlDatabase();
        QSqlDatabase::removeDatabase(migrationConnectionName);
        if (error) {
            *error = writeError;
            if (!compensationError.isEmpty()) {
                *error += QString("; additionally failed to roll back database migration: %1")
                          .arg(compensationError);
            }
        }
        return false;
    }
    QString markerError;
    if (!beginImmediate(migrationDatabase, &markerError) ||
        !deleteMetaValue(migrationDatabase, QString::fromLatin1(kPendingLegacyMigrationKey), &markerError) ||
        !commit(migrationDatabase, &markerError)) {
        rollback(migrationDatabase);
        migrationDatabase.close();
        migrationDatabase = QSqlDatabase();
        QSqlDatabase::removeDatabase(migrationConnectionName);
        return fail(error, markerError);
    }
    migrationDatabase.close();
    migrationDatabase = QSqlDatabase();
    QSqlDatabase::removeDatabase(migrationConnectionName);

    clearLegacyProfileFields(config);
    if (migrated) *migrated = true;
    return true;
}

} // namespace net_tunnel
