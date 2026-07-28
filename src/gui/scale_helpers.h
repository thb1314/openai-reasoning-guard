#pragma once

#include <QtCore/QVariant>
#include <QtWidgets/QAbstractSpinBox>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QRadioButton>
#include <QtWidgets/QWidget>

namespace net_tunnel_gui {

namespace ui_scale_detail {

inline int metric(int value, qreal scale)
{
    return qMax(0, qRound(value * scale));
}

inline bool belongsToRootWindow(QWidget *root, QWidget *widget)
{
    return root && widget && widget->window() == root->window();
}

inline void captureBaseStyleSheet(QWidget *widget)
{
    if (widget && !widget->property("guard_scale_base_style_sheet").isValid()) {
        widget->setProperty("guard_scale_base_style_sheet", widget->styleSheet());
    }
}

inline void applyStyleOverride(QWidget *widget, const QString &overrideStyle)
{
    if (!widget) return;
    captureBaseStyleSheet(widget);
    QString style = widget->property("guard_scale_base_style_sheet").toString();
    if (!overrideStyle.isEmpty()) {
        if (!style.isEmpty()) style.append('\n');
        style.append(overrideStyle);
    }
    if (widget->styleSheet() != style) {
        widget->setStyleSheet(style);
    }
}

} // namespace ui_scale_detail

inline void captureScaleSensitiveStyleBaselines(QWidget *root)
{
    if (!root) return;
    const QList<QCheckBox *> checkBoxes = root->findChildren<QCheckBox *>();
    for (int i = 0; i < checkBoxes.size(); ++i) {
        if (ui_scale_detail::belongsToRootWindow(root, checkBoxes.at(i))) {
            ui_scale_detail::captureBaseStyleSheet(checkBoxes.at(i));
        }
    }
    const QList<QRadioButton *> radioButtons = root->findChildren<QRadioButton *>();
    for (int i = 0; i < radioButtons.size(); ++i) {
        if (ui_scale_detail::belongsToRootWindow(root, radioButtons.at(i))) {
            ui_scale_detail::captureBaseStyleSheet(radioButtons.at(i));
        }
    }
    const QList<QAbstractSpinBox *> spinBoxes = root->findChildren<QAbstractSpinBox *>();
    for (int i = 0; i < spinBoxes.size(); ++i) {
        if (ui_scale_detail::belongsToRootWindow(root, spinBoxes.at(i))) {
            ui_scale_detail::captureBaseStyleSheet(spinBoxes.at(i));
        }
    }
    const QList<QComboBox *> comboBoxes = root->findChildren<QComboBox *>();
    for (int i = 0; i < comboBoxes.size(); ++i) {
        if (ui_scale_detail::belongsToRootWindow(root, comboBoxes.at(i))) {
            ui_scale_detail::captureBaseStyleSheet(comboBoxes.at(i));
        }
    }
}

inline void applyScaleSensitiveSubcontrols(QWidget *root, qreal scale)
{
    if (!root) return;

    const int checkExtent = ui_scale_detail::metric(13, scale);
    const QString checkStyle = checkExtent == 13 ? QString() : QString(
        "QCheckBox::indicator { width: %1px; height: %1px; }").arg(checkExtent);
    const QList<QCheckBox *> checkBoxes = root->findChildren<QCheckBox *>();
    for (int i = 0; i < checkBoxes.size(); ++i) {
        if (ui_scale_detail::belongsToRootWindow(root, checkBoxes.at(i))) {
            ui_scale_detail::applyStyleOverride(checkBoxes.at(i), checkStyle);
        }
    }

    const int radioExtent = ui_scale_detail::metric(15, scale);
    const QString radioStyle = radioExtent == 15 ? QString() : QString(
        "QRadioButton::indicator { width: %1px; height: %1px; }").arg(radioExtent);
    const QList<QRadioButton *> radioButtons = root->findChildren<QRadioButton *>();
    for (int i = 0; i < radioButtons.size(); ++i) {
        if (ui_scale_detail::belongsToRootWindow(root, radioButtons.at(i))) {
            ui_scale_detail::applyStyleOverride(radioButtons.at(i), radioStyle);
        }
    }

    const int spinArrowExtent = ui_scale_detail::metric(10, scale);
    const int spinVerticalPadding = ui_scale_detail::metric(2, scale);
    const int spinRightPadding = ui_scale_detail::metric(5, scale);
    const bool spinAtBaseline = spinArrowExtent == 10 &&
        spinVerticalPadding == 2 && spinRightPadding == 5;
    const QString spinStyle = spinAtBaseline ? QString() : QString(
        "QSpinBox::up-button, QDoubleSpinBox::up-button, QDateEdit::up-button, "
        "QTimeEdit::up-button, QDateTimeEdit::up-button { width: %1px; height: %1px; "
        "padding: %2px %3px 0px 0px; }"
        "QSpinBox::down-button, QDoubleSpinBox::down-button, QDateEdit::down-button, "
        "QTimeEdit::down-button, QDateTimeEdit::down-button { width: %1px; height: %1px; "
        "padding: 0px %3px %2px 0px; }")
            .arg(spinArrowExtent).arg(spinVerticalPadding).arg(spinRightPadding);
    const QList<QAbstractSpinBox *> spinBoxes = root->findChildren<QAbstractSpinBox *>();
    for (int i = 0; i < spinBoxes.size(); ++i) {
        if (ui_scale_detail::belongsToRootWindow(root, spinBoxes.at(i))) {
            ui_scale_detail::applyStyleOverride(spinBoxes.at(i), spinStyle);
        }
    }

    const int comboArrowExtent = ui_scale_detail::metric(10, scale);
    const int comboArrowOffset = ui_scale_detail::metric(2, scale);
    const int comboDropDownWidth = ui_scale_detail::metric(15, scale);
    const int comboItemHeight = ui_scale_detail::metric(20, scale);
    const int comboItemWidth = ui_scale_detail::metric(10, scale);
    const bool comboAtBaseline = comboArrowExtent == 10 && comboArrowOffset == 2 &&
        comboDropDownWidth == 15 && comboItemHeight == 20 && comboItemWidth == 10;
    const QString comboStyle = comboAtBaseline ? QString() : QString(
        "QComboBox::down-arrow { width: %1px; height: %1px; right: %2px; }"
        "QComboBox::drop-down { width: %3px; }"
        "QComboBox QAbstractItemView::item { min-height: %4px; min-width: %5px; }")
            .arg(comboArrowExtent).arg(comboArrowOffset).arg(comboDropDownWidth)
            .arg(comboItemHeight).arg(comboItemWidth);
    const QList<QComboBox *> comboBoxes = root->findChildren<QComboBox *>();
    for (int i = 0; i < comboBoxes.size(); ++i) {
        if (ui_scale_detail::belongsToRootWindow(root, comboBoxes.at(i))) {
            ui_scale_detail::applyStyleOverride(comboBoxes.at(i), comboStyle);
        }
    }
}

} // namespace net_tunnel_gui
