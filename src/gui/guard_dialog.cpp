#include "gui/guard_dialog.h"

#include <QtCore/QDir>
#include <QtCore/QEvent>
#include <QtCore/QFileInfo>
#include <QtGui/QFont>
#include <QtGui/QFontDatabase>
#include <QtGui/QGuiApplication>
#include <QtGui/QIcon>
#include <QtGui/QMouseEvent>
#include <QtGui/QPalette>
#include <QtWidgets/QFileSystemModel>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStyle>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QTreeView>
#include <QtWidgets/QVBoxLayout>

namespace net_tunnel_gui {

namespace {

QLabel *asLabel(QWidget *widget)
{
    return static_cast<QLabel *>(widget);
}

QStyle::StandardPixmap standardPixmapFor(GuardMessageIcon icon)
{
    switch (icon) {
    case GuardWarning:
        return QStyle::SP_MessageBoxWarning;
    case GuardQuestion:
        return QStyle::SP_MessageBoxQuestion;
    case GuardCritical:
        return QStyle::SP_MessageBoxCritical;
    case GuardInformation:
    default:
        return QStyle::SP_MessageBoxInformation;
    }
}

QString fontAwesomeFamily()
{
    const QString expectedFamily("FontAwesome");
    if (QFontDatabase().families().contains(expectedFamily)) {
        return expectedFamily;
    }

    const int fontId = QFontDatabase::addApplicationFont(":/image/fontawesome-webfont.ttf");
    if (fontId < 0) {
        return QString();
    }
    const QStringList families = QFontDatabase::applicationFontFamilies(fontId);
    return families.isEmpty() ? QString() : families.first();
}

void setGuardCloseGlyph(QPushButton *button)
{
    const QString family = fontAwesomeFamily();
    button->setIcon(QIcon());
    if (family.isEmpty()) {
        button->setText(QString(QChar(0x00d7)));
        return;
    }

    QFont font(family);
    font.setPointSize(9);
    button->setFont(font);
    button->setText(QString(QChar(0xf00d)));
}

class GuardMessageDialog : public GuardDialog {
public:
    GuardMessageDialog(const QString &title,
                       const QString &message,
                       GuardMessageIcon icon,
                       const QList<GuardMessageButton> &buttons,
                       QWidget *parent)
        : GuardDialog(parent)
    {
        setProperty("guard_dialog_kind", "message");
        setWindowTitle(title);
        setMinimumWidth(400);
        setMaximumWidth(680);

        QVBoxLayout *root = contentLayout();
        root->setContentsMargins(18, 16, 18, 16);
        root->setSpacing(16);

        QHBoxLayout *messageRow = new QHBoxLayout;
        messageRow->setSpacing(12);
        QLabel *iconLabel = new QLabel(this);
        iconLabel->setObjectName("guardMessageIcon");
        iconLabel->setPixmap(style()->standardIcon(standardPixmapFor(icon)).pixmap(28, 28));
        iconLabel->setFixedSize(32, 32);
        iconLabel->setAlignment(Qt::AlignCenter);
        QLabel *messageLabel = new QLabel(message, this);
        messageLabel->setObjectName("guardMessageText");
        messageLabel->setTextFormat(Qt::PlainText);
        messageLabel->setWordWrap(true);
        messageLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        messageRow->addWidget(iconLabel, 0, Qt::AlignTop);
        messageRow->addWidget(messageLabel, 1);
        root->addLayout(messageRow);

        QDialogButtonBox *buttonBox = new QDialogButtonBox(this);
        buttonBox->setObjectName("guardMessageButtons");
        buttonBox->setCenterButtons(false);
        const QList<GuardMessageButton> effectiveButtons = buttons.isEmpty()
            ? QList<GuardMessageButton>() << GuardMessageButton("OK", QDialog::Accepted,
                                                                  QDialogButtonBox::AcceptRole, true)
            : buttons;
        for (int i = 0; i < effectiveButtons.size(); ++i) {
            const GuardMessageButton &spec = effectiveButtons.at(i);
            QPushButton *button = buttonBox->addButton(spec.text, spec.role);
            button->setObjectName("guardMessageButton");
            button->setProperty("guard_result", spec.resultCode);
            if (spec.isDefault) {
                button->setDefault(true);
                button->setAutoDefault(true);
            }
            QObject::connect(button, &QPushButton::clicked, this, [this, spec]() {
                done(spec.resultCode);
            });
        }
        root->addWidget(buttonBox);
    }
};

} // namespace

GuardDialog::GuardDialog(QWidget *parent)
    : QDialog(parent),
      titleBar_(new QFrame(this)),
      contentWidget_(new QFrame(this)),
      contentLayout_(new QVBoxLayout(contentWidget_)),
      titleIcon_(new QLabel(titleBar_)),
      titleLabel_(new QLabel(titleBar_)),
      closeButton_(new QPushButton(titleBar_)),
      dragging_(false)
{
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint | Qt::CustomizeWindowHint);
    setModal(true);
    setObjectName("guardDialog");
    setProperty("guard_dialog_root", true);
    if (parent && !parent->windowIcon().isNull()) {
        setWindowIcon(parent->windowIcon());
    } else if (!QGuiApplication::windowIcon().isNull()) {
        setWindowIcon(QGuiApplication::windowIcon());
    }

