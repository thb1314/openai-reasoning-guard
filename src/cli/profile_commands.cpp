#include "cli/profile_commands.h"

#include "core/app_config.h"
#include "core/upstream_profile.h"

#include <QtCore/QCommandLineOption>
#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSet>
#include <QtCore/QTextStream>
#include <QtCore/QUuid>

namespace net_tunnel {
namespace {

int profileTokenIndex(const QStringList &arguments)
{
    const QSet<QString> valueOptions = QSet<QString>()
        << "c" << "config"
        << "proxy-host" << "proxy-port" << "proxy-prefix"
        << "upstream-base-url" << "upstream-api-key" << "upstream-user-agent"
        << "upstream-proxy" << "upstream-http-proxy" << "upstream-https-proxy" << "upstream-socks-proxy"
        << "upstream-timeout" << "first-token-timeout" << "upstream-first-byte-timeout"
        << "retry-after-override-sec"
        << "buffer-timeout" << "request-body-limit-bytes" << "response-buffer-limit-bytes"
        << "reasoning-equals" << "intercept-rule-mode" << "guard-retry-attempts"
        << "reasoning-516-retries" << "guard-endpoints" << "non-stream-status-code"
        << "stream-action" << "query-url"
        << "page" << "page-size" << "search" << "sort" << "order" << "name"
        << "base-url" << "api-key" << "user-agent" << "proxy"
        << "input" << "output" << "conflict";
    const QSet<QString> flagOptions = QSet<QString>()
        << "h" << "help" << "v" << "version" << "json" << "api-proxy"
        << "forward-user-agent" << "no-forward-user-agent"
        << "retry-upstream-capacity-errors" << "no-retry-upstream-capacity-errors"
        << "no-intercept-streaming" << "no-intercept-non-streaming"
        << "status-json" << "query-status" << "keep-config"
        << "show-secret" << "include-secrets";

    for (int i = 1; i < arguments.size(); ++i) {
        const QString argument = arguments.at(i);
        if (argument == "--") {
            return i + 1 < arguments.size() && arguments.at(i + 1) == "profile" ? i + 1 : -1;
        }
        if (!argument.startsWith('-') || argument == "-") {
            return argument == "profile" ? i : -1;
        }

        const int prefixLength = argument.startsWith("--") ? 2 : 1;
        const QString optionText = argument.mid(prefixLength);
        const int equalsIndex = optionText.indexOf('=');
        const QString optionName = equalsIndex >= 0 ? optionText.left(equalsIndex) : optionText;
        if (prefixLength == 1 && equalsIndex < 0 && optionText.size() > 1) {
            const QString shortName = optionText.left(1);
            if (valueOptions.contains(shortName)) {
                continue;
            }
            bool compactFlags = true;
            for (int j = 0; j < optionText.size(); ++j) {
                if (!flagOptions.contains(optionText.mid(j, 1))) {
                    compactFlags = false;
                    break;
                }
            }
            if (compactFlags) {
                continue;
            }
        }
        if (flagOptions.contains(optionName)) {
            continue;
        }
        if (!valueOptions.contains(optionName)) {
            return -1;
        }
        if (equalsIndex < 0) {
            ++i;
            if (i >= arguments.size()) {
                return -1;
            }
        }
    }
    return -1;
}

int fail(const QString &message)
{
    QTextStream stream(stderr);
    stream << "profile command failed: " << message << "\n";
    stream.flush();
    return 2;
}

void writeJson(const QJsonValue &value)
{
    QJsonDocument document;
    if (value.isArray()) {
        document = QJsonDocument(value.toArray());
    } else {
        document = QJsonDocument(value.toObject());
    }
    QTextStream stream(stdout);
    stream << document.toJson(QJsonDocument::Indented);
    stream.flush();
}

void writeLine(const QString &text)
{
    QTextStream stream(stdout);
    stream << text << "\n";
    stream.flush();
}

QString isoDate(const QDateTime &dateTime)
{
    return dateTime.isValid() ? dateTime.toUTC().toString(Qt::ISODateWithMs) : QString();
}

QJsonObject profileJson(const UpstreamProfile &profile, bool includeSecret)
{
    QJsonObject object;
    object.insert("id", profile.id);
    object.insert("display_name", profile.displayName);
    object.insert("base_url", profile.baseUrl);
    object.insert("api_key_configured", !profile.apiKey.isEmpty());
    object.insert("authorization_mode", profile.apiKey.isEmpty()
        ? QString("forward_client_authorization")
        : QString("profile_api_key"));
    if (includeSecret) {
        object.insert("api_key", profile.apiKey);
    }
    object.insert("user_agent", profile.userAgent);
    object.insert("forward_user_agent", profile.forwardUserAgent);
    object.insert("upstream_proxy", profile.upstreamProxy);
    object.insert("upstream_timeout_sec", profile.upstreamTimeoutSec);
    object.insert("first_token_timeout_sec", profile.firstTokenTimeoutSec);
    object.insert("retry_after_override_sec", profile.retryAfterOverrideSec);
    object.insert("created_at_utc", isoDate(profile.createdAtUtc));
    object.insert("updated_at_utc", isoDate(profile.updatedAtUtc));
    return object;
}

void writeProfile(const UpstreamProfile &profile, bool includeSecret)
{
    QTextStream stream(stdout);
    stream << "ID: " << profile.id << "\n"
           << "Name: " << profile.displayName << "\n"
           << "Base URL: " << profile.baseUrl << "\n";
    if (includeSecret) {
        stream << "API key: " << profile.apiKey << "\n";
    } else {
        stream << "API key: "
               << (profile.apiKey.isEmpty() ? "forward client authorization" : "configured")
               << "\n";
    }
    stream << "User-Agent: " << profile.userAgent << "\n"
           << "Forward client User-Agent: " << (profile.forwardUserAgent ? "true" : "false") << "\n"
           << "Upstream proxy: " << (profile.upstreamProxy.isEmpty() ? "direct" : profile.upstreamProxy) << "\n"
           << "Upstream timeout: " << profile.upstreamTimeoutSec << " s\n"
           << "First-token timeout: " << profile.firstTokenTimeoutSec << " s\n"
           << "Retry-After override: "
           << (profile.retryAfterOverrideSec.isEmpty() ? "disabled (pass through upstream)" : profile.retryAfterOverrideSec + " s") << "\n"
           << "Created: " << isoDate(profile.createdAtUtc) << "\n"
           << "Updated: " << isoDate(profile.updatedAtUtc) << "\n";
    stream.flush();
}

bool parseIntegerOption(const QCommandLineParser &parser,
                        const QCommandLineOption &option,
                        int minimum,
                        int *value,
                        QString *error)
{
    bool ok = false;
    const int parsed = parser.value(option).toInt(&ok);
    if (!ok || parsed < minimum) {
        if (error) {
            *error = QString("--%1 must be an integer greater than or equal to %2")
                .arg(option.names().first()).arg(minimum);
        }
        return false;
    }
    *value = parsed;
    return true;
}

bool parseRetryAfterOverrideOption(const QCommandLineParser &parser,
                                   const QCommandLineOption &option,
                                   QString *value,
                                   QString *error)
{
    const QString text = parser.value(option).trimmed();
    if (text.isEmpty()) {
        value->clear();
        return true;
    }
    int seconds = 0;
    if (!parseIntegerOption(parser, option, 1, &seconds, error) || seconds > 86400) {
        if (error) {
            *error = QString("--%1 must be empty or an integer between 1 and 86400 seconds")
                .arg(option.names().first());
        }
        return false;
    }
    *value = QString::number(seconds);
    return true;
}

bool findProfile(UpstreamProfileStore *store,
                 const QString &nameOrId,
                 UpstreamProfile *profile,
                 QString *error)
{
    const QString target = nameOrId.trimmed();
    if (target.isEmpty()) {
        if (error) {
            *error = "a profile name or UUID is required";
        }
        return false;
    }

    QString idError;
    const QUuid uuid(target);
    if (!uuid.isNull()) {
        const QString canonicalId = uuid.toString(QUuid::WithoutBraces);
        if (store->profileById(canonicalId, profile, &idError)) {
            return true;
        }
    }
    QString nameError;
    if (store->profileByName(target, profile, &nameError)) {
        return true;
    }
    if (error) {
        *error = !nameError.isEmpty() ? nameError
                                     : (!idError.isEmpty() ? idError : QString("profile not found: %1").arg(target));
    }
    return false;
}

UpstreamProfileSortField parseSortField(const QString &text, bool *ok)
{
    const QString normalized = text.trimmed().toLower();
    *ok = true;
    if (normalized.isEmpty() || normalized == "updated" || normalized == "updated-at") {
        return SortByUpdatedAt;
    }
    if (normalized == "name" || normalized == "display-name") {
        return SortByDisplayName;
    }
    if (normalized == "url" || normalized == "base-url") {
        return SortByBaseUrl;
    }
    *ok = false;
    return SortByUpdatedAt;
}

QStringList commandArguments(const QStringList &arguments)
{
    QStringList result = arguments;
    const int index = profileTokenIndex(result);
    if (index >= 0) {
        result.removeAt(index);
    }
    return result;
}

} // namespace

bool isProfileCommand(const QStringList &arguments)
{
    return profileTokenIndex(arguments) >= 0;
}

int runProfileCommand(const QStringList &arguments)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Manage saved upstream profiles");
    const QCommandLineOption helpOption = parser.addHelpOption();
    const QCommandLineOption versionOption = parser.addVersionOption();

