// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QApplication>
#include <QColor>
#include <QLineEdit>
#include <QWidget>

enum class Theme : int {
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

class WindowThemes : public QObject {
    Q_OBJECT
public:
    explicit WindowThemes(QObject* parent = nullptr) : QObject(parent) {}

    void SetWindowTheme(Theme theme, QLineEdit* mw_searchbar);
    void ApplyThemeToDialog(QDialog* dialog);

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
    QColor m_iconBaseColor{Qt::white};
    QColor m_iconHoverColor{Qt::lightGray};
    QColor m_textColor{Qt::white};
};
