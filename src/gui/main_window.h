#pragma once

#include "core/app_config.h"
#include "core/http_proxy_server.h"
#include "quiwidget.h"

#include <QtCore/QJsonObject>
#include <QtCore/QTimer>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>

class QAction;
class QCheckBox;
class QComboBox;
class QGroupBox;
class QMenu;
class QMenuBar;

class MainWindow : public QUIWidget {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = 0);
    ~MainWindow();

private slots:
    void startProxy();
    void stopProxy();
    void saveSettings();
    void copyProxyUrl();
    void appendLog(const QString &line);
    void updateProxyStats();
    void handleProxyStarted(const QString &url);
    void handleProxyStopped();
    void handleFailure(const QString &message);
    void switchToChinese();
    void switchToEnglish();

private:
    void buildUi();
    QMenuBar *buildMenuBar();
    QWidget *buildHeader();
    QWidget *buildProxyPanel();
    QWidget *buildRuntimePanel();
    QWidget *buildInfoPanel();
    QWidget *buildLogPanel();
    void applyStyle();
    void retranslateUi();
    QString textFor(const QString &key) const;
    QString infoLine(const QString &labelKey, const QString &value) const;
    QString infoSection(const QString &labelKey) const;
    QString infoIndentedSection(const QString &labelKey) const;
    QString infoIndentedLine(const QString &labelKey, const QString &value) const;
    QString infoIndentedItem(const QString &value) const;
    QString currentLanguage() const;
    void setLanguage(const QString &lang);
    void loadSettingsToUi();
    net_tunnel::AppConfig collectConfigFromUi() const;
    net_tunnel::ProxySettings collectProxySettings() const;
    void setProxyRunningUi(bool running);
    void setStatus(QLabel *label, const QString &text, const QString &state);
    void refreshInfoPanel();
    QString displayJsonNumber(const QJsonObject &object, const QString &key) const;
    QString selectedInterceptRuleMode() const;
    QString selectedStreamAction() const;

    QString configPath_;
    net_tunnel::AppConfig config_;
    net_tunnel::HttpProxyServer proxy_;
    QTimer statsTimer_;

    QMenuBar *menuBar_;
    QMenu *languageMenu_;
    QAction *zhAction_;
    QAction *enAction_;

    QLabel *proxyState_;
    QLabel *proxyUrl_;
    QLabel *requestsMetric_;
    QLabel *successMetric_;
    QLabel *failedMetric_;
    QLabel *blockedMetric_;
    QLabel *retryMetric_;
    QLabel *latencyMetric_;
    QLabel *uptimeMetric_;
    QPlainTextEdit *infoText_;

    QLineEdit *proxyHostEdit_;
    QSpinBox *proxyPortSpin_;
    QLineEdit *proxyPrefixEdit_;
    QLineEdit *upstreamUrlEdit_;
    QLineEdit *apiKeyEdit_;
    QLineEdit *userAgentEdit_;
    QLineEdit *upstreamProxyEdit_;
    QSpinBox *upstreamTimeoutSpin_;
    QSpinBox *bufferTimeoutSpin_;
    QSpinBox *requestBodyLimitSpin_;
    QSpinBox *responseBufferLimitSpin_;
    QComboBox *streamActionCombo_;
    QComboBox *interceptRuleModeCombo_;
    QSpinBox *reasoning516RetrySpin_;
    QLineEdit *reasoningEqualsEdit_;
    QLineEdit *guardEndpointsEdit_;
    QSpinBox *nonStreamStatusCodeSpin_;
    QCheckBox *interceptStreamingCheck_;
    QCheckBox *interceptNonStreamingCheck_;
    QCheckBox *retryCapacityCheck_;
    QCheckBox *forwardUserAgentCheck_;
    QPushButton *startProxyButton_;
    QPushButton *stopProxyButton_;
    QPushButton *copyProxyButton_;
    QPushButton *saveButton_;

    QPlainTextEdit *logEdit_;
    QJsonObject lastRuntimeSnapshot_;
};
