// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QProgressDialog>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSysInfo>
#include <QTableWidget>
#include <common/path_util.h>
#include "common/config.h"
#include "main_window_themes.h"

QString GetOSSpecificWindowStyle(const QString& windowBg) {
#ifdef Q_OS_WIN

    return QString("QMainWindow { background-color: %1; border: none; }").arg(windowBg);
#else
    return QString("QMainWindow { background-color: transparent !important; border: none; }");
#endif
}

static bool IsCyberpunkQssLoaded() {
    const QString style = qApp->styleSheet();
    return style.contains("Cyberpunk", Qt::CaseInsensitive);
}

QString GenerateUnifiedStylesheet(const QString& windowBg, const QString& textColor,
                                  const QString& toolbarBg, const QString& accentColor,
                                  const QString& hoverBg, const QString& inputBg,
                                  const QString& borderColor, const QString& selectionColor,
                                  const QString& gridColor) {

    QString osRootStyle = GetOSSpecificWindowStyle(windowBg);

    return QString(R"(
        /* --- OS SPECIFIC ROOT HANDLING --- */
        %9

        /* --- LISTS & TABLES --- */
        QTableWidget, QListWidget, QListView, 
        QTableWidget > QWidget, QListWidget > QWidget, QListView > QWidget {
            background-color: transparent !important;
            /* FIX: Removed 'background: transparent' so we don't wipe images */
            border: none;
            color: %2;
            gridline-color: %8;
            selection-background-color: %7;
            selection-color: white;
            outline: none;
        }

        QTableWidget::item, QListWidget::item, QListView::item {
            /* FIX: Removed 'background: transparent' */
            background-color: transparent !important;
            border: none;
        }

        QHeaderView::section {
            background-color: %3;
            color: %2;
            padding: 5px;
            border: none;
            border-bottom: 2px solid %7;
        }

        QScrollBar:vertical { background: %1; width: 12px; }
        QScrollBar::handle:vertical { background: %8; border-radius: 6px; margin: 2px; }

        /* --- GLOBAL WIDGETS --- */
        /* Force container backgrounds to transparent to reveal underlying gradient/image */
        QDialog, QStackedWidget, QScrollArea, QFrame { 
            /* FIX: Removed 'background: transparent !important;' */
            background-color: transparent !important;
            color: %2; 
        }
        
        /* Stop child widgets from overriding the background back to opaque grey */
        QWidget { 
            color: %2; 
            background-color: transparent; 
        }
        
        /* Navigation & Menus styling */
        QToolBar { background-color: %3; border-bottom: 2px solid %6; spacing: 12px; padding: 8px; }
        QMenuBar { background-color: %3; color: %2; border-bottom: 1px solid %6; }
        QMenuBar::item:selected { background-color: %4; color: white; }
        QMenu { background-color: %5; color: %2; border: 1px solid %6; }
        QMenu::item:selected { background-color: %4; color: white; }

        /* Group Panels */
        QGroupBox {
            background-color: %3;
            border: 1px solid %6;
            border-left: 3px solid %6;
            margin-top: 20px;
            padding-top: 10px;
        }
        QGroupBox::title { subcontrol-origin: margin; left: 10px; color: %6; font-weight: bold; }

        /* Buttons and Form controls */
        QPushButton { background-color: transparent; border-radius: 8px; color: %2; padding: 6px; font-weight: bold; border: 1px solid transparent; }
        QPushButton:hover { background-color: %5; border: 1px solid %4; }
        QLineEdit, QComboBox { background-color: %5; color: %2; border: 1px solid %6; border-radius: 8px; padding: 4px 10px; }
        
        /* Tab panels */
        QTabWidget::pane { border: 1px solid %6; background: %1; top: -1px; }
        QTabBar::tab { background: %5; color: %2; padding: 8px 12px; border: 1px solid %6; border-bottom: none; }
        QTabBar::tab:selected { background: %5; border-bottom: 2px solid %4; color: %4; }

        /* Sliders */
        QSlider::groove:horizontal { border: 1px solid %6; height: 4px; background: %5; }
        QSlider::handle:horizontal { background: %4; width: 14px; height: 14px; margin: -5px 0; border-radius: 7px; }
    )")
        .arg(windowBg, textColor, toolbarBg, accentColor, inputBg, borderColor, selectionColor,
             gridColor, osRootStyle);
}
void WindowThemes::SetWindowTheme(Theme theme, QLineEdit* mw_searchbar, const QString& qssPath) {
    QString wBg, txt, toolBg, accent, hov, inp, border, sel, grid;

    auto setSearchbar = [&](QString css) {
        if (mw_searchbar)
            mw_searchbar->setStyleSheet(css);
    };

    if (theme == Theme::QSS) {
        wBg = "#0a0a0f";
        txt = "#d9d9d9";
        toolBg = "#3a3a3a";
        accent = "#83a598";
        hov = "#4d4d4d";
        inp = "#1d1d1d";
        border = "#39ff14";
        sel = "#83a598";
        grid = "#39ff14";
        QColor base = QColor("#83a598");

        bool isCyberpunk = false;
        if (!qssPath.isEmpty()) {
            isCyberpunk = qssPath.contains("cyberpunk", Qt::CaseInsensitive);
        } else {
            isCyberpunk = IsCyberpunkQssLoaded();
        }

        if (isCyberpunk) {
            base = QColor(190, 160, 20);
            accent = base.lighter(125).name();
            border = base.darker(130).name();
        }

        if (accent.isEmpty())
            accent = base.lighter(125).name();
        if (border.isEmpty())
            border = base.darker(130).name();
        sel = base.name();
        grid = border;

        m_iconBaseColor = base;

        if (Config::getEnableColorFilter()) {
            QString unifiedCss =
                GenerateUnifiedStylesheet(wBg, txt, toolBg, accent, hov, inp, border, sel, grid);
            qApp->setStyleSheet(unifiedCss);
        }
    }

    qApp->setStyleSheet("");

    switch (theme) {
    case Theme::Dark:
        wBg = "#323232";
        txt = "#ffffff";
        toolBg = "#353535";
        accent = "#2A82DA";
        hov = "#353535";
        inp = "#141414";
        border = "#535353";
        sel = "#2A82DA";
        grid = "#535353";
        m_iconBaseColor = QColor(200, 200, 200);
        setSearchbar("QLineEdit { background-color:#1e1e1e; color:white; border:1px solid white; } "
                     "QLineEdit:focus { border:1px solid #2A82DA; }");
        break;

    case Theme::Light:
        wBg = "#f0f0f0";
        txt = "#000000";
        toolBg = "#e6e6e6";
        accent = "#2A82DA";
        hov = "#505050";
        inp = "#e6e6e6";
        border = "#cccccc";
        sel = "#2A82DA";
        grid = "#cccccc";
        m_iconBaseColor = Qt::black;
        setSearchbar("QLineEdit { background-color:#ffffff; color:black; border:1px solid black; } "
                     "QLineEdit:focus { border:1px solid #2A82DA; }");
        break;

    case Theme::Green:
        wBg = "#354535";
        txt = "#ffffff";
        toolBg = "#354535";
        accent = "#2A82DA";
        hov = "#2A82DA";
        inp = "#192819";
        border = "#354535";
        sel = "#2A82DA";
        grid = "#354535";
        m_iconBaseColor = QColor(144, 238, 144);
        setSearchbar("QLineEdit { background-color:#192819; color:white; border:1px solid white; } "
                     "QLineEdit:focus { border:1px solid #2A82DA; }");
        break;

    case Theme::Blue:
        wBg = "#283c5a";
        txt = "#ffffff";
        toolBg = "#283c5a";
        accent = "#2A82DA";
        hov = "#2A82DA";
        inp = "#14283c";
        border = "#283c5a";
        sel = "#2A82DA";
        grid = "#283c5a";
        m_iconBaseColor = QColor(100, 149, 237);
        setSearchbar("QLineEdit { background-color:#14283c; color:white; border:1px solid white; } "
                     "QLineEdit:focus { border:1px solid #2A82DA; }");
        break;

    case Theme::Violet:
        wBg = "#643278";
        txt = "#ffffff";
        toolBg = "#643278";
        accent = "#2A82DA";
        hov = "#2A82DA";
        inp = "#501e5a";
        border = "#643278";
        sel = "#2A82DA";
        grid = "#643278";
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
        inp = "#1d2021";
        border = "#50482f";
        sel = "#83a598";
        grid = "#50482f";
        m_iconBaseColor = QColor(250, 189, 47);
        setSearchbar("QLineEdit { background-color:#1d2021; color:#f9f5d7; border:1px solid "
                     "#f9f5d7; } QLineEdit:focus { border:1px solid #2A82DA; }");
        break;

    case Theme::TokyoNight:
        wBg = "#1f2335";
        txt = "#c0caf5";
        toolBg = "#1f2335";
        accent = "#7aa2f7";
        hov = "#292e42";
        inp = "#1a1b26";
        border = "#24283b";
        sel = "#7aa2f7";
        grid = "#24283b";
        m_iconBaseColor = QColor(122, 162, 247);
        setSearchbar("QLineEdit { background-color:#1a1b26; color:#9d7cd8; border:1px solid "
                     "#9d7cd8; } QLineEdit:focus { border:1px solid #2A82DA; }");
        break;

    case Theme::Oled:
        wBg = "#000000";
        txt = "#ffffff";
        toolBg = "#000000";
        accent = "#2A82DA";
        hov = "#1a1a1a";
        inp = "#000000";
        border = "#333333";
        sel = "#2A82DA";
        grid = "#333333";
        m_iconBaseColor = Qt::white;
        setSearchbar("QLineEdit { background-color:#000000; color:white; border:1px solid white; } "
                     "QLineEdit:focus { border:1px solid #2A82DA; }");
        break;

    case Theme::Neon:
        wBg = "#0a0a0f";
        txt = "#39ff14";
        toolBg = "#14141e";
        accent = "#ffff33";
        hov = "#ff00ff";
        inp = "#0d0d0d";
        border = "#39ff14";
        sel = "#ffff33";
        grid = "#39ff14";
        m_iconBaseColor = QColor(0, 255, 255);
        setSearchbar("QLineEdit { background-color:#0d0d0d; color:#39ff14; border:1px solid "
                     "#39ff14; border-radius: 6px; padding: 6px; font-weight: bold; } "
                     "QLineEdit:focus { border:1px solid #ff00ff; }");
        break;

    case Theme::Shadlix:
        wBg = "#1a1033";
        txt = "#9370db";
        toolBg = "#2d1950";
        accent = "#9370db";
        hov = "#40e0d0";
        inp = "#1a1033";
        border = "#40e0d0";
        sel = "#9370db";
        grid = "#40e0d0";
        m_iconBaseColor = QColor(64, 224, 208);
        setSearchbar("QLineEdit { background-color:#1a1033; color:#40e0d0; border:1px solid "
                     "#40e0d0; } QLineEdit:focus { border:1px solid #2A82DA; }");
        break;

    case Theme::ShadlixCave:
        wBg = "#1b5a3f";
        txt = "#cfe1d8";
        toolBg = "#1b563a";
        accent = "#00ddc6";
        hov = "#22634d";
        inp = "#0d3924";
        border = "#39c591";
        sel = "#00ddc6";
        grid = "#39c591";
        m_iconBaseColor = QColor(57, 202, 144);
        setSearchbar("QLineEdit { background-color:#0D3924; color:#39C591; border:1px solid "
                     "#39C591; } QLineEdit:focus { border:1px solid #2A82DA; }");
        break;

    case Theme::QSS:
        break;
    }

    QPalette themePalette;
    themePalette.setColor(QPalette::Window, QColor(wBg));
    themePalette.setColor(QPalette::WindowText, QColor(txt));
    themePalette.setColor(QPalette::Base, QColor(inp));
    themePalette.setColor(QPalette::AlternateBase, QColor(toolBg));
    themePalette.setColor(QPalette::Text, QColor(txt));
    themePalette.setColor(QPalette::Button, QColor(toolBg));
    themePalette.setColor(QPalette::ButtonText, QColor(txt));
    themePalette.setColor(QPalette::Highlight, QColor(sel));
    themePalette.setColor(QPalette::HighlightedText, Qt::white);
    themePalette.setColor(QPalette::Link, QColor(accent));
    qApp->setPalette(themePalette);

    m_iconHoverColor = m_iconBaseColor.lighter(150);
    if (theme == Theme::Light) {
        m_iconHoverColor = QColor(80, 80, 80);
    }
    m_textColor = m_iconBaseColor;
}

