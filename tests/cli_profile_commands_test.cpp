#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QTemporaryDir>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtTest/QTest>

namespace {

struct ProcessResult {
    int exitCode;
    QProcess::ExitStatus exitStatus;
    QByteArray standardOutput;
    QByteArray standardError;
};

ProcessResult runProcess(const QString &program,
                         const QStringList &arguments,
                         int timeoutMs = 15000)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(program, arguments);
    ProcessResult result;
    if (!process.waitForStarted(timeoutMs) || !process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished();
    }
    result.exitCode = process.exitCode();
    result.exitStatus = process.exitStatus();
    result.standardOutput = process.readAllStandardOutput();
    result.standardError = process.readAllStandardError();
    return result;
}

QJsonObject parseObject(const QByteArray &json)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return QJsonObject();
    }
    return document.object();
}

bool containsObjectKey(const QJsonValue &value, const QString &key)
{
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        if (object.contains(key)) {
            return true;
        }
        for (QJsonObject::ConstIterator it = object.constBegin(); it != object.constEnd(); ++it) {
            if (containsObjectKey(it.value(), key)) {
                return true;
            }
        }
    } else if (value.isArray()) {
        const QJsonArray array = value.toArray();
        for (int i = 0; i < array.size(); ++i) {
            if (containsObjectKey(array.at(i), key)) {
                return true;
            }
        }
    }
    return false;
}

bool waitForStandardOutput(QProcess *process, const QByteArray &needle, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    QByteArray output;
    while (timer.elapsed() < timeoutMs) {
        output += process->readAllStandardOutput();
        if (output.contains(needle)) {
            return true;
        }
        if (process->state() == QProcess::NotRunning) {
            return false;
        }
        process->waitForReadyRead(qMin(100, timeoutMs - int(timer.elapsed())));
    }
    output += process->readAllStandardOutput();
    return output.contains(needle);
}

QString configPath(const QTemporaryDir &directory)
{
    return directory.filePath("config.json");
}

quint16 availablePort()
{
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        return 0;
    }
    const quint16 port = server.serverPort();
    server.close();
    return port;
}

} // namespace

class CliProfileCommandsTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void crudPaginationAndSecretMasking();
    void rejectsOptionsFromOtherProfileSubcommands();
    void rejectsUnexpectedProxyPositionals();
    void compactConfigOptionDispatchesProfileCommand();
    void importExportPreservesOmittedSecret();
    void runningProxyLocksCurrentProfileAndSelection();
    void adHocRunAllowsAddingFirstProfileWithoutSelecting();
    void explicitBaseUrlStartsWithoutSelectedProfile();
    void selectedProfileAndTemporaryOverridesReachRuntime();
    void temporaryTimeoutOverridesAreStrictlyValidated();

private:
    ProcessResult run(const QStringList &arguments, int timeoutMs = 15000) const;
    QString cliPath_;
};

void CliProfileCommandsTest::initTestCase()
{
    cliPath_ = QString::fromLocal8Bit(qgetenv("NET_TUNNEL_CLI_TEST_BINARY")).trimmed();
    if (cliPath_.isEmpty()) {
        cliPath_ = QDir(QCoreApplication::applicationDirPath()).filePath(
#ifdef Q_OS_WIN
            "net-tunnel-cli.exe"
#else
            "net-tunnel-cli"
#endif
        );
    }
    QVERIFY2(QFile::exists(cliPath_), qPrintable(QString("CLI executable not found: %1").arg(cliPath_)));
}

ProcessResult CliProfileCommandsTest::run(const QStringList &arguments, int timeoutMs) const
{
    return runProcess(cliPath_, arguments, timeoutMs);
}

