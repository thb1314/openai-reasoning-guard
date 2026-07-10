#include "gui/main_window.h"

#include "gui/app_icon.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonObject>
#include <QtCore/QSize>
#include <QtWidgets/QAction>
#include <QtGui/QClipboard>
#include <QtGui/QIcon>
#include <QtGui/QTextOption>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDesktopWidget>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QStyle>
#include <QtWidgets/QSystemTrayIcon>
#include <QtWidgets/QToolBar>
#include <QtWidgets/QVBoxLayout>

using namespace net_tunnel;

static QLabel *makeI18nLabel(const QString &key, QWidget *parent)
{
    QLabel *label = new QLabel(key, parent);
    label->setProperty("i18n_key", key);
    return label;
}

static QLabel *makeMetricLabel(const QString &key, QWidget *parent)
{
    QLabel *label = makeI18nLabel(key, parent);
    label->setObjectName("metricLabel");
    label->setWordWrap(false);
    label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return label;
}

static QLabel *makeFormLabel(const QString &key, QWidget *parent)
{
    QLabel *label = makeI18nLabel(key, parent);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return label;
}

static QString proxyTextWithDefaultScheme(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || trimmed.contains("://")) {
        return trimmed;
    }
    return QString("http://%1").arg(trimmed);
}

MainWindow::MainWindow(QWidget *parent)
    : QUIWidget(parent),
      configPath_(defaultConfigPath()),
      config_(loadConfig(configPath_)),
      proxy_(this),
      menuBar_(0),
      languageMenu_(0),
      zhAction_(0),
      enAction_(0),
      trayIcon_(0),
      trayMenu_(0),
      trayShowAction_(0),
      trayQuitAction_(0),
      proxyState_(0),
      proxyUrl_(0),
      requestsMetric_(0),
      controlRequestsMetric_(0),
      successMetric_(0),
      failedMetric_(0),
      inFlightMetric_(0),
      guardMatchRateMetric_(0),
      blockedMetric_(0),
      retryMetric_(0),
      latencyMetric_(0),
      uptimeMetric_(0),
      infoText_(0),
      proxyHostEdit_(0),
      proxyPortSpin_(0),
      proxyPrefixEdit_(0),
      upstreamUrlEdit_(0),
      apiKeyEdit_(0),
      userAgentEdit_(0),
      upstreamProxyEdit_(0),
      upstreamTimeoutSpin_(0),
      firstTokenTimeoutSpin_(0),
      bufferTimeoutSpin_(0),
      requestBodyLimitSpin_(0),
      responseBufferLimitSpin_(0),
      streamActionCombo_(0),
      interceptRuleModeCombo_(0),
      reasoning516RetrySpin_(0),
      reasoningEqualsEdit_(0),
      guardEndpointsEdit_(0),
      nonStreamStatusCodeSpin_(0),
      interceptStreamingCheck_(0),
      interceptNonStreamingCheck_(0),
      retryCapacityCheck_(0),
      forwardUserAgentCheck_(0),
      startProxyButton_(0),
      stopProxyButton_(0),
      copyProxyButton_(0),
      saveButton_(0),
      logEdit_(0)
{
    buildUi();
    setupTrayIcon();
    applyStyle();
    loadSettingsToUi();
    retranslateUi();

    connect(&proxy_, SIGNAL(logLine(QString)), this, SLOT(appendLog(QString)));
    connect(&proxy_, SIGNAL(started(QString)), this, SLOT(handleProxyStarted(QString)));
    connect(&proxy_, SIGNAL(stopped()), this, SLOT(handleProxyStopped()));
    connect(&proxy_, SIGNAL(statsChanged()), this, SLOT(updateProxyStats()));
    connect(&statsTimer_, SIGNAL(timeout()), this, SLOT(updateProxyStats()));
    statsTimer_.start(1000);

    setProxyRunningUi(false);
    updateProxyStats();
    refreshInfoPanel();
    appendLog(textFor("log_config_path").arg(configPath_));
}

MainWindow::~MainWindow()
{
    if (trayIcon_) {
        trayIcon_->hide();
    }
    proxy_.stop();
}

void MainWindow::buildUi()
{
    setTitle(textFor("window_title"));
    setAlignment(Qt::AlignCenter);
    setVisible(QUIWidget::BtnMenu, false);
    setBtnWidth(36);
    setTitleHeight(42);
    setWindowIcon(makeAppIcon());
    setPixmap(QUIWidget::Lab_Ico, QStringLiteral(":/app-icon.png"), QSize(22, 22));
    setMinimumSize(980, 580);

    QWidget *root = new QWidget;
    root->setObjectName("rootContent");
    QVBoxLayout *rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(8, 8, 8, 8);
    rootLayout->setSpacing(8);
    rootLayout->setMenuBar(buildMenuBar());
    rootLayout->addWidget(buildHeader());

    QSplitter *splitter = new QSplitter(Qt::Horizontal, root);
    QWidget *left = new QWidget(splitter);
    QVBoxLayout *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(8);
    leftLayout->addWidget(buildProxyPanel(), 1);

    QWidget *right = new QWidget(splitter);
    QVBoxLayout *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(16);
    rightLayout->addWidget(buildRuntimePanel());
    rightLayout->addWidget(buildInfoPanel(), 1);
    rightLayout->addWidget(buildLogPanel(), 1);

    splitter->addWidget(left);
    splitter->addWidget(right);
    splitter->setChildrenCollapsible(false);
    splitter->setStretchFactor(0, 5);
    splitter->setStretchFactor(1, 3);
    splitter->setSizes(QList<int>() << 700 << 430);
    rootLayout->addWidget(splitter, 1);

    setMainWidget(root);
    const QRect available = QApplication::desktop()->availableGeometry(this);
    const int normalWidth = qMin(1080, qMax(980, available.width() - 220));
    const int normalHeight = qMin(680, qMax(580, available.height() - 160));
    resize(normalWidth, normalHeight);
    QUIWidget::setFormInCenter(this);
    setWindowState(Qt::WindowNoState);
}

void MainWindow::setupTrayIcon()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        return;
    }

    const QIcon appIcon(makeAppIcon());
    trayMenu_ = new QMenu(this);
    trayShowAction_ = trayMenu_->addAction(appIcon, QString());
    trayShowAction_->setProperty("i18n_tooltip_key", "tray_show");
    trayQuitAction_ = trayMenu_->addAction(style()->standardIcon(QStyle::SP_DialogCloseButton), QString());
    trayQuitAction_->setProperty("i18n_tooltip_key", "tray_quit");

    trayIcon_ = new QSystemTrayIcon(appIcon, this);
    trayIcon_->setContextMenu(trayMenu_);
    connect(trayShowAction_, SIGNAL(triggered()), this, SLOT(showFromTray()));
    connect(trayQuitAction_, SIGNAL(triggered()), qApp, SLOT(quit()));
    connect(trayIcon_, SIGNAL(activated(QSystemTrayIcon::ActivationReason)),
            this, SLOT(handleTrayActivated(QSystemTrayIcon::ActivationReason)));
    trayIcon_->show();
}