    QCommandLineOption configOption(QStringList() << "c" << "config", "Path to config.json", "path");
    QCommandLineOption jsonOption("json", "Write machine-readable JSON");
    QCommandLineOption pageOption("page", "One-based result page", "number", "1");
    QCommandLineOption pageSizeOption("page-size", "Results per page: 10, 20, 50, or 100", "number", "20");
    QCommandLineOption searchOption("search", "Search display name and Base URL", "text");
    QCommandLineOption sortOption("sort", "Sort by name, base-url, or updated-at", "field", "updated-at");
    QCommandLineOption orderOption("order", "Sort order: asc or desc", "order", "desc");
    QCommandLineOption nameOption("name", "Profile display name", "name");
    QCommandLineOption baseUrlOption(QStringList() << "base-url" << "upstream-base-url", "Upstream Base URL", "url");
    QCommandLineOption apiKeyOption(QStringList() << "api-key" << "upstream-api-key", "Upstream API key; an empty value forwards client authorization", "key");
    QCommandLineOption userAgentOption(QStringList() << "user-agent" << "upstream-user-agent", "User-Agent sent upstream", "value");
    QCommandLineOption forwardUserAgentOption("forward-user-agent", "Forward the client User-Agent");
    QCommandLineOption noForwardUserAgentOption("no-forward-user-agent", "Do not forward the client User-Agent");
    QCommandLineOption proxyOption(QStringList() << "proxy" << "upstream-proxy", "Proxy used for upstream requests; an empty value means direct", "url");
    QCommandLineOption upstreamTimeoutOption("upstream-timeout", "Upstream timeout in seconds", "seconds");
    QCommandLineOption firstTokenTimeoutOption("first-token-timeout", "First-token timeout in seconds; 0 disables it", "seconds");
    QCommandLineOption retryAfterOverrideOption("retry-after-override-sec", "Override Retry-After for final upstream HTTP 429/502/503; empty disables", "seconds");
    QCommandLineOption showSecretOption("show-secret", "Show the complete API key (show only)");
    QCommandLineOption inputOption("input", "JSON file to import", "path");
    QCommandLineOption outputOption("output", "JSON file to export", "path");
    QCommandLineOption conflictOption("conflict", "Import conflict policy: skip or overwrite", "policy");
    QCommandLineOption includeSecretsOption("include-secrets", "Include plaintext API keys in an export");

