#include "core/app_config.h"
#include "gui/main_window.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QSet>
#include <QtCore/QTemporaryDir>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QScreen>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLineEdit>

#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>

#ifdef FontChange
#undef FontChange
#endif
#ifdef CursorShape
#undef CursorShape
#endif

using namespace net_tunnel;

namespace {

class ContentPresentationCounter : public QObject {
public:
    explicit ContentPresentationCounter(QWidget *root)
        : root_(root), fontChanges_(0), styleChanges_(0)
    {
    }

    int fontChanges() const { return fontChanges_; }
    int styleChanges() const { return styleChanges_; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        QWidget *widget = qobject_cast<QWidget *>(watched);
        if (widget && root_ &&
            (widget == root_ || root_->isAncestorOf(widget))) {
            if (event->type() == QEvent::FontChange) {
                ++fontChanges_;
            } else if (event->type() == QEvent::StyleChange) {
                ++styleChanges_;
            }
        }
        return QObject::eventFilter(watched, event);
    }

private:
    QWidget *root_;
    int fontChanges_;
    int styleChanges_;
};

bool containsRenderedContent(const QPixmap &pixmap)
{
    if (pixmap.isNull()) {
        return false;
    }
    const QImage image = pixmap.toImage().convertToFormat(QImage::Format_RGB32);
    QSet<QRgb> colors;
    const int xStep = qMax(1, image.width() / 48);
    const int yStep = qMax(1, image.height() / 32);
    for (int y = 0; y < image.height(); y += yStep) {
        for (int x = 0; x < image.width(); x += xStep) {
            colors.insert(image.pixel(x, y));
            if (colors.size() >= 6) {
                return true;
            }
        }
    }
    return false;
}

QPixmap grabCompositedRegion(const QRect &geometry)
{
    QScreen *screen = QGuiApplication::primaryScreen();
    return screen
        ? screen->grabWindow(0, geometry.x(), geometry.y(),
                             geometry.width(), geometry.height())
        : QPixmap();
}

bool sampleRenderedFrames(const QRect &geometry, int durationMs, int intervalMs)
{
    QElapsedTimer timer;
    timer.start();
    bool allRendered = true;
    do {
        QTest::qWait(intervalMs);
        allRendered = containsRenderedContent(grabCompositedRegion(geometry)) &&
            allRendered;
    } while (timer.elapsed() < durationMs);
    return allRendered;
}

QWidget *findResizeOutline()
{
    const QWidgetList topLevels = QApplication::topLevelWidgets();
    for (int i = 0; i < topLevels.size(); ++i) {
        QWidget *widget = topLevels.at(i);
        if (widget && widget->objectName() == QLatin1String("resizeOutline")) {
            return widget;
        }
    }
    return 0;
}

} // namespace