void CliProfileCommandsTest::crudPaginationAndSecretMasking()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = configPath(directory);

    ProcessResult result = run(QStringList()
        << "--config" << path << "profile" << "add"
        << "--name" << "Primary"
        << "--upstream-base-url" << "https://example.com/v1/"
        << "--upstream-api-key" << "  sk-test-secret  "
        << "--upstream-proxy" << "http://127.0.0.1:7890"
        << "--forward-user-agent" << "--json");
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 0);
    QVERIFY(!result.standardOutput.contains("sk-test-secret"));
    QJsonObject added = parseObject(result.standardOutput).value("profile").toObject();
    QVERIFY(!added.value("id").toString().isEmpty());
    QCOMPARE(added.value("display_name").toString(), QString("Primary"));
    QCOMPARE(added.value("api_key_configured").toBool(), true);

    result = run(QStringList()
        << "--json" << "--config" << path << "profile" << "list"
        << "--page" << "1" << "--page-size" << "10"
        << "--sort" << "name" << "--order" << "asc");
    QCOMPARE(result.exitCode, 0);
    QVERIFY(!result.standardOutput.contains("sk-test-secret"));
    const QJsonObject list = parseObject(result.standardOutput);
    QCOMPARE(list.value("total_items").toInt(), 1);
    QCOMPARE(list.value("items").toArray().at(0).toObject().value("selected").toBool(), true);

    result = run(QStringList() << "profile" << "show" << "Primary"
                               << "--config" << path << "--json");
    QCOMPARE(result.exitCode, 0);
    QVERIFY(!result.standardOutput.contains("sk-test-secret"));
    QVERIFY(!parseObject(result.standardOutput).contains("api_key"));

    result = run(QStringList() << "profile" << "show" << "Primary"
                               << "--config" << path << "--show-secret" << "--json");
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(parseObject(result.standardOutput).value("api_key").toString(), QString("sk-test-secret"));

    result = run(QStringList() << "profile" << "update" << "Primary"
                               << "--config" << path
                               << "--upstream-api-key" << "   " << "--upstream-proxy="
                               << "--no-forward-user-agent" << "--json");
    QCOMPARE(result.exitCode, 0);
    const QJsonObject updated = parseObject(result.standardOutput).value("profile").toObject();
    QCOMPARE(updated.value("api_key_configured").toBool(), false);
    QCOMPARE(updated.value("upstream_proxy").toString(), QString());
    QCOMPARE(updated.value("forward_user_agent").toBool(), false);

    result = run(QStringList() << "profile" << "add" << "--config" << path
                               << "--name" << "Secondary"
                               << "--base-url" << "https://secondary.example/v1");
    QCOMPARE(result.exitCode, 0);
    result = run(QStringList() << "profile" << "select" << "Secondary" << "--config" << path);
    QCOMPARE(result.exitCode, 0);
    result = run(QStringList() << "profile" << "delete" << "Secondary" << "--config" << path);
    QCOMPARE(result.exitCode, 0);

    result = run(QStringList() << "profile" << "list" << "--config" << path
                               << "--page-size" << "11");
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QVERIFY(result.exitCode != 0);
    QVERIFY(result.standardOutput.isEmpty());

    result = run(QStringList() << "profile" << "list" << "unexpected" << "--config" << path);
    QVERIFY(result.exitCode != 0);
    result = run(QStringList() << "profile" << "update" << "Primary" << "--config" << path);
    QVERIFY(result.exitCode != 0);
}

