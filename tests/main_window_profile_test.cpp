#include "core/app_config.h"
#include "core/upstream_profile.h"
#include "gui/main_window.h"

#include <QtCore/QDir>
#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QDialog>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>

#include <functional>

using namespace net_tunnel;

namespace {

quint16 reservePort()
{
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        return 0;
    }
    return server.serverPort();
}

bool waitUntil(const std::function<bool()> &predicate, int timeoutMs = 3000)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        if (predicate()) {
            return true;
        }
        QTest::qWait(10);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    return predicate();
}

class RecordingUpstream : public QObject {
public:
    explicit RecordingUpstream(const QByteArray &marker)
        : marker_(marker), requestCount_(0)
    {
        connect(&server_, &QTcpServer::newConnection, this,
                &RecordingUpstream::acceptConnections);
    }

    bool start()
    {
        return server_.listen(QHostAddress::LocalHost, 0);
    }

    int port() const { return int(server_.serverPort()); }
    int requestCount() const { return requestCount_; }
    QByteArray lastAuthorization() const { return lastAuthorization_; }

private:
    void acceptConnections()
    {
        while (server_.hasPendingConnections()) {
            QTcpSocket *socket = server_.nextPendingConnection();
            socket->setParent(this);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
                QByteArray request = socket->property("request").toByteArray();
                request.append(socket->readAll());
                socket->setProperty("request", request);
                if (socket->property("handled").toBool()) {
                    return;
                }

                const int headerEnd = request.indexOf("\r\n\r\n");
                if (headerEnd < 0) {
                    return;
                }
                int contentLength = 0;
                const QList<QByteArray> lines = request.left(headerEnd).split('\n');
                for (int i = 1; i < lines.size(); ++i) {
                    const QByteArray line = lines.at(i).trimmed();
                    const int colon = line.indexOf(':');
                    if (colon <= 0) {
                        continue;
                    }
                    const QByteArray name = line.left(colon).trimmed().toLower();
                    const QByteArray value = line.mid(colon + 1).trimmed();
                    if (name == "content-length") {
                        contentLength = value.toInt();
                    } else if (name == "authorization") {
                        lastAuthorization_ = value;
                    }
                }
                if (request.size() < headerEnd + 4 + contentLength) {
                    return;
                }

                socket->setProperty("handled", true);
                ++requestCount_;
                const QByteArray body = "{\"profile\":\"" + marker_ + "\"}";
                QByteArray response = "HTTP/1.1 200 OK\r\n";
                response += "Content-Type: application/json\r\n";
                response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
                response += "Connection: close\r\n\r\n";
                response += body;
                socket->write(response);
                socket->flush();
                socket->disconnectFromHost();
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    }

    QTcpServer server_;
    QByteArray marker_;
    int requestCount_;
    QByteArray lastAuthorization_;
};

QByteArray proxyRequest(int port)
{
    QTcpSocket socket;
    QByteArray response;
    QObject::connect(&socket, &QTcpSocket::readyRead, [&socket, &response]() {
        response.append(socket.readAll());
    });
    socket.connectToHost(QHostAddress::LocalHost, quint16(port));
    if (!waitUntil([&socket]() {
        return socket.state() == QAbstractSocket::ConnectedState;
    }, 1000)) {
        return QByteArray("CONNECT_FAILED");
    }

    const QByteArray body = "{}";
    QByteArray request = "POST /v1/responses HTTP/1.1\r\n";
    request += "Host: 127.0.0.1\r\n";
    request += "Content-Type: application/json\r\n";
    request += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    request += "Connection: close\r\n\r\n";
    request += body;
    socket.write(request);
    socket.flush();
    waitUntil([&socket]() {
        return socket.state() == QAbstractSocket::UnconnectedState;
    });
    response.append(socket.readAll());
    return response;
}

QPushButton *buttonWithKey(QWidget *window, const char *key)
{
    const QList<QPushButton *> buttons = window->findChildren<QPushButton *>();
    for (int i = 0; i < buttons.size(); ++i) {
        if (buttons.at(i)->property("i18n_key").toString() == QString::fromLatin1(key)) {
            return buttons.at(i);
        }
    }
    return 0;
}

void dismissNextMessageDialog()
{
    QTimer::singleShot(0, []() {
        const QList<QWidget *> widgets = QApplication::topLevelWidgets();
        for (int i = 0; i < widgets.size(); ++i) {
            QWidget *widget = widgets.at(i);
            if (widget->property("guard_dialog_kind").toString() == "message") {
                if (QDialog *dialog = qobject_cast<QDialog *>(widget)) {
                    dialog->accept();
                }
            }
        }
    });
}

bool writeJson(const QString &path, const QJsonObject &object)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    return file.write(bytes) == bytes.size();
}

} // namespace