    QFrame *titleFrame = static_cast<QFrame *>(titleBar_);
    titleFrame->setObjectName("guardDialogTitleBar");
    titleFrame->setFixedHeight(42);
    QHBoxLayout *titleLayout = new QHBoxLayout(titleFrame);
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(0);

    QLabel *iconLabel = asLabel(titleIcon_);
    iconLabel->setObjectName("guardDialogTitleIcon");
    iconLabel->setFixedSize(36, 42);
    iconLabel->setAlignment(Qt::AlignCenter);
    QLabel *titleLabel = asLabel(titleLabel_);
    titleLabel->setObjectName("guardDialogTitleText");
    titleLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    titleLabel->setAlignment(Qt::AlignCenter);

    closeButton_->setObjectName("guardDialogCloseButton");
    closeButton_->setToolTip(tr("Close"));
    setGuardCloseGlyph(closeButton_);
    closeButton_->setFixedSize(36, 42);
    closeButton_->setFocusPolicy(Qt::NoFocus);

    titleLayout->addWidget(iconLabel);
    titleLayout->addWidget(titleLabel, 1);
    titleLayout->addWidget(closeButton_);

    QFrame *contentFrame = static_cast<QFrame *>(contentWidget_);
    contentFrame->setObjectName("guardDialogContent");
    contentLayout_->setContentsMargins(0, 0, 0, 0);
    contentLayout_->setSpacing(0);

    QVBoxLayout *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    outer->addWidget(titleFrame);
    outer->addWidget(contentFrame, 1);

    setStyleSheet(
        "QDialog[guard_dialog_root=\"true\"] { background: #EAF7FF; border: 1px solid #C0DCF2; }"
        "#guardDialogTitleBar { border: none; border-radius: 0px; background: qlineargradient("
            "spread:pad,x1:0,y1:0,x2:0,y2:1,stop:0 #DEF0FE,stop:1 #C0DEF6); }"
        "#guardDialogTitleIcon, #guardDialogTitleText { color: #386487; background: transparent; border: none; }"
        "QPushButton#guardDialogCloseButton { border: none; border-radius: 3px; color: #386487; "
            "padding: 3px; margin: 0px; background: transparent; }"
        "QPushButton#guardDialogCloseButton:hover { color: #FFFFFF; margin: 1px 1px 2px 1px; "
            "background-color: rgba(238,0,0,128); }"
        "QPushButton#guardDialogCloseButton:pressed { color: #FFFFFF; "
            "background-color: rgba(238,0,0,128); }"
        "#guardDialogContent { background: #EAF7FF; border: none; border-radius: 0px; }"
        "#guardMessageText { color: #386487; }"
        "#guardFileTree { background: white; border: 1px solid #c3d0d6; }"
        "QDialog[guard_dialog_root=\"true\"] QLineEdit { background: white; border: 1px solid #b8c6ce; padding: 5px; }"
        "QDialog[guard_dialog_root=\"true\"] QPushButton { min-height: 26px; padding: 2px 10px; }"
    );

    titleFrame->installEventFilter(this);
    iconLabel->installEventFilter(this);
    titleLabel->installEventFilter(this);
    QObject::connect(closeButton_, &QPushButton::clicked, this, &QDialog::reject);
    updateTitleChrome();
}

QVBoxLayout *GuardDialog::contentLayout() const
{
    return contentLayout_;
}

QWidget *GuardDialog::titleBar() const
{
    return titleBar_;
}

bool GuardDialog::event(QEvent *event)
{
    if (event->type() == QEvent::WindowTitleChange || event->type() == QEvent::WindowIconChange) {
        updateTitleChrome();
    }
    return QDialog::event(event);
}