class MainWindowX11ResizeIntegrationTest : public QObject {
    Q_OBJECT

private slots:
    void realPointerDragContinuesAfterPause()
    {
        if (QGuiApplication::platformName() != QLatin1String("xcb")) {
            QSKIP("This integration test requires Qt's xcb platform plugin.");
        }

        Display *display = XOpenDisplay(0);
        if (!display) {
            QSKIP("No X11 display is available.");
        }
        int eventBase = 0;
        int errorBase = 0;
        int majorVersion = 0;
        int minorVersion = 0;
        if (!XTestQueryExtension(display, &eventBase, &errorBase,
                                 &majorVersion, &minorVersion)) {
            XCloseDisplay(display);
            QSKIP("The XTEST extension is unavailable.");
        }

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString configPath = directory.filePath("config.json");
        qputenv("NET_TUNNEL_CONFIG", configPath.toLocal8Bit());
        AppConfig config;
        QString error;
        QVERIFY2(saveConfig(config, configPath, &error), qPrintable(error));

        MainWindow window;
        const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
        const QSize initialSize(
            qMin(3000, qMax(window.minimumWidth(), available.width() - 220)),
            qMin(1700, qMax(window.minimumHeight(), available.height() - 108)));
        window.resize(initialSize);
        window.show();
        window.move(20, 20);
        window.raise();
        window.activateWindow();
        QTRY_VERIFY(window.isVisible());
        QTest::qWait(100);

        QWidget *rootContent = window.findChild<QWidget *>("rootContent");
        QVERIFY(rootContent);
        QLineEdit *lineEdit = rootContent->findChild<QLineEdit *>("proxyHostEdit");
        QVERIFY(lineEdit);
        const qreal baseUiScale = window.property("ui_scale_factor").toReal();
        QVERIFY(baseUiScale > 0.0);
        const QFont baseLineEditFont = lineEdit->font();
        const QString baseLineEditStyle = lineEdit->styleSheet();
        const QString baseRootStyle = rootContent->styleSheet();
        ContentPresentationCounter presentationChanges(rootContent);
        qApp->installEventFilter(&presentationChanges);

        const QRect before = window.geometry();
        const int screen = DefaultScreen(display);
        const Window nativeWindow = static_cast<Window>(window.winId());
        XRaiseWindow(display, nativeWindow);
        XSetInputFocus(display, nativeWindow, RevertToParent, CurrentTime);
        XSync(display, False);
        QTest::qWait(30);
        Window rootWindow = 0;
        int rootX = 0;
        int rootY = 0;
        unsigned int nativeWidth = 0;
        unsigned int nativeHeight = 0;
        unsigned int borderWidth = 0;
        unsigned int depth = 0;
        QVERIFY(XGetGeometry(display, nativeWindow, &rootWindow,
                             &rootX, &rootY, &nativeWidth, &nativeHeight,
                             &borderWidth, &depth));
        int translatedX = 0;
        int translatedY = 0;
        Window childWindow = 0;
        QVERIFY(XTranslateCoordinates(display, nativeWindow,
                                     DefaultRootWindow(display), 0, 0,
                                     &translatedX, &translatedY, &childWindow));
        const QPoint start(translatedX + int(nativeWidth) - 2,
                           translatedY + int(nativeHeight) - 2);
        const QPoint center(translatedX + int(nativeWidth) / 2,
                            translatedY + int(nativeHeight) / 2);
        QVERIFY(XTestFakeMotionEvent(display, screen, center.x(), center.y(), CurrentTime));
        XSync(display, False);
        QTest::qWait(20);
        QVERIFY(XTestFakeMotionEvent(display, screen, start.x(), start.y(), CurrentTime));
        XSync(display, False);
        QTRY_COMPARE(window.cursor().shape(), Qt::SizeFDiagCursor);

        QVERIFY(XTestFakeButtonEvent(display, 1, True, CurrentTime));
        XSync(display, False);
        QWidget *resizePreview = 0;
        QTRY_VERIFY((resizePreview = findResizeOutline()) != 0);
        QTRY_VERIFY(resizePreview->isVisible());
        QVERIFY(resizePreview->mask().contains(QPoint(1, 1)));
        QVERIFY(!resizePreview->mask().contains(resizePreview->rect().center()));
        QCOMPARE(window.property("resize_preview_geometry").toRect(), before);
        QTRY_VERIFY(window.property("resize_uses_native_gesture").isValid());
        QVERIFY(!window.property("resize_uses_native_gesture").toBool());
        bool allFramesRendered = true;
        const QList<QPoint> firstDrag = QList<QPoint>()
            << QPoint(12, 8) << QPoint(28, 22) << QPoint(21, 31)
            << QPoint(46, 38) << QPoint(60, 50);
        for (int i = 0; i < firstDrag.size(); ++i) {
            const QPoint destination = start + firstDrag.at(i);
            QVERIFY(XTestFakeMotionEvent(display, screen,
                                        destination.x(), destination.y(), CurrentTime));
            XSync(display, False);
            allFramesRendered = sampleRenderedFrames(before, 32, 4) &&
                allFramesRendered;
        }
        allFramesRendered = sampleRenderedFrames(before, 1000, 10) &&
            allFramesRendered;

        QCOMPARE(window.geometry(), before);
        const QRect afterPausePreview =
            window.property("resize_preview_geometry").toRect();
        QVERIFY(afterPausePreview.width() > before.width());
        QVERIFY(afterPausePreview.height() > before.height());
        QVERIFY(resizePreview->isVisible());
        QVERIFY(containsRenderedContent(grabCompositedRegion(before)));
        QCOMPARE(window.property("ui_scale_factor").toReal(), baseUiScale);
        QCOMPARE(lineEdit->font(), baseLineEditFont);
        QCOMPARE(lineEdit->styleSheet(), baseLineEditStyle);
        QCOMPARE(rootContent->styleSheet(), baseRootStyle);
        QCOMPARE(presentationChanges.fontChanges(), 0);
        QCOMPARE(presentationChanges.styleChanges(), 0);

        const QList<QPoint> secondDrag = QList<QPoint>()
            << QPoint(74, 57) << QPoint(68, 66) << QPoint(96, 72)
            << QPoint(112, 88) << QPoint(140, 104);
        for (int i = 0; i < secondDrag.size(); ++i) {
            const QPoint destination = start + secondDrag.at(i);
            QVERIFY(XTestFakeMotionEvent(display, screen,
                                        destination.x(), destination.y(), CurrentTime));
            XSync(display, False);
            allFramesRendered = sampleRenderedFrames(before, 32, 4) &&
                allFramesRendered;
        }
        QCOMPARE(window.geometry(), before);
        const QRect finalPreview =
            window.property("resize_preview_geometry").toRect();
        QVERIFY(finalPreview.width() > afterPausePreview.width());
        QVERIFY(finalPreview.height() > afterPausePreview.height());
        QVERIFY(containsRenderedContent(grabCompositedRegion(before)));
        QCOMPARE(window.property("ui_scale_factor").toReal(), baseUiScale);
        QCOMPARE(lineEdit->font(), baseLineEditFont);
        QCOMPARE(lineEdit->styleSheet(), baseLineEditStyle);
        QCOMPARE(rootContent->styleSheet(), baseRootStyle);
        QCOMPARE(presentationChanges.fontChanges(), 0);
        QCOMPARE(presentationChanges.styleChanges(), 0);

        QVERIFY(XTestFakeButtonEvent(display, 1, False, CurrentTime));
        XSync(display, False);
        QTRY_VERIFY(!resizePreview->isVisible());
        QTRY_COMPARE(window.geometry(), finalPreview);
        QTRY_COMPARE(window.cursor().shape(), Qt::ArrowCursor);
        QVERIFY(containsRenderedContent(grabCompositedRegion(finalPreview)));
        QVERIFY(allFramesRendered);
        QCOMPARE(window.property("ui_scale_factor").toReal(), baseUiScale);
        QCOMPARE(lineEdit->font(), baseLineEditFont);
        QCOMPARE(lineEdit->styleSheet(), baseLineEditStyle);
        QCOMPARE(rootContent->styleSheet(), baseRootStyle);
        QCOMPARE(presentationChanges.fontChanges(), 0);
        QCOMPARE(presentationChanges.styleChanges(), 0);
        qApp->removeEventFilter(&presentationChanges);
        XCloseDisplay(display);
    }
};

int main(int argc, char **argv)
{
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif
    QApplication app(argc, argv);
    MainWindowX11ResizeIntegrationTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "main_window_x11_resize_integration_test.moc"