    parser.addOptions(QList<QCommandLineOption>()
        << configOption << jsonOption
        << pageOption << pageSizeOption << searchOption << sortOption << orderOption
        << nameOption << baseUrlOption << apiKeyOption << userAgentOption
        << forwardUserAgentOption << noForwardUserAgentOption << proxyOption
        << upstreamTimeoutOption << firstTokenTimeoutOption << retryAfterOverrideOption << showSecretOption
        << inputOption << outputOption << conflictOption << includeSecretsOption);
    parser.addPositionalArgument("command", "list, show, add, update, delete, select, import, or export");
    parser.addPositionalArgument("profile", "Profile display name or UUID for show, update, delete, and select", "[profile]");

    if (!parser.parse(commandArguments(arguments))) {
        return fail(parser.errorText());
    }
    if (parser.isSet(helpOption)) {
        QTextStream(stdout) << parser.helpText();
        return 0;
    }
    if (parser.isSet(versionOption)) {
        writeLine(QString("%1 %2")
            .arg(QCoreApplication::applicationName(), QCoreApplication::applicationVersion()));
        return 0;
    }

    const QStringList positional = parser.positionalArguments();
    if (positional.isEmpty()) {
        return fail("missing command; use 'profile --help' for usage");
    }
    if (positional.size() > 2) {
        return fail("too many positional arguments");
    }
    const QString command = positional.first().trimmed().toLower();
    const QSet<QString> supported = QSet<QString>()
        << "list" << "show" << "add" << "update" << "delete" << "select" << "import" << "export";
    if (!supported.contains(command)) {
        return fail(QString("unknown command: %1").arg(command));
    }
    const QString target = positional.size() > 1 ? positional.at(1) : QString();
    const bool json = parser.isSet(jsonOption);