void CliProfileCommandsTest::rejectsOptionsFromOtherProfileSubcommands()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = configPath(directory);
    const QString exportPath = directory.filePath("should-not-exist.json");

    ProcessResult result = run(QStringList()
        << "profile" << "add" << "--config" << path
        << "--name" << "Existing" << "--base-url" << "https://example.com/v1"
        << "--api-key" << "sk-original");
    QCOMPARE(result.exitCode, 0);

    const QList<QStringList> invalidCommands = QList<QStringList>()
        << (QStringList() << "profile" << "add"
            << "--name" << "Rejected Add" << "--base-url" << "https://add.example/v1"
            << "--show-secret")
        << (QStringList() << "profile" << "show" << "Existing"
            << "--api-key" << "sk-ignored")
        << (QStringList() << "profile" << "update" << "Existing"
            << "--output" << exportPath)
        << (QStringList() << "profile" << "delete" << "Existing"
            << "--api-key" << "sk-ignored")
        << (QStringList() << "profile" << "select" << "Existing"
            << "--name" << "ignored")
        << (QStringList() << "profile" << "import"
            << "--input" << directory.filePath("missing.json") << "--conflict" << "skip"
            << "--include-secrets")
        << (QStringList() << "profile" << "export"
            << "--output" << exportPath << "--conflict" << "skip");
    const QStringList rejectedOptions = QStringList()
        << "--show-secret" << "--api-key" << "--output" << "--api-key"
        << "--name" << "--include-secrets" << "--conflict";
    const QStringList subcommands = QStringList()
        << "add" << "show" << "update" << "delete" << "select" << "import" << "export";
    QCOMPARE(invalidCommands.size(), rejectedOptions.size());
    QCOMPARE(invalidCommands.size(), subcommands.size());
    for (int i = 0; i < invalidCommands.size(); ++i) {
        QStringList arguments = invalidCommands.at(i);
        arguments << "--config" << path << "--json";
        result = run(arguments);
        QCOMPARE(result.exitStatus, QProcess::NormalExit);
        QCOMPARE(result.exitCode, 2);
        QVERIFY2(result.standardError.contains(rejectedOptions.at(i).toUtf8()),
                 result.standardError.constData());
        QVERIFY2(result.standardError.contains(QString("profile %1").arg(subcommands.at(i)).toUtf8()),
                 result.standardError.constData());
        QVERIFY(result.standardOutput.isEmpty());
    }

    result = run(QStringList() << "profile" << "show" << "Rejected Add"
                               << "--config" << path << "--json");
    QCOMPARE(result.exitCode, 2);
    result = run(QStringList() << "profile" << "show" << "Existing"
                               << "--config" << path << "--show-secret" << "--json");
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(parseObject(result.standardOutput).value("api_key").toString(), QString("sk-original"));
    QVERIFY(!QFile::exists(exportPath));
}

void CliProfileCommandsTest::rejectsUnexpectedProxyPositionals()
{
    const QList<QStringList> invalidArguments = QList<QStringList>()
        << (QStringList() << "profil" << "list")
        << (QStringList() << "Profile" << "list")
        << (QStringList() << "unexpected");
    for (int i = 0; i < invalidArguments.size(); ++i) {
        const ProcessResult result = run(invalidArguments.at(i));
        QCOMPARE(result.exitStatus, QProcess::NormalExit);
        QCOMPARE(result.exitCode, 2);
        QVERIFY2(result.standardError.contains("unexpected positional arguments"),
                 result.standardError.constData());
        QVERIFY(result.standardOutput.isEmpty());
    }
}

void CliProfileCommandsTest::compactConfigOptionDispatchesProfileCommand()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = configPath(directory);
    ProcessResult result = run(QStringList()
        << "profile" << "add" << "--config" << path
        << "--name" << "Compact" << "--base-url" << "https://example.com/v1");
    QCOMPARE(result.exitCode, 0);

    result = run(QStringList() << (QString("-c") + path) << "profile" << "list" << "--json");
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 0);
    const QJsonObject output = parseObject(result.standardOutput);
    QCOMPARE(output.value("total_items").toInt(), 1);
    QCOMPARE(output.value("items").toArray().first().toObject().value("display_name").toString(),
             QString("Compact"));
}

