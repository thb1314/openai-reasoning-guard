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
#include <QtGui/QGuiApplication>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <QtWidgets/QAction>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTableWidget>

#include <functional>

using namespace net_tunnel;

namespace {

template <typename T>
T *findWindowChild(MainWindow *window, const char *objectName = 0)
{
    if (!window) {
        return 0;
    }
    return objectName
        ? window->findChild<T *>(objectName)
        : window->findChild<T *>();
}

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

QWidget *findResizeOutline()
{
    const QWidgetList topLevels = QApplication::topLevelWidgets();
    for (int i = 0; i < topLevels.size(); ++i) {
        QWidget *widget = topLevels.at(i);
        if (widget && widget->objectName() == QLatin1String("resizeOutline")) {
            return widget;
        }
    }
    return 0;
}

class WindowPresentationChangeCounter : public QObject {
public:
    explicit WindowPresentationChangeCounter(MainWindow *window)
        : root_(window ? window->findChild<QWidget *>("rootContent") : 0),
          styleCount_(0),
          fontCount_(0)
    {
    }

    int styleCount() const { return styleCount_; }
    int fontCount() const { return fontCount_; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        QWidget *widget = qobject_cast<QWidget *>(watched);
        if (widget && root_ &&
            (widget == root_ || root_->isAncestorOf(widget))) {
            if (event->type() == QEvent::StyleChange) {
                ++styleCount_;
            } else if (event->type() == QEvent::FontChange) {
                ++fontCount_;
            }
        }
        return QObject::eventFilter(watched, event);
    }

private:
    QWidget *root_;
    int styleCount_;
    int fontCount_;
};

class ApplicationFontGuard {
public:
    ApplicationFontGuard() : original_(QApplication::font()) {}
    ~ApplicationFontGuard() { QApplication::setFont(original_); }

private:
    QFont original_;
};

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

struct AboutDialogSnapshot {
    bool shown = false;
    bool frameless = false;
    bool customTitleBar = false;
    bool authorExternalLink = false;
    bool projectExternalLink = false;
    bool closeInvoked = false;
    int height = 0;
    QString title;
    QString authorCaption;
    QString authorLink;
    QString projectCaption;
    QString projectLink;
    QString closeText;
};

AboutDialogSnapshot captureAboutDialog(QAction *action)
{
    AboutDialogSnapshot snapshot;
    QTimer captureTimer;
    captureTimer.setInterval(5);
    QObject::connect(&captureTimer, &QTimer::timeout, [&captureTimer, &snapshot]() {
        const QList<QWidget *> widgets = QApplication::topLevelWidgets();
        for (int i = 0; i < widgets.size(); ++i) {
            QWidget *widget = widgets.at(i);
            if (widget->property("guard_dialog_kind").toString() != "about") {
                continue;
            }

            QDialog *dialog = qobject_cast<QDialog *>(widget);
            if (!dialog) {
                continue;
            }
            snapshot.shown = true;
            snapshot.frameless = dialog->windowFlags().testFlag(Qt::FramelessWindowHint);
            snapshot.customTitleBar = dialog->findChild<QWidget *>("guardDialogTitleBar") != 0;
            snapshot.title = dialog->windowTitle();
            snapshot.height = dialog->height();

            QLabel *authorCaption = dialog->findChild<QLabel *>("aboutAuthorCaption");
            QLabel *authorLink = dialog->findChild<QLabel *>("aboutAuthorLink");
            QLabel *projectCaption = dialog->findChild<QLabel *>("aboutProjectCaption");
            QLabel *projectLink = dialog->findChild<QLabel *>("aboutProjectLink");
            QPushButton *closeButton = dialog->findChild<QPushButton *>("aboutCloseButton");
            if (authorCaption) snapshot.authorCaption = authorCaption->text();
            if (authorLink) {
                snapshot.authorLink = authorLink->text();
                snapshot.authorExternalLink = authorLink->openExternalLinks()
                    && authorLink->textInteractionFlags().testFlag(Qt::LinksAccessibleByMouse);
            }
            if (projectCaption) snapshot.projectCaption = projectCaption->text();
            if (projectLink) {
                snapshot.projectLink = projectLink->text();
                snapshot.projectExternalLink = projectLink->openExternalLinks()
                    && projectLink->textInteractionFlags().testFlag(Qt::LinksAccessibleByMouse);
            }
            if (closeButton) {
                snapshot.closeText = closeButton->text();
                snapshot.closeInvoked = true;
                captureTimer.stop();
                closeButton->click();
            }
            return;
        }
    });

    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, []() {
        if (QDialog *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
            dialog->reject();
        }
    });
    captureTimer.start();
    watchdog.start(2000);
    action->trigger();
    captureTimer.stop();
    watchdog.stop();
    return snapshot;
}

struct InterfaceSettingsSnapshot {
    bool shown = false;
    bool frameless = false;
    bool customTitleBar = false;
    bool interactionInvoked = false;
    bool initiallyUsesSystemFont = false;
    int initialPointSize = 0;
    QString title;
    QString useSystemText;
    QString fontSizeText;
    QString applyText;
    QString cancelText;
};

