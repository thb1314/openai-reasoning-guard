#include "quiwidget_macos.h"

#include <QGuiApplication>
#include <QWidget>

#import <AppKit/AppKit.h>

namespace {

NSWindow *nativeWindowForWidget(QWidget *widget)
{
    if (!widget || QGuiApplication::platformName() != QLatin1String("cocoa")) {
        return nil;
    }

    NSView *view = reinterpret_cast<NSView *>(widget->winId());
    return view.window;
}

} // namespace

void quiEnsureFramelessWindowCanMinimize(QWidget *widget)
{
    NSWindow *window = nativeWindowForWidget(widget);
    if (!window) {
        return;
    }

    window.styleMask |= NSWindowStyleMaskMiniaturizable;
}

bool quiMacWindowCanMinimize(QWidget *widget)
{
    NSWindow *window = nativeWindowForWidget(widget);
    return window && (window.styleMask & NSWindowStyleMaskMiniaturizable);
}

bool quiMacWindowHasNativeTitleBar(QWidget *widget)
{
    NSWindow *window = nativeWindowForWidget(widget);
    return window && (window.styleMask & NSWindowStyleMaskTitled);
}
