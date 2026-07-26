#include "core/app_config.h"
#include "core/upstream_profile.h"
#include "gui/main_window.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtNetwork/QTcpServer>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>

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

void dismissNextMessageBox()
{
    QTimer::singleShot(0, []() {
        const QList<QWidget *> widgets = QApplication::topLevelWidgets();
        for (int i = 0; i < widgets.size(); ++i) {
            if (QMessageBox *messageBox = qobject_cast<QMessageBox *>(widgets.at(i))) {
                messageBox->accept();
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

    void selectionPopulatesReadOnlyFieldsAndLocksWhileRunning()
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
        QTRY_VERIFY(!combo->isEnabled());
        QVERIFY(!start->isEnabled());
        QVERIFY(stop->isEnabled());
        QVERIFY(store.isProfileLocked(backup.id));

        QVERIFY(QMetaObject::invokeMethod(&window, "stopProxy", Qt::DirectConnection));
        QTRY_VERIFY(combo->isEnabled());
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

        dismissNextMessageBox();
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

        dismissNextMessageBox();
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
