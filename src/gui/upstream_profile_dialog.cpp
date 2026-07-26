#include "gui/upstream_profile_dialog.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSignalBlocker>
#include <QtGui/QClipboard>
#include <QtGui/QFont>
#include <QtGui/QIcon>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QPalette>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStyle>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>

using net_tunnel::OverwriteImportConflicts;
using net_tunnel::SkipImportConflicts;
using net_tunnel::SortByBaseUrl;
using net_tunnel::SortByDisplayName;
using net_tunnel::SortByUpdatedAt;
using net_tunnel::UpstreamProfile;
using net_tunnel::UpstreamProfileImportResult;
using net_tunnel::UpstreamProfilePage;
using net_tunnel::UpstreamProfileStore;
using net_tunnel_gui::GuardDialog;
using net_tunnel_gui::GuardFileDialog;
using net_tunnel_gui::GuardMessageButton;
using net_tunnel_gui::GuardQuestion;
using net_tunnel_gui::chooseGuardMessage;
using net_tunnel_gui::confirmGuardMessage;
using net_tunnel_gui::showGuardInformation;
using net_tunnel_gui::showGuardWarning;

namespace {

QIcon themedIcon(QWidget *widget, const QString &name, QStyle::StandardPixmap fallback)
{
    const QIcon icon = QIcon::fromTheme(name);
    return icon.isNull() ? widget->style()->standardIcon(fallback) : icon;
}

QIcon apiKeyVisibilityIcon(const QWidget *widget, bool hidden)
{
    QPixmap pixmap(18, 18);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    QColor color = widget->palette().color(QPalette::ButtonText);
    if (!color.isValid()) color = QColor("#255b82");
    QPen pen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    QPainterPath outline;
    outline.moveTo(1.5, 9.0);
    outline.cubicTo(3.5, 5.3, 5.9, 3.5, 9.0, 3.5);
    outline.cubicTo(12.1, 3.5, 14.5, 5.3, 16.5, 9.0);
    outline.cubicTo(14.5, 12.7, 12.1, 14.5, 9.0, 14.5);
    outline.cubicTo(5.9, 14.5, 3.5, 12.7, 1.5, 9.0);
    painter.drawPath(outline);
    painter.setBrush(color);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(QPointF(9.0, 9.0), 2.2, 2.2);

    if (hidden) {
        painter.setPen(pen);
        painter.drawLine(QPointF(2.5, 2.5), QPointF(15.5, 15.5));
    }
    return QIcon(pixmap);
}

QString normalizedBaseUrl(const QString &value)
{
    QString result = value.trimmed();
    const int schemeEnd = result.indexOf("://");
    while (result.endsWith('/') && result.length() > schemeEnd + 3) {
        result.chop(1);
    }
    return result;
}

class UpstreamProfileEditor : public GuardDialog {
public:
    UpstreamProfileEditor(const UpstreamProfile &profile,
                          bool editable,
                          const QString &language,
                          UpstreamProfileStore *store,
                          QWidget *parent)
        : GuardDialog(parent),
          editable_(editable),
          english_(language.trimmed().toLower() == "en"),
          store_(store),
          original_(profile),
          nameEdit_(0),
          baseUrlEdit_(0),
          apiKeyEdit_(0),
          userAgentEdit_(0),
          forwardUserAgentCheck_(0),
          proxyEdit_(0),
          upstreamTimeoutSpin_(0),
          firstTokenTimeoutSpin_(0),
          revealButton_(0),
          copyButton_(0),
          buttons_(0)
    {
        buildUi();
        load(profile);
    }

    UpstreamProfile profile() const
    {
        UpstreamProfile result = original_;
        result.displayName = nameEdit_->text().trimmed();
        result.baseUrl = normalizedBaseUrl(baseUrlEdit_->text());
        result.apiKey = apiKeyEdit_->text();
        result.userAgent = userAgentEdit_->text().trimmed();
        result.forwardUserAgent = forwardUserAgentCheck_->isChecked();
        result.upstreamProxy = proxyEdit_->text().trimmed();
        result.upstreamTimeoutSec = upstreamTimeoutSpin_->value();
        result.firstTokenTimeoutSec = firstTokenTimeoutSpin_->value();
        return result;
    }

private:
    QString trText(const QString &zh, const QString &en) const
    {
        return english_ ? en : zh;
    }

    QLabel *requiredLabel(const QString &text)
    {
        QLabel *label = new QLabel(text + " *", this);
        label->setToolTip(trText("必填", "Required"));
        return label;
    }