QMenuBar *MainWindow::buildMenuBar()
{
    QMenuBar *bar = new QMenuBar(this);
    bar->setObjectName("customMenuBar");
    bar->setNativeMenuBar(false);
    languageMenu_ = bar->addMenu("");
    zhAction_ = languageMenu_->addAction("");
    enAction_ = languageMenu_->addAction("");
    zhAction_->setCheckable(true);
    enAction_->setCheckable(true);
    connect(zhAction_, SIGNAL(triggered()), this, SLOT(switchToChinese()));
    connect(enAction_, SIGNAL(triggered()), this, SLOT(switchToEnglish()));
    return bar;
}

QWidget *MainWindow::buildHeader()
{
    QGroupBox *box = new QGroupBox(this);
    box->setProperty("i18n_key", "group_runtime_target");
    QGridLayout *layout = new QGridLayout(box);
    layout->setContentsMargins(12, 16, 12, 12);
    layout->setHorizontalSpacing(12);
    layout->setVerticalSpacing(8);

    QLabel *title = new QLabel("OpenAI Reasoning Guard", box);
    title->setObjectName("titleLabel");
    QLabel *subtitle = makeI18nLabel("subtitle", box);
    subtitle->setObjectName("subtitleLabel");

    proxyState_ = new QLabel(box);
    proxyUrl_ = new QLabel("http://127.0.0.1:8010/v1", box);
    proxyUrl_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    layout->addWidget(title, 0, 0, 1, 2);
    layout->addWidget(subtitle, 1, 0, 1, 2);
    layout->addWidget(makeI18nLabel("api_proxy", box), 0, 2);
    layout->addWidget(proxyState_, 0, 3);
    layout->addWidget(makeI18nLabel("proxy_url", box), 2, 0);
    layout->addWidget(proxyUrl_, 2, 1, 1, 3);
    layout->setColumnStretch(1, 2);
    layout->setColumnStretch(3, 1);
    return box;
}

