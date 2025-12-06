// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QColor>
#include <QString>

class QLineEdit;
class QDialog;
class QWidget;

enum class Theme {
    Dark,
    Light,
    Green,
    Blue,
    Violet,
    Gruvbox,
    TokyoNight,
    Oled,
    Neon,
    Shadlix,
    ShadlixCave
};

class WindowThemes {
public:
    // Updated signature to accept listWidget and gridWidget again
    void SetWindowTheme(Theme theme, QLineEdit* mw_searchbar, QWidget* listWidget,
                        QWidget* gridWidget, bool applyGlobalStylesheet = true);

    void ApplyThemeToDialog(QDialog* dialog);
    void ApplyThemeToWidget(QWidget* widget);

    QColor iconBaseColor() const {
        return m_iconBaseColor;
    }
    QColor iconHoverColor() const {
        return m_iconHoverColor;
    }
    QColor textColor() const {
        return m_textColor;
    }

private:
    QColor m_iconBaseColor;
    QColor m_iconHoverColor;
    QColor m_textColor;
};