    void buildUi()
    {
        setObjectName("upstreamProfileEditor");
        setModal(true);
        setWindowTitle(editable_
            ? trText("编辑上游配置", "Edit Upstream Profile")
            : trText("查看上游配置", "View Upstream Profile"));
        setMinimumWidth(620);

        QVBoxLayout *root = contentLayout();
        root->setContentsMargins(16, 16, 16, 14);
        root->setSpacing(14);

        QFormLayout *form = new QFormLayout;
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        form->setHorizontalSpacing(16);
        form->setVerticalSpacing(10);

        nameEdit_ = new QLineEdit(this);
        nameEdit_->setObjectName("profileNameEdit");
        nameEdit_->setMaxLength(120);
        nameEdit_->setPlaceholderText(trText("例如：主线路", "For example: Primary"));

        baseUrlEdit_ = new QLineEdit(this);
        baseUrlEdit_->setObjectName("profileBaseUrlEdit");
        baseUrlEdit_->setPlaceholderText("https://api.example.com/v1");

        apiKeyEdit_ = new QLineEdit(this);
        apiKeyEdit_->setObjectName("profileApiKeyEdit");
        apiKeyEdit_->setEchoMode(QLineEdit::Password);
        apiKeyEdit_->setPlaceholderText(trText(
            "留空时透传客户端 Authorization",
            "Empty: forward the client Authorization header"));
        apiKeyEdit_->setToolTip(trText(
            "API Key 以明文保存在当前用户的 SQLite 数据库中，留空表示透传客户端授权。",
            "The API key is stored as plaintext in the current user's SQLite database. Empty means client authorization passthrough."));

        revealButton_ = new QToolButton(this);
        revealButton_->setObjectName("profileRevealApiKeyButton");
        revealButton_->setCheckable(true);
        revealButton_->setIcon(apiKeyVisibilityIcon(revealButton_, true));
        revealButton_->setToolTip(trText("显示 API Key", "Reveal API key"));
        revealButton_->setAutoRaise(false);

        copyButton_ = new QToolButton(this);
        copyButton_->setObjectName("profileCopyApiKeyButton");
        copyButton_->setIcon(themedIcon(this, "edit-copy", QStyle::SP_DialogSaveButton));
        copyButton_->setToolTip(trText("复制 API Key", "Copy API key"));
        copyButton_->setAutoRaise(false);

        QWidget *apiKeyRow = new QWidget(this);
        QHBoxLayout *apiKeyLayout = new QHBoxLayout(apiKeyRow);
        apiKeyLayout->setContentsMargins(0, 0, 0, 0);
        apiKeyLayout->setSpacing(6);
        apiKeyLayout->addWidget(apiKeyEdit_, 1);
        apiKeyLayout->addWidget(revealButton_);
        apiKeyLayout->addWidget(copyButton_);

        userAgentEdit_ = new QLineEdit(this);
        userAgentEdit_->setObjectName("profileUserAgentEdit");
        userAgentEdit_->setPlaceholderText("curl/8.7.1");

        forwardUserAgentCheck_ = new QCheckBox(
            trText("优先转发客户端 User-Agent", "Prefer the client User-Agent"), this);
        forwardUserAgentCheck_->setObjectName("profileForwardUserAgentCheck");

        proxyEdit_ = new QLineEdit(this);
        proxyEdit_->setObjectName("profileProxyEdit");
        proxyEdit_->setPlaceholderText(trText(
            "留空表示直连；例如 http://127.0.0.1:7890",
            "Empty means direct; for example http://127.0.0.1:7890"));

        upstreamTimeoutSpin_ = new QSpinBox(this);
        upstreamTimeoutSpin_->setObjectName("profileUpstreamTimeoutSpin");
        upstreamTimeoutSpin_->setRange(1, 86400);
        upstreamTimeoutSpin_->setSuffix(trText(" 秒", " sec"));

        firstTokenTimeoutSpin_ = new QSpinBox(this);
        firstTokenTimeoutSpin_->setObjectName("profileFirstTokenTimeoutSpin");
        firstTokenTimeoutSpin_->setRange(0, 3600);
        firstTokenTimeoutSpin_->setSuffix(trText(" 秒", " sec"));
        firstTokenTimeoutSpin_->setSpecialValueText(trText("禁用", "Disabled"));

        form->addRow(requiredLabel(trText("显示名称", "Display name")), nameEdit_);
        form->addRow(requiredLabel("Base URL"), baseUrlEdit_);
        form->addRow("API Key", apiKeyRow);
        form->addRow("User-Agent", userAgentEdit_);
        form->addRow("", forwardUserAgentCheck_);
        form->addRow(trText("上游代理", "Upstream proxy"), proxyEdit_);
        form->addRow(trText("上游超时", "Upstream timeout"), upstreamTimeoutSpin_);
        form->addRow(trText("首 Token 超时", "First-token timeout"), firstTokenTimeoutSpin_);
        root->addLayout(form);

        buttons_ = new QDialogButtonBox(this);
        buttons_->setObjectName("profileEditorButtons");
        if (editable_) {
            buttons_->setStandardButtons(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
            buttons_->button(QDialogButtonBox::Save)->setText(trText("保存", "Save"));
            buttons_->button(QDialogButtonBox::Cancel)->setText(trText("取消", "Cancel"));
            QObject::connect(buttons_->button(QDialogButtonBox::Save), &QPushButton::clicked,
                             [this]() { attemptAccept(); });
            QObject::connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
        } else {
            buttons_->setStandardButtons(QDialogButtonBox::Close);
            buttons_->button(QDialogButtonBox::Close)->setText(trText("关闭", "Close"));
            QObject::connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
        }
        root->addWidget(buttons_);

        QObject::connect(revealButton_, &QToolButton::toggled, [this](bool checked) {
            apiKeyEdit_->setEchoMode(checked ? QLineEdit::Normal : QLineEdit::Password);
            revealButton_->setIcon(apiKeyVisibilityIcon(revealButton_, !checked));
            revealButton_->setToolTip(checked
                ? trText("隐藏 API Key", "Hide API key")
                : trText("显示 API Key", "Reveal API key"));
        });
        QObject::connect(copyButton_, &QToolButton::clicked, [this]() {
            QApplication::clipboard()->setText(apiKeyEdit_->text());
        });
    }

    void load(const UpstreamProfile &profile)
    {
        nameEdit_->setText(profile.displayName);
        baseUrlEdit_->setText(profile.baseUrl);
        apiKeyEdit_->setText(profile.apiKey);
        userAgentEdit_->setText(profile.userAgent);
        forwardUserAgentCheck_->setChecked(profile.forwardUserAgent);
        proxyEdit_->setText(profile.upstreamProxy);
        upstreamTimeoutSpin_->setValue(profile.upstreamTimeoutSec);
        firstTokenTimeoutSpin_->setValue(profile.firstTokenTimeoutSec);

        nameEdit_->setReadOnly(!editable_);
        baseUrlEdit_->setReadOnly(!editable_);
        apiKeyEdit_->setReadOnly(!editable_);
        userAgentEdit_->setReadOnly(!editable_);
        forwardUserAgentCheck_->setEnabled(editable_);
        proxyEdit_->setReadOnly(!editable_);
        upstreamTimeoutSpin_->setReadOnly(!editable_);
        upstreamTimeoutSpin_->setButtonSymbols(editable_ ? QAbstractSpinBox::UpDownArrows
                                                         : QAbstractSpinBox::NoButtons);
        firstTokenTimeoutSpin_->setReadOnly(!editable_);
        firstTokenTimeoutSpin_->setButtonSymbols(editable_ ? QAbstractSpinBox::UpDownArrows
                                                           : QAbstractSpinBox::NoButtons);
    }

    void attemptAccept()
    {
        UpstreamProfile candidate = profile();
        QString field;
        QString error;
        if (!net_tunnel::validateUpstreamProfile(candidate, &field, &error)) {
            QWidget *focus = 0;
            if (field == "display_name" || field == "displayName") focus = nameEdit_;
            else if (field == "base_url" || field == "baseUrl") focus = baseUrlEdit_;
            else if (field == "upstream_proxy" || field == "upstreamProxy") focus = proxyEdit_;
            else if (field == "upstream_timeout_sec") focus = upstreamTimeoutSpin_;
            else if (field == "first_token_timeout_sec") focus = firstTokenTimeoutSpin_;
            if (focus) focus->setFocus();
            QString message = error;
            if (field == "display_name" || field == "displayName") {
                message = trText("显示名称不能为空。", "Display name is required.");
            } else if (field == "base_url" || field == "baseUrl") {
                message = trText(
                    "Base URL 必须是包含主机名的完整 http 或 https 地址，且不能包含查询参数或片段。",
                    "Base URL must be a complete HTTP or HTTPS URL with a host and no query or fragment.");
            } else if (field == "upstream_proxy" || field == "upstreamProxy") {
                message = trText("请填写有效的 HTTP 或 SOCKS5 代理地址，或留空表示直连。",
                                 "Enter a valid HTTP or SOCKS5 proxy URL, or leave it empty for direct access.");
            } else if (field == "upstream_timeout_sec") {
                message = trText("上游超时必须在 1 到 86400 秒之间。",
                                 "Upstream timeout must be between 1 and 86400 seconds.");
            } else if (field == "first_token_timeout_sec") {
                message = trText("首 Token 超时必须在 0 到 3600 秒之间。",
                                 "First-token timeout must be between 0 and 3600 seconds.");
            }
            showGuardWarning(this,
                             trText("无法保存", "Cannot Save"),
                             message.isEmpty()
                                 ? trText("请检查必填项和 URL 格式。", "Check the required fields and URL format.")
                                 : message,
                             trText("确定", "OK"));
            return;
        }
        if (store_) {
            QString storeError;
            const bool saved = candidate.id.isEmpty()
                ? store_->addProfile(&candidate, &storeError)
                : store_->updateProfile(candidate, &storeError);
            if (!saved) {
                showGuardWarning(this, trText("无法保存", "Cannot Save"),
                                 storeError.isEmpty()
                                     ? trText("上游配置保存失败。", "Failed to save the upstream profile.")
                                     : storeError,
                                 trText("确定", "OK"));
                return;
            }
            original_ = candidate;
        }
        accept();
    }

    bool editable_;
    bool english_;
    UpstreamProfileStore *store_;
    UpstreamProfile original_;
    QLineEdit *nameEdit_;
    QLineEdit *baseUrlEdit_;
    QLineEdit *apiKeyEdit_;
    QLineEdit *userAgentEdit_;
    QCheckBox *forwardUserAgentCheck_;
    QLineEdit *proxyEdit_;
    QSpinBox *upstreamTimeoutSpin_;
    QSpinBox *firstTokenTimeoutSpin_;
    QToolButton *revealButton_;
    QToolButton *copyButton_;
    QDialogButtonBox *buttons_;
};

} // namespace

UpstreamProfileDialog::UpstreamProfileDialog(UpstreamProfileStore *store,
                                             const QString &language,
                                             const QString &activeProfileId,
                                             bool proxyRunning,
                                             QWidget *parent)
    : GuardDialog(parent),
      store_(store),
      language_(language.trimmed().toLower() == "en" ? "en" : "zh"),
      activeProfileId_(activeProfileId),
      proxyRunning_(proxyRunning),
      currentPage_(1),
      pageSize_(20),
      totalPages_(0),
      sortField_(SortByUpdatedAt),
      sortOrder_(Qt::DescendingOrder),
      searchEdit_(0),
      table_(0),
      addButton_(0),
      viewEditButton_(0),
      removeButton_(0),
      selectButton_(0),
      importButton_(0),
      exportButton_(0),
      firstPageButton_(0),
      previousPageButton_(0),
      nextPageButton_(0),
      lastPageButton_(0),
      closeButton_(0),
      pageSizeCombo_(0),
      pageLabel_(0),
      summaryLabel_(0)
{
    buildUi();
    retranslateUi();
    loadPage();
}

QString UpstreamProfileDialog::selectedProfileId() const
{
    return selectedProfileId_;
}

void UpstreamProfileDialog::setLanguage(const QString &language)
{
    language_ = language.trimmed().toLower() == "en" ? "en" : "zh";
    retranslateUi();
    loadPage(currentRowProfileId());
}

void UpstreamProfileDialog::setRuntimeState(bool proxyRunning, const QString &activeProfileId)
{
    proxyRunning_ = proxyRunning;
    activeProfileId_ = activeProfileId;
    updateActionStates();
}

void UpstreamProfileDialog::refresh()
{
    loadPage(currentRowProfileId());
}

void UpstreamProfileDialog::buildUi()
{
    setObjectName("upstreamProfileDialog");
    setModal(true);
    setWindowModality(Qt::WindowModal);
    setMinimumSize(880, 560);
    resize(1080, 680);

    QVBoxLayout *root = contentLayout();
    root->setContentsMargins(14, 14, 14, 12);
    root->setSpacing(10);

    QHBoxLayout *searchRow = new QHBoxLayout;
    searchRow->setSpacing(8);
    QLabel *searchLabel = new QLabel(this);
    searchLabel->setObjectName("profileSearchLabel");
    searchEdit_ = new QLineEdit(this);
    searchEdit_->setObjectName("profileSearchEdit");
    searchEdit_->setClearButtonEnabled(true);
    searchEdit_->setMinimumWidth(260);
    searchRow->addWidget(searchLabel);
    searchRow->addWidget(searchEdit_, 1);

    addButton_ = new QPushButton(this);
    addButton_->setObjectName("profileAddButton");
    addButton_->setIcon(themedIcon(this, "list-add", QStyle::SP_FileDialogNewFolder));
    viewEditButton_ = new QPushButton(this);
    viewEditButton_->setObjectName("profileViewEditButton");
    viewEditButton_->setIcon(themedIcon(this, "document-edit", QStyle::SP_FileDialogDetailedView));
    removeButton_ = new QPushButton(this);
    removeButton_->setObjectName("profileRemoveButton");
    removeButton_->setIcon(themedIcon(this, "edit-delete", QStyle::SP_TrashIcon));
    selectButton_ = new QPushButton(this);
    selectButton_->setObjectName("profileSelectButton");
    selectButton_->setIcon(themedIcon(this, "emblem-default", QStyle::SP_DialogApplyButton));
    searchRow->addWidget(addButton_);
    searchRow->addWidget(viewEditButton_);
    searchRow->addWidget(removeButton_);
    searchRow->addWidget(selectButton_);
    root->addLayout(searchRow);

    table_ = new QTableWidget(this);
    table_->setObjectName("profileTable");
    table_->setColumnCount(5);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->setShowGrid(false);
    table_->setSortingEnabled(false);
    table_->setStyleSheet("QTableView::item { padding-left: 6px; padding-right: 6px; }");
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(34);
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setSectionsClickable(true);
    table_->horizontalHeader()->setSortIndicatorShown(true);
    table_->horizontalHeader()->setSortIndicator(4, Qt::DescendingOrder);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    root->addWidget(table_, 1);

    QHBoxLayout *bottomRow = new QHBoxLayout;
    bottomRow->setSpacing(7);
    importButton_ = new QPushButton(this);
    importButton_->setObjectName("profileImportButton");
    importButton_->setIcon(themedIcon(this, "document-open", QStyle::SP_DialogOpenButton));
    exportButton_ = new QPushButton(this);
    exportButton_->setObjectName("profileExportButton");
    exportButton_->setIcon(themedIcon(this, "document-save-as", QStyle::SP_DialogSaveButton));
    bottomRow->addWidget(importButton_);
    bottomRow->addWidget(exportButton_);
    summaryLabel_ = new QLabel(this);
    summaryLabel_->setObjectName("profileSummaryLabel");
    bottomRow->addWidget(summaryLabel_);
    bottomRow->addStretch(1);

    firstPageButton_ = new QPushButton(this);
    firstPageButton_->setObjectName("profileFirstPageButton");
    firstPageButton_->setIcon(themedIcon(this, "go-first", QStyle::SP_MediaSkipBackward));
    firstPageButton_->setFixedSize(34, 30);
    previousPageButton_ = new QPushButton(this);
    previousPageButton_->setObjectName("profilePreviousPageButton");
    previousPageButton_->setIcon(themedIcon(this, "go-previous", QStyle::SP_ArrowBack));
    previousPageButton_->setFixedSize(34, 30);
    pageLabel_ = new QLabel(this);
    pageLabel_->setObjectName("profilePageLabel");
    pageLabel_->setMinimumWidth(100);
    pageLabel_->setAlignment(Qt::AlignCenter);
    nextPageButton_ = new QPushButton(this);
    nextPageButton_->setObjectName("profileNextPageButton");
    nextPageButton_->setIcon(themedIcon(this, "go-next", QStyle::SP_ArrowForward));
    nextPageButton_->setFixedSize(34, 30);
    lastPageButton_ = new QPushButton(this);
    lastPageButton_->setObjectName("profileLastPageButton");
    lastPageButton_->setIcon(themedIcon(this, "go-last", QStyle::SP_MediaSkipForward));
    lastPageButton_->setFixedSize(34, 30);
    pageSizeCombo_ = new QComboBox(this);
    pageSizeCombo_->setObjectName("profilePageSizeCombo");
    pageSizeCombo_->addItem("10", 10);
    pageSizeCombo_->addItem("20", 20);
    pageSizeCombo_->addItem("50", 50);
    pageSizeCombo_->addItem("100", 100);
    pageSizeCombo_->setCurrentIndex(1);

    bottomRow->addWidget(firstPageButton_);
    bottomRow->addWidget(previousPageButton_);
    bottomRow->addWidget(pageLabel_);
    bottomRow->addWidget(nextPageButton_);
    bottomRow->addWidget(lastPageButton_);
    bottomRow->addWidget(pageSizeCombo_);

    closeButton_ = new QPushButton(this);
    closeButton_->setObjectName("profileCloseButton");
    closeButton_->setIcon(themedIcon(this, "window-close", QStyle::SP_DialogCloseButton));
    bottomRow->addWidget(closeButton_);
    root->addLayout(bottomRow);

    connect(searchEdit_, SIGNAL(textChanged(QString)), this, SLOT(applySearch()));
    connect(addButton_, SIGNAL(clicked()), this, SLOT(addProfile()));
    connect(viewEditButton_, SIGNAL(clicked()), this, SLOT(viewOrEditSelectedProfile()));
    connect(removeButton_, SIGNAL(clicked()), this, SLOT(removeSelectedProfile()));
    connect(selectButton_, SIGNAL(clicked()), this, SLOT(selectCurrentProfile()));
    connect(importButton_, SIGNAL(clicked()), this, SLOT(importProfiles()));
    connect(exportButton_, SIGNAL(clicked()), this, SLOT(exportProfiles()));
    connect(firstPageButton_, SIGNAL(clicked()), this, SLOT(firstPage()));
    connect(previousPageButton_, SIGNAL(clicked()), this, SLOT(previousPage()));
    connect(nextPageButton_, SIGNAL(clicked()), this, SLOT(nextPage()));
    connect(lastPageButton_, SIGNAL(clicked()), this, SLOT(lastPage()));
    connect(pageSizeCombo_, SIGNAL(currentIndexChanged(int)), this, SLOT(changePageSize(int)));
    connect(table_->horizontalHeader(), SIGNAL(sectionClicked(int)), this, SLOT(changeSort(int)));
    connect(table_, SIGNAL(itemSelectionChanged()), this, SLOT(updateActionStates()));
    connect(table_, SIGNAL(cellDoubleClicked(int,int)),
            this, SLOT(viewOrEditSelectedProfile()));
    connect(closeButton_, SIGNAL(clicked()), this, SLOT(accept()));
}

QString UpstreamProfileDialog::textFor(const QString &key) const
{
    const bool en = language_ == "en";
    if (key == "title") return en ? "Upstream Profiles" : "上游配置";
    if (key == "search") return en ? "Search" : "搜索";
    if (key == "search_placeholder") return en ? "Display name or Base URL" : "显示名称或 Base URL";
    if (key == "add") return en ? "Add" : "新增";
    if (key == "edit") return en ? "Edit" : "编辑";
    if (key == "view") return en ? "View" : "查看";
    if (key == "remove") return en ? "Delete" : "删除";
    if (key == "select") return en ? "Set Current" : "设为当前";
    if (key == "import") return en ? "Import" : "导入";
    if (key == "export") return en ? "Export" : "导出";
    if (key == "close") return en ? "Close" : "关闭";
    if (key == "name") return en ? "Display Name" : "显示名称";
    if (key == "base_url") return "Base URL";
    if (key == "authorization") return en ? "Authorization" : "授权方式";
    if (key == "proxy") return en ? "Upstream Proxy" : "上游代理";
    if (key == "updated") return en ? "Updated" : "更新时间";
    if (key == "api_key_configured") return en ? "API key configured" : "已配置 API Key";
    if (key == "authorization_passthrough") return en ? "Client passthrough" : "透传客户端授权";
    if (key == "direct") return en ? "Direct" : "直连";
    if (key == "page") return en ? "Page %1 / %2" : "第 %1 / %2 页";
    if (key == "summary") return en ? "%1 profile(s)" : "共 %1 条";
    if (key == "page_size") return en ? "%1 per page" : "每页 %1 条";
    if (key == "first_page_tip") return en ? "First page" : "首页";
    if (key == "previous_page_tip") return en ? "Previous page" : "上一页";
    if (key == "next_page_tip") return en ? "Next page" : "下一页";
    if (key == "last_page_tip") return en ? "Last page" : "末页";
    if (key == "locked_tip") return en
        ? "The proxy is using this profile. Stop the proxy before editing or deleting it."
        : "代理正在使用此配置；停止代理后才能编辑或删除。";
    if (key == "selection_locked_tip") return en
        ? "Stop all proxy instances before changing the current profile."
        : "停止所有代理实例后才能切换当前配置。";
    if (key == "no_selection") return en ? "Select a profile first." : "请先选择一条上游配置。";
    if (key == "operation_failed") return en ? "%1 failed" : "%1失败";
    if (key == "delete_title") return en ? "Delete Upstream Profile" : "删除上游配置";
    if (key == "delete_confirm") return en
        ? "Delete upstream profile \"%1\"? This cannot be undone."
        : "确定删除上游配置“%1”吗？此操作无法撤销。";
    if (key == "import_title") return en ? "Import Upstream Profiles" : "导入上游配置";
    if (key == "export_title") return en ? "Export Upstream Profiles" : "导出上游配置";
    if (key == "json_filter") return en ? "JSON files (*.json);;All files (*)" : "JSON 文件 (*.json);;所有文件 (*)";
    if (key == "import_conflict") return en
        ? "How should profiles with a matching UUID or display name be handled?"
        : "遇到 UUID 或显示名称冲突时如何处理？";
    if (key == "skip") return en ? "Skip Conflicts" : "跳过冲突项";
    if (key == "overwrite") return en ? "Overwrite" : "覆盖已有项";
    if (key == "cancel") return en ? "Cancel" : "取消";
    if (key == "import_complete") return en ? "Import Complete" : "导入完成";
    if (key == "import_summary") return en
        ? "Added: %1\nUpdated: %2\nSkipped: %3"
        : "新增：%1\n更新：%2\n跳过：%3";
    if (key == "secret_choice") return en
        ? "Export API keys? Exports without secrets are safer for sharing."
        : "是否导出 API Key？不含密钥的文件更适合分享。";
    if (key == "without_secrets") return en ? "Without API Keys" : "不包含 API Key";
    if (key == "with_secrets") return en ? "Include API Keys" : "包含 API Key";
    if (key == "secret_warning_title") return en ? "Confirm Plaintext Export" : "确认导出明文密钥";
    if (key == "secret_warning") return en
        ? "The export will contain plaintext API keys. Anyone with this file can use them. Continue?"
        : "导出文件将包含明文 API Key，任何获得该文件的人都可以使用这些密钥。是否继续？";
    if (key == "export_complete") return en ? "Export Complete" : "导出完成";
    if (key == "export_complete_message") return en ? "Profiles exported to:\n%1" : "上游配置已导出到：\n%1";
    if (key == "current") return en ? "Current profile" : "当前配置";
    if (key == "add_operation") return en ? "Add profile" : "新增配置";
    if (key == "update_operation") return en ? "Update profile" : "更新配置";
    if (key == "delete_operation") return en ? "Delete profile" : "删除配置";
    if (key == "select_operation") return en ? "Select profile" : "选择配置";
    if (key == "load_operation") return en ? "Load profiles" : "加载配置";
    if (key == "import_operation") return en ? "Import profiles" : "导入配置";
    if (key == "export_operation") return en ? "Export profiles" : "导出配置";
    return key;
}

void UpstreamProfileDialog::retranslateUi()
{
    setWindowTitle(textFor("title"));
    QLabel *searchLabel = findChild<QLabel *>("profileSearchLabel");
    if (searchLabel) searchLabel->setText(textFor("search"));
    searchEdit_->setPlaceholderText(textFor("search_placeholder"));
    addButton_->setText(textFor("add"));
    removeButton_->setText(textFor("remove"));
    selectButton_->setText(textFor("select"));
    importButton_->setText(textFor("import"));
    exportButton_->setText(textFor("export"));
    closeButton_->setText(textFor("close"));
    firstPageButton_->setToolTip(textFor("first_page_tip"));
    previousPageButton_->setToolTip(textFor("previous_page_tip"));
    nextPageButton_->setToolTip(textFor("next_page_tip"));
    lastPageButton_->setToolTip(textFor("last_page_tip"));

    QStringList headers;
    headers << textFor("name") << textFor("base_url") << textFor("authorization")
            << textFor("proxy") << textFor("updated");
    table_->setHorizontalHeaderLabels(headers);
    for (int i = 0; i < pageSizeCombo_->count(); ++i) {
        pageSizeCombo_->setItemText(i, textFor("page_size").arg(pageSizeCombo_->itemData(i).toInt()));
    }
    updateActionStates();
}

void UpstreamProfileDialog::loadPage(const QString &preferredRowId)
{
    if (!store_ || !store_->isOpen()) {
        table_->setRowCount(0);
        pageLabel_->setText(textFor("page").arg(0).arg(0));
        summaryLabel_->setText(textFor("summary").arg(0));
        updateActionStates();
        return;
    }

    QString error;
    UpstreamProfilePage result;
    if (!store_->listProfiles(searchEdit_->text().trimmed(), currentPage_, pageSize_,
                              sortField_, sortOrder_, &result, &error)) {
        showStoreError(textFor("load_operation"), error);
        return;
    }
    if (result.totalPages > 0 && currentPage_ > result.totalPages) {
        currentPage_ = result.totalPages;
        if (!store_->listProfiles(searchEdit_->text().trimmed(), currentPage_, pageSize_,
                                  sortField_, sortOrder_, &result, &error)) {
            showStoreError(textFor("load_operation"), error);
            return;
        }
    }

    const bool hasPreferredRow = !preferredRowId.isEmpty();
    const QString beforeSelection = hasPreferredRow ? preferredRowId : currentRowProfileId();
    selectedProfileId_ = store_->selectedProfileId(&error);
    if (!error.isEmpty()) {
        showStoreError(textFor("load_operation"), error);
        return;
    }

    QSignalBlocker blocker(table_);
    table_->clearContents();
    table_->setRowCount(result.items.size());
    int rowToSelect = -1;
    for (int row = 0; row < result.items.size(); ++row) {
        const UpstreamProfile &profile = result.items.at(row);
        const bool current = profile.id == selectedProfileId_;
        const bool locked = profileIsLocked(profile.id);

        QTableWidgetItem *name = new QTableWidgetItem(profile.displayName);
        name->setData(Qt::UserRole, profile.id);
        if (current) {
            QFont font = name->font();
            font.setBold(true);
            name->setFont(font);
            name->setIcon(themedIcon(this, "emblem-default", QStyle::SP_DialogApplyButton));
            name->setToolTip(textFor("current"));
        }
        if (locked) {
            const QString existing = name->toolTip();
            name->setToolTip(existing.isEmpty() ? textFor("locked_tip")
                                                 : existing + "\n" + textFor("locked_tip"));
        }

        QTableWidgetItem *baseUrl = new QTableWidgetItem(profile.baseUrl);
        baseUrl->setToolTip(profile.baseUrl);
        QTableWidgetItem *authorization = new QTableWidgetItem(
            profile.apiKey.isEmpty() ? textFor("authorization_passthrough")
                                     : textFor("api_key_configured"));
        QTableWidgetItem *proxy = new QTableWidgetItem(
            profile.upstreamProxy.isEmpty() ? textFor("direct") : profile.upstreamProxy);
        proxy->setToolTip(profile.upstreamProxy);
        QTableWidgetItem *updated = new QTableWidgetItem(
            profile.updatedAtUtc.isValid()
                ? profile.updatedAtUtc.toLocalTime().toString("yyyy-MM-dd HH:mm:ss")
                : QString());
        updated->setData(Qt::UserRole, profile.updatedAtUtc);

        table_->setItem(row, 0, name);
        table_->setItem(row, 1, baseUrl);
        table_->setItem(row, 2, authorization);
        table_->setItem(row, 3, proxy);
        table_->setItem(row, 4, updated);
        if (profile.id == beforeSelection) rowToSelect = row;
    }

    currentPage_ = result.totalPages == 0 ? 1 : result.page;
    totalPages_ = result.totalPages;
    pageLabel_->setText(textFor("page").arg(result.totalPages == 0 ? 0 : currentPage_)
                                      .arg(totalPages_));
    summaryLabel_->setText(textFor("summary").arg(result.totalItems));
    if (rowToSelect < 0 && !hasPreferredRow && table_->rowCount() > 0) rowToSelect = 0;
    if (rowToSelect >= 0) table_->selectRow(rowToSelect);
    updateActionStates();
}

void UpstreamProfileDialog::loadPageContaining(const QString &profileId)
{
    if (profileId.isEmpty() || !store_ || !store_->isOpen()) {
        loadPage(profileId);
        return;
    }

    const int originalPage = currentPage_;
    int page = 1;
    int totalPages = 1;
    QString error;
    while (page <= totalPages) {
        UpstreamProfilePage result;
        if (!store_->listProfiles(searchEdit_->text().trimmed(), page, pageSize_,
                                  sortField_, sortOrder_, &result, &error)) {
            showStoreError(textFor("load_operation"), error);
            return;
        }
        totalPages = result.totalPages;
        for (int i = 0; i < result.items.size(); ++i) {
            if (result.items.at(i).id == profileId) {
                currentPage_ = page;
                loadPage(profileId);
                return;
            }
        }
        ++page;
    }

    currentPage_ = originalPage;
    loadPage(profileId);
}

QString UpstreamProfileDialog::currentRowProfileId() const
{
    const int row = table_ ? table_->currentRow() : -1;
    QTableWidgetItem *item = row >= 0 ? table_->item(row, 0) : 0;
    return item ? item->data(Qt::UserRole).toString() : QString();
}

bool UpstreamProfileDialog::currentRowProfile(UpstreamProfile *profile) const
{
    if (!profile || !store_) return false;
    const QString id = currentRowProfileId();
    if (id.isEmpty()) return false;
    QString error;
    if (!store_->profileById(id, profile, &error)) {
        const_cast<UpstreamProfileDialog *>(this)->showStoreError(textFor("load_operation"), error);
        return false;
    }
    return true;
}

bool UpstreamProfileDialog::profileIsLocked(const QString &profileId) const
{
    if (profileId.isEmpty()) return false;
    if (proxyRunning_ && profileId == activeProfileId_) return true;
    return store_ && store_->isProfileLocked(profileId);
}

void UpstreamProfileDialog::showStoreError(const QString &operation, const QString &error)
{
    showGuardWarning(this, textFor("operation_failed").arg(operation),
                     error.isEmpty() ? textFor("operation_failed").arg(operation) : error,
                     language_ == "en" ? "OK" : "确定");
}

void UpstreamProfileDialog::addProfile()
{
    UpstreamProfile profile;
    profile.userAgent = "curl/8.7.1";
    profile.forwardUserAgent = false;
    profile.upstreamProxy.clear();
    profile.upstreamTimeoutSec = 1800;
    profile.firstTokenTimeoutSec = 30;
    UpstreamProfileEditor editor(profile, true, language_, store_, this);
    if (editor.exec() != QDialog::Accepted) return;

    profile = editor.profile();
    QString error;
    if (selectedProfileId_.isEmpty() && !store_->isSelectionLocked()) {
        if (!store_->setSelectedProfileId(profile.id, &error)) {
            showStoreError(textFor("select_operation"), error);
        } else {
            selectedProfileId_ = profile.id;
            emit selectedProfileChanged(profile.id);
        }
    }
    loadPageContaining(profile.id);
}

void UpstreamProfileDialog::viewOrEditSelectedProfile()
{
    UpstreamProfile profile;
    if (!currentRowProfile(&profile)) {
        showGuardInformation(this, textFor("title"), textFor("no_selection"),
                             language_ == "en" ? "OK" : "确定");
        return;
    }
    const bool editable = !profileIsLocked(profile.id);
    UpstreamProfileEditor editor(profile, editable, language_, editable ? store_ : 0, this);
    const int result = editor.exec();
    if (!editable || result != QDialog::Accepted) {
        return;
    }

    const UpstreamProfile updated = editor.profile();
    loadPageContaining(updated.id);
    if (updated.id == selectedProfileId_) emit selectedProfileChanged(updated.id);
}

void UpstreamProfileDialog::removeSelectedProfile()
{
    UpstreamProfile profile;
    if (!currentRowProfile(&profile)) {
        showGuardInformation(this, textFor("title"), textFor("no_selection"),
                             language_ == "en" ? "OK" : "确定");
        return;
    }
    if (profileIsLocked(profile.id)) {
        showGuardInformation(this, textFor("title"), textFor("locked_tip"),
                             language_ == "en" ? "OK" : "确定");
        return;
    }
    if (profile.id == selectedProfileId_ && store_->isSelectionLocked()) {
        showGuardInformation(this, textFor("title"), textFor("selection_locked_tip"),
                             language_ == "en" ? "OK" : "确定");
        return;
    }
    if (!confirmGuardMessage(this, textFor("delete_title"),
                             textFor("delete_confirm").arg(profile.displayName),
                             textFor("remove"), textFor("cancel"), true)) {
        return;
    }

    const bool removedCurrent = profile.id == selectedProfileId_;
    QString error;
    if (!store_->removeProfile(profile.id, &error)) {
        showStoreError(textFor("delete_operation"), error);
        return;
    }

    if (removedCurrent) {
        const QString replacementId = store_->selectedProfileId(&error);
        if (!error.isEmpty()) {
            showStoreError(textFor("select_operation"), error);
            loadPage();
            return;
        }
        selectedProfileId_ = replacementId;
        loadPage(replacementId);
        emit selectedProfileChanged(replacementId);
    } else {
        loadPage();
    }
}

void UpstreamProfileDialog::selectCurrentProfile()
{
    const QString id = currentRowProfileId();
    if (id.isEmpty()) {
        showGuardInformation(this, textFor("title"), textFor("no_selection"),
                             language_ == "en" ? "OK" : "确定");
        return;
    }
    if (proxyRunning_ || store_->isSelectionLocked()) {
        showGuardInformation(this, textFor("title"), textFor("selection_locked_tip"),
                             language_ == "en" ? "OK" : "确定");
        return;
    }
    QString error;
    if (!store_->setSelectedProfileId(id, &error)) {
        showStoreError(textFor("select_operation"), error);
        return;
    }
    selectedProfileId_ = id;
    loadPage(id);
    emit selectedProfileChanged(id);
}

void UpstreamProfileDialog::importProfiles()
{
    GuardFileDialog fileDialog(GuardFileDialog::OpenExistingFile, textFor("import_title"),
                               QDir::homePath(), this, language_);
    if (fileDialog.exec() != QDialog::Accepted) return;
    const QString path = fileDialog.selectedFile();
    if (path.isEmpty()) return;

    const int conflictChoice = chooseGuardMessage(
        this, textFor("import_title"), textFor("import_conflict"), GuardQuestion,
        QList<GuardMessageButton>()
            << GuardMessageButton(textFor("skip"), 1, QDialogButtonBox::AcceptRole, true)
            << GuardMessageButton(textFor("overwrite"), 2, QDialogButtonBox::DestructiveRole)
            << GuardMessageButton(textFor("cancel"), QDialog::Rejected, QDialogButtonBox::RejectRole));
    if (conflictChoice == QDialog::Rejected) return;

    UpstreamProfileImportResult result;
    QString error;
    if (!store_->importJson(path,
                            conflictChoice == 2 ? OverwriteImportConflicts : SkipImportConflicts,
                            &result, &error)) {
        showStoreError(textFor("import_operation"), error);
        return;
    }
    currentPage_ = 1;
    loadPage();
    showGuardInformation(this, textFor("import_complete"),
                         textFor("import_summary").arg(result.added).arg(result.updated).arg(result.skipped),
                         language_ == "en" ? "OK" : "确定");
}

void UpstreamProfileDialog::exportProfiles()
{
    GuardFileDialog fileDialog(GuardFileDialog::SaveFile, textFor("export_title"),
                               QDir(QDir::homePath()).filePath("upstream-profiles.json"), this,
                               language_);
    if (fileDialog.exec() != QDialog::Accepted) return;
    QString path = fileDialog.selectedFile();
    if (path.isEmpty()) return;
    if (QFileInfo(path).suffix().isEmpty()) path += ".json";

    const int secretChoice = chooseGuardMessage(
        this, textFor("export_title"), textFor("secret_choice"), GuardQuestion,
        QList<GuardMessageButton>()
            << GuardMessageButton(textFor("without_secrets"), 1, QDialogButtonBox::AcceptRole, true)
            << GuardMessageButton(textFor("with_secrets"), 2, QDialogButtonBox::DestructiveRole)
            << GuardMessageButton(textFor("cancel"), QDialog::Rejected, QDialogButtonBox::RejectRole));
    if (secretChoice == QDialog::Rejected) return;

    const bool includeSecrets = secretChoice == 2;
    if (includeSecrets &&
        !confirmGuardMessage(this, textFor("secret_warning_title"), textFor("secret_warning"),
                             textFor("with_secrets"), textFor("cancel"), true)) {
        return;
    }

    QString error;
    if (!store_->exportJson(path, includeSecrets, &error)) {
        showStoreError(textFor("export_operation"), error);
        return;
    }
    showGuardInformation(this, textFor("export_complete"),
                         textFor("export_complete_message").arg(path),
                         language_ == "en" ? "OK" : "确定");
}

void UpstreamProfileDialog::applySearch()
{
    currentPage_ = 1;
    loadPage();
}

void UpstreamProfileDialog::changePageSize(int index)
{
    const int value = pageSizeCombo_->itemData(index).toInt();
    if (value <= 0 || value == pageSize_) return;
    pageSize_ = value;
    currentPage_ = 1;
    loadPage();
}

void UpstreamProfileDialog::changeSort(int logicalIndex)
{
    net_tunnel::UpstreamProfileSortField nextField;
    if (logicalIndex == 0) nextField = SortByDisplayName;
    else if (logicalIndex == 1) nextField = SortByBaseUrl;
    else if (logicalIndex == 4) nextField = SortByUpdatedAt;
    else return;

    if (sortField_ == nextField) {
        sortOrder_ = sortOrder_ == Qt::AscendingOrder ? Qt::DescendingOrder : Qt::AscendingOrder;
    } else {
        sortField_ = nextField;
        sortOrder_ = nextField == SortByUpdatedAt ? Qt::DescendingOrder : Qt::AscendingOrder;
    }
    table_->horizontalHeader()->setSortIndicator(logicalIndex, sortOrder_);
    currentPage_ = 1;
    loadPage();
}

void UpstreamProfileDialog::firstPage()
{
    if (currentPage_ <= 1) return;
    currentPage_ = 1;
    loadPage();
}

void UpstreamProfileDialog::previousPage()
{
    if (currentPage_ <= 1) return;
    --currentPage_;
    loadPage();
}

void UpstreamProfileDialog::nextPage()
{
    if (currentPage_ >= totalPages_) return;
    ++currentPage_;
    loadPage();
}

void UpstreamProfileDialog::lastPage()
{
    if (totalPages_ <= 0 || currentPage_ >= totalPages_) return;
    currentPage_ = totalPages_;
    loadPage();
}

void UpstreamProfileDialog::updateActionStates()
{
    const bool storeAvailable = store_ && store_->isOpen();
    const QString id = currentRowProfileId();
    const bool hasSelection = !id.isEmpty();
    const bool locked = hasSelection && profileIsLocked(id);
    const bool selectionLocked = proxyRunning_ || (store_ && store_->isSelectionLocked());
    const bool deletingWouldChangeSelection = id == selectedProfileId_ && selectionLocked;

    addButton_->setEnabled(storeAvailable);
    importButton_->setEnabled(storeAvailable);
    exportButton_->setEnabled(storeAvailable);
    viewEditButton_->setEnabled(hasSelection);
    viewEditButton_->setText(locked ? textFor("view") : textFor("edit"));
    viewEditButton_->setToolTip(locked ? textFor("locked_tip") : textFor("edit"));
    removeButton_->setEnabled(hasSelection && !locked && !deletingWouldChangeSelection);
    removeButton_->setToolTip(locked ? textFor("locked_tip")
                                     : (deletingWouldChangeSelection
                                            ? textFor("selection_locked_tip")
                                            : textFor("remove")));
    selectButton_->setEnabled(hasSelection && id != selectedProfileId_ && !selectionLocked);
    selectButton_->setToolTip(selectionLocked ? textFor("selection_locked_tip") : textFor("select"));
    firstPageButton_->setEnabled(totalPages_ > 0 && currentPage_ > 1);
    previousPageButton_->setEnabled(totalPages_ > 0 && currentPage_ > 1);
    nextPageButton_->setEnabled(totalPages_ > 0 && currentPage_ < totalPages_);
    lastPageButton_->setEnabled(totalPages_ > 0 && currentPage_ < totalPages_);
}
