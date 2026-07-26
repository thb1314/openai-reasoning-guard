#include "gui/guard_dialog.h"

#include <QtCore/QDir>
#include <QtCore/QTimer>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>

using namespace net_tunnel_gui;

class GuardDialogTest : public QObject {
    Q_OBJECT

private slots:
    void dialogUsesFramelessCustomTitleBar()
    {
        GuardDialog dialog;
        dialog.setWindowTitle("Custom title");

        QVERIFY(dialog.windowFlags() & Qt::FramelessWindowHint);
        QWidget *titleBar = dialog.findChild<QWidget *>("guardDialogTitleBar");
        QLabel *title = dialog.findChild<QLabel *>("guardDialogTitleText");
        QPushButton *close = dialog.findChild<QPushButton *>("guardDialogCloseButton");
        QVERIFY(titleBar);
        QVERIFY(title);
        QVERIFY(close);
        QCOMPARE(title->text(), QString("Custom title"));
        QCOMPARE(close->size(), QSize(38, 38));
    }

    void messagesAndFilePickerUseTheSameChrome()
    {
        GuardFileDialog fileDialog(GuardFileDialog::SaveFile, "Export profiles",
                                   QDir::home().filePath("profiles.json"));
        QVERIFY(fileDialog.windowFlags() & Qt::FramelessWindowHint);
        QVERIFY(fileDialog.findChild<QWidget *>("guardDialogTitleBar"));
        QVERIFY(fileDialog.findChild<QWidget *>("guardFileTree"));

        bool sawMessage = false;
        QTimer::singleShot(0, [&sawMessage]() {
            const QList<QWidget *> windows = QApplication::topLevelWidgets();
            for (int i = 0; i < windows.size(); ++i) {
                QWidget *window = windows.at(i);
                if (window->property("guard_dialog_kind").toString() != "message") {
                    continue;
                }
                sawMessage = (window->windowFlags() & Qt::FramelessWindowHint)
                    && window->findChild<QWidget *>("guardDialogTitleBar")
                    && window->findChild<QPushButton *>("guardDialogCloseButton");
                QDialog *dialog = qobject_cast<QDialog *>(window);
                if (dialog) dialog->reject();
            }
        });
        QCOMPARE(chooseGuardMessage(0, "Notice", "A custom message", GuardInformation,
                                    QList<GuardMessageButton>()
                                        << GuardMessageButton("OK", QDialog::Accepted,
                                                              QDialogButtonBox::AcceptRole, true)),
                 int(QDialog::Rejected));
        QVERIFY(sawMessage);
    }
};

QTEST_MAIN(GuardDialogTest)

#include "guard_dialog_test.moc"