bool GuardDialog::eventFilter(QObject *watched, QEvent *event)
{
    Q_UNUSED(watched)
    if (event->type() == QEvent::MouseButtonPress) {
        QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
            dragging_ = true;
            dragOffset_ = mouseEvent->globalPos() - frameGeometry().topLeft();
            return true;
        }
    } else if (event->type() == QEvent::MouseMove && dragging_) {
        QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->buttons() & Qt::LeftButton) {
            move(mouseEvent->globalPos() - dragOffset_);
            return true;
        }
    } else if (event->type() == QEvent::MouseButtonRelease) {
        dragging_ = false;
        return true;
    }
    return QDialog::eventFilter(watched, event);
}

void GuardDialog::updateTitleChrome()
{
    if (!titleLabel_ || !titleIcon_) {
        return;
    }
    asLabel(titleLabel_)->setText(windowTitle());
    const QIcon icon = windowIcon();
    asLabel(titleIcon_)->setPixmap(icon.isNull() ? QPixmap() : icon.pixmap(22, 22));
}

GuardMessageButton::GuardMessageButton(const QString &buttonText,
                                       int buttonResultCode,
                                       QDialogButtonBox::ButtonRole buttonRole,
                                       bool defaultButton)
    : text(buttonText),
      resultCode(buttonResultCode),
      role(buttonRole),
      isDefault(defaultButton)
{
}

int chooseGuardMessage(QWidget *parent,
                       const QString &title,
                       const QString &message,
                       GuardMessageIcon icon,
                       const QList<GuardMessageButton> &buttons)
{
    GuardMessageDialog dialog(title, message, icon, buttons, parent);
    return dialog.exec();
}

void showGuardInformation(QWidget *parent,
                          const QString &title,
                          const QString &message,
                          const QString &acceptText)
{
    chooseGuardMessage(parent, title, message, GuardInformation,
                       QList<GuardMessageButton>()
                           << GuardMessageButton(acceptText, QDialog::Accepted,
                                                 QDialogButtonBox::AcceptRole, true));
}

void showGuardWarning(QWidget *parent,
                      const QString &title,
                      const QString &message,
                      const QString &acceptText)
{
    chooseGuardMessage(parent, title, message, GuardWarning,
                       QList<GuardMessageButton>()
                           << GuardMessageButton(acceptText, QDialog::Accepted,
                                                 QDialogButtonBox::AcceptRole, true));
}

bool confirmGuardMessage(QWidget *parent,
                         const QString &title,
                         const QString &message,
                         const QString &acceptText,
                         const QString &rejectText,
                         bool destructive)
{
    return chooseGuardMessage(parent, title, message, GuardQuestion,
                              QList<GuardMessageButton>()
                                  << GuardMessageButton(acceptText, QDialog::Accepted,
                                                        destructive ? QDialogButtonBox::DestructiveRole
                                                                    : QDialogButtonBox::AcceptRole)
                                  << GuardMessageButton(rejectText, QDialog::Rejected,
                                                        QDialogButtonBox::RejectRole, true))
        == QDialog::Accepted;
}