void CliProfileCommandsTest::importExportPreservesOmittedSecret()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = configPath(directory);
    const QString publicExport = directory.filePath("profiles-public.json");
    const QString secretExport = directory.filePath("profiles-secret.json");

    ProcessResult result = run(QStringList() << "profile" << "add" << "--config" << path
        << "--name" << "Importable" << "--base-url" << "https://example.com/v1"
        << "--api-key" << "sk-original");
    QCOMPARE(result.exitCode, 0);

    result = run(QStringList() << "profile" << "export" << "--config" << path
                               << "--output" << publicExport);
    QCOMPARE(result.exitCode, 0);
    QFile publicFile(publicExport);
    QVERIFY(publicFile.open(QIODevice::ReadOnly));
    const QByteArray publicJson = publicFile.readAll();
    QVERIFY(!publicJson.contains("sk-original"));
    const QJsonDocument publicDocument = QJsonDocument::fromJson(publicJson);
    QVERIFY(!publicDocument.isNull());
    QVERIFY(!containsObjectKey(publicDocument.isObject()
        ? QJsonValue(publicDocument.object()) : QJsonValue(publicDocument.array()), "api_key"));

    result = run(QStringList() << "profile" << "export" << "--config" << path
                               << "--output" << secretExport << "--include-secrets");
    QCOMPARE(result.exitCode, 0);
    QFile secretFile(secretExport);
    QVERIFY(secretFile.open(QIODevice::ReadOnly));
    QVERIFY(secretFile.readAll().contains("sk-original"));

    result = run(QStringList() << "profile" << "update" << "Importable" << "--config" << path
                               << "--api-key" << "sk-replacement");
    QCOMPARE(result.exitCode, 0);
    result = run(QStringList() << "profile" << "import" << "--config" << path
                               << "--input" << publicExport << "--conflict" << "overwrite");
    QCOMPARE(result.exitCode, 0);
    result = run(QStringList() << "profile" << "show" << "Importable" << "--config" << path
                               << "--show-secret" << "--json");
    QCOMPARE(result.exitCode, 0);
    QCOMPARE(parseObject(result.standardOutput).value("api_key").toString(), QString("sk-replacement"));

    result = run(QStringList() << "profile" << "import" << "--config" << path
                               << "--input" << publicExport);
    QVERIFY(result.exitCode != 0);
    QVERIFY(result.standardOutput.isEmpty());
}

void CliProfileCommandsTest::runningProxyLocksCurrentProfileAndSelection()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = configPath(directory);

    ProcessResult result = run(QStringList() << "profile" << "add" << "--config" << path
        << "--name" << "Running" << "--base-url" << "http://127.0.0.1:9/v1");
    QCOMPARE(result.exitCode, 0);

    const quint16 port = availablePort();
    QVERIFY(port > 0);

    QProcess proxy;
    proxy.setProcessChannelMode(QProcess::SeparateChannels);
    proxy.start(cliPath_, QStringList() << "--config" << path
                                       << "--proxy-port" << QString::number(port));
    QVERIFY(proxy.waitForStarted(5000));
    QVERIFY2(waitForStandardOutput(&proxy, "listening", 5000),
             qPrintable(QString::fromLocal8Bit(proxy.readAllStandardError())));
    QCOMPARE(proxy.state(), QProcess::Running);

    result = run(QStringList() << "profile" << "update" << "Running" << "--config" << path
                               << "--name" << "Changed");
    QVERIFY(result.exitCode != 0);
    result = run(QStringList() << "profile" << "delete" << "Running" << "--config" << path);
    QVERIFY(result.exitCode != 0);
    result = run(QStringList() << "profile" << "select" << "Running" << "--config" << path);
    QVERIFY(result.exitCode != 0);

    result = run(QStringList() << "profile" << "add" << "--config" << path
                               << "--name" << "Other"
                               << "--base-url" << "https://other.example/v1");
    QCOMPARE(result.exitCode, 0);
    result = run(QStringList() << "profile" << "update" << "Other" << "--config" << path
                               << "--upstream-proxy" << "http://127.0.0.1:7890");
    QCOMPARE(result.exitCode, 0);

    proxy.terminate();
    if (!proxy.waitForFinished(5000)) {
        proxy.kill();
        proxy.waitForFinished();
    }
}

void CliProfileCommandsTest::adHocRunAllowsAddingFirstProfileWithoutSelecting()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = configPath(directory);
    const quint16 port = availablePort();
    QVERIFY(port > 0);

    QProcess proxy;
    proxy.setProcessChannelMode(QProcess::SeparateChannels);
    proxy.start(cliPath_, QStringList() << "--config" << path
        << "--proxy-port" << QString::number(port)
        << "--upstream-base-url" << "https://temporary.example/v1");
    QVERIFY(proxy.waitForStarted(5000));
    QVERIFY2(waitForStandardOutput(&proxy, "listening", 5000),
             qPrintable(QString::fromLocal8Bit(proxy.readAllStandardError())));
    QCOMPARE(proxy.state(), QProcess::Running);

    ProcessResult result = run(QStringList() << "profile" << "add" << "--config" << path
        << "--name" << "First Saved" << "--base-url" << "https://saved.example/v1"
        << "--json");
    QCOMPARE(result.exitCode, 0);
    result = run(QStringList() << "profile" << "list" << "--config" << path << "--json");
    QCOMPARE(result.exitCode, 0);
    const QJsonArray items = parseObject(result.standardOutput).value("items").toArray();
    QCOMPARE(items.size(), 1);
    QCOMPARE(items.at(0).toObject().value("selected").toBool(), false);
    result = run(QStringList() << "profile" << "select" << "First Saved" << "--config" << path);
    QVERIFY(result.exitCode != 0);

    proxy.terminate();
    if (!proxy.waitForFinished(5000)) {
        proxy.kill();
        proxy.waitForFinished();
    }
}

