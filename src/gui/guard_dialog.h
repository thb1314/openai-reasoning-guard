#pragma once

#include <QtCore/QList>
#include <QtCore/QPoint>
#include <QtCore/QString>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>

class QFileSystemModel;
class QLineEdit;
class QPushButton;
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

private:
    void updateTitleChrome();

    QWidget *titleBar_;
    QWidget *contentWidget_;
    QVBoxLayout *contentLayout_;
    QWidget *titleIcon_;
    QWidget *titleLabel_;
    QPushButton *closeButton_;
    bool dragging_;
    QPoint dragOffset_;
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