GuardFileDialog::GuardFileDialog(Mode mode,
                                 const QString &title,
                                 const QString &initialPath,
                                 QWidget *parent,
                                 const QString &language)
    : GuardDialog(parent),
      mode_(mode),
      english_(language.trimmed().toLower() == "en"),
      model_(new QFileSystemModel(this)),
      tree_(new QTreeView(this)),
      directoryEdit_(new QLineEdit(this)),
      fileNameEdit_(new QLineEdit(this))
{
    setProperty("guard_dialog_kind", "file");
    setWindowTitle(title);
    setMinimumSize(720, 480);
    resize(820, 560);

    QVBoxLayout *root = contentLayout();
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(10);

    QHBoxLayout *locationRow = new QHBoxLayout;
    locationRow->setSpacing(6);
    QLabel *locationLabel = new QLabel(localized("文件夹", "Folder"), this);
    QToolButton *upButton = new QToolButton(this);
    upButton->setObjectName("guardFileUpButton");
    upButton->setIcon(style()->standardIcon(QStyle::SP_ArrowUp));
    upButton->setToolTip(localized("上一级文件夹", "Up one folder"));
    directoryEdit_->setObjectName("guardFileDirectoryEdit");
    locationRow->addWidget(locationLabel);
    locationRow->addWidget(directoryEdit_, 1);
    locationRow->addWidget(upButton);
    root->addLayout(locationRow);

    model_->setFilter(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot);
    model_->setNameFilters(QStringList() << "*.json");
    model_->setNameFilterDisables(false);
    tree_->setObjectName("guardFileTree");
    tree_->setModel(model_);
    tree_->setRootIsDecorated(false);
    tree_->setAlternatingRowColors(true);
    tree_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setSortingEnabled(true);
    tree_->sortByColumn(0, Qt::AscendingOrder);
    tree_->hideColumn(1);
    tree_->hideColumn(2);
    tree_->hideColumn(3);
    root->addWidget(tree_, 1);

    QHBoxLayout *fileNameRow = new QHBoxLayout;
    fileNameRow->setSpacing(6);
    fileNameRow->addWidget(new QLabel(localized("文件名", "File name"), this));
    fileNameEdit_->setObjectName("guardFileNameEdit");
    fileNameRow->addWidget(fileNameEdit_, 1);
    root->addLayout(fileNameRow);

    QDialogButtonBox *buttons = new QDialogButtonBox(this);
    QPushButton *acceptButton = buttons->addButton(
        mode_ == OpenExistingFile ? localized("打开", "Open") : localized("保存", "Save"),
        QDialogButtonBox::AcceptRole);
    buttons->addButton(localized("取消", "Cancel"), QDialogButtonBox::RejectRole);
    acceptButton->setDefault(true);
    root->addWidget(buttons);

    QFileInfo initialInfo(initialPath);
    QString initialDirectory = QDir::homePath();
    if (!initialPath.trimmed().isEmpty()) {
        if (initialInfo.isDir()) {
            initialDirectory = initialInfo.absoluteFilePath();
        } else {
            initialDirectory = initialInfo.absolutePath();
            fileNameEdit_->setText(initialInfo.fileName());
        }
    }
    navigateTo(initialDirectory);

    QObject::connect(directoryEdit_, &QLineEdit::returnPressed, this, [this]() {
        navigateTo(directoryEdit_->text());
    });
    QObject::connect(upButton, &QToolButton::clicked, this, [this]() {
        QDir directory(currentDirectory_);
        if (directory.cdUp()) {
            navigateTo(directory.absolutePath());
        }
    });
    QObject::connect(tree_, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) {
        const QString path = model_->filePath(index);
        const QFileInfo info(path);
        if (info.isDir()) {
            navigateTo(path);
        } else {
            fileNameEdit_->setText(info.fileName());
            if (mode_ == OpenExistingFile) {
                selectCurrentFile();
            }
        }
    });
    QObject::connect(acceptButton, &QPushButton::clicked, this, [this]() { selectCurrentFile(); });
    QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

QString GuardFileDialog::selectedFile() const
{
    return selectedFile_;
}

QString GuardFileDialog::localized(const QString &zh, const QString &en) const
{
    return english_ ? en : zh;
}

void GuardFileDialog::navigateTo(const QString &directory)
{
    QFileInfo info(directory.trimmed());
    if (!info.exists() || !info.isDir()) {
        showGuardWarning(this, windowTitle(),
                         localized("请选择已存在的文件夹。", "Choose an existing folder."),
                         localized("确定", "OK"));
        return;
    }
    currentDirectory_ = info.absoluteFilePath();
    directoryEdit_->setText(currentDirectory_);
    tree_->setRootIndex(model_->setRootPath(currentDirectory_));
}

void GuardFileDialog::selectCurrentFile()
{
    QString fileName = fileNameEdit_->text().trimmed();
    if (fileName.isEmpty()) {
        const QModelIndex index = tree_->currentIndex();
        if (index.isValid() && !model_->isDir(index)) {
            fileName = model_->fileName(index);
        }
    }
    if (fileName.isEmpty()) {
        showGuardWarning(this, windowTitle(),
                         localized("请选择 JSON 文件。", "Choose a JSON file."),
                         localized("确定", "OK"));
        return;
    }

    const QString path = QFileInfo(fileName).isAbsolute()
        ? QDir::cleanPath(fileName)
        : QDir(currentDirectory_).filePath(fileName);
    const QFileInfo info(path);
    if (mode_ == OpenExistingFile && (!info.exists() || !info.isFile())) {
        showGuardWarning(this, windowTitle(),
                         localized("所选文件不存在。", "The selected file does not exist."),
                         localized("确定", "OK"));
        return;
    }
    if (mode_ == SaveFile && info.exists() && info.isDir()) {
        showGuardWarning(this, windowTitle(),
                         localized("请选择文件名，而不是文件夹。", "Choose a file name, not a folder."),
                         localized("确定", "OK"));
        return;
    }
    if (mode_ == SaveFile && info.exists() &&
        !confirmGuardMessage(this, windowTitle(),
                             localized("该文件已存在，是否覆盖？", "The file already exists. Replace it?"),
                             localized("覆盖", "Overwrite"), localized("取消", "Cancel"), true)) {
        return;
    }
    selectedFile_ = QDir::cleanPath(path);
    accept();
}

} // namespace net_tunnel_gui
