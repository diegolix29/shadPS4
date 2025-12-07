// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QRandomGenerator>
#include <QTableWidget>
#include <common/path_util.h>
#include "main_window_themes.h"

QString GenerateListStylesheet(const QString& windowBg, const QString& textColor,
                               const QString& selectionColor, const QString& gridColor,
                               const QString& headerBg) {
    return QString(R"(
        /* Target the main list widgets and their internal canvases */
        QTableWidget, QListWidget, QListView, 
        QTableWidget > QWidget, QListWidget > QWidget, QListView > QWidget {
            background-color: transparent !important;
            background: transparent !important;
            border: none;
            color: %2;
            gridline-color: %4;
            selection-background-color: %3;
            selection-color: white;
            outline: none;
        }

        /* Prevent list items from blocking the main window background */
        QTableWidget::item, QListWidget::item, QListView::item {
            background-color: transparent !important;
            border: none;
        }

        /* Column Headers transparency */
        QHeaderView::section {
            background-color: %5;
            color: %2;
            padding: 5px;
            border: none;
            border-bottom: 2px solid %3;
        }

        /* Custom Scrollbar colors */
        QScrollBar:vertical { 
            background: %1; 
            width: 12px; 
        }

        QScrollBar::handle:vertical { 
            background: %4; 
            border-radius: 6px; 
            margin: 2px; 
        }
    )")
        .arg(windowBg, textColor, selectionColor, gridColor, headerBg);
}

QString GenerateGlobalStylesheet(const QString& windowBg, const QString& textColor,
                                 const QString& toolbarBg, const QString& accentColor,
                                 const QString& hoverBg, const QString& inputBg,
                                 const QString& borderColor) {
    return QString(R"(
        /* Force container backgrounds to reveal underlying gradient/image */
        QMainWindow, QDialog, QStackedWidget, QScrollArea, QFrame { 
            background-color: %1; 
            color: %2; 
        }
        
        /* Stop child widgets from overriding the background back to opaque grey */
        QWidget { 
            color: %2; 
            background-color: transparent; 
        }
        
        /* Navigation & Menus styling */
        QToolBar { background-color: %3; border-bottom: 2px solid %7; spacing: 12px; padding: 8px; }
        QMenuBar { background-color: %3; color: %2; border-bottom: 1px solid %7; }
        QMenuBar::item:selected { background-color: %4; color: white; }
        QMenu { background-color: %6; color: %2; border: 1px solid %7; }
        QMenu::item:selected { background-color: %4; color: white; }

        /* Group Panels with neon left-accent borders */
        QGroupBox {
            background-color: %3;
            border: 1px solid %7;
            border-left: 3px solid %7;
            margin-top: 20px;
            padding-top: 10px;
        }
        QGroupBox::title { subcontrol-origin: margin; left: 10px; color: %7; font-weight: bold; }

        /* Buttons and Form controls */
        QPushButton { background-color: transparent; border-radius: 8px; color: %2; padding: 6px; font-weight: bold; border: 1px solid transparent; }
        QPushButton:hover { background-color: %5; border: 1px solid %4; }
        QLineEdit, QComboBox { background-color: %6; color: %2; border: 1px solid %7; border-radius: 8px; padding: 4px 10px; }
        
        /* Tab panels with selected-item underlines */
        QTabWidget::pane { border: 1px solid %7; background: %1; top: -1px; }
        QTabBar::tab { background: %6; color: %2; padding: 8px 12px; border: 1px solid %7; border-bottom: none; }
        QTabBar::tab:selected { background: %5; border-bottom: 2px solid %4; color: %4; }

        /* Sliders / Volume tracks */
        QSlider::groove:horizontal { border: 1px solid %7; height: 4px; background: %6; }
        QSlider::handle:horizontal { background: %4; width: 14px; height: 14px; margin: -5px 0; border-radius: 7px; }
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

    case Theme::QSS:
        wBg = "#0a0a0f";
        txt = "#d9d9d9";
        toolBg = "#3a3a3a";
        accent = "#83a598";
        hov = "#4d4d4d";
        inp = "#00d9d9d9";
        border = "#39ff14";
        int r = QRandomGenerator::global()->bounded(256);
        int g = QRandomGenerator::global()->bounded(256);
        int b = QRandomGenerator::global()->bounded(256);

        m_iconBaseColor = QColor(r, g, b);
        QString iconBaseHex = m_iconBaseColor.name();

        setSearchbar("");
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

    WindowThemes::m_iconHoverColor = m_iconBaseColor.lighter(150);
    if (theme == Theme::Light)
        m_iconHoverColor = QColor(80, 80, 80);
    m_textColor = m_iconBaseColor;
    if (theme == Theme::QSS)
        QString iconBaseHex = m_iconBaseColor.name();
    int r2 = QRandomGenerator::global()->bounded(256);
    int g2 = QRandomGenerator::global()->bounded(256);
    int b2 = QRandomGenerator::global()->bounded(256);
    m_iconHoverColor = QColor(r2, g2, b2);
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