void CliProfileCommandsTest::explicitBaseUrlStartsWithoutSelectedProfile()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = configPath(directory);
    const quint16 port = availablePort();
    QVERIFY(port > 0);

    ProcessResult result = run(QStringList() << "--config" << path
                                             << "--proxy-port" << QString::number(port)
                                             << "--status-json");
    QVERIFY(result.exitCode != 0);
    QVERIFY(result.standardOutput.isEmpty());
    QVERIFY(result.standardError.contains("no upstream profile"));

    result = run(QStringList() << "--config" << path
                               << "--proxy-port" << QString::number(port)
                               << "--upstream-base-url" << "https://example.com/v1"
                               << "--upstream-api-key" << "sk-temporary"
                               << "--status-json" << "--keep-config");
    QCOMPARE(result.exitStatus, QProcess::NormalExit);
    QCOMPARE(result.exitCode, 0);
    QVERIFY(parseObject(result.standardOutput).size() > 0);

    QFile config(path);
    QVERIFY(config.open(QIODevice::ReadOnly));
    const QByteArray savedConfig = config.readAll();
    QVERIFY(!savedConfig.contains("sk-temporary"));
    QVERIFY(!savedConfig.contains("upstream_base_url"));
}

void CliProfileCommandsTest::selectedProfileAndTemporaryOverridesReachRuntime()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = configPath(directory);

    ProcessResult result = run(QStringList() << "profile" << "add" << "--config" << path
        << "--name" << "Runtime"
        << "--base-url" << "https://profile.example/v1"
        << "--api-key" << "sk-runtime-secret"
        << "--user-agent" << "profile-agent/1.0"
        << "--forward-user-agent"
        << "--upstream-proxy" << "http://127.0.0.1:7890"
        << "--upstream-timeout" << "321"
        << "--first-token-timeout" << "17"
        << "--retry-after-override-sec" << "30");
    QCOMPARE(result.exitCode, 0);

    quint16 port = availablePort();
    QVERIFY(port > 0);
    result = run(QStringList() << "--config" << path
        << "--proxy-port" << QString::number(port) << "--status-json");
    QCOMPARE(result.exitCode, 0);
    QVERIFY(!result.standardOutput.contains("sk-runtime-secret"));
    QJsonObject status = parseObject(result.standardOutput);
    QCOMPARE(status.value("upstream_base_url").toString(), QString("https://profile.example/v1"));
    QCOMPARE(status.value("forward_user_agent").toBool(), true);
    QCOMPARE(status.value("upstream_proxy").toString(), QString("http://127.0.0.1:7890"));
    QCOMPARE(status.value("upstream_timeout_sec").toInt(), 321);
    QCOMPARE(status.value("first_token_timeout_sec").toInt(), 17);
    QCOMPARE(status.value("retry_after_override_sec").toString(), QString("30"));

    port = availablePort();
    QVERIFY(port > 0);
    result = run(QStringList() << "--config" << path
        << "--proxy-port" << QString::number(port)
        << "--upstream-base-url" << "https://override.example/v1"
        << "--upstream-api-key="
        << "--upstream-user-agent" << "override-agent/2.0"
        << "--upstream-proxy=" << "--no-forward-user-agent"
        << "--upstream-timeout" << "654" << "--first-token-timeout" << "9"
        << "--status-json");
    QCOMPARE(result.exitCode, 0);
    QVERIFY(!result.standardOutput.contains("sk-runtime-secret"));
    status = parseObject(result.standardOutput);
    QCOMPARE(status.value("upstream_base_url").toString(), QString("https://override.example/v1"));
    QCOMPARE(status.value("forward_user_agent").toBool(), false);
    QCOMPARE(status.value("upstream_proxy").toString(), QString());
    QCOMPARE(status.value("upstream_timeout_sec").toInt(), 654);
    QCOMPARE(status.value("first_token_timeout_sec").toInt(), 9);

    result = run(QStringList() << "profile" << "show" << "Runtime" << "--config" << path
                               << "--show-secret" << "--json");
    QCOMPARE(result.exitCode, 0);
    const QJsonObject persisted = parseObject(result.standardOutput);
    QCOMPARE(persisted.value("base_url").toString(), QString("https://profile.example/v1"));
    QCOMPARE(persisted.value("api_key").toString(), QString("sk-runtime-secret"));
    QCOMPARE(persisted.value("user_agent").toString(), QString("profile-agent/1.0"));
    QCOMPARE(persisted.value("forward_user_agent").toBool(), true);
    QCOMPARE(persisted.value("upstream_proxy").toString(), QString("http://127.0.0.1:7890"));
    QCOMPARE(persisted.value("upstream_timeout_sec").toInt(), 321);
    QCOMPARE(persisted.value("first_token_timeout_sec").toInt(), 17);
    QCOMPARE(persisted.value("retry_after_override_sec").toString(), QString("30"));

    result = run(QStringList() << "profile" << "update" << "Runtime" << "--config" << path
                               << "--retry-after-override-sec=" << "--json");
    QCOMPARE(result.exitCode, 0);
    const QJsonObject cleared = parseObject(result.standardOutput).value("profile").toObject();
    QCOMPARE(cleared.value("retry_after_override_sec").toString(), QString());
}