    QSet<QString> allowedOptionNames;
    if (command == "list") {
        allowedOptionNames << "page" << "page-size" << "search" << "sort" << "order";
    } else if (command == "show") {
        allowedOptionNames << "show-secret";
    } else if (command == "add" || command == "update") {
        allowedOptionNames << "name" << "base-url" << "api-key" << "user-agent"
                           << "forward-user-agent" << "no-forward-user-agent" << "proxy"
                           << "upstream-timeout" << "first-token-timeout" << "retry-after-override-sec";
    } else if (command == "import") {
        allowedOptionNames << "input" << "conflict";
    } else if (command == "export") {
        allowedOptionNames << "output" << "include-secrets";
    }
    const QList<const QCommandLineOption *> profileSpecificOptions =
        QList<const QCommandLineOption *>()
        << &pageOption << &pageSizeOption << &searchOption << &sortOption << &orderOption
        << &nameOption << &baseUrlOption << &apiKeyOption << &userAgentOption
        << &forwardUserAgentOption << &noForwardUserAgentOption << &proxyOption
        << &upstreamTimeoutOption << &firstTokenTimeoutOption << &retryAfterOverrideOption << &showSecretOption
        << &inputOption << &outputOption << &conflictOption << &includeSecretsOption;
    for (int i = 0; i < profileSpecificOptions.size(); ++i) {
        const QCommandLineOption &option = *profileSpecificOptions.at(i);
        const QString canonicalName = option.names().first();
        if (parser.isSet(option) && !allowedOptionNames.contains(canonicalName)) {
            return fail(QString("--%1 is not valid for 'profile %2'")
                .arg(canonicalName, command));
        }
    }

    const QString configPath = parser.isSet(configOption) ? parser.value(configOption) : defaultConfigPath();
    AppConfig config = loadConfig(configPath);
    UpstreamProfileStore store(upstreamProfileDatabasePath(configPath));
    QString error;
    if (!store.open(&error)) {
        return fail(error);
    }
    if (!migrateLegacyUpstreamConfig(configPath, &config, &store, 0, &error)) {
        return fail(QString("legacy configuration migration failed: %1").arg(error));
    }

    if (command == "list") {
        if (!target.isEmpty()) {
            return fail("list does not accept a positional profile");
        }
        int page = 0;
        int pageSize = 0;
        if (!parseIntegerOption(parser, pageOption, 1, &page, &error)
            || !parseIntegerOption(parser, pageSizeOption, 1, &pageSize, &error)) {
            return fail(error);
        }
        if (pageSize != 10 && pageSize != 20 && pageSize != 50 && pageSize != 100) {
            return fail("--page-size must be one of 10, 20, 50, or 100");
        }
        bool sortOk = false;
        const UpstreamProfileSortField sortField = parseSortField(parser.value(sortOption), &sortOk);
        if (!sortOk) {
            return fail("--sort must be name, base-url, or updated-at");
        }
        const QString orderText = parser.value(orderOption).trimmed().toLower();
        if (orderText != "asc" && orderText != "desc") {
            return fail("--order must be asc or desc");
        }
        UpstreamProfilePage result;
        if (!store.listProfiles(parser.value(searchOption), page, pageSize, sortField,
                                orderText == "asc" ? Qt::AscendingOrder : Qt::DescendingOrder,
                                &result, &error)) {
            return fail(error);
        }
        QString selectionError;
        const QString selectedId = store.selectedProfileId(&selectionError);
        if (!selectionError.isEmpty()) {
            return fail(selectionError);
        }
        if (json) {
            QJsonArray items;
            for (int i = 0; i < result.items.size(); ++i) {
                QJsonObject item = profileJson(result.items.at(i), false);
                item.insert("selected", result.items.at(i).id == selectedId);
                items.append(item);
            }
            QJsonObject output;
            output.insert("items", items);
            output.insert("page", result.page);
            output.insert("page_size", result.pageSize);
            output.insert("total_items", result.totalItems);
            output.insert("total_pages", result.totalPages);
            writeJson(output);
        } else {
            QTextStream stream(stdout);
            stream << "CURRENT\tNAME\tBASE URL\tAUTHORIZATION\tUPDATED\tID\n";
            for (int i = 0; i < result.items.size(); ++i) {
                const UpstreamProfile &profile = result.items.at(i);
                stream << (profile.id == selectedId ? "*" : "") << "\t"
                       << profile.displayName << "\t"
                       << profile.baseUrl << "\t"
                       << (profile.apiKey.isEmpty() ? "forward-client" : "configured") << "\t"
                       << isoDate(profile.updatedAtUtc) << "\t"
                       << profile.id << "\n";
            }
            stream << "Page " << result.page << "/" << result.totalPages
                   << ", " << result.totalItems << " profile(s)\n";
            stream.flush();
        }
        return 0;
    }