QWidget *MainWindow::buildProxyPanel()
{
    QGroupBox *box = new QGroupBox(this);
    box->setProperty("i18n_key", "group_proxy");
    QVBoxLayout *outer = new QVBoxLayout(box);
    outer->setContentsMargins(12, 16, 12, 12);
    outer->setSpacing(10);

    QWidget *content = new QWidget(box);
    content->setObjectName("proxyFormContent");
    QVBoxLayout *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(10);

    QGridLayout *grid = new QGridLayout();
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(8);
    grid->setColumnMinimumWidth(0, 105);
    grid->setColumnMinimumWidth(1, 145);
    grid->setColumnMinimumWidth(2, 115);
    grid->setColumnMinimumWidth(3, 145);

    proxyHostEdit_ = new QLineEdit(box);
    proxyPortSpin_ = new QSpinBox(box);
    proxyPortSpin_->setRange(1, 65535);
    proxyPrefixEdit_ = new QLineEdit(box);
    upstreamUrlEdit_ = new QLineEdit(box);
    apiKeyEdit_ = new QLineEdit(box);
    apiKeyEdit_->setEchoMode(QLineEdit::Password);
    userAgentEdit_ = new QLineEdit(box);
    upstreamProxyEdit_ = new QLineEdit(box);
    upstreamTimeoutSpin_ = new QSpinBox(box);
    upstreamTimeoutSpin_->setRange(1, 86400);
    upstreamTimeoutSpin_->setMinimumWidth(118);
    firstTokenTimeoutSpin_ = new QSpinBox(box);
    firstTokenTimeoutSpin_->setRange(0, 3600);
    firstTokenTimeoutSpin_->setMinimumWidth(118);
    bufferTimeoutSpin_ = new QSpinBox(box);
    bufferTimeoutSpin_->setRange(1, 86400);
    bufferTimeoutSpin_->setMinimumWidth(118);
    requestBodyLimitSpin_ = new QSpinBox(box);
    requestBodyLimitSpin_->setRange(1, 0x7fffffff);
    requestBodyLimitSpin_->setMinimumWidth(150);
    responseBufferLimitSpin_ = new QSpinBox(box);
    responseBufferLimitSpin_->setRange(1, 0x7fffffff);
    responseBufferLimitSpin_->setMinimumWidth(150);
    streamActionCombo_ = new QComboBox(box);
    streamActionCombo_->addItem("strict_502", "strict_502");
    streamActionCombo_->addItem("disconnect", "disconnect");
    interceptRuleModeCombo_ = new QComboBox(box);
    interceptRuleModeCombo_->addItem("", "reasoning_tokens");
    interceptRuleModeCombo_->addItem("", "final_answer_only_high_xhigh");
    reasoning516RetrySpin_ = new QSpinBox(box);
    reasoning516RetrySpin_->setRange(0, 20);
    reasoning516RetrySpin_->setMinimumWidth(92);
    reasoningEqualsEdit_ = new QLineEdit(box);
    guardEndpointsEdit_ = new QLineEdit(box);
    nonStreamStatusCodeSpin_ = new QSpinBox(box);
    nonStreamStatusCodeSpin_->setRange(100, 599);
    nonStreamStatusCodeSpin_->setMinimumWidth(92);
    interceptStreamingCheck_ = new QCheckBox(box);
    interceptStreamingCheck_->setProperty("i18n_key", "intercept_streaming");
    interceptNonStreamingCheck_ = new QCheckBox(box);
    interceptNonStreamingCheck_->setProperty("i18n_key", "intercept_non_streaming");
    retryCapacityCheck_ = new QCheckBox(box);
    retryCapacityCheck_->setProperty("i18n_key", "retry_upstream_capacity_errors");
    forwardUserAgentCheck_ = new QCheckBox(box);
    forwardUserAgentCheck_->setProperty("i18n_key", "forward_user_agent");

    grid->addWidget(makeFormLabel("listen_host", box), 0, 0);
    grid->addWidget(proxyHostEdit_, 0, 1);
    grid->addWidget(makeFormLabel("listen_port", box), 0, 2);
    grid->addWidget(proxyPortSpin_, 0, 3);
    grid->addWidget(makeFormLabel("path_prefix", box), 1, 0);
    grid->addWidget(proxyPrefixEdit_, 1, 1);
    grid->addWidget(makeFormLabel("upstream_base_url", box), 1, 2);
    grid->addWidget(upstreamUrlEdit_, 1, 3);
    grid->addWidget(makeFormLabel("fallback_api_key", box), 2, 0);
    grid->addWidget(apiKeyEdit_, 2, 1);
    grid->addWidget(makeFormLabel("user_agent", box), 2, 2);
    grid->addWidget(userAgentEdit_, 2, 3);
    grid->addWidget(makeFormLabel("upstream_proxy", box), 3, 0);
    grid->addWidget(upstreamProxyEdit_, 3, 1, 1, 3);
    grid->addWidget(makeFormLabel("upstream_timeout_sec", box), 4, 0);
    grid->addWidget(upstreamTimeoutSpin_, 4, 1);
    grid->addWidget(makeFormLabel("buffer_timeout_sec", box), 4, 2);
    grid->addWidget(bufferTimeoutSpin_, 4, 3);
    grid->addWidget(makeFormLabel("first_token_timeout_sec", box), 5, 0);
    grid->addWidget(firstTokenTimeoutSpin_, 5, 1);
    grid->addWidget(makeFormLabel("request_body_limit_bytes", box), 6, 0);
    grid->addWidget(requestBodyLimitSpin_, 6, 1);
    grid->addWidget(makeFormLabel("response_buffer_limit_bytes", box), 6, 2);
    grid->addWidget(responseBufferLimitSpin_, 6, 3);
    grid->addWidget(makeFormLabel("stream_action", box), 7, 0);
    grid->addWidget(streamActionCombo_, 7, 1, 1, 3);
    grid->addWidget(makeFormLabel("intercept_rule_mode", box), 8, 0);
    grid->addWidget(interceptRuleModeCombo_, 8, 1, 1, 3);
    grid->addWidget(makeFormLabel("guard_values", box), 9, 0);
    grid->addWidget(reasoningEqualsEdit_, 9, 1);
    grid->addWidget(makeFormLabel("guard_retries", box), 9, 2);
    grid->addWidget(reasoning516RetrySpin_, 9, 3);
    grid->addWidget(makeFormLabel("guard_endpoints", box), 10, 0);
    grid->addWidget(guardEndpointsEdit_, 10, 1, 1, 3);
    grid->addWidget(makeFormLabel("block_status_code", box), 11, 0);
    grid->addWidget(nonStreamStatusCodeSpin_, 11, 1);
    grid->addWidget(interceptStreamingCheck_, 12, 0, 1, 2);
    grid->addWidget(interceptNonStreamingCheck_, 12, 2, 1, 2);
    grid->addWidget(retryCapacityCheck_, 13, 0, 1, 2);
    grid->addWidget(forwardUserAgentCheck_, 13, 2, 1, 2);
    grid->setColumnStretch(1, 2);
    grid->setColumnStretch(3, 3);
    contentLayout->addLayout(grid);

    QHBoxLayout *buttons = new QHBoxLayout();
    startProxyButton_ = new QPushButton(box);
    startProxyButton_->setProperty("i18n_key", "start_proxy");
    stopProxyButton_ = new QPushButton(box);
    stopProxyButton_->setProperty("i18n_key", "stop_proxy");
    copyProxyButton_ = new QPushButton(box);
    copyProxyButton_->setProperty("i18n_key", "copy_proxy_url");
    saveButton_ = new QPushButton(box);
    saveButton_->setProperty("i18n_key", "save_config");
    buttons->addWidget(startProxyButton_);
    buttons->addWidget(stopProxyButton_);
    buttons->addWidget(copyProxyButton_);
    buttons->addStretch(1);
    buttons->addWidget(saveButton_);
    contentLayout->addLayout(buttons);
    contentLayout->addStretch(1);

    QScrollArea *scrollArea = new QScrollArea(box);
    scrollArea->setObjectName("proxyScrollArea");
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea->setWidget(content);
    outer->addWidget(scrollArea, 1);

    connect(startProxyButton_, SIGNAL(clicked()), this, SLOT(startProxy()));
    connect(stopProxyButton_, SIGNAL(clicked()), this, SLOT(stopProxy()));
    connect(copyProxyButton_, SIGNAL(clicked()), this, SLOT(copyProxyUrl()));
    connect(saveButton_, SIGNAL(clicked()), this, SLOT(saveSettings()));
    connect(proxyHostEdit_, SIGNAL(textChanged(QString)), this, SLOT(updateProxyStats()));
    connect(proxyPortSpin_, SIGNAL(valueChanged(int)), this, SLOT(updateProxyStats()));
    connect(proxyPrefixEdit_, SIGNAL(textChanged(QString)), this, SLOT(updateProxyStats()));
    connect(upstreamUrlEdit_, SIGNAL(textChanged(QString)), this, SLOT(updateProxyStats()));
    connect(upstreamProxyEdit_, SIGNAL(textChanged(QString)), this, SLOT(updateProxyStats()));
    connect(upstreamTimeoutSpin_, SIGNAL(valueChanged(int)), this, SLOT(updateProxyStats()));
    connect(firstTokenTimeoutSpin_, SIGNAL(valueChanged(int)), this, SLOT(updateProxyStats()));
    connect(bufferTimeoutSpin_, SIGNAL(valueChanged(int)), this, SLOT(updateProxyStats()));
    connect(requestBodyLimitSpin_, SIGNAL(valueChanged(int)), this, SLOT(updateProxyStats()));
    connect(responseBufferLimitSpin_, SIGNAL(valueChanged(int)), this, SLOT(updateProxyStats()));
    connect(streamActionCombo_, SIGNAL(currentIndexChanged(int)), this, SLOT(updateProxyStats()));
    connect(interceptRuleModeCombo_, SIGNAL(currentIndexChanged(int)), this, SLOT(updateProxyStats()));
    connect(reasoningEqualsEdit_, SIGNAL(textChanged(QString)), this, SLOT(updateProxyStats()));
    connect(reasoning516RetrySpin_, SIGNAL(valueChanged(int)), this, SLOT(updateProxyStats()));
    connect(guardEndpointsEdit_, SIGNAL(textChanged(QString)), this, SLOT(updateProxyStats()));
    connect(nonStreamStatusCodeSpin_, SIGNAL(valueChanged(int)), this, SLOT(updateProxyStats()));
    connect(interceptStreamingCheck_, SIGNAL(stateChanged(int)), this, SLOT(updateProxyStats()));
    connect(interceptNonStreamingCheck_, SIGNAL(stateChanged(int)), this, SLOT(updateProxyStats()));
    connect(retryCapacityCheck_, SIGNAL(stateChanged(int)), this, SLOT(updateProxyStats()));
    connect(forwardUserAgentCheck_, SIGNAL(stateChanged(int)), this, SLOT(updateProxyStats()));
    return box;
}