void CliProfileCommandsTest::temporaryTimeoutOverridesAreStrictlyValidated()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = configPath(directory);
    const QList<QStringList> invalidOptions = QList<QStringList>()
        << (QStringList() << "--upstream-timeout" << "not-an-integer")
        << (QStringList() << "--upstream-timeout" << "0")
        << (QStringList() << "--upstream-timeout" << "86401")
        << (QStringList() << "--first-token-timeout" << "not-an-integer")
        << (QStringList() << "--first-token-timeout" << "-1")
        << (QStringList() << "--first-token-timeout" << "3601");
    for (int i = 0; i < invalidOptions.size(); ++i) {
        const QStringList option = invalidOptions.at(i);
        const ProcessResult result = run(QStringList()
            << "--config" << path
            << "--upstream-base-url" << "https://example.com/v1"
            << option
            << "--status-json");
        QCOMPARE(result.exitStatus, QProcess::NormalExit);
        QCOMPARE(result.exitCode, 2);
        QVERIFY2(result.standardError.contains(option.first().toUtf8()), result.standardError.constData());
        QVERIFY(result.standardError.contains("must be an integer between"));
        QVERIFY(result.standardOutput.isEmpty());
    }

    quint16 port = availablePort();
    QVERIFY(port > 0);
    ProcessResult result = run(QStringList()
        << "--config" << path
        << "--proxy-port" << QString::number(port)
        << "--upstream-base-url" << "https://example.com/v1"
        << "--upstream-timeout" << "1"
        << "--first-token-timeout" << "0"
        << "--status-json");
    QCOMPARE(result.exitCode, 0);
    QJsonObject status = parseObject(result.standardOutput);
    QCOMPARE(status.value("upstream_timeout_sec").toInt(), 1);
    QCOMPARE(status.value("first_token_timeout_sec").toInt(), 0);

    port = availablePort();
    QVERIFY(port > 0);
    result = run(QStringList()
        << "--config" << path
        << "--proxy-port" << QString::number(port)
        << "--upstream-base-url" << "https://example.com/v1"
        << "--upstream-timeout" << "86400"
        << "--first-token-timeout" << "3600"
        << "--status-json");
    QCOMPARE(result.exitCode, 0);
    status = parseObject(result.standardOutput);
    QCOMPARE(status.value("upstream_timeout_sec").toInt(), 86400);
    QCOMPARE(status.value("first_token_timeout_sec").toInt(), 3600);
}

QTEST_APPLESS_MAIN(CliProfileCommandsTest)

#include "cli_profile_commands_test.moc"
