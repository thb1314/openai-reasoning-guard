#ifndef QUIWIDGET_MACOS_H
#define QUIWIDGET_MACOS_H

class QWidget;

void quiEnsureFramelessWindowCanMinimize(QWidget *widget);
bool quiMacWindowCanMinimize(QWidget *widget);
bool quiMacWindowHasNativeTitleBar(QWidget *widget);

#endif