InterfaceSettingsSnapshot useInterfaceSettingsDialog(QAction *action,
                                                      int pointSize,
                                                      bool apply,
                                                      bool useSystemDefault = false)
{
    InterfaceSettingsSnapshot snapshot;
    QTimer captureTimer;
    captureTimer.setInterval(5);
    QObject::connect(&captureTimer, &QTimer::timeout,
                     [&captureTimer, &snapshot, pointSize, apply,
                      useSystemDefault]() {
        const QList<QWidget *> widgets = QApplication::topLevelWidgets();
        for (int i = 0; i < widgets.size(); ++i) {
            QWidget *widget = widgets.at(i);
            if (widget->property("guard_dialog_kind").toString() !=
                QLatin1String("interface_settings")) {
                continue;
            }

            QDialog *dialog = qobject_cast<QDialog *>(widget);
            if (!dialog) {
                continue;
            }
            snapshot.shown = true;
            snapshot.frameless = dialog->windowFlags().testFlag(Qt::FramelessWindowHint);
            snapshot.customTitleBar =
                dialog->findChild<QWidget *>("guardDialogTitleBar") != 0;
            snapshot.title = dialog->windowTitle();

            QCheckBox *useSystem =
                dialog->findChild<QCheckBox *>("interfaceUseSystemFontCheck");
            QSpinBox *fontSize =
                dialog->findChild<QSpinBox *>("interfaceFontSizeSpin");
            QLabel *fontSizeLabel =
                dialog->findChild<QLabel *>("interfaceFontSizeLabel");
            QPushButton *applyButton =
                dialog->findChild<QPushButton *>("interfaceSettingsApplyButton");
            QPushButton *cancelButton =
                dialog->findChild<QPushButton *>("interfaceSettingsCancelButton");
            if (!useSystem || !fontSize || !fontSizeLabel ||
                !applyButton || !cancelButton) {
                continue;
            }

            snapshot.initiallyUsesSystemFont = useSystem->isChecked();
            snapshot.initialPointSize = fontSize->value();
            snapshot.useSystemText = useSystem->text();
            snapshot.fontSizeText = fontSizeLabel->text();
            snapshot.applyText = applyButton->text();
            snapshot.cancelText = cancelButton->text();
            snapshot.interactionInvoked = true;
            captureTimer.stop();

            useSystem->setChecked(useSystemDefault);
            if (!useSystemDefault) {
                fontSize->setValue(pointSize);
            }
            (apply ? applyButton : cancelButton)->click();
            return;
        }
    });

    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, []() {
        if (QDialog *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
            dialog->reject();
        }
    });
    captureTimer.start();
    watchdog.start(2000);
    action->trigger();
    captureTimer.stop();
    watchdog.stop();
    return snapshot;
}

} // namespace

class MainWindowProfileTest : public QObject {
    Q_OBJECT

private slots:
    void cleanup()
    {
        qunsetenv("NET_TUNNEL_CONFIG");
    }

    void aboutMenuUsesCustomLocalizedDialog()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString configPath = dir.filePath("config.json");
        qputenv("NET_TUNNEL_CONFIG", configPath.toLocal8Bit());

        AppConfig config;
        config.lang = "en";
        QString error;
        QVERIFY2(saveConfig(config, configPath, &error), qPrintable(error));

        MainWindow window;
        QAction *aboutAction = findWindowChild<QAction>(&window, "aboutAction");
        QVERIFY(aboutAction);
        QCOMPARE(aboutAction->text(), QString("About"));

        AboutDialogSnapshot english = captureAboutDialog(aboutAction);
        QVERIFY(english.shown);
        QVERIFY(english.frameless);
        QVERIFY(english.customTitleBar);
        QVERIFY(english.height > 0 && english.height <= 260);
        QCOMPARE(english.title, QString("About"));
        QCOMPARE(english.authorCaption, QString("Author"));
        QVERIFY(english.authorLink.contains("href=\"https://github.com/thb1314\""));
        QVERIFY(english.authorLink.contains("thb1314"));
        QVERIFY(english.authorExternalLink);
        QCOMPARE(english.projectCaption, QString("Project"));
        QVERIFY(english.projectLink.contains(
            "href=\"https://github.com/thb1314/openai-reasoning-guard\""));
        QVERIFY(english.projectExternalLink);
        QCOMPARE(english.closeText, QString("Close"));
        QVERIFY(english.closeInvoked);

