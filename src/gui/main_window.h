#pragma once

#include "core/app_config.h"
#include "core/http_proxy_server.h"
#include "core/upstream_profile.h"
#include "quiwidget.h"

#include <QtCore/QJsonObject>
#include <QtCore/QHash>
#include <QtCore/QMargins>
#include <QtCore/QPointer>
#include <QtCore/QPoint>
#include <QtCore/QRect>
#include <QtCore/QSize>
#include <QtCore/QTimer>
#include <QtGui/QFont>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSystemTrayIcon>

class QAction;
class QCheckBox;
class QComboBox;
class QEvent;
class QGroupBox;
class QMenu;
class QMenuBar;
class QScrollBar;
class QShowEvent;
class QWidget;

class MainWindow : public QUIWidget {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = 0);
    ~MainWindow();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;

private slots:
    void startProxy();
    void stopProxy();
    void saveSettings();
    void copyProxyUrl();
    void copyConsole();
    void clearConsole();
    void appendLog(const QString &line);
    void updateProxyStats();
    void handleProxyStarted(const QString &url);
    void handleProxyStopped();
    void restartProxyAfterUpstreamProfileSwitch();
    void handleFailure(const QString &message);
    void switchToChinese();
    void switchToEnglish();
    void openUpstreamProfiles();
    void openInterfaceSettings();
    void showAboutDialog();
    void handleUpstreamProfileChanged(int index);
    void showFromTray();
    void handleTrayActivated(QSystemTrayIcon::ActivationReason reason);

private:
    void buildUi();
    void setupTrayIcon();
    QMenuBar *buildMenuBar();
    QWidget *buildHeader();
    QWidget *buildProxyPanel();
    QWidget *buildRuntimePanel();
    QWidget *buildInfoPanel();
    QWidget *buildLogPanel();
    void applyStyle();
    Qt::Edges resizeEdgesAt(const QPoint &position) const;
    void updateResizeCursor(Qt::Edges edges, QWidget *target = 0);
    void restoreResizeCursor();
    void continueManualResize(const QPoint &globalPosition);
    void finishManualResize(bool applyGeometry,
                            bool keepCursorSuppressed = false);
    void cancelResizeGesture(bool keepCursorSuppressed = false);
    bool canStartResizeFrom(QWidget *target, const QPoint &globalPosition) const;
    bool isAtMaximumGeometry() const;
    void captureUiScaleBaseline();
    void applyUiScale(qreal scale);
    qreal configuredUiScale(int pointSize) const;
    int defaultUiFontPointSize() const;
    void updateTitleAppIcon();
    void updateMinimumSizeForCurrentScreen();
    void constrainGeometryToCurrentScreen();
    QRect currentAvailableGeometry() const;
    void connectWindowScreenSignals();
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
    bool initializeUpstreamProfiles();
    void refreshUpstreamProfiles();
    void applyCurrentUpstreamProfile();
    void clearCurrentUpstreamProfile();
    bool selectUpstreamProfile(const QString &id, QString *error = 0);
    void restoreCurrentUpstreamProfileSelection();
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
    QAction *manageUpstreamProfilesAction_;
    QAction *interfaceSettingsAction_;
    QMenu *languageMenu_;
    QAction *zhAction_;
    QAction *enAction_;
    QAction *aboutAction_;
    QSystemTrayIcon *trayIcon_;
    QMenu *trayMenu_;
    QAction *trayShowAction_;
    QAction *trayQuitAction_;

    QLabel *proxyState_;
    QLabel *proxyUrl_;
    QLabel *requestsMetric_;
    QLabel *controlRequestsMetric_;
    QLabel *successMetric_;
    QLabel *failedMetric_;
    QLabel *inFlightMetric_;
    QLabel *guardMatchRateMetric_;
    QLabel *blockedMetric_;
    QLabel *retryMetric_;
    QLabel *latencyMetric_;
    QLabel *uptimeMetric_;
    QPlainTextEdit *infoText_;

    QLineEdit *proxyHostEdit_;
    QSpinBox *proxyPortSpin_;
    QLineEdit *proxyPrefixEdit_;
    QComboBox *upstreamProfileCombo_;
    QLineEdit *upstreamUrlEdit_;
    QLineEdit *apiKeyEdit_;
    QLineEdit *userAgentEdit_;
    QLineEdit *upstreamProxyEdit_;
    QSpinBox *upstreamTimeoutSpin_;
    QSpinBox *firstTokenTimeoutSpin_;
    QSpinBox *retryAfterOverrideSpin_;
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
    QCheckBox *mapUpstreamErrorsTo502Check_;
    QPushButton *startProxyButton_;
    QPushButton *stopProxyButton_;
    QPushButton *copyProxyButton_;
    QPushButton *saveButton_;

    QPlainTextEdit *logEdit_;
    QWidget *rootContent_;
    QJsonObject lastRuntimeSnapshot_;

    net_tunnel::UpstreamProfileStore *upstreamProfileStore_;
    net_tunnel::UpstreamProfileRunLock *upstreamProfileRunLock_;
    net_tunnel::UpstreamProfile currentUpstreamProfile_;
    net_tunnel::UpstreamProfile pendingUpstreamProfile_;
    bool legacyUpstreamMigrationComplete_;
    bool upstreamProfilesReady_;
    bool hasCurrentUpstreamProfile_;
    bool hasPendingUpstreamProfileSwitch_;
    bool upstreamProfileSwitchRestartPending_;
    bool resizeGestureActive_;
    bool suppressResizeCursor_;
    bool resizeCursorOverrideActive_;
    Qt::CursorShape resizeCursorShape_;
    QPointer<QWidget> resizeCursorTarget_;
    bool manualResizing_;
    Qt::Edges manualResizeEdges_;
    QPoint manualResizeStartGlobal_;
    QRect manualResizeStartGeometry_;
    QRect manualResizePreviewGeometry_;
    QWidget *resizeOutline_;
    QHash<QWidget *, QFont> baseTextControlFonts_;
    QHash<QWidget *, QSize> baseMinimumSizes_;
    QHash<QWidget *, QSize> baseMaximumSizes_;
    QHash<QWidget *, int> baseInteractiveHeights_;
    QHash<QScrollBar *, int> baseVerticalScrollBarWidths_;
    QHash<QScrollBar *, int> baseHorizontalScrollBarHeights_;
    QSize baselineWindowSize_;
    QSize baselineMinimumSize_;
    QSize baseTitleIconSize_;
    int baseTitleHeight_;
    int baseChromeExtent_;
    qreal baseUiFontPointSize_;
    qreal uiScale_;
    bool screenSignalsConnected_;
};