QWidget *MainWindow::buildRuntimePanel()
{
    QGroupBox *box = new QGroupBox(this);
    box->setProperty("i18n_key", "group_live");
    box->setMinimumHeight(122);
    QGridLayout *grid = new QGridLayout(box);
    grid->setContentsMargins(10, 14, 10, 8);
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(2);

    requestsMetric_ = new QLabel("0", box);
    controlRequestsMetric_ = new QLabel("0", box);
    successMetric_ = new QLabel("0", box);
    failedMetric_ = new QLabel("0", box);
    inFlightMetric_ = new QLabel("0", box);
    guardMatchRateMetric_ = new QLabel("0.00%", box);
    blockedMetric_ = new QLabel("0", box);
    retryMetric_ = new QLabel("0", box);
    latencyMetric_ = new QLabel("-", box);
    uptimeMetric_ = new QLabel("-", box);

    QList<QLabel *> values;
    values << requestsMetric_ << controlRequestsMetric_ << successMetric_ << failedMetric_ << inFlightMetric_
           << guardMatchRateMetric_ << blockedMetric_ << retryMetric_ << latencyMetric_ << uptimeMetric_;
    for (int i = 0; i < values.size(); ++i) {
        values.at(i)->setObjectName("metricValue");
        values.at(i)->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        values.at(i)->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }

    grid->addWidget(makeMetricLabel("metric_requests", box), 0, 0);
    grid->addWidget(requestsMetric_, 0, 1);
    grid->addWidget(makeMetricLabel("metric_control_requests", box), 1, 0);
    grid->addWidget(controlRequestsMetric_, 1, 1);
    grid->addWidget(makeMetricLabel("metric_success", box), 2, 0);
    grid->addWidget(successMetric_, 2, 1);
    grid->addWidget(makeMetricLabel("metric_failed", box), 3, 0);
    grid->addWidget(failedMetric_, 3, 1);
    grid->addWidget(makeMetricLabel("metric_in_flight", box), 4, 0);
    grid->addWidget(inFlightMetric_, 4, 1);
    grid->addWidget(makeMetricLabel("metric_guard_match_rate", box), 0, 2);
    grid->addWidget(guardMatchRateMetric_, 0, 3);
    grid->addWidget(makeMetricLabel("metric_guard_blocked", box), 1, 2);
    grid->addWidget(blockedMetric_, 1, 3);
    grid->addWidget(makeMetricLabel("metric_guard_retries", box), 2, 2);
    grid->addWidget(retryMetric_, 2, 3);
    grid->addWidget(makeMetricLabel("metric_avg_latency", box), 3, 2);
    grid->addWidget(latencyMetric_, 3, 3);
    grid->addWidget(makeMetricLabel("metric_uptime", box), 4, 2);
    grid->addWidget(uptimeMetric_, 4, 3);
    grid->setColumnMinimumWidth(0, 70);
    grid->setColumnMinimumWidth(2, 70);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 2);
    grid->setColumnStretch(2, 1);
    grid->setColumnStretch(3, 2);
    return box;
}

QWidget *MainWindow::buildInfoPanel()
{
    QGroupBox *box = new QGroupBox(this);
    box->setProperty("i18n_key", "group_info");
    box->setMinimumHeight(180);
    QVBoxLayout *outer = new QVBoxLayout(box);
    outer->setContentsMargins(12, 16, 12, 28);
    outer->setSpacing(4);

    infoText_ = new QPlainTextEdit(box);
    infoText_->setObjectName("infoTextEdit");
    infoText_->setReadOnly(true);
    infoText_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    infoText_->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    infoText_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    infoText_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    infoText_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    infoText_->setFrameShape(QFrame::NoFrame);
    outer->addWidget(infoText_);
    return box;
}

QWidget *MainWindow::buildLogPanel()
{
    QGroupBox *box = new QGroupBox(this);
    box->setProperty("i18n_key", "group_console");
    QVBoxLayout *layout = new QVBoxLayout(box);
    layout->setContentsMargins(12, 16, 12, 12);

    QHBoxLayout *content = new QHBoxLayout;
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(6);

    logEdit_ = new QPlainTextEdit(box);
    logEdit_->setReadOnly(true);
    logEdit_->setMaximumBlockCount(1200);
    content->addWidget(logEdit_, 1);

    QToolBar *tools = new QToolBar(box);
    tools->setObjectName("consoleToolBar");
    tools->setOrientation(Qt::Vertical);
    tools->setMovable(false);
    tools->setFloatable(false);
    tools->setIconSize(QSize(18, 18));
    tools->setToolButtonStyle(Qt::ToolButtonIconOnly);

    QAction *copyAction = tools->addAction(style()->standardIcon(QStyle::SP_FileDialogDetailedView), QString());
    copyAction->setProperty("i18n_tooltip_key", "console_copy");
    connect(copyAction, SIGNAL(triggered()), this, SLOT(copyConsole()));

    QAction *clearAction = tools->addAction(style()->standardIcon(QStyle::SP_DialogDiscardButton), QString());
    clearAction->setProperty("i18n_tooltip_key", "console_clear");
    connect(clearAction, SIGNAL(triggered()), this, SLOT(clearConsole()));

    content->addWidget(tools);
    layout->addLayout(content);
    return box;
}

void MainWindow::applyStyle()
{
    setStyleSheet(
        "QWidget#rootContent, QWidget#rootContent QWidget { background: #eaf6fd; color: #2c5f87; font-family: 'DejaVu Sans', 'Droid Sans Fallback'; font-size: 13px; }"
        "QMenuBar#customMenuBar { background: transparent; color: #255b82; spacing: 4px; }"
        "QMenuBar#customMenuBar::item { background: transparent; padding: 3px 9px; border-radius: 4px; }"
        "QMenuBar#customMenuBar::item:selected { background: #c7e3f5; }"
        "QMenu { background: #f8fcff; border: 1px solid #bfdcf0; color: #255b82; }"
        "QMenu::item { padding: 5px 28px 5px 18px; }"
        "QMenu::item:selected { background: #dff1fb; }"
        "QWidget#rootContent QGroupBox { border: 1px solid #b7d8ee; border-radius: 5px; margin-top: 8px; background: #eef9ff; }"
        "QWidget#rootContent QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: #2b638f; }"
        "QWidget#rootContent QScrollArea#proxyScrollArea { background: #eef9ff; border: 0; }"
        "QWidget#rootContent QWidget#proxyFormContent { background: #eef9ff; }"
        "QWidget#rootContent QScrollArea#proxyScrollArea QScrollBar:vertical { background: #e1f1fb; border: 0; width: 9px; margin: 0; }"
        "QWidget#rootContent QScrollArea#proxyScrollArea QScrollBar::handle:vertical { background: #9fcce8; border-radius: 4px; min-height: 24px; }"
        "QWidget#rootContent QScrollArea#proxyScrollArea QScrollBar::add-line:vertical, QWidget#rootContent QScrollArea#proxyScrollArea QScrollBar::sub-line:vertical { height: 0; }"
        "QWidget#rootContent QScrollArea#proxyScrollArea QScrollBar::add-page:vertical, QWidget#rootContent QScrollArea#proxyScrollArea QScrollBar::sub-page:vertical { background: transparent; }"
        "QWidget#rootContent QLabel#titleLabel { font-size: 20px; font-weight: 600; color: #174e78; }"
        "QWidget#rootContent QLabel#subtitleLabel { color: #5f86a5; }"
        "QWidget#rootContent QLineEdit, QWidget#rootContent QSpinBox, QWidget#rootContent QComboBox, QWidget#rootContent QPlainTextEdit { background: #f8fcff; border: 1px solid #bfdcf0; border-radius: 4px; padding: 4px 6px; selection-background-color: #8cc5e9; }"
        "QWidget#rootContent QPlainTextEdit { font-family: 'DejaVu Sans Mono', monospace; color: #204d70; }"
        "QWidget#rootContent QPlainTextEdit#infoTextEdit { background: #eef9ff; border: 0; color: #204d70; padding: 0; }"
        "QWidget#rootContent QPlainTextEdit#infoTextEdit QScrollBar:vertical { background: #e1f1fb; border: 0; width: 9px; margin: 0; }"
        "QWidget#rootContent QPlainTextEdit#infoTextEdit QScrollBar::handle:vertical { background: #9fcce8; border-radius: 4px; min-height: 24px; }"
        "QWidget#rootContent QPlainTextEdit#infoTextEdit QScrollBar::add-line:vertical, QWidget#rootContent QPlainTextEdit#infoTextEdit QScrollBar::sub-line:vertical { height: 0; }"
        "QWidget#rootContent QPlainTextEdit#infoTextEdit QScrollBar::add-page:vertical, QWidget#rootContent QPlainTextEdit#infoTextEdit QScrollBar::sub-page:vertical { background: transparent; }"
        "QWidget#rootContent QPushButton { background: #cde7f8; border: 1px solid #accfe6; border-radius: 5px; padding: 6px 12px; color: #255b82; }"
        "QWidget#rootContent QPushButton:hover { background: #dff1fb; }"
        "QWidget#rootContent QPushButton:pressed { background: #b8dbf1; }"
        "QWidget#rootContent QPushButton:disabled { color: #8aabc1; background: #e1edf5; }"
        "QWidget#rootContent QToolBar#consoleToolBar { background: #eef9ff; border: 0; spacing: 5px; }"
        "QWidget#rootContent QToolBar#consoleToolBar QToolButton { background: #cde7f8; border: 1px solid #accfe6; border-radius: 4px; min-width: 28px; min-height: 28px; padding: 2px; color: #255b82; }"
        "QWidget#rootContent QToolBar#consoleToolBar QToolButton:hover { background: #dff1fb; }"
        "QWidget#rootContent QToolBar#consoleToolBar QToolButton:pressed { background: #b8dbf1; }"
        "QWidget#rootContent QLabel[state='ok'] { color: #16824a; font-weight: 600; }"
        "QWidget#rootContent QLabel[state='warn'] { color: #9b6a00; font-weight: 600; }"
        "QWidget#rootContent QLabel[state='bad'] { color: #b13b3b; font-weight: 600; }"
        "QWidget#rootContent QLabel[state='idle'] { color: #5f86a5; font-weight: 600; }"
        "QWidget#rootContent QLabel#metricLabel { font-size: 12px; color: #3d6e93; }"
        "QWidget#rootContent QLabel#metricValue { font-size: 14px; font-weight: 600; color: #174e78; }"
        "QWidget#rootContent QLabel#infoValue { color: #204d70; }"
    );
}

