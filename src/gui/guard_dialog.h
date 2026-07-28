#pragma once

#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QMargins>
#include <QtCore/QPoint>
#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtGui/QFont>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>

class QAbstractButton;
class QFileSystemModel;
class QFormLayout;
class QLineEdit;
class QLayout;
class QPushButton;
class QScrollBar;
class QShowEvent;
class QTableView;
class QTreeView;
class QVBoxLayout;
class QWidget;

namespace net_tunnel_gui {

class GuardDialog : public QDialog {
public:
    explicit GuardDialog(QWidget *parent = 0);

    QVBoxLayout *contentLayout() const;
    QWidget *titleBar() const;

protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;

    // Dialogs with an intentional starting geometry register it in logical pixels.
    void setLogicalInitialSize(const QSize &size);
    qreal uiScaleFactor() const;
    virtual void uiScaleChanged();

private:
    void captureUiScaleBaseline();
    void applyInheritedUiScale();
    void constrainToAvailableScreen();
    qreal inheritedUiScaleFactor() const;
    void updateTitleChrome();

    QWidget *titleBar_;
    QWidget *contentWidget_;
    QVBoxLayout *contentLayout_;
    QWidget *titleIcon_;
    QWidget *titleLabel_;
    QPushButton *closeButton_;
    bool dragging_;
    QPoint dragOffset_;
    bool uiScaleBaselineCaptured_;
    bool logicalInitialSizeApplied_;
    qreal uiScale_;
    QSize logicalInitialSize_;
    QHash<QWidget *, QFont> baseFonts_;
    QHash<QWidget *, QSize> baseMinimumSizes_;
    QHash<QWidget *, QSize> baseMaximumSizes_;
    QHash<QLayout *, QMargins> baseLayoutMargins_;
    QHash<QLayout *, int> baseLayoutSpacings_;
    QHash<QFormLayout *, QSize> baseFormSpacings_;
    QHash<QAbstractButton *, QSize> baseButtonIconSizes_;
    QHash<QTableView *, int> baseTableRowHeights_;
    QHash<QWidget *, int> baseInteractiveHeights_;
    QHash<QScrollBar *, int> baseVerticalScrollBarWidths_;
    QHash<QScrollBar *, int> baseHorizontalScrollBarHeights_;
};

enum GuardMessageIcon {
    GuardInformation,
    GuardWarning,
    GuardQuestion,
    GuardCritical
};

struct GuardMessageButton {
    GuardMessageButton(const QString &text,
                       int resultCode,
                       QDialogButtonBox::ButtonRole role,
                       bool isDefault = false);

    QString text;
    int resultCode;
    QDialogButtonBox::ButtonRole role;
    bool isDefault;
};

int chooseGuardMessage(QWidget *parent,
                       const QString &title,
                       const QString &message,
                       GuardMessageIcon icon,
                       const QList<GuardMessageButton> &buttons);

void showGuardInformation(QWidget *parent,
                          const QString &title,
                          const QString &message,
                          const QString &acceptText);

void showGuardWarning(QWidget *parent,
                      const QString &title,
                      const QString &message,
                      const QString &acceptText);

bool confirmGuardMessage(QWidget *parent,
                         const QString &title,
                         const QString &message,
                         const QString &acceptText,
                         const QString &rejectText,
                         bool destructive = false);

class GuardFileDialog : public GuardDialog {
public:
    enum Mode {
        OpenExistingFile,
        SaveFile
    };

    GuardFileDialog(Mode mode,
                    const QString &title,
                    const QString &initialPath,
                    QWidget *parent = 0,
                    const QString &language = QString());

    QString selectedFile() const;

private:
    QString localized(const QString &zh, const QString &en) const;
    void navigateTo(const QString &directory);
    void selectCurrentFile();

    Mode mode_;
    bool english_;
    QFileSystemModel *model_;
    QTreeView *tree_;
    QLineEdit *directoryEdit_;
    QLineEdit *fileNameEdit_;
    QString currentDirectory_;
    QString selectedFile_;
};

} // namespace net_tunnel_gui
