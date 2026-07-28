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
        QVERIFY(window.isMaximized());
        QCOMPARE(window.getBtnMenuMax()->text(), QString(QUIConfig::IconMax));

        QTest::mouseDClick(title,
                          Qt::LeftButton,
                          Qt::NoModifier,
                          QPoint(title->width() / 2, title->height() / 2));
        QCoreApplication::processEvents();
        QVERIFY(!window.isMaximized());
        QCOMPARE(window.geometry(), normalGeometry);
        QCOMPARE(window.getBtnMenuMax()->text(), QString(QUIConfig::IconNormal));
    }

    void externalWindowStateChangesSynchronizeMaximizeButton()
    {
        QUIWidget window;
        window.setGeometry(80, 80, 480, 320);
        window.getBtnMenuMax()->setVisible(true);
        window.show();
        QCoreApplication::processEvents();

        window.showMaximized();
        QTRY_VERIFY(window.isMaximized());
        QCOMPARE(window.getBtnMenuMax()->text(), QString(QUIConfig::IconMax));

        window.showNormal();
        QTRY_VERIFY(!window.isMaximized());
        QCOMPARE(window.getBtnMenuMax()->text(), QString(QUIConfig::IconNormal));
    }

    void pixelSizeMaximizeButtonKeepsItsIconSize()
    {
        QUIWidget window;
        window.setGeometry(80, 80, 480, 320);
        window.getBtnMenuMax()->setVisible(true);
        QFont pixelFont = window.getBtnMenuMax()->font();
        pixelFont.setPixelSize(19);
        QCOMPARE(pixelFont.pixelSize(), 19);
        QCOMPARE(pixelFont.pointSize(), -1);
        window.getBtnMenuMax()->setFont(pixelFont);
        window.show();
        QCoreApplication::processEvents();

        window.getBtnMenuMax()->click();
        QTRY_VERIFY(window.isMaximized());
        QCOMPARE(window.getBtnMenuMax()->font().pixelSize(), 19);
        QCOMPARE(window.getBtnMenuMax()->font().pointSize(), -1);
        QCOMPARE(window.getBtnMenuMax()->text(), QString(QUIConfig::IconMax));

        window.getBtnMenuMax()->click();
        QTRY_VERIFY(!window.isMaximized());
        QCOMPARE(window.getBtnMenuMax()->font().pixelSize(), 19);
        QCOMPARE(window.getBtnMenuMax()->font().pointSize(), -1);
        QCOMPARE(window.getBtnMenuMax()->text(), QString(QUIConfig::IconNormal));
    }

    void pointSizeMaximizeButtonKeepsItsIconSize()
    {
        QUIWidget window;
        window.setGeometry(80, 80, 480, 320);
        window.getBtnMenuMax()->setVisible(true);
        QFont pointFont = window.getBtnMenuMax()->font();
        pointFont.setPointSizeF(13.5);
        QCOMPARE(pointFont.pixelSize(), -1);
        QVERIFY(qFuzzyCompare(pointFont.pointSizeF(), 13.5));
        window.getBtnMenuMax()->setFont(pointFont);
        window.show();
        QCoreApplication::processEvents();

        window.getBtnMenuMax()->click();
        QTRY_VERIFY(window.isMaximized());
        QCOMPARE(window.getBtnMenuMax()->font().pixelSize(), -1);
        QVERIFY(qFuzzyCompare(window.getBtnMenuMax()->font().pointSizeF(), 13.5));
        QCOMPARE(window.getBtnMenuMax()->text(), QString(QUIConfig::IconMax));
    }

    void titleBarManualMoveFallbackMovesOnlyTheWindow()
    {
        QUIWidget window;
        window.setGeometry(80, 80, 480, 320);
        window.show();
        QCoreApplication::processEvents();

        QLabel *title = window.getLabTitle();
        QVERIFY(title);
        const QPoint originalPosition = window.pos();
        const QPoint start(title->width() / 2, title->height() / 2);
        const QPoint destination = start + QPoint(50, 35);

        QTest::mousePress(title, Qt::LeftButton, Qt::NoModifier, start);
        QMouseEvent moveEvent(QEvent::MouseMove,
                              destination,
                              title->mapToGlobal(destination),
                              Qt::NoButton,
                              Qt::LeftButton,
                              Qt::NoModifier);
        QCoreApplication::sendEvent(title, &moveEvent);
        QTest::mouseRelease(title, Qt::LeftButton, Qt::NoModifier, destination);
        QCoreApplication::processEvents();

        QCOMPARE(window.pos(), originalPosition + QPoint(50, 35));
    }
};

QTEST_MAIN(QuiWidgetTest)

#include "qui_widget_test.moc"