QString MainWindow::currentLanguage() const
{
    const QString lang = config_.lang.trimmed().toLower();
    return lang == "en" ? QString("en") : QString("zh");
}

QString MainWindow::textFor(const QString &key) const
{
    const bool en = currentLanguage() == "en";
    if (key == "window_title") return "OpenAI Reasoning Guard";
    if (key == "window_subtitle") return en ? "Qt + C++11 intelligent OpenAI-compatible proxy" : "Qt + C++11 智能拦截 / OpenAI 兼容转发";
    if (key == "menu_language") return en ? "Language" : "语言";
    if (key == "lang_zh") return en ? "Chinese" : "中文";
    if (key == "lang_en") return en ? "English" : "英文";
    if (key == "group_runtime_target") return en ? "Runtime Target" : "运行目标";
    if (key == "subtitle") return textFor("window_subtitle");
    if (key == "api_proxy") return en ? "API Proxy" : "API 代理";
    if (key == "proxy_url") return en ? "Proxy URL" : "代理地址";
    if (key == "group_proxy") return en ? "Reasoning Guard + OpenAI Proxy" : "智能拦截 + OpenAI 兼容转发";
    if (key == "listen_host") return en ? "Listen Host" : "监听 Host";
    if (key == "listen_port") return en ? "Listen Port" : "监听端口";
    if (key == "path_prefix") return en ? "Path Prefix" : "路径前缀";
    if (key == "upstream_base_url") return en ? "Upstream Base URL" : "上游 Base URL";
    if (key == "fallback_api_key") return en ? "Fallback API Key" : "备用 API Key";
    if (key == "user_agent") return "User-Agent";
    if (key == "upstream_proxy") return en ? "Upstream Proxy" : "上游代理";
    if (key == "upstream_proxy_placeholder") return en
        ? "http://127.0.0.1:7890 or socks5://127.0.0.1:7890"
        : "http://127.0.0.1:7890 或 socks5://127.0.0.1:7890";
    if (key == "upstream_timeout_sec") return en ? "Upstream Timeout (sec)" : "上游超时（秒）";
    if (key == "first_token_timeout_sec") return en ? "First Token Timeout (sec)" : "首 Token 超时（秒）";
    if (key == "buffer_timeout_sec") return en ? "Buffer Timeout (sec)" : "缓冲超时（秒）";
    if (key == "request_body_limit_bytes") return en ? "Request Body Limit" : "请求体上限";
    if (key == "response_buffer_limit_bytes") return en ? "Response Buffer Limit" : "响应缓冲上限";
    if (key == "stream_action") return en ? "Stream Action" : "流式动作";
    if (key == "intercept_rule_mode") return en ? "Rule Mode" : "拦截规则模式";
    if (key == "rule_mode_reasoning_tokens") return en ? "reasoning_tokens (Recommended)" : "reasoning_tokens（推荐）";
    if (key == "rule_mode_final_only") return en ? "final answer only (Experimental)" : "final answer only（实验）";
    if (key == "guard_values") return en ? "Guard Values" : "Guard 规则值";
    if (key == "guard_retries") return en ? "Retry Budget" : "内部重试预算";
    if (key == "guard_endpoints") return en ? "Guard Endpoints" : "Guard 端点";
    if (key == "block_status_code") return en ? "Block Status Code" : "拦截状态码";
    if (key == "intercept_streaming") return en ? "Intercept Streaming" : "拦截流式";
    if (key == "intercept_non_streaming") return en ? "Intercept Non-streaming" : "拦截非流式";
    if (key == "retry_upstream_capacity_errors") return en ? "Retry upstream capacity errors" : "上游 capacity 错误内重试";
    if (key == "forward_user_agent") return en ? "Forward Client User-Agent" : "转发客户端 User-Agent";
    if (key == "start_proxy") return en ? "Start Proxy" : "启动代理";
    if (key == "stop_proxy") return en ? "Stop Proxy" : "停止代理";
    if (key == "copy_proxy_url") return en ? "Copy Proxy URL" : "复制代理地址";
    if (key == "save_config") return en ? "Save Config" : "保存配置";
    if (key == "group_live") return en ? "Live Overview" : "实时概览";
    if (key == "metric_requests") return en ? "Business" : "业务请求";
    if (key == "metric_control_requests") return en ? "Control" : "控制请求";
    if (key == "metric_success") return en ? "Success" : "成功";
    if (key == "metric_failed") return en ? "Failed" : "失败";
    if (key == "metric_in_flight") return en ? "In Flight" : "处理中";
    if (key == "metric_guard_match_rate") return en ? "Match Rate" : "命中率";
    if (key == "metric_guard_blocked") return en ? "Final Block" : "最终阻断";
    if (key == "metric_guard_retries") return en ? "Retries" : "重试次数";
    if (key == "metric_avg_latency") return en ? "Latency" : "平均延迟";
    if (key == "metric_uptime") return en ? "Uptime" : "运行时间";
    if (key == "group_info") return en ? "Info Panel" : "信息面板";
    if (key == "info_listen_url") return en ? "Listen URL" : "监听地址";
    if (key == "info_upstream_url") return en ? "Upstream URL" : "上游地址";
    if (key == "info_prefix") return en ? "Path Prefix" : "路径前缀";
    if (key == "info_control_endpoints") return en ? "Control Endpoints" : "控制端点";
    if (key == "info_control_endpoints_value") return "/status  |  /healthz  |  /version  |  /props";
    if (key == "info_buffer_limits") return en ? "Buffer Limits" : "缓冲上限";
    if (key == "info_first_token_timeout") return en ? "First Token Timeout" : "首 Token 超时";
    if (key == "info_policy") return en ? "Guard Policy" : "拦截策略";
    if (key == "info_rule_mode") return en ? "Rule Mode" : "规则模式";
    if (key == "info_guard_paths") return en ? "Guard Paths" : "拦截路径";
    if (key == "info_guard_values") return en ? "Guard Values" : "命中值";
    if (key == "info_guard_retries") return en ? "Retry Budget" : "重试预算";
    if (key == "info_capacity_retry") return en ? "Capacity Retry" : "capacity 重试";
    if (key == "info_stream_action") return en ? "Stream Action" : "流式动作";
    if (key == "info_enabled") return en ? "enabled" : "开启";
    if (key == "info_disabled") return en ? "disabled" : "关闭";
    if (key == "info_guard_final_status") return en ? "Final Status" : "耗尽返回";
    if (key == "info_upstream_proxy") return en ? "Upstream Proxy" : "上游代理";
    if (key == "info_proxy_unset") return en ? "(unset)" : "（未设置）";
    if (key == "info_line_template") return "%1: %2";
    if (key == "info_section_template") return "%1:";
    if (key == "info_indented_section_template") return "  %1:";
    if (key == "info_indented_line_template") return "  %1: %2";
    if (key == "info_indented_item_template") return "    %1";
    if (key == "group_console") return en ? "Console" : "控制台";
    if (key == "console_copy") return en ? "Copy console" : "复制控制台";
    if (key == "console_clear") return en ? "Clear console" : "清空控制台";
    if (key == "tray_show") return en ? "Show window" : "显示主窗口";
    if (key == "tray_quit") return en ? "Quit" : "退出";
    if (key == "tray_tooltip") return en ? "OpenAI Reasoning Guard" : "OpenAI Reasoning Guard";
    if (key == "state_running") return en ? "Running" : "运行中";
    if (key == "state_stopped") return en ? "Stopped" : "已停止";
    if (key == "error_save_config_failed") return en ? "Save config failed: %1" : "保存配置失败: %1";
    if (key == "log_saved_config") return en ? "saved config: %1" : "已保存配置: %1";
    if (key == "log_proxy_url_copied") return en ? "proxy url copied" : "已复制代理地址";
    if (key == "log_console_copied") return en ? "console copied to clipboard" : "已复制控制台内容";
    if (key == "log_config_path") return "config=%1";
    if (key == "log_error") return en ? "error: %1" : "错误: %1";
    return key;
}