    if (command == "show") {
        UpstreamProfile profile;
        if (!findProfile(&store, target, &profile, &error)) {
            return fail(error);
        }
        const bool showSecret = parser.isSet(showSecretOption);
        if (json) {
            writeJson(profileJson(profile, showSecret));
        } else {
            writeProfile(profile, showSecret);
        }
        return 0;
    }

    if (command == "add") {
        if (!target.isEmpty()) {
            return fail("add does not accept a positional profile; use --name");
        }
        if (parser.isSet(forwardUserAgentOption) && parser.isSet(noForwardUserAgentOption)) {
            return fail("--forward-user-agent and --no-forward-user-agent are mutually exclusive");
        }
        UpstreamProfile profile;
        profile.displayName = parser.value(nameOption);
        profile.baseUrl = parser.value(baseUrlOption);
        profile.apiKey = parser.isSet(apiKeyOption) ? parser.value(apiKeyOption) : QString();
        profile.userAgent = parser.isSet(userAgentOption) ? parser.value(userAgentOption) : QString("curl/8.7.1");
        profile.forwardUserAgent = parser.isSet(forwardUserAgentOption);
        profile.upstreamProxy = parser.isSet(proxyOption) ? parser.value(proxyOption) : QString();
        profile.upstreamTimeoutSec = 1800;
        profile.firstTokenTimeoutSec = 30;
        profile.retryAfterOverrideSec.clear();
        if (parser.isSet(upstreamTimeoutOption)
            && !parseIntegerOption(parser, upstreamTimeoutOption, 1, &profile.upstreamTimeoutSec, &error)) {
            return fail(error);
        }
        if (parser.isSet(firstTokenTimeoutOption)
            && !parseIntegerOption(parser, firstTokenTimeoutOption, 0, &profile.firstTokenTimeoutSec, &error)) {
            return fail(error);
        }
        if (parser.isSet(retryAfterOverrideOption)
            && !parseRetryAfterOverrideOption(parser, retryAfterOverrideOption, &profile.retryAfterOverrideSec, &error)) {
            return fail(error);
        }
        if (!store.addProfile(&profile, &error)) {
            return fail(error);
        }
        if (json) {
            QJsonObject output;
            output.insert("ok", true);
            output.insert("profile", profileJson(profile, false));
            writeJson(output);
        } else {
            writeLine(QString("Added profile '%1' (%2).").arg(profile.displayName, profile.id));
        }
        return 0;
    }

