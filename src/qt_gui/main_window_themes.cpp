// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QTableWidget>
#include "main_window_themes.h"

// Helper: Generates the stylesheet for the Game List / Grid
QString GenerateListStylesheet(const QString& windowBg, const QString& textColor,
                               const QString& selectionColor, const QString& gridColor,
                               const QString& headerBg) {
    return QString(R"(
        QTableWidget, QListWidget {
            /* background-color: transparent; <--- REMOVED! This was hiding the image */
            border: none;
            color: %2;
            gridline-color: %4;
            selection-background-color: %3;
            selection-color: white;
            outline: none;
        }
        /* Items must be transparent to see the list background/image */
        QTableWidget::item, QListWidget::item {
            border: none;
            background-color: transparent; 
        }
        QHeaderView::section {
            background-color: %5;
            color: %2;
            padding: 5px;
            border: none;
            border-bottom: 2px solid %3;
        }
        QScrollBar:vertical {
            background: %1;
            width: 12px;
            margin: 0px;
        }
        QScrollBar::handle:vertical {
            background: %4;
            border-radius: 6px;
            min-height: 20px;
            margin: 2px;
        }
        QScrollBar::handle:vertical:hover {
            background: %3;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
            height: 0px;
        }
    )")
        .arg(windowBg, textColor, selectionColor, gridColor, headerBg);
}

QString GenerateGlobalStylesheet(const QString& windowBg, const QString& textColor,
                                 const QString& toolbarBg, const QString& accentColor,
                                 const QString& hoverBg, const QString& inputBg,
                                 const QString& borderColor) {
    return QString(R"(
        QMainWindow, QDialog { background-color: %1; color: %2; }
        QWidget { color: %2; }
        QToolBar { background-color: %3; border-bottom: 2px solid %7; spacing: 12px; padding: 8px; }
        QMenuBar { background-color: %3; color: %2; border-bottom: 1px solid %7; }
        QMenuBar::item { background-color: transparent; color: %2; padding: 4px 10px; }
        QMenuBar::item:selected { background-color: %4; color: white; }
        QMenu { background-color: %6; color: %2; border: 1px solid %7; }
        QMenu::item { padding: 5px 20px; }
        QMenu::item:selected { background-color: %4; color: white; }
        QPushButton { background-color: transparent; border-radius: 8px; color: %2; padding: 4px; font-weight: bold; border: 1px solid transparent; }
        QPushButton:hover { background-color: %5; border: 1px solid %4; color: %2; }
        QPushButton:pressed { background-color: %4; color: white; }
        QLineEdit, QComboBox { background-color: %6; color: %2; border: 1px solid %7; border-radius: 8px; padding: 4px 10px; }
        QLineEdit:focus, QComboBox:focus { border: 1px solid %4; }
        QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: top right; width: 20px; border-left-width: 0px; border-top-right-radius: 8px; border-bottom-right-radius: 8px; }
        QComboBox QAbstractItemView { background-color: %6; color: %2; selection-background-color: %4; selection-color: white; border: 1px solid %7; }
    )")
        .arg(windowBg, textColor, toolbarBg, accentColor, hoverBg, inputBg, borderColor);
}