QString MainWindow::infoLine(const QString &labelKey, const QString &value) const
{
    return textFor("info_line_template").arg(textFor(labelKey), value);
}

QString MainWindow::infoSection(const QString &labelKey) const
{
    return textFor("info_section_template").arg(textFor(labelKey));
}

QString MainWindow::infoIndentedSection(const QString &labelKey) const
{
    return textFor("info_indented_section_template").arg(textFor(labelKey));
}

QString MainWindow::infoIndentedLine(const QString &labelKey, const QString &value) const
{
    return textFor("info_indented_line_template").arg(textFor(labelKey), value);
}

QString MainWindow::infoIndentedItem(const QString &value) const
{
    return textFor("info_indented_item_template").arg(value);
}

void MainWindow::retranslateUi()
{
    setTitle(textFor("window_title"));
    if (languageMenu_) {
        languageMenu_->setTitle(textFor("menu_language"));
    }
    if (zhAction_) {
        zhAction_->setText(textFor("lang_zh"));
        zhAction_->setChecked(currentLanguage() == "zh");
    }
    if (enAction_) {
        enAction_->setText(textFor("lang_en"));
        enAction_->setChecked(currentLanguage() == "en");
    }
    if (trayIcon_) {
        trayIcon_->setToolTip(textFor("tray_tooltip"));
    }
    if (upstreamTimeoutSpin_ && firstTokenTimeoutSpin_ && bufferTimeoutSpin_) {
        const QString suffix = currentLanguage() == "en" ? QString(" sec") : QString(" 秒");
        upstreamTimeoutSpin_->setSuffix(suffix);
        firstTokenTimeoutSpin_->setSuffix(suffix);
        bufferTimeoutSpin_->setSuffix(suffix);
    }
    if (requestBodyLimitSpin_ && responseBufferLimitSpin_) {
        const QString suffix = currentLanguage() == "en" ? QString(" bytes") : QString(" 字节");
        requestBodyLimitSpin_->setSuffix(suffix);
        responseBufferLimitSpin_->setSuffix(suffix);
    }
    if (upstreamProxyEdit_) {
        upstreamProxyEdit_->setPlaceholderText(textFor("upstream_proxy_placeholder"));
    }
    if (interceptRuleModeCombo_) {
        interceptRuleModeCombo_->setItemText(0, textFor("rule_mode_reasoning_tokens"));
        interceptRuleModeCombo_->setItemText(1, textFor("rule_mode_final_only"));
    }

    const QList<QWidget *> widgets = findChildren<QWidget *>();
    for (int i = 0; i < widgets.size(); ++i) {
        QWidget *widget = widgets.at(i);
        const QString key = widget->property("i18n_key").toString();
        if (!key.isEmpty()) {
            if (QGroupBox *group = qobject_cast<QGroupBox *>(widget)) {
                group->setTitle(textFor(key));
            } else if (QLabel *label = qobject_cast<QLabel *>(widget)) {
                label->setText(textFor(key));
            } else if (QPushButton *button = qobject_cast<QPushButton *>(widget)) {
                button->setText(textFor(key));
            } else if (QCheckBox *check = qobject_cast<QCheckBox *>(widget)) {
                check->setText(textFor(key));
            }
        }
        const QString tooltipKey = widget->property("i18n_tooltip_key").toString();
        if (!tooltipKey.isEmpty()) {
            widget->setToolTip(textFor(tooltipKey));
        }
    }

    const QList<QAction *> actions = findChildren<QAction *>();
    for (int i = 0; i < actions.size(); ++i) {
        QAction *action = actions.at(i);
        const QString tooltipKey = action->property("i18n_tooltip_key").toString();
        if (!tooltipKey.isEmpty()) {
            const QString text = textFor(tooltipKey);
            action->setText(text);
            action->setToolTip(text);
        }
    }
    setProxyRunningUi(proxy_.isRunning());
    refreshInfoPanel();
}

void MainWindow::setLanguage(const QString &lang)
{
    const QString normalized = lang == "en" ? QString("en") : QString("zh");
    if (config_.lang == normalized) {
        retranslateUi();
        return;
    }
    config_ = collectConfigFromUi();
    config_.lang = normalized;
    QString error;
    saveConfig(config_, configPath_, &error);
    retranslateUi();
}

void MainWindow::switchToChinese()
{
    setLanguage("zh");
}

