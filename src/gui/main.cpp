#include "gui/main_window.h"

#include "quiwidget.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtGui/QFont>
#include <QtGui/QFontDatabase>
#include <QtGui/QIcon>
#include <QtWidgets/QApplication>

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
        QApplication::setFont(QFont(families.first(), 10));
    }
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("openai-reasoning-guard-gui");
    QApplication::setApplicationVersion("0.1.0");
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/app-icon.png")));
    loadApplicationFonts();
    QUIWidget::setStyle(QUIWidget::Style_LightBlue);

    MainWindow window;
    window.show();
    return app.exec();
}