    if (command == "update") {
        if (parser.isSet(forwardUserAgentOption) && parser.isSet(noForwardUserAgentOption)) {
            return fail("--forward-user-agent and --no-forward-user-agent are mutually exclusive");
        }
        const bool hasChanges = parser.isSet(nameOption) || parser.isSet(baseUrlOption)
            || parser.isSet(apiKeyOption) || parser.isSet(userAgentOption)
            || parser.isSet(forwardUserAgentOption) || parser.isSet(noForwardUserAgentOption)
            || parser.isSet(proxyOption) || parser.isSet(upstreamTimeoutOption)
            || parser.isSet(firstTokenTimeoutOption) || parser.isSet(retryAfterOverrideOption);
        if (!hasChanges) {
            return fail("update requires at least one profile field option");
        }
        UpstreamProfile profile;
        if (!findProfile(&store, target, &profile, &error)) {
            return fail(error);
        }
        if (parser.isSet(nameOption)) {
            profile.displayName = parser.value(nameOption);
        }
        if (parser.isSet(baseUrlOption)) {
            profile.baseUrl = parser.value(baseUrlOption);
        }
        if (parser.isSet(apiKeyOption)) {
            profile.apiKey = parser.value(apiKeyOption);
        }
        if (parser.isSet(userAgentOption)) {
            profile.userAgent = parser.value(userAgentOption);
        }
        if (parser.isSet(forwardUserAgentOption)) {
            profile.forwardUserAgent = true;
        } else if (parser.isSet(noForwardUserAgentOption)) {
            profile.forwardUserAgent = false;
        }
        if (parser.isSet(proxyOption)) {
            profile.upstreamProxy = parser.value(proxyOption);
        }
        if (parser.isSet(upstreamTimeoutOption)
            && !parseIntegerOption(parser, upstreamTimeoutOption, 1, &profile.upstreamTimeoutSec, &error)) {
            return fail(error);
        }
        if (parser.isSet(firstTokenTimeoutOption)
            && !parseIntegerOption(parser, firstTokenTimeoutOption, 0, &profile.firstTokenTimeoutSec, &error)) {
            return fail(error);
        }
        if (parser.isSet(retryAfterOverrideOption)
            && !parseRetryAfterOverrideOption(parser, retryAfterOverrideOption, &profile.retryAfterOverrideSec, &error)) {
            return fail(error);
        }
        if (!store.updateProfile(profile, &error)) {
            return fail(error);
        }
        if (!store.profileById(profile.id, &profile, &error)) {
            return fail(QString("profile was updated but could not be reloaded: %1").arg(error));
        }
        if (json) {
            QJsonObject output;
            output.insert("ok", true);
            output.insert("profile", profileJson(profile, false));
            writeJson(output);
        } else {
            writeLine(QString("Updated profile '%1' (%2).").arg(profile.displayName, profile.id));
        }
        return 0;
    }

    if (command == "delete") {
        UpstreamProfile profile;
        if (!findProfile(&store, target, &profile, &error)) {
            return fail(error);
        }
        if (!store.removeProfile(profile.id, &error)) {
            return fail(error);
        }
        if (json) {
            QJsonObject output;
            output.insert("ok", true);
            output.insert("deleted_id", profile.id);
            writeJson(output);
        } else {
            writeLine(QString("Deleted profile '%1' (%2).").arg(profile.displayName, profile.id));
        }
        return 0;
    }

    if (command == "select") {
        UpstreamProfile profile;
        if (!findProfile(&store, target, &profile, &error)) {
            return fail(error);
        }
        if (!store.setSelectedProfileId(profile.id, &error)) {
            return fail(error);
        }
        if (json) {
            QJsonObject output;
            output.insert("ok", true);
            output.insert("selected_id", profile.id);
            writeJson(output);
        } else {
            writeLine(QString("Selected profile '%1' (%2).").arg(profile.displayName, profile.id));
        }
        return 0;
    }

    if (command == "export") {
        if (!target.isEmpty()) {
            return fail("export does not accept a positional profile");
        }
        if (!parser.isSet(outputOption) || parser.value(outputOption).trimmed().isEmpty()) {
            return fail("--output is required");
        }
        if (!store.exportJson(parser.value(outputOption), parser.isSet(includeSecretsOption), &error)) {
            return fail(error);
        }
        if (json) {
            QJsonObject output;
            output.insert("ok", true);
            output.insert("path", parser.value(outputOption));
            output.insert("included_secrets", parser.isSet(includeSecretsOption));
            writeJson(output);
        } else {
            writeLine(QString("Exported profiles to %1.").arg(parser.value(outputOption)));
        }
        return 0;
    }

    if (!target.isEmpty()) {
        return fail("import does not accept a positional profile");
    }
    if (!parser.isSet(inputOption) || parser.value(inputOption).trimmed().isEmpty()) {
        return fail("--input is required");
    }
    if (!parser.isSet(conflictOption)) {
        return fail("--conflict skip|overwrite is required");
    }
    const QString conflict = parser.value(conflictOption).trimmed().toLower();
    if (conflict != "skip" && conflict != "overwrite") {
        return fail("--conflict must be skip or overwrite");
    }
    UpstreamProfileImportResult result;
    if (!store.importJson(parser.value(inputOption),
                          conflict == "skip" ? SkipImportConflicts : OverwriteImportConflicts,
                          &result, &error)) {
        return fail(error);
    }
    if (json) {
        QJsonObject output;
        output.insert("ok", true);
        output.insert("added", result.added);
        output.insert("updated", result.updated);
        output.insert("skipped", result.skipped);
        writeJson(output);
    } else {
        writeLine(QString("Imported profiles: %1 added, %2 updated, %3 skipped.")
            .arg(result.added).arg(result.updated).arg(result.skipped));
    }
    return 0;
}

} // namespace net_tunnel