void MainWindow::switchToEnglish()
{
    setLanguage("en");
}

void MainWindow::showFromTray()
{
    show();
    if (isMinimized()) {
        showNormal();
    }
    raise();
    activateWindow();
}

void MainWindow::handleTrayActivated(QSystemTrayIcon::ActivationReason reason)
{
    if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
        showFromTray();
    }
}

void MainWindow::loadSettingsToUi()
{
    proxyHostEdit_->setText(config_.proxyHost);
    proxyPortSpin_->setValue(config_.proxyPort);
    proxyPrefixEdit_->setText(config_.proxyPrefix);
    upstreamUrlEdit_->setText(config_.upstreamBaseUrl);
    apiKeyEdit_->setText(config_.upstreamApiKey);
    userAgentEdit_->setText(config_.upstreamUserAgent);
    upstreamProxyEdit_->setText(config_.upstreamProxy);
    upstreamTimeoutSpin_->setValue(config_.upstreamTimeoutSec);
    firstTokenTimeoutSpin_->setValue(config_.firstTokenTimeoutSec);
    bufferTimeoutSpin_->setValue(config_.bufferTimeoutSec);
    requestBodyLimitSpin_->setValue(int(qMin(config_.requestBodyLimitBytes, qint64(0x7fffffff))));
    responseBufferLimitSpin_->setValue(int(qMin(config_.responseBufferLimitBytes, qint64(0x7fffffff))));
    const int streamActionIndex = streamActionCombo_->findData(config_.streamAction);
    streamActionCombo_->setCurrentIndex(streamActionIndex >= 0 ? streamActionIndex : 0);
    const int ruleIndex = interceptRuleModeCombo_->findData(config_.interceptRuleMode);
    interceptRuleModeCombo_->setCurrentIndex(ruleIndex >= 0 ? ruleIndex : 0);
    reasoningEqualsEdit_->setText(config_.reasoningEquals);
    reasoning516RetrySpin_->setValue(config_.guardRetryAttempts);
    retryCapacityCheck_->setChecked(config_.retryUpstreamCapacityErrors);
    guardEndpointsEdit_->setText(config_.guardEndpoints);
    nonStreamStatusCodeSpin_->setValue(config_.nonStreamStatusCode);
    interceptStreamingCheck_->setChecked(config_.interceptStreaming);
    interceptNonStreamingCheck_->setChecked(config_.interceptNonStreaming);
    forwardUserAgentCheck_->setChecked(config_.forwardUserAgent);
    proxyUrl_->setText(QString("http://%1:%2%3")
        .arg(config_.proxyHost)
        .arg(config_.proxyPort)
        .arg(config_.proxyPrefix.isEmpty() ? QString("/") : config_.proxyPrefix));
}

AppConfig MainWindow::collectConfigFromUi() const
{
    AppConfig config = config_;
    config.proxyHost = proxyHostEdit_->text().trimmed();
    config.proxyPort = proxyPortSpin_->value();
    config.proxyPrefix = proxyPrefixEdit_->text().trimmed();
    config.upstreamBaseUrl = upstreamUrlEdit_->text().trimmed();
    config.upstreamApiKey = apiKeyEdit_->text().trimmed();
    config.upstreamUserAgent = userAgentEdit_->text().trimmed();
    config.forwardUserAgent = forwardUserAgentCheck_->isChecked();
    config.upstreamProxy = proxyTextWithDefaultScheme(upstreamProxyEdit_->text());
    config.upstreamTimeoutSec = upstreamTimeoutSpin_->value();
    config.firstTokenTimeoutSec = firstTokenTimeoutSpin_->value();
    config.bufferTimeoutSec = bufferTimeoutSpin_->value();
    config.requestBodyLimitBytes = requestBodyLimitSpin_->value();
    config.responseBufferLimitBytes = responseBufferLimitSpin_->value();
    config.streamAction = selectedStreamAction();
    config.interceptRuleMode = selectedInterceptRuleMode();
    config.reasoningEquals = reasoningEqualsEdit_->text().trimmed();
    config.guardRetryAttempts = reasoning516RetrySpin_->value();
    config.reasoning516RetryCount = config.guardRetryAttempts;
    config.retryUpstreamCapacityErrors = retryCapacityCheck_->isChecked();
    config.guardEndpoints = guardEndpointsEdit_->text().trimmed();
    config.nonStreamStatusCode = nonStreamStatusCodeSpin_->value();
    config.interceptStreaming = interceptStreamingCheck_->isChecked();
    config.interceptNonStreaming = interceptNonStreamingCheck_->isChecked();
    return config;
}

ProxySettings MainWindow::collectProxySettings() const
{
    ProxySettings settings;
    settings.listenHost = proxyHostEdit_->text().trimmed();
    settings.listenPort = proxyPortSpin_->value();
    settings.proxyPrefix = proxyPrefixEdit_->text().trimmed();
    settings.upstreamBaseUrl = upstreamUrlEdit_->text().trimmed();
    settings.upstreamApiKey = apiKeyEdit_->text().trimmed();
    settings.upstreamUserAgent = userAgentEdit_->text().trimmed();
    settings.forwardUserAgent = forwardUserAgentCheck_->isChecked();
    settings.upstreamProxy = proxyTextWithDefaultScheme(upstreamProxyEdit_->text());
    settings.upstreamHttpProxy = config_.upstreamHttpProxy;
    settings.upstreamHttpsProxy = config_.upstreamHttpsProxy;
    settings.upstreamSocksProxy = config_.upstreamSocksProxy;
    settings.upstreamTimeoutSec = upstreamTimeoutSpin_->value();
    settings.firstTokenTimeoutSec = firstTokenTimeoutSpin_->value();
    settings.bufferTimeoutSec = bufferTimeoutSpin_->value();
    settings.requestBodyLimitBytes = requestBodyLimitSpin_->value();
    settings.responseBufferLimitBytes = responseBufferLimitSpin_->value();
    settings.interceptRuleMode = selectedInterceptRuleMode();
    settings.reasoningEquals = normalizeIntegerList(reasoningEqualsEdit_->text().trimmed(), defaultReasoningEquals());
    settings.guardRetryAttempts = reasoning516RetrySpin_->value();
    settings.reasoning516RetryCount = settings.guardRetryAttempts;
    settings.retryUpstreamCapacityErrors = retryCapacityCheck_->isChecked();
    settings.guardEndpoints = normalizePathList(guardEndpointsEdit_->text().trimmed(), defaultGuardEndpoints());
    settings.nonStreamStatusCode = nonStreamStatusCodeSpin_->value();
    settings.interceptStreaming = interceptStreamingCheck_->isChecked();
    settings.interceptNonStreaming = interceptNonStreamingCheck_->isChecked();
    settings.streamAction = selectedStreamAction();
    return settings;
}

QString MainWindow::selectedInterceptRuleMode() const
{
    const QString value = interceptRuleModeCombo_
        ? interceptRuleModeCombo_->currentData().toString().trimmed()
        : QString();
    return value == "final_answer_only_high_xhigh" ? value : QString("reasoning_tokens");
}

QString MainWindow::selectedStreamAction() const
{
    const QString value = streamActionCombo_
        ? streamActionCombo_->currentData().toString().trimmed()
        : QString();
    return value == "disconnect" ? value : QString("strict_502");
}

