#include "gui/main_window.h"

#include "gui/app_icon.h"
#include "core/openssl_runtime.h"
#include "quiwidget.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtGui/QFont>
#include <QtGui/QFontDatabase>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QApplication>

#include <cstdio>

static void loadApplicationFonts()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList fontFiles = QStringList()
        << ":/fonts/LXGWWenKai-Bold.ttf"
        << QDir(appDir).filePath("../third_party/fonts/LXGWWenKai-Bold.ttf")
        << QDir(appDir).filePath("../third_party/fonts/DroidSansFallbackFull.ttf")
        << QDir(appDir).filePath("../../third_party/fonts/LXGWWenKai-Bold.ttf")
        << QDir(appDir).filePath("../../third_party/fonts/DroidSansFallbackFull.ttf")
        << "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
        << "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf";

    QStringList families;
    for (int i = 0; i < fontFiles.size(); ++i) {
        if (!QFile::exists(fontFiles.at(i))) {
            continue;
        }
        const int id = QFontDatabase::addApplicationFont(fontFiles.at(i));
        if (id >= 0) {
            families.append(QFontDatabase::applicationFontFamilies(id));
        }
    }
    if (!families.isEmpty()) {
        QFont font = QApplication::font();
        font.setFamily(families.first());
        QApplication::setFont(font);
    }
}

int main(int argc, char **argv)
{
    QString runtimeError;
    if (!net_tunnel::ensureBundledOpenSslRuntime(argc, argv, &runtimeError)) {
        fprintf(stderr, "%s\n", runtimeError.toLocal8Bit().constData());
        return 127;
    }

    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif

    QApplication app(argc, argv);
    QApplication::setApplicationName("openai-reasoning-guard-gui");
    QApplication::setApplicationVersion("0.1.0");
#if defined(Q_OS_LINUX)
    QGuiApplication::setDesktopFileName(QStringLiteral("openai-reasoning-guard-gui"));
#endif
    QApplication::setWindowIcon(makeAppIcon());
    loadApplicationFonts();
    QUIWidget::setStyle(QUIWidget::Style_LightBlue);

    MainWindow window;
    window.show();
    return app.exec();
}
