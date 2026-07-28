#include "gui/guard_dialog.h"

#include <QtCore/QDir>
#include <QtCore/QTimer>
#include <QtGui/QFont>
#include <QtGui/QFontDatabase>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QTreeView>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

using namespace net_tunnel_gui;

namespace {

QRect safeDialogGeometry()
{
    return QGuiApplication::primaryScreen()->availableGeometry().adjusted(24, 24, -24, -24);
}

} // namespace

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
        QCOMPARE(titleBar->height(), 30);
        QCOMPARE(close->size(), QSize(30, 30));
        QCOMPARE(title->alignment(), Qt::AlignCenter);
        QVERIFY(QFontDatabase().families().contains("FontAwesome"));
        QCOMPARE(close->font().family(), QString("FontAwesome"));
        QCOMPARE(close->text(), QString(QChar(0xf00d)));
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

    void inheritsParentUiScaleWithoutChangingApplicationPresentation()
    {
        QWidget parent;
        parent.setProperty("ui_scale_factor", 1.30);
        parent.setAttribute(Qt::WA_DontShowOnScreen);
        parent.show();

        GuardFileDialog dialog(GuardFileDialog::SaveFile, "Export profiles",
                               QDir::home().filePath("profiles.json"), &parent);
        const QFont applicationFont = QApplication::font();
        const QString dialogStyleSheet = dialog.styleSheet();
        QLineEdit *directory = dialog.findChild<QLineEdit *>("guardFileDirectoryEdit");
        QDialogButtonBox *buttonBox = dialog.findChild<QDialogButtonBox *>();
        QTreeView *tree = dialog.findChild<QTreeView *>("guardFileTree");
        QWidget *titleBar = dialog.findChild<QWidget *>("guardDialogTitleBar");
        QPushButton *close = dialog.findChild<QPushButton *>("guardDialogCloseButton");
        QVERIFY(directory);
        QVERIFY(buttonBox);
        QVERIFY(tree);
        QVERIFY(!buttonBox->buttons().isEmpty());
        QVERIFY(titleBar);
        QVERIFY(close);
        const qreal baseDirectoryPointSize = directory->font().pointSizeF();
        QAbstractButton *dialogButton = buttonBox->buttons().first();
        const int baseButtonHeight = dialogButton->sizeHint().height();
        const int baseScrollBarWidth = tree->verticalScrollBar()->sizeHint().width();

        dialog.setAttribute(Qt::WA_DontShowOnScreen);
        dialog.show();
        QTest::qWait(20);

        QVERIFY(qAbs(dialog.property("ui_scale_factor").toReal() - 1.30) < 0.001);
        QCOMPARE(titleBar->height(), 39);
        QCOMPARE(close->size(), QSize(39, 39));
        const QRect safeGeometry = safeDialogGeometry();
        const QSize expectedMinimum = QSize(936, 624).boundedTo(safeGeometry.size());
        const QSize expectedSize = QSize(1066, 728).boundedTo(safeGeometry.size())
            .expandedTo(expectedMinimum);
        QCOMPARE(dialog.minimumSize(), expectedMinimum);
        QCOMPARE(dialog.size(), expectedSize);
        QVERIFY(safeGeometry.contains(dialog.geometry()));
        QCOMPARE(dialog.contentLayout()->contentsMargins(), QMargins(18, 18, 18, 18));
        if (baseDirectoryPointSize > 0.0) {
            QVERIFY(directory->font().pointSizeF() > baseDirectoryPointSize);
        }
        QVERIFY(dialogButton->minimumHeight() >= qRound(baseButtonHeight * 1.30));
        QVERIFY(tree->verticalScrollBar()->width() >= qRound(baseScrollBarWidth * 1.30));
        QCOMPARE(QApplication::font(), applicationFont);
        QCOMPARE(dialog.styleSheet(), dialogStyleSheet);

        dialog.hide();
        parent.hide();
    }
};

QTEST_MAIN(GuardDialogTest)

#include "guard_dialog_test.moc"