void MainWindow::startProxy()
{
    QString error;
    if (!proxy_.start(collectProxySettings(), &error)) {
        handleFailure(error);
        return;
    }
}

void MainWindow::stopProxy()
{
    proxy_.stop();
}

void MainWindow::saveSettings()
{
    config_ = collectConfigFromUi();
    QString error;
    if (!saveConfig(config_, configPath_, &error)) {
        handleFailure(textFor("error_save_config_failed").arg(error));
        return;
    }
    appendLog(textFor("log_saved_config").arg(configPath_));
}

void MainWindow::copyProxyUrl()
{
    QApplication::clipboard()->setText(proxyUrl_->text());
    appendLog(textFor("log_proxy_url_copied"));
}

void MainWindow::copyConsole()
{
    if (!logEdit_) {
        return;
    }
    QApplication::clipboard()->setText(logEdit_->toPlainText());
    appendLog(textFor("log_console_copied"));
}

void MainWindow::clearConsole()
{
    if (logEdit_) {
        logEdit_->clear();
    }
}

void MainWindow::appendLog(const QString &line)
{
    if (!logEdit_) {
        return;
    }
    logEdit_->appendPlainText(QString("[%1] %2")
        .arg(QTime::currentTime().toString("HH:mm:ss"))
        .arg(line));
}

void MainWindow::updateProxyStats()
{
    refreshInfoPanel();
    QJsonObject runtime = proxy_.statusPayload().value("runtime").toObject();
    if (proxy_.isRunning() || runtime.value("requests_total").toDouble(0.0) > 0.0) {
        lastRuntimeSnapshot_ = runtime;
    } else if (!lastRuntimeSnapshot_.isEmpty()) {
        runtime = lastRuntimeSnapshot_;
    }
    requestsMetric_->setText(displayJsonNumber(runtime, "intercepted_requests_total"));
    controlRequestsMetric_->setText(displayJsonNumber(runtime, "control_requests_total"));
    successMetric_->setText(displayJsonNumber(runtime, "successful_requests_total"));
    failedMetric_->setText(displayJsonNumber(runtime, "failed_requests_total"));
    inFlightMetric_->setText(displayJsonNumber(runtime, "in_flight_proxy_requests"));
    const double guardMatchRate = runtime.value("guard_match_rate").toDouble(0.0);
    guardMatchRateMetric_->setText(QString::number(guardMatchRate * 100.0, 'f', 2) + "%");
    blockedMetric_->setText(displayJsonNumber(runtime, "blocked_response_count"));
    retryMetric_->setText(displayJsonNumber(runtime, "guard_retry_total"));
    const double avg = runtime.value("avg_latency_ms").toDouble(-1.0);
    latencyMetric_->setText(avg >= 0.0 ? QString::number(avg, 'f', 1) + " ms" : "-");
    const double uptime = runtime.value("uptime_sec").toDouble(0.0);
    uptimeMetric_->setText(proxy_.isRunning() ? QString::number(uptime, 'f', 0) + " s" : "-");
}

void MainWindow::handleProxyStarted(const QString &url)
{
    proxyUrl_->setText(url);
    setProxyRunningUi(true);
    refreshInfoPanel();
    updateProxyStats();
}

void MainWindow::handleProxyStopped()
{
    setProxyRunningUi(false);
    refreshInfoPanel();
    updateProxyStats();
}

void MainWindow::handleFailure(const QString &message)
{
    appendLog(textFor("log_error").arg(message));
    QMessageBox::warning(this, textFor("window_title"), message);
}

void MainWindow::setProxyRunningUi(bool running)
{
    setStatus(proxyState_, running ? textFor("state_running") : textFor("state_stopped"), running ? "ok" : "idle");
    startProxyButton_->setEnabled(!running);
    stopProxyButton_->setEnabled(running);
    copyProxyButton_->setEnabled(running);
}

void MainWindow::setStatus(QLabel *label, const QString &text, const QString &state)
{
    if (!label) {
        return;
    }
    label->setText(text);
    label->setProperty("state", state);
    label->style()->unpolish(label);
    label->style()->polish(label);
}

void MainWindow::refreshInfoPanel()
{
    if (!infoText_) {
        return;
    }
    const ProxySettings settings = collectProxySettings();
    QString prefix = normalizePathPrefix(settings.proxyPrefix);
    if (prefix.isEmpty()) {
        prefix = "/";
    }
    const QString listenUrl = proxy_.isRunning()
        ? proxy_.listenUrl()
        : QString("http://%1:%2%3").arg(settings.listenHost).arg(settings.listenPort).arg(prefix);

    QStringList lines;
    lines << infoLine("info_listen_url", listenUrl);
    lines << infoLine("info_upstream_url", settings.upstreamBaseUrl);
    lines << infoLine("info_prefix", prefix);
    lines << infoLine("info_control_endpoints", textFor("info_control_endpoints_value"));
    lines << infoLine("info_upstream_proxy", settings.upstreamProxy.isEmpty() ? textFor("info_proxy_unset") : settings.upstreamProxy);
    lines << infoLine("info_buffer_limits", QString("request=%1, response=%2")
        .arg(settings.requestBodyLimitBytes)
        .arg(settings.responseBufferLimitBytes));
    lines << infoLine("info_first_token_timeout", QString::number(settings.firstTokenTimeoutSec) +
        (currentLanguage() == "en" ? QString(" sec") : QString(" 秒")));
    lines << infoSection("info_policy");
    lines << infoIndentedLine("info_rule_mode", settings.interceptRuleMode);
    lines << infoIndentedLine("info_stream_action", settings.streamAction);
    lines << infoIndentedSection("info_guard_paths");
    for (int i = 0; i < settings.guardEndpoints.size(); ++i) {
        lines << infoIndentedItem(settings.guardEndpoints.at(i));
    }
    lines << infoIndentedLine("info_guard_values", QString("[%1]").arg(settings.reasoningEquals.join(",")));
    lines << infoIndentedLine("info_guard_retries", QString::number(settings.guardRetryAttempts));
    lines << infoIndentedLine("info_capacity_retry", settings.retryUpstreamCapacityErrors ? textFor("info_enabled") : textFor("info_disabled"));
    lines << infoIndentedLine("info_guard_final_status", QString::number(settings.nonStreamStatusCode));
    const QString text = lines.join("\n");
    if (infoText_->toPlainText() != text) {
        QScrollBar *bar = infoText_->verticalScrollBar();
        const int oldValue = bar ? bar->value() : 0;
        const bool wasAtBottom = bar && oldValue >= bar->maximum();
        infoText_->setPlainText(text);
        if (bar) {
            bar->setValue(wasAtBottom ? bar->maximum() : qMin(oldValue, bar->maximum()));
        }
    }
    if (!proxy_.isRunning()) {
        proxyUrl_->setText(listenUrl);
    }
}

QString MainWindow::displayJsonNumber(const QJsonObject &object, const QString &key) const
{
    const QJsonValue value = object.value(key);
    if (value.isDouble()) {
        return QString::number(qint64(value.toDouble()));
    }
    return "0";
}
