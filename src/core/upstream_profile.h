#pragma once

#include "core/app_config.h"

#include <QtCore/QDateTime>
#include <QtCore/QList>
#include <QtCore/QScopedPointer>
#include <QtCore/QString>

class QLockFile;

namespace net_tunnel {

struct UpstreamProfile {
    QString id;
    QString displayName;
    QString baseUrl;
    QString apiKey;
    QString userAgent;
    bool forwardUserAgent;
    QString upstreamProxy;
    int upstreamTimeoutSec;
    int firstTokenTimeoutSec;
    QDateTime createdAtUtc;
    QDateTime updatedAtUtc;

    UpstreamProfile();
};

enum UpstreamProfileSortField {
    SortByDisplayName,
    SortByBaseUrl,
    SortByUpdatedAt
};

struct UpstreamProfilePage {
    QList<UpstreamProfile> items;
    int page;
    int pageSize;
    int totalItems;
    int totalPages;

    UpstreamProfilePage();
};

enum UpstreamProfileImportConflictPolicy {
    SkipImportConflicts,
    OverwriteImportConflicts
};

struct UpstreamProfileImportResult {
    int added;
    int updated;
    int skipped;

    UpstreamProfileImportResult();
};

QString upstreamProfileDatabasePath(const QString &configPath = QString());
QString effectiveLegacyUpstreamProxy(const AppConfig &config);
bool validateUpstreamProxy(const QString &value, QString *error = 0);
bool validateOutboundHeaderValue(const QString &value, QString *error = 0);
bool validateUpstreamProfile(const UpstreamProfile &profile,
                             QString *field = 0,
                             QString *error = 0);

class UpstreamProfileStore {
public:
    explicit UpstreamProfileStore(const QString &databasePath);
    ~UpstreamProfileStore();

    bool open(QString *error = 0);
    bool isOpen() const;
    QString databasePath() const;

    bool addProfile(UpstreamProfile *profile, QString *error = 0);
    bool updateProfile(const UpstreamProfile &profile, QString *error = 0);
    bool removeProfile(const QString &id, QString *error = 0);
    bool profileById(const QString &id, UpstreamProfile *profile, QString *error = 0) const;
    bool profileByName(const QString &displayName, UpstreamProfile *profile, QString *error = 0) const;
    bool listProfiles(const QString &search,
                      int page,
                      int pageSize,
                      UpstreamProfileSortField sortField,
                      Qt::SortOrder sortOrder,
                      UpstreamProfilePage *result,
                      QString *error = 0) const;

    QString selectedProfileId(QString *error = 0) const;
    bool selectedProfile(UpstreamProfile *profile, QString *error = 0) const;
    bool setSelectedProfileId(const QString &id, QString *error = 0);
    bool clearSelectedProfile(QString *error = 0);

    bool exportJson(const QString &path, bool includeSecrets, QString *error = 0) const;
    bool importJson(const QString &path,
                    UpstreamProfileImportConflictPolicy conflictPolicy,
                    UpstreamProfileImportResult *result,
                    QString *error = 0);

    bool isProfileLocked(const QString &id) const;
    bool isSelectionLocked() const;

private:
    class Private;
    QScopedPointer<Private> d;
    Q_DISABLE_COPY(UpstreamProfileStore)
};

class UpstreamProfileRunLock {
public:
    explicit UpstreamProfileRunLock(const QString &databasePath);
    ~UpstreamProfileRunLock();

    bool tryLock(const QString &profileId, QString *error = 0);
    void unlock();
    bool isLocked() const;
    QString profileId() const;

private:
    QString databasePath_;
    QString profileId_;
    QScopedPointer<QLockFile> selectionLock_;
    QScopedPointer<QLockFile> profileLock_;
    Q_DISABLE_COPY(UpstreamProfileRunLock)
};

bool migrateLegacyUpstreamConfig(const QString &configPath,
                                 AppConfig *config,
                                 UpstreamProfileStore *store,
                                 bool *migrated = 0,
                                 QString *error = 0);

} // namespace net_tunnel