        QVERIFY(QMetaObject::invokeMethod(&window, "switchToChinese", Qt::DirectConnection));
        QCOMPARE(aboutAction->text(), QString::fromUtf8("关于"));
        AboutDialogSnapshot chinese = captureAboutDialog(aboutAction);
        QVERIFY(chinese.shown);
        QCOMPARE(chinese.title, QString::fromUtf8("关于"));
        QCOMPARE(chinese.authorCaption, QString::fromUtf8("作者"));
        QCOMPARE(chinese.projectCaption, QString::fromUtf8("项目地址"));
        QCOMPARE(chinese.closeText, QString::fromUtf8("关闭"));
        QVERIFY(chinese.closeInvoked);
    }

    void interfaceSettingsMenuIsLocalized()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString configPath = dir.filePath("config.json");
        qputenv("NET_TUNNEL_CONFIG", configPath.toLocal8Bit());

        AppConfig config;
        config.lang = "zh";
        QString error;
        QVERIFY2(saveConfig(config, configPath, &error), qPrintable(error));

        MainWindow window;
        QAction *action = findWindowChild<QAction>(&window, "interfaceSettingsAction");
        QVERIFY(action);
        QCOMPARE(action->text(), QString::fromUtf8("界面设置"));

        QVERIFY(QMetaObject::invokeMethod(&window, "switchToEnglish", Qt::DirectConnection));
        QCOMPARE(action->text(), QString("Interface Settings"));
    }

    void windowResizeDoesNotChangeConfiguredFont()
    {
        ApplicationFontGuard fontGuard;
        QFont applicationFont = QApplication::font();
        applicationFont.setPointSize(10);
        QApplication::setFont(applicationFont);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString configPath = dir.filePath("config.json");
        qputenv("NET_TUNNEL_CONFIG", configPath.toLocal8Bit());

        AppConfig config;
        config.uiFontPointSize = 14;
        QString error;
        QVERIFY2(saveConfig(config, configPath, &error), qPrintable(error));

        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1080, 680);
        window.show();
        QTest::qWait(120);
        QLineEdit *lineEdit = findWindowChild<QLineEdit>(&window, "proxyHostEdit");
        QComboBox *comboBox = findWindowChild<QComboBox>(&window, "upstreamProfileCombo");
        QSpinBox *spinBox = findWindowChild<QSpinBox>(&window, "proxyPortSpin");
        QPushButton *maximizeButton = window.getBtnMenuMax();
        QVERIFY(lineEdit && comboBox && spinBox && maximizeButton);

        const qreal configuredScale = window.property("ui_scale_factor").toReal();
        const QFont lineFont = lineEdit->font();
        const QFont comboFont = comboBox->font();
        const QFont spinFont = spinBox->font();
        const QFont maximizeFont = maximizeButton->font();
        const int lineHeight = lineEdit->sizeHint().height();
        QVERIFY(configuredScale > 1.0);

        window.resize(1600, 960);
        QTest::qWait(180);
        QCOMPARE(window.property("ui_scale_factor").toReal(), configuredScale);
        QCOMPARE(lineEdit->font(), lineFont);
        QCOMPARE(comboBox->font(), comboFont);
        QCOMPARE(spinBox->font(), spinFont);
        QCOMPARE(maximizeButton->font(), maximizeFont);
        QCOMPARE(lineEdit->sizeHint().height(), lineHeight);

        window.resize(1080, 680);
        QTest::qWait(180);
        QCOMPARE(window.property("ui_scale_factor").toReal(), configuredScale);
        QCOMPARE(lineEdit->font(), lineFont);
        QCOMPARE(comboBox->font(), comboFont);
        QCOMPARE(spinBox->font(), spinFont);
        QCOMPARE(maximizeButton->font(), maximizeFont);
        QCOMPARE(lineEdit->sizeHint().height(), lineHeight);
    }

    void proxyPanelProvidesHorizontalScrollForLargeFixedFont()
    {
        ApplicationFontGuard fontGuard;
        QFont applicationFont = QApplication::font();
        applicationFont.setPointSize(9);
        QApplication::setFont(applicationFont);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString configPath = dir.filePath("config.json");
        qputenv("NET_TUNNEL_CONFIG", configPath.toLocal8Bit());

        AppConfig config;
        config.uiFontPointSize = 0;
        QString error;
        QVERIFY2(saveConfig(config, configPath, &error), qPrintable(error));

        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1000, 680);
        window.show();

        QScrollArea *scrollArea = findWindowChild<QScrollArea>(&window, "proxyScrollArea");
        QAction *settingsAction =
            findWindowChild<QAction>(&window, "interfaceSettingsAction");
        QVERIFY(scrollArea && settingsAction);
        QCOMPARE(scrollArea->horizontalScrollBarPolicy(), Qt::ScrollBarAsNeeded);

        QScrollBar *horizontal = scrollArea->horizontalScrollBar();
        QVERIFY(horizontal);
        QTRY_COMPARE(horizontal->maximum(), horizontal->minimum());

        const InterfaceSettingsSnapshot snapshot =
            useInterfaceSettingsDialog(settingsAction, 20, true);
        QVERIFY(snapshot.shown);
        QVERIFY(snapshot.interactionInvoked);
        QCOMPARE(loadConfig(configPath).uiFontPointSize, 20);
        QVERIFY(waitUntil([horizontal]() {
            return horizontal->isVisible() && horizontal->isEnabled() &&
                horizontal->maximum() > horizontal->minimum();
        }));
        QVERIFY(horizontal->height() > 9);

        const int minimum = horizontal->minimum();
        const int maximum = horizontal->maximum();
        horizontal->setValue(maximum);
        QCOMPARE(horizontal->value(), maximum);
        QVERIFY(scrollArea->widget()->pos().x() < 0);

        horizontal->setValue(minimum);
        QCOMPARE(horizontal->value(), minimum);
        QCOMPARE(scrollArea->widget()->pos().x(), 0);

        QScrollBar *vertical = scrollArea->verticalScrollBar();
        QVERIFY(vertical);
        QCOMPARE(scrollArea->verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOn);
        QVERIFY(vertical->isVisible());
    }

    void interfaceSettingsAppliesAndPersistsFontSize()
    {
        ApplicationFontGuard fontGuard;
        QFont applicationFont = QApplication::font();
        applicationFont.setPointSizeF(9.5);
        QApplication::setFont(applicationFont);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString configPath = dir.filePath("config.json");
        qputenv("NET_TUNNEL_CONFIG", configPath.toLocal8Bit());

        AppConfig config;
        config.lang = "en";
        config.uiFontPointSize = 0;
        QString error;
        QVERIFY2(saveConfig(config, configPath, &error), qPrintable(error));

        MainWindow window;
        QAction *action = findWindowChild<QAction>(&window, "interfaceSettingsAction");
        QLineEdit *lineEdit = findWindowChild<QLineEdit>(&window, "proxyHostEdit");
        QVERIFY(action && lineEdit);
        const QFont originalFont = lineEdit->font();
        const qreal originalScale = window.property("ui_scale_factor").toReal();

        const InterfaceSettingsSnapshot snapshot =
            useInterfaceSettingsDialog(action, 14, true);
        QVERIFY(snapshot.shown);
        QVERIFY(snapshot.frameless);
        QVERIFY(snapshot.customTitleBar);
        QVERIFY(snapshot.interactionInvoked);
        QVERIFY(snapshot.initiallyUsesSystemFont);
        QCOMPARE(snapshot.title, QString("Interface Settings"));
        QCOMPARE(snapshot.useSystemText, QString("Use system default font size"));
        QCOMPARE(snapshot.fontSizeText, QString("Font size"));
        QCOMPARE(snapshot.applyText, QString("Apply"));
        QCOMPARE(snapshot.cancelText, QString("Cancel"));

        QVERIFY(window.property("ui_scale_factor").toReal() > originalScale);
        QVERIFY(lineEdit->font().pointSizeF() > originalFont.pointSizeF());
        QVERIFY(qAbs(lineEdit->font().pointSizeF() - 14.0) < 0.01);

        const AppConfig persisted = loadConfig(configPath);
        QCOMPARE(persisted.uiFontPointSize, 14);
    }

    void interfaceSettingsCancelDoesNotChangeFont()
    {
        ApplicationFontGuard fontGuard;
        QFont applicationFont = QApplication::font();
        applicationFont.setPointSize(10);
        QApplication::setFont(applicationFont);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString configPath = dir.filePath("config.json");
        qputenv("NET_TUNNEL_CONFIG", configPath.toLocal8Bit());

        AppConfig config;
        config.lang = "zh";
        config.uiFontPointSize = 12;
        QString error;
        QVERIFY2(saveConfig(config, configPath, &error), qPrintable(error));

        MainWindow window;
        QAction *action = findWindowChild<QAction>(&window, "interfaceSettingsAction");
        QLineEdit *lineEdit = findWindowChild<QLineEdit>(&window, "proxyHostEdit");
        QVERIFY(action && lineEdit);
        const QFont originalFont = lineEdit->font();
        const qreal originalScale = window.property("ui_scale_factor").toReal();

        const InterfaceSettingsSnapshot snapshot =
            useInterfaceSettingsDialog(action, 18, false);
        QVERIFY(snapshot.shown);
        QVERIFY(snapshot.interactionInvoked);
        QVERIFY(!snapshot.initiallyUsesSystemFont);
        QCOMPARE(snapshot.initialPointSize, 12);
        QCOMPARE(snapshot.title, QString::fromUtf8("界面设置"));
        QCOMPARE(snapshot.useSystemText, QString::fromUtf8("使用系统默认字体大小"));
        QCOMPARE(snapshot.fontSizeText, QString::fromUtf8("字体大小"));
        QCOMPARE(snapshot.applyText, QString::fromUtf8("应用"));
        QCOMPARE(snapshot.cancelText, QString::fromUtf8("取消"));

        QCOMPARE(window.property("ui_scale_factor").toReal(), originalScale);
        QCOMPARE(lineEdit->font(), originalFont);
        QCOMPARE(loadConfig(configPath).uiFontPointSize, 12);
    }

    void interfaceSettingsCanRestoreSystemFont()
    {
        ApplicationFontGuard fontGuard;
        QFont applicationFont = QApplication::font();
        applicationFont.setPointSize(10);
        QApplication::setFont(applicationFont);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString configPath = dir.filePath("config.json");
        qputenv("NET_TUNNEL_CONFIG", configPath.toLocal8Bit());

        AppConfig config;
        config.lang = "en";
        config.uiFontPointSize = 14;
        QString error;
        QVERIFY2(saveConfig(config, configPath, &error), qPrintable(error));

        MainWindow window;
        QAction *action = findWindowChild<QAction>(&window, "interfaceSettingsAction");
        QLineEdit *lineEdit = findWindowChild<QLineEdit>(&window, "proxyHostEdit");
        QVERIFY(action && lineEdit);
        QVERIFY(window.property("ui_scale_factor").toReal() > 1.0);
        QVERIFY(lineEdit->font().pointSizeF() > applicationFont.pointSizeF());

        const InterfaceSettingsSnapshot snapshot =
            useInterfaceSettingsDialog(action, 14, true, true);
        QVERIFY(snapshot.shown);
        QVERIFY(snapshot.interactionInvoked);
        QVERIFY(!snapshot.initiallyUsesSystemFont);

        QCOMPARE(window.property("ui_scale_factor").toReal(), 1.0);
        QCOMPARE(lineEdit->font().pointSizeF(), applicationFont.pointSizeF());
        QCOMPARE(loadConfig(configPath).uiFontPointSize, 0);
    }

    void controlsHonorSystemLargeFont()
    {
        ApplicationFontGuard fontGuard;
        const QFont originalFont = QApplication::font();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString configPath = dir.filePath("config.json");
        qputenv("NET_TUNNEL_CONFIG", configPath.toLocal8Bit());
        AppConfig config;
        QString error;
        QVERIFY2(saveConfig(config, configPath, &error), qPrintable(error));

        int regularHeight = 0;
        int regularChromeWidth = 0;
        {
            MainWindow regularWindow;
            QLineEdit *regularEdit = findWindowChild<QLineEdit>(&regularWindow, "proxyHostEdit");
            QPushButton *regularMaximizeButton = regularWindow.getBtnMenuMax();
            QVERIFY(regularEdit && regularMaximizeButton);
            regularHeight = regularEdit->sizeHint().height();
            regularChromeWidth = regularMaximizeButton->width();
        }

        QFont largeFont(originalFont);
        largeFont.setPointSizeF(qMax(14.0, originalFont.pointSizeF() + 4.0));
        QApplication::setFont(largeFont);

        MainWindow largeWindow;
        QLineEdit *largeEdit = findWindowChild<QLineEdit>(&largeWindow, "proxyHostEdit");
        QPushButton *maximizeButton = largeWindow.getBtnMenuMax();
        QVERIFY(largeEdit && maximizeButton);
        QCOMPARE(largeEdit->font().pointSizeF(), largeFont.pointSizeF());
        QCOMPARE(maximizeButton->font().pointSizeF(), largeFont.pointSizeF());
        QVERIFY(largeEdit->sizeHint().height() > regularHeight);
        QVERIFY(maximizeButton->width() > regularChromeWidth);
    }

    void framelessWindowSupportsEdgeAndCornerResize()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString configPath = dir.filePath("config.json");
        qputenv("NET_TUNNEL_CONFIG", configPath.toLocal8Bit());

        AppConfig config;
        QString error;
        QVERIFY2(saveConfig(config, configPath, &error), qPrintable(error));

        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1080, 680);
        window.show();
        QTest::qWait(180);

        const auto verifyCursor = [&window](const QPoint &position,
                                             Qt::CursorShape expected) {
            QMouseEvent move(QEvent::MouseMove, position,
                             window.mapToGlobal(position), Qt::NoButton,
                             Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(&window, &move);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QCOMPARE(window.cursor().shape(), expected);
        };

        verifyCursor(QPoint(1, window.height() / 2), Qt::SizeHorCursor);
        QWidget *rootContent = findWindowChild<QWidget>(&window, "rootContent");
        QVERIFY(rootContent);
        QEvent childLeave(QEvent::Leave);
        QApplication::sendEvent(rootContent, &childLeave);
        QCOMPARE(window.cursor().shape(), Qt::SizeHorCursor);
        verifyCursor(QPoint(window.width() - 2, window.height() / 2), Qt::SizeHorCursor);
        verifyCursor(QPoint(window.width() / 2, 1), Qt::SizeVerCursor);
        verifyCursor(QPoint(window.width() / 2, window.height() - 2), Qt::SizeVerCursor);
        verifyCursor(QPoint(1, 1), Qt::SizeFDiagCursor);
        verifyCursor(QPoint(18, 1), Qt::SizeFDiagCursor);
        verifyCursor(QPoint(1, 18), Qt::SizeFDiagCursor);
        verifyCursor(QPoint(14, 14), Qt::SizeFDiagCursor);
        verifyCursor(QPoint(window.width() - 2, window.height() - 2),
                     Qt::SizeFDiagCursor);
        // The close button owns the top-right corner and must not be shadowed
        // by the resize hit area.
        verifyCursor(QPoint(window.width() - 2, 1), Qt::ArrowCursor);
        verifyCursor(QPoint(1, window.height() - 2), Qt::SizeBDiagCursor);
        verifyCursor(window.rect().center(), Qt::ArrowCursor);

        const QRect before = window.geometry();
        QLineEdit *lineEdit = findWindowChild<QLineEdit>(&window, "proxyHostEdit");
        QPushButton *maximizeButton = window.getBtnMenuMax();
        QVERIFY(lineEdit && maximizeButton);
        const qreal fontBeforeDrag = lineEdit->font().pointSizeF();
        const qreal maximizeFontBeforeDrag = maximizeButton->font().pointSizeF();
        const int lineHeightBeforeDrag = lineEdit->sizeHint().height();
        WindowPresentationChangeCounter presentationChanges(&window);
        qApp->installEventFilter(&presentationChanges);
        const QPoint pressLocal(window.width() - 2, window.height() - 2);
        const QPoint pressGlobal = window.mapToGlobal(pressLocal);
        QMouseEvent press(QEvent::MouseButtonPress, pressLocal, pressGlobal,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&window, &press);
        QWidget *resizePreview = findResizeOutline();
        QVERIFY(resizePreview);
        QVERIFY(resizePreview->isVisible());
        QVERIFY(resizePreview->mask().contains(QPoint(1, 1)));
        QVERIFY(!resizePreview->mask().contains(resizePreview->rect().center()));
        QCOMPARE(window.property("resize_preview_geometry").toRect(), before);

        const QPoint moveLocal = pressLocal + QPoint(180, 120);
        const QPoint moveGlobal = pressGlobal + QPoint(180, 120);
        QMouseEvent move(QEvent::MouseMove, moveLocal, moveGlobal,
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&window, &move);
        QTest::qWait(400);
        QCOMPARE(window.geometry(), before);
        const QRect pausedPreviewGeometry =
            window.property("resize_preview_geometry").toRect();
        QVERIFY(pausedPreviewGeometry.width() > before.width());
        QVERIFY(pausedPreviewGeometry.height() > before.height());
        QMouseEvent pausedMove(QEvent::MouseMove, moveLocal, moveGlobal,
                               Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&window, &pausedMove);
        QTest::qWait(600);
        QVERIFY(resizePreview->isVisible());
        QCOMPARE(window.geometry(), before);
        QCOMPARE(window.property("resize_preview_geometry").toRect(),
                 pausedPreviewGeometry);
        QVERIFY(lineEdit->font().pointSizeF() >= fontBeforeDrag);
        QVERIFY(lineEdit->sizeHint().height() >= lineHeightBeforeDrag);

        const QPoint continuedLocal = pressLocal + QPoint(240, 170);
        const QPoint continuedGlobal = pressGlobal + QPoint(240, 170);
        QMouseEvent continuedMove(QEvent::MouseMove, continuedLocal, continuedGlobal,
                                  Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&window, &continuedMove);
        QTest::qWait(120);
        QCOMPARE(window.geometry(), before);
        const QRect finalPreviewGeometry =
            window.property("resize_preview_geometry").toRect();
        QVERIFY(finalPreviewGeometry.width() > pausedPreviewGeometry.width());
        QVERIFY(finalPreviewGeometry.height() > pausedPreviewGeometry.height());
        QVERIFY(lineEdit->font().pointSizeF() >= fontBeforeDrag);
        QVERIFY(maximizeButton->font().pointSizeF() >= maximizeFontBeforeDrag);

        QMouseEvent release(QEvent::MouseButtonRelease, continuedLocal, continuedGlobal,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&window, &release);
        QVERIFY(!resizePreview->isVisible());
        QCOMPARE(window.geometry(), finalPreviewGeometry);
        QCOMPARE(window.cursor().shape(), Qt::ArrowCursor);
        QTest::qWait(180);
        QVERIFY(lineEdit->font().pointSizeF() >= fontBeforeDrag);
        QVERIFY(maximizeButton->font().pointSizeF() >= maximizeFontBeforeDrag);
        QVERIFY(lineEdit->sizeHint().height() >= lineHeightBeforeDrag);
        QCOMPARE(presentationChanges.styleCount(), 0);
        QVERIFY(window.property("ui_scale_factor").toReal() >= 1.0);
        qApp->removeEventFilter(&presentationChanges);

        verifyCursor(window.rect().center(), Qt::ArrowCursor);
        verifyCursor(QPoint(window.width() - 2, window.height() / 2),
                     Qt::SizeHorCursor);

        // A title button placed in an edge hit zone must keep its normal click.
        const QPoint buttonPoint(maximizeButton->width() - 1, 1);
        QTest::mouseClick(maximizeButton, Qt::LeftButton, Qt::NoModifier, buttonPoint);
        QTRY_VERIFY(window.isMaximized());
        window.showNormal();
        QTRY_VERIFY(!window.isMaximized());
    }

    void manualResizeCancelsWhenWindowDeactivates()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString configPath = dir.filePath("config.json");
        qputenv("NET_TUNNEL_CONFIG", configPath.toLocal8Bit());

        AppConfig config;
        QString error;
        QVERIFY2(saveConfig(config, configPath, &error), qPrintable(error));

        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1080, 680);
        window.show();
        QTest::qWait(120);
        const QPoint pressLocal(window.width() - 2, window.height() - 2);
        const QPoint pressGlobal = window.mapToGlobal(pressLocal);
        QMouseEvent press(QEvent::MouseButtonPress, pressLocal, pressGlobal,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&window, &press);
        QWidget *resizePreview = findResizeOutline();
        QVERIFY(resizePreview);
        QVERIFY(resizePreview->isVisible());

        QEvent deactivate(QEvent::ApplicationDeactivate);
        QApplication::sendEvent(&window, &deactivate);
        QVERIFY(!resizePreview->isVisible());
        QCOMPARE(window.cursor().shape(), Qt::ArrowCursor);

        const QRect beforeMove = window.geometry();
        const QPoint moveLocal = pressLocal + QPoint(160, 110);
        QMouseEvent move(QEvent::MouseMove, moveLocal,
                         window.mapToGlobal(moveLocal), Qt::NoButton,
                         Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&window, &move);
        QCOMPARE(window.geometry(), beforeMove);

        window.show();
        QTest::qWait(30);
        const QPoint secondPressLocal(window.width() - 2, window.height() - 2);
        QMouseEvent secondPress(QEvent::MouseButtonPress, secondPressLocal,
                                window.mapToGlobal(secondPressLocal),
                                Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&window, &secondPress);
        QVERIFY(resizePreview->isVisible());
        window.hide();
        QVERIFY(!resizePreview->isVisible());
    }

    void edgeResizeCursorDoesNotMutateApplicationOverrideCursor()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString configPath = dir.filePath("config.json");
        qputenv("NET_TUNNEL_CONFIG", configPath.toLocal8Bit());

        AppConfig config;
        QString error;
        QVERIFY2(saveConfig(config, configPath, &error), qPrintable(error));

        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1080, 680);
        window.show();
        QTest::qWait(120);

        QApplication::setOverrideCursor(QCursor(Qt::CrossCursor));
        QMouseEvent edgeMove(QEvent::MouseMove,
                             QPoint(1, window.height() / 2),
                             window.mapToGlobal(QPoint(1, window.height() / 2)),
                             Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&window, &edgeMove);
        QCOMPARE(window.cursor().shape(), Qt::SizeHorCursor);
        QVERIFY(QApplication::overrideCursor());
        QCOMPARE(QApplication::overrideCursor()->shape(), Qt::CrossCursor);

        QMouseEvent centerMove(QEvent::MouseMove,
                               window.rect().center(),
                               window.mapToGlobal(window.rect().center()),
                               Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&window, &centerMove);
        QCOMPARE(window.cursor().shape(), Qt::ArrowCursor);
        QVERIFY(QApplication::overrideCursor());
        QCOMPARE(QApplication::overrideCursor()->shape(), Qt::CrossCursor);
        QApplication::restoreOverrideCursor();
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
        backup.retryAfterOverrideSec = "30";
        backup.mapUpstreamErrorsTo502 = true;
        QVERIFY2(store.addProfile(&backup, &error), qPrintable(error));
        QVERIFY2(store.setSelectedProfileId(primary.id, &error), qPrintable(error));

        MainWindow window;
        QComboBox *combo = findWindowChild<QComboBox>(&window, "upstreamProfileCombo");
        QLineEdit *url = findWindowChild<QLineEdit>(&window, "upstreamBaseUrlEdit");
        QLineEdit *apiKey = findWindowChild<QLineEdit>(&window, "upstreamApiKeyEdit");
        QLineEdit *userAgent = findWindowChild<QLineEdit>(&window, "upstreamUserAgentEdit");
        QLineEdit *proxy = findWindowChild<QLineEdit>(&window, "upstreamProxyEdit");
        QSpinBox *upstreamTimeout = findWindowChild<QSpinBox>(&window, "upstreamTimeoutSpin");
        QSpinBox *firstTokenTimeout = findWindowChild<QSpinBox>(&window, "firstTokenTimeoutSpin");
        QSpinBox *retryAfterOverride = findWindowChild<QSpinBox>(&window, "retryAfterOverrideSpin");
        QCheckBox *forwardUserAgent = findWindowChild<QCheckBox>(&window, "forwardUserAgentCheck");
        QCheckBox *mapErrors =
            findWindowChild<QCheckBox>(&window, "mapUpstreamErrorsTo502Check");
        QPushButton *start = buttonWithKey(&window, "start_proxy");
        QPushButton *stop = buttonWithKey(&window, "stop_proxy");
        QVERIFY(combo && url && apiKey && userAgent && proxy);
        QVERIFY(upstreamTimeout && firstTokenTimeout && retryAfterOverride &&
                forwardUserAgent && mapErrors && start && stop);
        QVERIFY(url->isReadOnly());
        QVERIFY(apiKey->isReadOnly());
        QVERIFY(userAgent->isReadOnly());
        QVERIFY(proxy->isReadOnly());
        QVERIFY(upstreamTimeout->isReadOnly());
        QVERIFY(firstTokenTimeout->isReadOnly());
        QVERIFY(retryAfterOverride->isReadOnly());
        QVERIFY(!forwardUserAgent->isEnabled());
        QVERIFY(!mapErrors->isEnabled());

        QCOMPARE(combo->currentData().toString(), primary.id);
        QCOMPARE(url->text(), primary.baseUrl);
        QCOMPARE(apiKey->text(), primary.apiKey);
        QCOMPARE(retryAfterOverride->value(), 0);
        QVERIFY(!mapErrors->isChecked());

        const int backupIndex = combo->findData(backup.id);
        QVERIFY(backupIndex >= 0);
        combo->setCurrentIndex(backupIndex);
        QTRY_COMPARE(url->text(), backup.baseUrl);
        QCOMPARE(userAgent->text(), backup.userAgent);
        QCOMPARE(proxy->text(), backup.upstreamProxy);
        QCOMPARE(upstreamTimeout->value(), backup.upstreamTimeoutSec);
        QCOMPARE(firstTokenTimeout->value(), backup.firstTokenTimeoutSec);
        QCOMPARE(retryAfterOverride->value(), 30);
        QVERIFY(forwardUserAgent->isChecked());
        QVERIFY(mapErrors->isChecked());
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
        backup.retryAfterOverrideSec = "30";
        backup.mapUpstreamErrorsTo502 = true;
        QVERIFY2(store.addProfile(&backup, &error), qPrintable(error));
        QVERIFY2(store.setSelectedProfileId(primary.id, &error), qPrintable(error));

        MainWindow window;
        QComboBox *combo = findWindowChild<QComboBox>(&window, "upstreamProfileCombo");
        QLineEdit *url = findWindowChild<QLineEdit>(&window, "upstreamBaseUrlEdit");
        HttpProxyServer *proxy = findWindowChild<HttpProxyServer>(&window);
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

        bool activationRequested = false;
        QTimer::singleShot(0, [&activationRequested, &backup]() {
            QDialog *manager = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!manager) return;
            QTableWidget *table = manager->findChild<QTableWidget *>("profileTable");
            QPushButton *select = manager->findChild<QPushButton *>("profileSelectButton");
            if (!table || !select) {
                manager->reject();
                return;
            }
            for (int row = 0; row < table->rowCount(); ++row) {
                if (table->item(row, 0)->data(Qt::UserRole).toString() == backup.id) {
                    table->selectRow(row);
                    break;
                }
            }
            activationRequested = select->isEnabled();
            select->click();
            manager->accept();
        });
        QVERIFY(QMetaObject::invokeMethod(&window, "openUpstreamProfiles",
                                          Qt::DirectConnection));
        QVERIFY(activationRequested);

        QTRY_COMPARE(combo->currentData().toString(), backup.id);
        QTRY_COMPARE(url->text(), backup.baseUrl);
        QTRY_COMPARE(store.selectedProfileId(&error), backup.id);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QTRY_VERIFY(!store.isProfileLocked(primary.id));
        QTRY_VERIFY(store.isProfileLocked(backup.id));
        QTRY_VERIFY(proxy->isRunning());
        QVERIFY(!start->isEnabled());
        QVERIFY(stop->isEnabled());
        QCOMPARE(proxy->settings().retryAfterOverrideSec, QString("30"));
        QCOMPARE(proxy->settings().mapUpstreamErrorsTo502, true);

        const QByteArray backupResponse = proxyRequest(config.proxyPort);
        QVERIFY2(backupResponse.contains("\"profile\":\"backup\""), backupResponse.constData());
        QTRY_COMPARE(backupUpstream.requestCount(), 1);
        QCOMPARE(backupUpstream.lastAuthorization(), QByteArray("Bearer backup-key"));
        QCOMPARE(primaryUpstream.requestCount(), 1);

        QVERIFY(QMetaObject::invokeMethod(&window, "stopProxy", Qt::DirectConnection));
        QTRY_VERIFY(!proxy->isRunning());
        QVERIFY(!store.isProfileLocked(backup.id));
    }

    void runningIdleProfileRetryAfterEditDoesNotRestartProxy()
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
        UpstreamProfile active;
        active.displayName = "Active";
        active.baseUrl = "https://active.example/v1";
        QVERIFY2(store.addProfile(&active, &error), qPrintable(error));
        UpstreamProfile idle;
        idle.displayName = "Editable Idle";
        idle.baseUrl = "https://idle.example/v1";
        QVERIFY2(store.addProfile(&idle, &error), qPrintable(error));
        QVERIFY2(store.setSelectedProfileId(active.id, &error), qPrintable(error));

        MainWindow window;
        HttpProxyServer *proxy = findWindowChild<HttpProxyServer>(&window);
        QVERIFY(proxy);
        QVERIFY(QMetaObject::invokeMethod(&window, "startProxy", Qt::DirectConnection));
        QTRY_VERIFY(proxy->isRunning());
        QVERIFY(store.isProfileLocked(active.id));
        QVERIFY(!store.isProfileLocked(idle.id));
        QSignalSpy stopped(proxy, SIGNAL(stopped()));
        QSignalSpy started(proxy, SIGNAL(started(QString)));

        bool editorSubmitted = false;
        bool proxyStayedRunningWhileEditing = false;
        QTimer::singleShot(0, [&editorSubmitted, &proxyStayedRunningWhileEditing,
                               &active, &idle, &proxy, &store]() {
            QDialog *manager = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!manager) return;
            QTableWidget *table = manager->findChild<QTableWidget *>("profileTable");
            if (!table) {
                manager->reject();
                return;
            }
            for (int row = 0; row < table->rowCount(); ++row) {
                if (table->item(row, 0)->data(Qt::UserRole).toString() == idle.id) {
                    table->selectRow(row);
                    break;
                }
            }
            QTimer::singleShot(0, [&editorSubmitted, &proxyStayedRunningWhileEditing,
                                   &active, &proxy, &store]() {
                QDialog *editor = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (!editor) return;
                QSpinBox *retryAfter =
                    editor->findChild<QSpinBox *>("profileRetryAfterOverrideSpin");
                QDialogButtonBox *buttons =
                    editor->findChild<QDialogButtonBox *>("profileEditorButtons");
                if (!retryAfter || !buttons || retryAfter->isReadOnly()) {
                    editor->reject();
                    return;
                }
                proxyStayedRunningWhileEditing = proxy->isRunning() &&
                    store.isProfileLocked(active.id);
                retryAfter->setValue(75);
                editorSubmitted = true;
                buttons->button(QDialogButtonBox::Save)->click();
            });
            QMetaObject::invokeMethod(manager, "viewOrEditSelectedProfile",
                                      Qt::DirectConnection);
            manager->accept();
        });
        QVERIFY(QMetaObject::invokeMethod(&window, "openUpstreamProfiles",
                                          Qt::DirectConnection));

        QVERIFY(editorSubmitted);
        QVERIFY(proxyStayedRunningWhileEditing);
        QVERIFY(proxy->isRunning());
        QCOMPARE(stopped.count(), 0);
        QCOMPARE(started.count(), 0);
        QCOMPARE(proxy->settings().retryAfterOverrideSec, QString());
        QVERIFY(store.isProfileLocked(active.id));
        UpstreamProfile updated;
        QVERIFY2(store.profileById(idle.id, &updated, &error), qPrintable(error));
        QCOMPARE(updated.retryAfterOverrideSec, QString("75"));

        QVERIFY(QMetaObject::invokeMethod(&window, "stopProxy", Qt::DirectConnection));
        QTRY_VERIFY(!proxy->isRunning());
        QVERIFY(!store.isProfileLocked(active.id));
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
        QComboBox *combo = findWindowChild<QComboBox>(&window, "upstreamProfileCombo");
        QSpinBox *proxyPort = findWindowChild<QSpinBox>(&window, "proxyPortSpin");
        HttpProxyServer *proxy = findWindowChild<HttpProxyServer>(&window);
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
        QComboBox *combo = findWindowChild<QComboBox>(&window, "upstreamProfileCombo");
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
        QLineEdit *host = findWindowChild<QLineEdit>(&window, "proxyHostEdit");
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
        QComboBox *combo = findWindowChild<QComboBox>(&window, "upstreamProfileCombo");
        QLineEdit *url = findWindowChild<QLineEdit>(&window, "upstreamBaseUrlEdit");
        QVERIFY(combo && url);
        QCOMPARE(combo->count(), 105);
        QCOMPARE(combo->currentData().toString(), selected.id);
        QCOMPARE(url->text(), selected.baseUrl);
        QCOMPARE(store.selectedProfileId(&error), selected.id);
        QVERIFY2(error.isEmpty(), qPrintable(error));
    }
};

int main(int argc, char **argv)
{
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif
    QApplication app(argc, argv);
    MainWindowProfileTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "main_window_profile_test.moc"