void WindowThemes::SetWindowTheme(Theme theme, QLineEdit* mw_searchbar, QWidget* listWidget,
                                  QWidget* gridWidget, bool applyGlobalStylesheet) {
    QPalette themePalette;

    if (applyGlobalStylesheet) {
        qApp->setStyleSheet("");
    }

    auto setSearchbar = [&](QString css) {
        if (mw_searchbar)
            mw_searchbar->setStyleSheet(css);
    };

    QString wBg, txt, toolBg, accent, hov, inp, border;

    switch (theme) {
    case Theme::Dark:
        wBg = "#323232";
        txt = "#ffffff";
        toolBg = "#252525";
        accent = "#2A82DA";
        hov = "#3e3e3e";
        inp = "#141414";
        border = "#555555";
        m_iconBaseColor = QColor(200, 200, 200);
        setSearchbar("QLineEdit { background-color:#1e1e1e; color:white; border:1px solid white; } "
                     "QLineEdit:focus { border:1px solid #2A82DA; }");
        break;
    case Theme::Light:
        wBg = "#f5f5f5";
        txt = "#000000";
        toolBg = "#e0e0e0";
        accent = "#0078d7";
        hov = "#d0d0d0";
        inp = "#ffffff";
        border = "#a0a0a0";
        m_iconBaseColor = Qt::black;
        setSearchbar("QLineEdit { background-color:#ffffff; color:black; border:1px solid black; } "
                     "QLineEdit:focus { border:1px solid #2A82DA; }");
        break;
    case Theme::Green:
        wBg = "#354535";
        txt = "#ffffff";
        toolBg = "#2a382a";
        accent = "#90ee90";
        hov = "#405540";
        inp = "#192819";
        border = "#90ee90";
        m_iconBaseColor = QColor(144, 238, 144);
        setSearchbar("QLineEdit { background-color:#192819; color:white; border:1px solid white; } "
                     "QLineEdit:focus { border:1px solid #2A82DA; }");
        break;
    case Theme::Blue:
        wBg = "#283c5a";
        txt = "#ffffff";
        toolBg = "#203048";
        accent = "#6495ed";
        hov = "#324b70";
        inp = "#14283c";
        border = "#6495ed";
        m_iconBaseColor = QColor(100, 149, 237);
        setSearchbar("QLineEdit { background-color:#14283c; color:white; border:1px solid white; } "
                     "QLineEdit:focus { border:1px solid #2A82DA; }");
        break;
    case Theme::Violet:
        wBg = "#643278";
        txt = "#ffffff";
        toolBg = "#502860";
        accent = "#ba55d3";
        hov = "#783c90";
        inp = "#3c1e48";
        border = "#ba55d3";
        m_iconBaseColor = QColor(186, 85, 211);
        setSearchbar("QLineEdit { background-color:#501e5a; color:white; border:1px solid white; } "
                     "QLineEdit:focus { border:1px solid #2A82DA; }");
        break;
    case Theme::Gruvbox:
        wBg = "#1d2021";
        txt = "#f9f5d7";
        toolBg = "#282828";
        accent = "#83a598";
        hov = "#3c3836";
        inp = "#32302f";
        border = "#504945";
        m_iconBaseColor = QColor(250, 189, 47);
        setSearchbar("QLineEdit { background-color:#1d2021; color:#f9f5d7; border:1px solid "
                     "#f9f5d7; } QLineEdit:focus { border:1px solid #2A82DA; }");
        break;
    case Theme::TokyoNight:
        wBg = "#1a1b26";
        txt = "#c0caf5";
        toolBg = "#16161e";
        accent = "#7aa2f7";
        hov = "#292e42";
        inp = "#1f2335";
        border = "#414868";
        m_iconBaseColor = QColor(122, 162, 247);
        setSearchbar("QLineEdit { background-color:#1a1b26; color:#9d7cd8; border:1px solid "
                     "#9d7cd8; } QLineEdit:focus { border:1px solid #2A82DA; }");
        break;
    case Theme::Oled:
        wBg = "#000000";
        txt = "#ffffff";
        toolBg = "#000000";
        accent = "#ffffff";
        hov = "#1a1a1a";
        inp = "#000000";
        border = "#333333";
        m_iconBaseColor = Qt::white;
        setSearchbar("QLineEdit { background-color:#000000; color:white; border:1px solid white; } "
                     "QLineEdit:focus { border:1px solid #2A82DA; }");
        break;
    case Theme::Neon:
        wBg = "#0a0a0f";
        txt = "#39ff14";
        toolBg = "#0f0f14";
        accent = "#ff00ff";
        hov = "#1a1a25";
        inp = "#050505";
        border = "#39ff14";
        m_iconBaseColor = QColor(0, 255, 255);
        setSearchbar("QLineEdit { background-color:#0d0d0d; color:#39ff14; border:1px solid "
                     "#39ff14; border-radius: 6px; padding: 6px; font-weight: bold; } "
                     "QLineEdit:focus { border:1px solid #ff00ff; }");
        break;
    case Theme::Shadlix:
        wBg = "#1a1033";
        txt = "#40e0d0";
        toolBg = "#150d28";
        accent = "#9370db";
        hov = "#251745";
        inp = "#1a1033";
        border = "#40e0d0";
        m_iconBaseColor = QColor(64, 224, 208);
        setSearchbar("QLineEdit { background-color:#1a1033; color:#40e0d0; border:1px solid "
                     "#40e0d0; } QLineEdit:focus { border:1px solid #2A82DA; }");
        break;
    case Theme::ShadlixCave:
        wBg = "#1b5a3f";
        txt = "#cfe1d8";
        toolBg = "#154832";
        accent = "#39c591";
        hov = "#22704e";
        inp = "#0d3924";
        border = "#39c591";
        m_iconBaseColor = QColor(57, 202, 144);
        setSearchbar("QLineEdit { background-color:#0D3924; color:#39C591; border:1px solid "
                     "#39C591; } QLineEdit:focus { border:1px solid #2A82DA; }");
        break;
    }

    QString listCss = GenerateListStylesheet(inp, txt, accent, border, toolBg);

    if (listWidget)
        listWidget->setStyleSheet(listCss);
    if (gridWidget)
        gridWidget->setStyleSheet(listCss);

    if (applyGlobalStylesheet) {
        QString globalCss = GenerateGlobalStylesheet(wBg, txt, toolBg, accent, hov, inp, border);
        qApp->setStyleSheet(globalCss);
    }

    themePalette.setColor(QPalette::Window, QColor(wBg));
    themePalette.setColor(QPalette::WindowText, QColor(txt));
    themePalette.setColor(QPalette::Base, QColor(inp));
    themePalette.setColor(QPalette::AlternateBase, QColor(toolBg));
    themePalette.setColor(QPalette::Text, QColor(txt));
    themePalette.setColor(QPalette::Button, QColor(toolBg));
    themePalette.setColor(QPalette::ButtonText, QColor(txt));
    themePalette.setColor(QPalette::Highlight, QColor(accent));
    themePalette.setColor(QPalette::HighlightedText, Qt::white);
    themePalette.setColor(QPalette::Link, QColor(accent));

    qApp->setPalette(themePalette);

    m_iconHoverColor = m_iconBaseColor.lighter(150);
    if (theme == Theme::Light)
        m_iconHoverColor = QColor(80, 80, 80);
    m_textColor = m_iconBaseColor;
}

void WindowThemes::ApplyThemeToDialog(QDialog* dialog) {
    if (dialog)
        dialog->setPalette(qApp->palette());
}

void WindowThemes::ApplyThemeToWidget(QWidget* widget) {
    if (widget)
        widget->setPalette(qApp->palette());
}