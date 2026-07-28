#include "gui/guard_dialog.h"
#include "gui/scale_helpers.h"

#include <QtCore/QDir>
#include <QtCore/QEvent>
#include <QtCore/QFileInfo>
#include <QtCore/QVariant>
#include <QtGui/QFont>
#include <QtGui/QFontDatabase>
#include <QtGui/QGuiApplication>
#include <QtGui/QIcon>
#include <QtGui/QMouseEvent>
#include <QtGui/QPalette>
#include <QtGui/QScreen>
#include <QtGui/QShowEvent>
#include <QtGui/QWindow>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFileSystemModel>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QLayout>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QStyle>
#include <QtWidgets/QTableView>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QTreeView>
#include <QtWidgets/QVBoxLayout>

namespace net_tunnel_gui {

namespace {

const int kDialogScreenMargin = 24;

QLabel *asLabel(QWidget *widget)
{
    return static_cast<QLabel *>(widget);
}

QFont relativeFont(const QFont &base, qreal scale)
{
    QFont font(base);
    if (font.pointSizeF() > 0.0) {
        font.setPointSizeF(qMax(1.0, font.pointSizeF() * scale));
    } else if (font.pixelSize() > 0) {
        font.setPixelSize(qMax(1, qRound(font.pixelSize() * scale)));
    }
    return font;
}

int scaledMetric(int value, qreal scale)
{
    return qMax(0, qRound(value * scale));
}

QSize scaledSize(const QSize &size, qreal scale)
{
    return QSize(scaledMetric(size.width(), scale),
                 scaledMetric(size.height(), scale));
}

QSize scaledMaximumSize(const QSize &size, qreal scale)
{
    return QSize(size.width() == QWIDGETSIZE_MAX
                     ? QWIDGETSIZE_MAX
                     : scaledMetric(size.width(), scale),
                 size.height() == QWIDGETSIZE_MAX
                     ? QWIDGETSIZE_MAX
                     : scaledMetric(size.height(), scale));
}

QMargins scaledMargins(const QMargins &margins, qreal scale)
{
    return QMargins(scaledMetric(margins.left(), scale),
                    scaledMetric(margins.top(), scale),
                    scaledMetric(margins.right(), scale),
                    scaledMetric(margins.bottom(), scale));
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
        : GuardDialog(parent),
          icon_(icon),
          iconLabel_(0)
    {
        setProperty("guard_dialog_kind", "message");
        setWindowTitle(title);
        setMinimumWidth(400);
        setMaximumWidth(680);
        setProperty("guard_logical_maximum_size", QSize(680, QWIDGETSIZE_MAX));

        QVBoxLayout *root = contentLayout();
        root->setContentsMargins(18, 16, 18, 16);
        root->setSpacing(16);

        QHBoxLayout *messageRow = new QHBoxLayout;
        messageRow->setSpacing(12);
        iconLabel_ = new QLabel(this);
        iconLabel_->setObjectName("guardMessageIcon");
        iconLabel_->setPixmap(style()->standardIcon(standardPixmapFor(icon)).pixmap(28, 28));
        iconLabel_->setFixedSize(32, 32);
        iconLabel_->setProperty("guard_logical_fixed_size", QSize(32, 32));
        iconLabel_->setAlignment(Qt::AlignCenter);
        QLabel *messageLabel = new QLabel(message, this);
        messageLabel->setObjectName("guardMessageText");
        messageLabel->setTextFormat(Qt::PlainText);
        messageLabel->setWordWrap(true);
        messageLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        messageRow->addWidget(iconLabel_, 0, Qt::AlignTop);
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

protected:
    void uiScaleChanged() override
    {
        if (!iconLabel_) return;
        const int iconExtent = scaledMetric(28, uiScaleFactor());
        iconLabel_->setPixmap(style()->standardIcon(standardPixmapFor(icon_)).pixmap(iconExtent,
                                                                                       iconExtent));
    }

private:
    GuardMessageIcon icon_;
    QLabel *iconLabel_;
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
      dragging_(false),
      uiScaleBaselineCaptured_(false),
      logicalInitialSizeApplied_(false),
      uiScale_(1.0)
{
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint | Qt::CustomizeWindowHint);
    setModal(true);
    setObjectName("guardDialog");
    setProperty("guard_dialog_root", true);
    // A child dialog must start from the application font, not its already-scaled parent font.
    setFont(QApplication::font());
    if (parent && !parent->windowIcon().isNull()) {
        setWindowIcon(parent->windowIcon());
    } else if (!QGuiApplication::windowIcon().isNull()) {
        setWindowIcon(QGuiApplication::windowIcon());
    }

    QFrame *titleFrame = static_cast<QFrame *>(titleBar_);
    titleFrame->setObjectName("guardDialogTitleBar");
    titleFrame->setFixedHeight(30);
    QHBoxLayout *titleLayout = new QHBoxLayout(titleFrame);
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(0);

    QLabel *iconLabel = asLabel(titleIcon_);
    iconLabel->setObjectName("guardDialogTitleIcon");
    iconLabel->setFixedSize(30, 30);
    iconLabel->setProperty("guard_logical_fixed_size", QSize(30, 30));
    iconLabel->setAlignment(Qt::AlignCenter);
    QLabel *titleLabel = asLabel(titleLabel_);
    titleLabel->setObjectName("guardDialogTitleText");
    titleLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    titleLabel->setAlignment(Qt::AlignCenter);

    closeButton_->setObjectName("guardDialogCloseButton");
    closeButton_->setToolTip(tr("Close"));
    setGuardCloseGlyph(closeButton_);
    closeButton_->setFixedSize(30, 30);
    closeButton_->setProperty("guard_logical_fixed_size", QSize(30, 30));
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

void GuardDialog::showEvent(QShowEvent *event)
{
    applyInheritedUiScale();
    QDialog::showEvent(event);
    constrainToAvailableScreen();
}

bool GuardDialog::eventFilter(QObject *watched, QEvent *event)
{
    Q_UNUSED(watched)
    if (event->type() == QEvent::MouseButtonPress) {
        QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
            if (windowHandle() && windowHandle()->startSystemMove()) {
                dragging_ = false;
                return true;
            }
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
    const int iconExtent = scaledMetric(18, uiScale_);
    asLabel(titleIcon_)->setPixmap(icon.isNull() ? QPixmap() : icon.pixmap(iconExtent, iconExtent));
}

void GuardDialog::setLogicalInitialSize(const QSize &size)
{
    logicalInitialSize_ = size;
    logicalInitialSizeApplied_ = false;
}

qreal GuardDialog::uiScaleFactor() const
{
    return uiScale_;
}

void GuardDialog::uiScaleChanged()
{
}

void GuardDialog::captureUiScaleBaseline()
{
    baseFonts_.clear();
    baseMinimumSizes_.clear();
    baseMaximumSizes_.clear();
    baseLayoutMargins_.clear();
    baseLayoutSpacings_.clear();
    baseFormSpacings_.clear();
    baseButtonIconSizes_.clear();
    baseTableRowHeights_.clear();
    baseInteractiveHeights_.clear();
    baseVerticalScrollBarWidths_.clear();
    baseHorizontalScrollBarHeights_.clear();

    QList<QWidget *> widgets;
    widgets.append(this);
    widgets.append(findChildren<QWidget *>());
    for (int i = 0; i < widgets.size(); ++i) {
        QWidget *widget = widgets.at(i);
        if (!widget) continue;
        baseFonts_.insert(widget, widget->font());

        if (qobject_cast<QLineEdit *>(widget) ||
            qobject_cast<QAbstractSpinBox *>(widget) ||
            qobject_cast<QComboBox *>(widget) ||
            qobject_cast<QAbstractButton *>(widget)) {
            const int height = widget->sizeHint().height();
            if (height > 0) {
                baseInteractiveHeights_.insert(widget, height);
            }
        }
        QScrollBar *scrollBar = qobject_cast<QScrollBar *>(widget);
        if (scrollBar) {
            if (scrollBar->orientation() == Qt::Vertical) {
                const int width = scrollBar->sizeHint().width();
                if (width > 0) {
                    baseVerticalScrollBarWidths_.insert(scrollBar, width);
                }
            } else {
                const int height = scrollBar->sizeHint().height();
                if (height > 0) {
                    baseHorizontalScrollBarHeights_.insert(scrollBar, height);
                }
            }
        }

        const QSize logicalFixedSize = widget->property("guard_logical_fixed_size").toSize();
        if (logicalFixedSize.isValid()) {
            baseMinimumSizes_.insert(widget, logicalFixedSize);
            baseMaximumSizes_.insert(widget, logicalFixedSize);
        } else {
            const QSize minimum = widget->minimumSize();
            if (!minimum.isNull()) {
                baseMinimumSizes_.insert(widget, minimum);
            }
            const QSize maximum = widget->maximumSize();
            const QSize logicalMaximum =
                widget->property("guard_logical_maximum_size").toSize();
            if (logicalMaximum.isValid()) {
                baseMaximumSizes_.insert(widget, logicalMaximum);
            } else if ((maximum.width() != QWIDGETSIZE_MAX &&
                         minimum.width() == maximum.width()) ||
                        (maximum.height() != QWIDGETSIZE_MAX &&
                         minimum.height() == maximum.height())) {
                baseMaximumSizes_.insert(widget, maximum);
            }
        }
    }

    const QList<QLayout *> layouts = findChildren<QLayout *>();
    for (int i = 0; i < layouts.size(); ++i) {
        QLayout *layout = layouts.at(i);
        if (!layout) continue;
        baseLayoutMargins_.insert(layout, layout->contentsMargins());
        baseLayoutSpacings_.insert(layout, layout->spacing());
        if (QFormLayout *form = qobject_cast<QFormLayout *>(layout)) {
            baseFormSpacings_.insert(form,
                                     QSize(form->horizontalSpacing(), form->verticalSpacing()));
        }
    }

    const QList<QAbstractButton *> buttons = findChildren<QAbstractButton *>();
    for (int i = 0; i < buttons.size(); ++i) {
        QAbstractButton *button = buttons.at(i);
        if (button) baseButtonIconSizes_.insert(button, button->iconSize());
    }

    const QList<QTableView *> tables = findChildren<QTableView *>();
    for (int i = 0; i < tables.size(); ++i) {
        QTableView *table = tables.at(i);
        if (table && table->verticalHeader()) {
            baseTableRowHeights_.insert(table, table->verticalHeader()->defaultSectionSize());
        }
    }

    captureScaleSensitiveStyleBaselines(this);

    uiScaleBaselineCaptured_ = true;
}

qreal GuardDialog::inheritedUiScaleFactor() const
{
    for (QWidget *widget = parentWidget(); widget; widget = widget->parentWidget()) {
        const QVariant value = widget->property("ui_scale_factor");
        bool converted = false;
        const qreal scale = value.toDouble(&converted);
        if (converted && scale > 0.0) {
            return qMax<qreal>(0.5, scale);
        }
    }
    return 1.0;
}

void GuardDialog::applyInheritedUiScale()
{
    if (!uiScaleBaselineCaptured_) {
        captureUiScaleBaseline();
    }

    const qreal nextScale = inheritedUiScaleFactor();
    const bool scaleChanged = qAbs(nextScale - uiScale_) >= 0.001;
    uiScale_ = nextScale;
    setProperty("ui_scale_factor", uiScale_);

    if (scaleChanged) {
        QHash<QWidget *, QFont>::const_iterator fontIt = baseFonts_.constBegin();
        for (; fontIt != baseFonts_.constEnd(); ++fontIt) {
            if (fontIt.key()) {
                fontIt.key()->setFont(relativeFont(fontIt.value(), uiScale_));
            }
        }

        QHash<QWidget *, QSize>::const_iterator minimumIt = baseMinimumSizes_.constBegin();
        for (; minimumIt != baseMinimumSizes_.constEnd(); ++minimumIt) {
            if (minimumIt.key()) {
                minimumIt.key()->setMinimumSize(scaledSize(minimumIt.value(), uiScale_));
            }
        }

        QHash<QWidget *, QSize>::const_iterator maximumIt = baseMaximumSizes_.constBegin();
        for (; maximumIt != baseMaximumSizes_.constEnd(); ++maximumIt) {
            if (maximumIt.key()) {
                maximumIt.key()->setMaximumSize(scaledMaximumSize(maximumIt.value(), uiScale_));
            }
        }

        QHash<QWidget *, int>::const_iterator interactiveIt =
            baseInteractiveHeights_.constBegin();
        for (; interactiveIt != baseInteractiveHeights_.constEnd(); ++interactiveIt) {
            QWidget *widget = interactiveIt.key();
            if (!widget) continue;
            const int baseMinimum = baseMinimumSizes_.value(widget).height();
            widget->setMinimumHeight(scaledMetric(baseMinimum > 0
                                                      ? baseMinimum
                                                      : interactiveIt.value(),
                                                  uiScale_));
        }

        QHash<QScrollBar *, int>::const_iterator scrollBarIt =
            baseVerticalScrollBarWidths_.constBegin();
        for (; scrollBarIt != baseVerticalScrollBarWidths_.constEnd(); ++scrollBarIt) {
            if (scrollBarIt.key()) {
                scrollBarIt.key()->setFixedWidth(
                    scaledMetric(scrollBarIt.value(), uiScale_));
            }
        }

        QHash<QScrollBar *, int>::const_iterator horizontalScrollBarIt =
            baseHorizontalScrollBarHeights_.constBegin();
        for (; horizontalScrollBarIt != baseHorizontalScrollBarHeights_.constEnd();
             ++horizontalScrollBarIt) {
            if (horizontalScrollBarIt.key()) {
                horizontalScrollBarIt.key()->setFixedHeight(
                    scaledMetric(horizontalScrollBarIt.value(), uiScale_));
            }
        }

        QHash<QLayout *, QMargins>::const_iterator marginIt = baseLayoutMargins_.constBegin();
        for (; marginIt != baseLayoutMargins_.constEnd(); ++marginIt) {
            if (marginIt.key()) {
                marginIt.key()->setContentsMargins(scaledMargins(marginIt.value(), uiScale_));
                marginIt.key()->invalidate();
            }
        }

        QHash<QLayout *, int>::const_iterator spacingIt = baseLayoutSpacings_.constBegin();
        for (; spacingIt != baseLayoutSpacings_.constEnd(); ++spacingIt) {
            if (spacingIt.key() && spacingIt.value() >= 0) {
                spacingIt.key()->setSpacing(scaledMetric(spacingIt.value(), uiScale_));
            }
        }

        QHash<QFormLayout *, QSize>::const_iterator formSpacingIt = baseFormSpacings_.constBegin();
        for (; formSpacingIt != baseFormSpacings_.constEnd(); ++formSpacingIt) {
            QFormLayout *form = formSpacingIt.key();
            const QSize baseSpacing = formSpacingIt.value();
            if (!form) continue;
            if (baseSpacing.width() >= 0) {
                form->setHorizontalSpacing(scaledMetric(baseSpacing.width(), uiScale_));
            }
            if (baseSpacing.height() >= 0) {
                form->setVerticalSpacing(scaledMetric(baseSpacing.height(), uiScale_));
            }
        }

        QHash<QAbstractButton *, QSize>::const_iterator iconIt = baseButtonIconSizes_.constBegin();
        for (; iconIt != baseButtonIconSizes_.constEnd(); ++iconIt) {
            if (iconIt.key() && iconIt.value().isValid()) {
                iconIt.key()->setIconSize(scaledSize(iconIt.value(), uiScale_));
            }
        }

        QHash<QTableView *, int>::const_iterator rowHeightIt = baseTableRowHeights_.constBegin();
        for (; rowHeightIt != baseTableRowHeights_.constEnd(); ++rowHeightIt) {
            if (rowHeightIt.key() && rowHeightIt.key()->verticalHeader()) {
                rowHeightIt.key()->verticalHeader()->setDefaultSectionSize(
                    scaledMetric(rowHeightIt.value(), uiScale_));
            }
        }

        applyScaleSensitiveSubcontrols(this, uiScale_);

        updateTitleChrome();
        uiScaleChanged();
    }

    if (!logicalInitialSizeApplied_ && logicalInitialSize_.isValid()) {
        resize(scaledSize(logicalInitialSize_, uiScale_));
        logicalInitialSizeApplied_ = true;
    }

    constrainToAvailableScreen();
}

void GuardDialog::constrainToAvailableScreen()
{
    QScreen *screen = 0;
    QWidget *ownerWindow = parentWidget() ? parentWidget()->window() : 0;
    if (ownerWindow && ownerWindow->windowHandle()) {
        screen = ownerWindow->windowHandle()->screen();
    }
    if (!screen && ownerWindow) {
        screen = QGuiApplication::screenAt(ownerWindow->frameGeometry().center());
    }
    if (!screen && windowHandle()) {
        screen = windowHandle()->screen();
    }
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }
    if (!screen) return;

    const QRect available = screen->availableGeometry();
    const QSize sizeLimit(qMax(1, available.width() - 2 * kDialogScreenMargin),
                          qMax(1, available.height() - 2 * kDialogScreenMargin));
    const QSize boundedMinimum(qMin(minimumWidth(), sizeLimit.width()),
                               qMin(minimumHeight(), sizeLimit.height()));
    if (minimumSize() != boundedMinimum) {
        setMinimumSize(boundedMinimum);
    }

    QSize boundedSize = size().boundedTo(sizeLimit);
    boundedSize = boundedSize.expandedTo(boundedMinimum);
    if (size() != boundedSize) {
        resize(boundedSize);
    }

    const QRect ownerGeometry = ownerWindow ? ownerWindow->frameGeometry() : available;
    const QRect visibleOwner = ownerGeometry.intersected(available);
    const QPoint ownerCenter = visibleOwner.isEmpty() ? available.center()
                                                       : visibleOwner.center();
    const int maximumX = available.right() - kDialogScreenMargin - width() + 1;
    const int maximumY = available.bottom() - kDialogScreenMargin - height() + 1;
    const int targetX = qBound(available.left() + kDialogScreenMargin,
                               ownerCenter.x() - width() / 2,
                               maximumX);
    const int targetY = qBound(available.top() + kDialogScreenMargin,
                               ownerCenter.y() - height() / 2,
                               maximumY);
    move(targetX, targetY);
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
    setLogicalInitialSize(size());

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