class MainWindowProfileTest : public QObject {
    Q_OBJECT

private slots:
    void cleanup()
    {
        qunsetenv("NET_TUNNEL_CONFIG");
    }

    void selectionPopulatesReadOnlyFieldsAndRemainsAvailableWhileRunning()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString configPath = dir.filePath("config.json");
        qputenv("NET_TUNNEL_CONFIG", configPath.toLocal8Bit());

        AppConfig config;
        config.proxyPort = reservePort();
        QVERIFY(config.proxyPort > 0);
        QString error;
        QVERIFY2(saveConfig(config, configPath, &error), qPrintable(error));

        UpstreamProfileStore store(upstreamProfileDatabasePath(configPath));
        QVERIFY2(store.open(&error), qPrintable(error));
        UpstreamProfile primary;
        primary.displayName = "Primary";
        primary.baseUrl = "https://primary.example/v1";
        primary.apiKey = "sk-primary";
        QVERIFY2(store.addProfile(&primary, &error), qPrintable(error));
        UpstreamProfile backup;
        backup.displayName = "Backup";
        backup.baseUrl = "https://backup.example/v1";
        backup.userAgent = "backup-agent/1.0";
        backup.forwardUserAgent = true;
        backup.upstreamProxy = "http://127.0.0.1:7890";
        backup.upstreamTimeoutSec = 600;
        backup.firstTokenTimeoutSec = 12;
        QVERIFY2(store.addProfile(&backup, &error), qPrintable(error));
        QVERIFY2(store.setSelectedProfileId(primary.id, &error), qPrintable(error));

        MainWindow window;
        QComboBox *combo = window.findChild<QComboBox *>("upstreamProfileCombo");
        QLineEdit *url = window.findChild<QLineEdit *>("upstreamBaseUrlEdit");
        QLineEdit *apiKey = window.findChild<QLineEdit *>("upstreamApiKeyEdit");
        QLineEdit *userAgent = window.findChild<QLineEdit *>("upstreamUserAgentEdit");
        QLineEdit *proxy = window.findChild<QLineEdit *>("upstreamProxyEdit");
        QSpinBox *upstreamTimeout = window.findChild<QSpinBox *>("upstreamTimeoutSpin");
        QSpinBox *firstTokenTimeout = window.findChild<QSpinBox *>("firstTokenTimeoutSpin");
        QCheckBox *forwardUserAgent = window.findChild<QCheckBox *>("forwardUserAgentCheck");
        QPushButton *start = buttonWithKey(&window, "start_proxy");
        QPushButton *stop = buttonWithKey(&window, "stop_proxy");
        QVERIFY(combo && url && apiKey && userAgent && proxy);
        QVERIFY(upstreamTimeout && firstTokenTimeout && forwardUserAgent && start && stop);
        QVERIFY(url->isReadOnly());
        QVERIFY(apiKey->isReadOnly());
        QVERIFY(userAgent->isReadOnly());
        QVERIFY(proxy->isReadOnly());
        QVERIFY(upstreamTimeout->isReadOnly());
        QVERIFY(firstTokenTimeout->isReadOnly());
        QVERIFY(!forwardUserAgent->isEnabled());

        QCOMPARE(combo->currentData().toString(), primary.id);
        QCOMPARE(url->text(), primary.baseUrl);
        QCOMPARE(apiKey->text(), primary.apiKey);