void WindowThemes::ApplyThemeToDialog(QDialog* dialog) {
    if (!Config::getEnableColorFilter()) {
        return;
    }
    dialog->setPalette(qApp->palette());

    if (!qApp->styleSheet().isEmpty()) {
        dialog->setStyleSheet(qApp->styleSheet());
    }

    QList<QLabel*> labels = dialog->findChildren<QLabel*>();
    for (auto* lbl : labels)
        lbl->setStyleSheet("color: " + m_textColor.name() + "; background: transparent;");

    QList<QPushButton*> buttons = dialog->findChildren<QPushButton*>();
    for (auto* btn : buttons) {
        btn->setStyleSheet(QString("color:%1; background-color:%2;")
                               .arg(m_textColor.name())
                               .arg(qApp->palette().button().color().name()));
    }

    QList<QLineEdit*> edits = dialog->findChildren<QLineEdit*>();
    for (auto* edit : edits) {
        edit->setStyleSheet(
            QString("color:%1; background-color:%2; border-radius:4px; padding:4px;")
                .arg(m_textColor.name())
                .arg(qApp->palette().base().color().name()));
    }
}

void WindowThemes::ApplyThemeToWidget(QWidget* widget) {
    if (!widget)
        return;

    if (!Config::getEnableColorFilter()) {
        return;
    }

    widget->setPalette(qApp->palette());
    if (!qApp->styleSheet().isEmpty()) {
        widget->setStyleSheet(qApp->styleSheet());
    }

    QList<QLabel*> labels = widget->findChildren<QLabel*>();
    for (auto* lbl : labels)
        lbl->setStyleSheet("color: " + m_textColor.name() + "; background: transparent;");

    QList<QPushButton*> buttons = widget->findChildren<QPushButton*>();
    for (auto* btn : buttons) {
        btn->setStyleSheet(QString("color:%1; background-color:%2;")
                               .arg(m_textColor.name())
                               .arg(qApp->palette().button().color().name()));
    }

    QList<QLineEdit*> edits = widget->findChildren<QLineEdit*>();
    for (auto* edit : edits) {
        edit->setStyleSheet(
            QString("color:%1; background-color:%2; border-radius:4px; padding:4px;")
                .arg(m_textColor.name())
                .arg(qApp->palette().base().color().name()));
    }
}