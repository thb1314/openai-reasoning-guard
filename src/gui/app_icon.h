#ifndef GUI_APP_ICON_H
#define GUI_APP_ICON_H

#include <QtCore/QString>
#include <QtGui/QIcon>

static inline QIcon makeAppIcon()
{
    QIcon icon;
    const int sizes[] = {16, 22, 24, 32, 48, 64, 128, 256, 512};
    for (int size : sizes) {
        icon.addFile(QStringLiteral(":/app-icon-%1.png").arg(size), QSize(size, size));
    }
    return icon;
}

#endif