        const int backupIndex = combo->findData(backup.id);
        QVERIFY(backupIndex >= 0);
        combo->setCurrentIndex(backupIndex);
        QTRY_COMPARE(url->text(), backup.baseUrl);
        QCOMPARE(userAgent->text(), backup.userAgent);
        QCOMPARE(proxy->text(), backup.upstreamProxy);
        QCOMPARE(upstreamTimeout->value(), backup.upstreamTimeoutSec);
        QCOMPARE(firstTokenTimeout->value(), backup.firstTokenTimeoutSec);
        QVERIFY(forwardUserAgent->isChecked());
        QCOMPARE(store.selectedProfileId(&error), backup.id);
        QVERIFY2(error.isEmpty(), qPrintable(error));

        QVERIFY(QMetaObject::invokeMethod(&window, "startProxy", Qt::DirectConnection));
        QTRY_VERIFY(combo->isEnabled());
        QVERIFY(!start->isEnabled());
        QVERIFY(stop->isEnabled());
        QVERIFY(store.isProfileLocked(backup.id));

        QVERIFY(QMetaObject::invokeMethod(&window, "stopProxy", Qt::DirectConnection));
        QTRY_VERIFY(combo->isEnabled());
        QVERIFY(!store.isProfileLocked(backup.id));
    }

    void runningProfileSelectionStopsAndRestartsWithNewUpstream()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString configPath = dir.filePath("config.json");
        qputenv("NET_TUNNEL_CONFIG", configPath.toLocal8Bit());

        RecordingUpstream primaryUpstream("primary");
        RecordingUpstream backupUpstream("backup");
        QVERIFY(primaryUpstream.start());
        QVERIFY(backupUpstream.start());

        AppConfig config;
        config.proxyPort = reservePort();
        QVERIFY(config.proxyPort > 0);
        QString error;
        QVERIFY2(saveConfig(config, configPath, &error), qPrintable(error));

        UpstreamProfileStore store(upstreamProfileDatabasePath(configPath));
        QVERIFY2(store.open(&error), qPrintable(error));
        UpstreamProfile primary;
        primary.displayName = "Primary";
        primary.baseUrl = QString("http://127.0.0.1:%1/v1").arg(primaryUpstream.port());
        primary.apiKey = "primary-key";
        QVERIFY2(store.addProfile(&primary, &error), qPrintable(error));
        UpstreamProfile backup;
        backup.displayName = "Backup";
        backup.baseUrl = QString("http://127.0.0.1:%1/v1").arg(backupUpstream.port());
        backup.apiKey = "backup-key";
        QVERIFY2(store.addProfile(&backup, &error), qPrintable(error));
        QVERIFY2(store.setSelectedProfileId(primary.id, &error), qPrintable(error));

        MainWindow window;
        QComboBox *combo = window.findChild<QComboBox *>("upstreamProfileCombo");
        QLineEdit *url = window.findChild<QLineEdit *>("upstreamBaseUrlEdit");
        HttpProxyServer *proxy = window.findChild<HttpProxyServer *>();
        QPushButton *start = buttonWithKey(&window, "start_proxy");
        QPushButton *stop = buttonWithKey(&window, "stop_proxy");
        QVERIFY(combo && url && proxy && start && stop);
        QCOMPARE(combo->currentData().toString(), primary.id);

        QVERIFY(QMetaObject::invokeMethod(&window, "startProxy", Qt::DirectConnection));
        QTRY_VERIFY(proxy->isRunning());
        QVERIFY(combo->isEnabled());
        QVERIFY(store.isProfileLocked(primary.id));

        const QByteArray primaryResponse = proxyRequest(config.proxyPort);
        QVERIFY2(primaryResponse.contains("\"profile\":\"primary\""), primaryResponse.constData());
        QTRY_COMPARE(primaryUpstream.requestCount(), 1);
        QCOMPARE(primaryUpstream.lastAuthorization(), QByteArray("Bearer primary-key"));

        const int backupIndex = combo->findData(backup.id);
        QVERIFY(backupIndex >= 0);
        combo->setCurrentIndex(backupIndex);

        QTRY_COMPARE(combo->currentData().toString(), backup.id);
        QTRY_COMPARE(url->text(), backup.baseUrl);
        QTRY_COMPARE(store.selectedProfileId(&error), backup.id);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QTRY_VERIFY(!store.isProfileLocked(primary.id));
        QTRY_VERIFY(store.isProfileLocked(backup.id));
        QTRY_VERIFY(proxy->isRunning());
        QVERIFY(!start->isEnabled());
        QVERIFY(stop->isEnabled());

        const QByteArray backupResponse = proxyRequest(config.proxyPort);
        QVERIFY2(backupResponse.contains("\"profile\":\"backup\""), backupResponse.constData());
        QTRY_COMPARE(backupUpstream.requestCount(), 1);
        QCOMPARE(backupUpstream.lastAuthorization(), QByteArray("Bearer backup-key"));
        QCOMPARE(primaryUpstream.requestCount(), 1);

        QVERIFY(QMetaObject::invokeMethod(&window, "stopProxy", Qt::DirectConnection));
        QTRY_VERIFY(!proxy->isRunning());
        QVERIFY(!store.isProfileLocked(backup.id));
    }

    void failedRunningProfileSwitchLeavesProxyStoppedAndUnlocksProfiles()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString configPath = dir.filePath("config.json");
        qputenv("NET_TUNNEL_CONFIG", configPath.toLocal8Bit());

        RecordingUpstream primaryUpstream("primary");
        QVERIFY(primaryUpstream.start());

        AppConfig config;
        config.proxyPort = reservePort();
        QVERIFY(config.proxyPort > 0);
        QString error;
        QVERIFY2(saveConfig(config, configPath, &error), qPrintable(error));

        UpstreamProfileStore store(upstreamProfileDatabasePath(configPath));
        QVERIFY2(store.open(&error), qPrintable(error));
        UpstreamProfile primary;
        primary.displayName = "Primary";
        primary.baseUrl = QString("http://127.0.0.1:%1/v1").arg(primaryUpstream.port());
        QVERIFY2(store.addProfile(&primary, &error), qPrintable(error));
        UpstreamProfile backup;
        backup.displayName = "Backup";
        backup.baseUrl = "https://backup.example/v1";
        QVERIFY2(store.addProfile(&backup, &error), qPrintable(error));
        QVERIFY2(store.setSelectedProfileId(primary.id, &error), qPrintable(error));

        MainWindow window;
        QComboBox *combo = window.findChild<QComboBox *>("upstreamProfileCombo");
        QSpinBox *proxyPort = window.findChild<QSpinBox *>("proxyPortSpin");
        HttpProxyServer *proxy = window.findChild<HttpProxyServer *>();
        QPushButton *start = buttonWithKey(&window, "start_proxy");
        QVERIFY(combo && proxyPort && proxy && start);

        QVERIFY(QMetaObject::invokeMethod(&window, "startProxy", Qt::DirectConnection));
        QTRY_VERIFY(proxy->isRunning());
        QVERIFY(store.isProfileLocked(primary.id));

        const int blockedPort = reservePort();
        QVERIFY(blockedPort > 0);
        QTcpServer blocker;
        QVERIFY(blocker.listen(QHostAddress::LocalHost, quint16(blockedPort)));
        proxyPort->setValue(blockedPort);

        QTimer dismissTimer;
        dismissTimer.setInterval(10);
        QObject::connect(&dismissTimer, &QTimer::timeout, &window, []() {
            const QList<QWidget *> widgets = QApplication::topLevelWidgets();
            for (int i = 0; i < widgets.size(); ++i) {
                QWidget *widget = widgets.at(i);
                if (widget->property("guard_dialog_kind").toString() == "message") {
                    if (QDialog *dialog = qobject_cast<QDialog *>(widget)) {
                        dialog->accept();
                    }
                }
            }
        });
        dismissTimer.start();

        const int backupIndex = combo->findData(backup.id);
        QVERIFY(backupIndex >= 0);
        combo->setCurrentIndex(backupIndex);
        QTRY_VERIFY(start->isEnabled());
        dismissTimer.stop();

        QVERIFY(!proxy->isRunning());
        QCOMPARE(combo->currentData().toString(), backup.id);
        QCOMPARE(store.selectedProfileId(&error), backup.id);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!store.isProfileLocked(primary.id));
        QVERIFY(!store.isProfileLocked(backup.id));
    }

    void noProfileDisablesProxyStart()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        qputenv("NET_TUNNEL_CONFIG", dir.filePath("config.json").toLocal8Bit());

        MainWindow window;
        QComboBox *combo = window.findChild<QComboBox *>("upstreamProfileCombo");
        QPushButton *start = buttonWithKey(&window, "start_proxy");
        QVERIFY(combo && start);
        QCOMPARE(combo->count(), 1);
        QVERIFY(combo->currentData().toString().isEmpty());
        QVERIFY(!combo->isEnabled());
        QVERIFY(!start->isEnabled());
    }

    void databaseFailureDoesNotBlockGlobalSaveWithoutLegacyFields()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString configPath = dir.filePath("config.json");
        qputenv("NET_TUNNEL_CONFIG", configPath.toLocal8Bit());

        AppConfig config;
        QString error;
        QVERIFY2(saveConfig(config, configPath, &error), qPrintable(error));
        QVERIFY(QDir().mkpath(upstreamProfileDatabasePath(configPath)));

        dismissNextMessageDialog();
        MainWindow window;
        QPushButton *save = buttonWithKey(&window, "save_config");
        QLineEdit *host = window.findChild<QLineEdit *>("proxyHostEdit");
        QVERIFY(save && host);
        QVERIFY(save->isEnabled());

        host->setText("127.0.0.2");
        QVERIFY(QMetaObject::invokeMethod(&window, "saveSettings", Qt::DirectConnection));
        QCOMPARE(loadConfig(configPath).proxyHost, QString("127.0.0.2"));
    }

    void databaseFailureBlocksSaveWhenLegacyFieldsNeedMigration()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString configPath = dir.filePath("config.json");
        qputenv("NET_TUNNEL_CONFIG", configPath.toLocal8Bit());
        QJsonObject legacy;
        legacy.insert("proxy_host", "127.0.0.1");
        legacy.insert("proxy_port", "8010");
        legacy.insert("proxy_prefix", "/v1");
        legacy.insert("upstream_base_url", "https://legacy.example/v1");
        QVERIFY(writeJson(configPath, legacy));
        QVERIFY(QDir().mkpath(upstreamProfileDatabasePath(configPath)));

        dismissNextMessageDialog();
        MainWindow window;
        QPushButton *save = buttonWithKey(&window, "save_config");
        QVERIFY(save);
        QVERIFY(!save->isEnabled());
    }

    void selectedProfileIsPreservedAcrossMultiplePages()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString configPath = dir.filePath("config.json");
        qputenv("NET_TUNNEL_CONFIG", configPath.toLocal8Bit());

        AppConfig config;
        QString error;
        QVERIFY2(saveConfig(config, configPath, &error), qPrintable(error));
        UpstreamProfileStore store(upstreamProfileDatabasePath(configPath));
        QVERIFY2(store.open(&error), qPrintable(error));

        UpstreamProfile selected;
        for (int i = 0; i < 105; ++i) {
            UpstreamProfile profile;
            profile.displayName = QString("Profile %1").arg(i, 3, 10, QChar('0'));
            profile.baseUrl = QString("https://profile-%1.example/v1").arg(i);
            QVERIFY2(store.addProfile(&profile, &error), qPrintable(error));
            if (i == 104) {
                selected = profile;
            }
        }
        QVERIFY2(store.setSelectedProfileId(selected.id, &error), qPrintable(error));

        MainWindow window;
        QComboBox *combo = window.findChild<QComboBox *>("upstreamProfileCombo");
        QLineEdit *url = window.findChild<QLineEdit *>("upstreamBaseUrlEdit");
        QVERIFY(combo && url);
        QCOMPARE(combo->count(), 105);
        QCOMPARE(combo->currentData().toString(), selected.id);
        QCOMPARE(url->text(), selected.baseUrl);
        QCOMPARE(store.selectedProfileId(&error), selected.id);
        QVERIFY2(error.isEmpty(), qPrintable(error));
    }
};

QTEST_MAIN(MainWindowProfileTest)

#include "main_window_profile_test.moc"
