#ifndef GUI_APP_ICON_H
#define GUI_APP_ICON_H

#include <QtCore/QString>
#include <QtGui/QIcon>
#include <QtGui/QPixmap>

static inline QIcon makeAppIcon()
{
    const QPixmap source(QStringLiteral(":/app-icon.png"));
    QIcon icon;
    if (!source.isNull()) {
        const int sizes[] = {16, 24, 32, 48, 64, 128, 256};
        for (int size : sizes) {
            icon.addPixmap(source.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
    }
    if (icon.isNull()) {
        icon = QIcon(QStringLiteral(":/app-icon.png"));
    }
    return icon;
}

#endif
