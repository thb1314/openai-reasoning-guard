#include "quiwidget.h"

#include <QtCore/QCoreApplication>
#include <QtTest/QtTest>

class QuiWidgetTest : public QObject {
    Q_OBJECT

private slots:
    void doubleClickOnlyMaximizesFromTitleBar()
    {
        QUIWidget window;
        window.setGeometry(80, 80, 480, 320);
        window.getBtnMenuMax()->setVisible(true);
        window.show();
        QCoreApplication::processEvents();

        const QRect normalGeometry = window.geometry();
        QTest::mouseDClick(&window,
                          Qt::LeftButton,
                          Qt::NoModifier,
                          QPoint(window.width() / 2, window.height() - 40));
        QCoreApplication::processEvents();
        QCOMPARE(window.geometry(), normalGeometry);

        QLabel *title = window.getLabTitle();
        QVERIFY(title);
        QTest::mouseDClick(title,
                          Qt::LeftButton,
                          Qt::NoModifier,
                          QPoint(title->width() / 2, title->height() / 2));
        QCoreApplication::processEvents();
        QVERIFY(window.geometry() != normalGeometry);

        QTest::mouseDClick(title,
                          Qt::LeftButton,
                          Qt::NoModifier,
                          QPoint(title->width() / 2, title->height() / 2));
        QCoreApplication::processEvents();
        QCOMPARE(window.geometry(), normalGeometry);
    }
};

QTEST_MAIN(QuiWidgetTest)

#include "qui_widget_test.moc"
