// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QLabel>
#include <QProgressDialog>
#include <QPushButton>
#include "main_window_themes.h"

void WindowThemes::SetWindowTheme(Theme theme, QLineEdit* mw_searchbar) {
    QPalette themePalette;
    qApp->setStyleSheet("");
    auto setSearchbar = [&](QString css) {
        if (mw_searchbar)
            mw_searchbar->setStyleSheet(css);
    };
    switch (theme) {
    case Theme::Dark:
        setSearchbar("QLineEdit { background-color:#1e1e1e; color:white; border:1px solid white; }"
                     "QLineEdit:focus { border:1px solid #2A82DA; }");
        themePalette.setColor(QPalette::Window, QColor(50, 50, 50));
        themePalette.setColor(QPalette::WindowText, Qt::white);
        themePalette.setColor(QPalette::Base, QColor(20, 20, 20));
        themePalette.setColor(QPalette::AlternateBase, QColor(53, 53, 53));
        themePalette.setColor(QPalette::ToolTipBase, QColor(20, 20, 20));
        themePalette.setColor(QPalette::ToolTipText, Qt::white);
        themePalette.setColor(QPalette::Text, Qt::white);
        themePalette.setColor(QPalette::Button, QColor(53, 53, 53));
        themePalette.setColor(QPalette::ButtonText, Qt::white);
        themePalette.setColor(QPalette::BrightText, Qt::red);
        themePalette.setColor(QPalette::Link, QColor(42, 130, 218));
        themePalette.setColor(QPalette::Highlight, QColor(42, 130, 218));
        themePalette.setColor(QPalette::HighlightedText, Qt::black);

        qApp->setPalette(themePalette);

        m_iconBaseColor = QColor(200, 200, 200);
        m_iconHoverColor = m_iconBaseColor.lighter(150);
        m_textColor = m_iconBaseColor;

        break;

    case Theme::Light:
        setSearchbar("QLineEdit { background-color:#ffffff; color:black; border:1px solid black; }"
                     "QLineEdit:focus { border:1px solid #2A82DA; }");
        themePalette.setColor(QPalette::Window, QColor(240, 240, 240));          // Light gray
        themePalette.setColor(QPalette::WindowText, Qt::black);                  // Black
        themePalette.setColor(QPalette::Base, QColor(230, 230, 230, 80));        // Grayish
        themePalette.setColor(QPalette::ToolTipBase, QColor(230, 230, 230, 80)); // Grayish
        themePalette.setColor(QPalette::ToolTipText, Qt::black);                 // Black
        themePalette.setColor(QPalette::Text, Qt::black);                        // Black
        themePalette.setColor(QPalette::Button, QColor(240, 240, 240));          // Light gray
        themePalette.setColor(QPalette::ButtonText, Qt::black);                  // Black
        themePalette.setColor(QPalette::BrightText, Qt::red);                    // Red
        themePalette.setColor(QPalette::Link, QColor(42, 130, 218));             // Blue
        themePalette.setColor(QPalette::Highlight, QColor(42, 130, 218));        // Blue
        themePalette.setColor(QPalette::HighlightedText, Qt::white);             // White

        qApp->setPalette(themePalette);

        m_iconBaseColor = Qt::black;
        m_iconHoverColor = QColor(80, 80, 80);
        m_textColor = m_iconBaseColor;

        break;

    case Theme::Green:
        setSearchbar("QLineEdit { background-color:#192819; color:white; border:1px solid white; }"
                     "QLineEdit:focus { border:1px solid #2A82DA; }");
        themePalette.setColor(QPalette::Window, QColor(53, 69, 53)); // Dark green background
        themePalette.setColor(QPalette::WindowText, Qt::white);      // White text
        themePalette.setColor(QPalette::Base, QColor(25, 40, 25));   // Darker green base
        themePalette.setColor(QPalette::AlternateBase,
                              QColor(53, 69, 53)); // Dark green alternate base
        themePalette.setColor(QPalette::ToolTipBase,
                              QColor(25, 40, 25));                   // White tooltip background
        themePalette.setColor(QPalette::ToolTipText, Qt::white);     // White tooltip text
        themePalette.setColor(QPalette::Text, Qt::white);            // White text
        themePalette.setColor(QPalette::Button, QColor(53, 69, 53)); // Dark green button
        themePalette.setColor(QPalette::ButtonText, Qt::white);      // White button text
        themePalette.setColor(QPalette::BrightText, Qt::red);        // Bright red text for alerts
        themePalette.setColor(QPalette::Link, QColor(42, 130, 218)); // Light blue links
        themePalette.setColor(QPalette::Highlight, QColor(42, 130, 218)); // Light blue highlight
        themePalette.setColor(QPalette::HighlightedText, Qt::black);      // Black highlighted text

        qApp->setPalette(themePalette);

        m_iconBaseColor = QColor(144, 238, 144);
        m_iconHoverColor = m_iconBaseColor.lighter(150);
        m_textColor = m_iconBaseColor;

        break;

    case Theme::Blue:
        setSearchbar("QLineEdit { background-color:#14283c; color:white; border:1px solid white; }"
                     "QLineEdit:focus { border:1px solid #2A82DA; }");
        themePalette.setColor(QPalette::Window, QColor(40, 60, 90)); // Dark blue background
        themePalette.setColor(QPalette::WindowText, Qt::white);      // White text
        themePalette.setColor(QPalette::Base, QColor(20, 40, 60));   // Darker blue base
        themePalette.setColor(QPalette::AlternateBase,
                              QColor(40, 60, 90)); // Dark blue alternate base
        themePalette.setColor(QPalette::ToolTipBase,
                              QColor(20, 40, 60));                   // White tooltip background
        themePalette.setColor(QPalette::ToolTipText, Qt::white);     // White tooltip text
        themePalette.setColor(QPalette::Text, Qt::white);            // White text
        themePalette.setColor(QPalette::Button, QColor(40, 60, 90)); // Dark blue button
        themePalette.setColor(QPalette::ButtonText, Qt::white);      // White button text
        themePalette.setColor(QPalette::BrightText, Qt::red);        // Bright red text for alerts
        themePalette.setColor(QPalette::Link, QColor(42, 130, 218)); // Light blue links
        themePalette.setColor(QPalette::Highlight, QColor(42, 130, 218)); // Light blue highlight
        themePalette.setColor(QPalette::HighlightedText, Qt::black);      // Black highlighted text

        qApp->setPalette(themePalette);

        m_iconBaseColor = QColor(100, 149, 237);
        m_iconHoverColor = m_iconBaseColor.lighter(150);
        m_textColor = m_iconBaseColor;

        break;

    case Theme::Violet:
        setSearchbar("QLineEdit { background-color:#501e5a; color:white; border:1px solid white; }"
                     "QLineEdit:focus { border:1px solid #2A82DA; }");
        themePalette.setColor(QPalette::Window, QColor(100, 50, 120)); // Violet background
        themePalette.setColor(QPalette::WindowText, Qt::white);        // White text
        themePalette.setColor(QPalette::Base, QColor(80, 30, 90));     // Darker violet base
        themePalette.setColor(QPalette::AlternateBase,
                              QColor(100, 50, 120)); // Violet alternate base
        themePalette.setColor(QPalette::ToolTipBase,
                              QColor(80, 30, 90));                     // White tooltip background
        themePalette.setColor(QPalette::ToolTipText, Qt::white);       // White tooltip text
        themePalette.setColor(QPalette::Text, Qt::white);              // White text
        themePalette.setColor(QPalette::Button, QColor(100, 50, 120)); // Violet button
        themePalette.setColor(QPalette::ButtonText, Qt::white);        // White button text
        themePalette.setColor(QPalette::BrightText, Qt::red);          // Bright red text for alerts
        themePalette.setColor(QPalette::Link, QColor(42, 130, 218));   // Light blue links
        themePalette.setColor(QPalette::Highlight, QColor(42, 130, 218)); // Light blue highlight
        themePalette.setColor(QPalette::HighlightedText, Qt::black);      // Black highlighted text

        qApp->setPalette(themePalette);

        m_iconBaseColor = QColor(186, 85, 211);
        m_iconHoverColor = m_iconBaseColor.lighter(150);
        m_textColor = m_iconBaseColor;

        break;

    case Theme::Gruvbox:
        setSearchbar(
            "QLineEdit { background-color:#1d2021; color:#f9f5d7; border:1px solid #f9f5d7; }"
            "QLineEdit:focus { border:1px solid #2A82DA; }");
        themePalette.setColor(QPalette::Window, QColor(29, 32, 33));
        themePalette.setColor(QPalette::WindowText, QColor(249, 245, 215));
        themePalette.setColor(QPalette::Base, QColor(29, 32, 33));
        themePalette.setColor(QPalette::AlternateBase, QColor(50, 48, 47));
        themePalette.setColor(QPalette::ToolTipBase, QColor(29, 32, 33));
        themePalette.setColor(QPalette::ToolTipText, QColor(249, 245, 215));
        themePalette.setColor(QPalette::Text, QColor(249, 245, 215));
        themePalette.setColor(QPalette::Button, QColor(40, 40, 40));
        themePalette.setColor(QPalette::ButtonText, QColor(249, 245, 215));
        themePalette.setColor(QPalette::BrightText, QColor(251, 73, 52));
        themePalette.setColor(QPalette::Link, QColor(131, 165, 152));
        themePalette.setColor(QPalette::Highlight, QColor(131, 165, 152));
        themePalette.setColor(QPalette::HighlightedText, Qt::black);

        qApp->setPalette(themePalette);

        m_iconBaseColor = QColor(250, 189, 47);
        m_iconHoverColor = m_iconBaseColor.lighter(150);
        m_textColor = m_iconBaseColor;

        break;

    case Theme::TokyoNight:
        setSearchbar(
            "QLineEdit { background-color:#1a1b26; color:#9d7cd8; border:1px solid #9d7cd8; }"
            "QLineEdit:focus { border:1px solid #2A82DA; }");
        themePalette.setColor(QPalette::Window, QColor(31, 35, 53));
        themePalette.setColor(QPalette::WindowText, QColor(192, 202, 245));
        themePalette.setColor(QPalette::Base, QColor(25, 28, 39));
        themePalette.setColor(QPalette::AlternateBase, QColor(36, 40, 59));
        themePalette.setColor(QPalette::ToolTipBase, QColor(25, 28, 39));
        themePalette.setColor(QPalette::ToolTipText, QColor(192, 202, 245));
        themePalette.setColor(QPalette::Text, QColor(192, 202, 245));
        themePalette.setColor(QPalette::Button, QColor(30, 30, 41));
        themePalette.setColor(QPalette::ButtonText, QColor(192, 202, 245));
        themePalette.setColor(QPalette::BrightText, QColor(197, 59, 83));
        themePalette.setColor(QPalette::Link, QColor(79, 214, 190));
        themePalette.setColor(QPalette::Highlight, QColor(79, 214, 190));
        themePalette.setColor(QPalette::HighlightedText, Qt::black);

        qApp->setPalette(themePalette);

        m_iconBaseColor = QColor(122, 162, 247);
        m_iconHoverColor = m_iconBaseColor.lighter(150);
        m_textColor = m_iconBaseColor;

        break;

    case Theme::Oled:
        setSearchbar("QLineEdit { background-color:#000000; color:white; border:1px solid white; }"
                     "QLineEdit:focus { border:1px solid #2A82DA; }");
        themePalette.setColor(QPalette::Window, Qt::black);
        themePalette.setColor(QPalette::WindowText, Qt::white);
        themePalette.setColor(QPalette::Base, Qt::black);
        themePalette.setColor(QPalette::AlternateBase, Qt::black);
        themePalette.setColor(QPalette::ToolTipBase, Qt::black);
        themePalette.setColor(QPalette::ToolTipText, Qt::white);
        themePalette.setColor(QPalette::Text, Qt::white);
        themePalette.setColor(QPalette::Button, Qt::black);
        themePalette.setColor(QPalette::ButtonText, Qt::white);
        themePalette.setColor(QPalette::BrightText, Qt::red);
        themePalette.setColor(QPalette::Link, QColor(42, 130, 218));
        themePalette.setColor(QPalette::Highlight, QColor(42, 130, 218));
        themePalette.setColor(QPalette::HighlightedText, Qt::black);

        qApp->setPalette(themePalette);

        m_iconBaseColor = Qt::white;
        m_iconHoverColor = m_iconBaseColor.darker(150);
        m_textColor = m_iconBaseColor;

        break;

    case Theme::Neon:
        setSearchbar("QLineEdit { background-color:#0d0d0d; color:39ff14; border:1px solid "
                     "#39ff14; border-radius: 6px; padding: 6px; font-weight: bold; }"
                     "QLineEdit:focus { border:1px solid #ff00ff; }");
        themePalette.setColor(QPalette::Window, QColor(10, 10, 15));       // Deep dark
        themePalette.setColor(QPalette::WindowText, QColor(255, 20, 147)); // Neon pink

        themePalette.setColor(QPalette::Base, QColor(15, 15, 20));          // Input bg
        themePalette.setColor(QPalette::AlternateBase, QColor(25, 25, 35)); // Row alt bg
        themePalette.setColor(QPalette::Text, QColor(57, 255, 20));         // Neon green text
        themePalette.setColor(QPalette::PlaceholderText,
                              QColor(128, 255, 128, 150)); // Dim neon green

        themePalette.setColor(QPalette::Button, QColor(20, 20, 30));      // Button bg
        themePalette.setColor(QPalette::ButtonText, QColor(0, 255, 255)); // Cyan text

        themePalette.setColor(QPalette::ToolTipBase, QColor(20, 20, 25));
        themePalette.setColor(QPalette::ToolTipText, QColor(255, 221, 51)); // Neon yellow

        themePalette.setColor(QPalette::Highlight, QColor(255, 221, 51));    // Neon yellow
        themePalette.setColor(QPalette::HighlightedText, Qt::black);         // Contrast text
        themePalette.setColor(QPalette::Link, QColor(0, 255, 255));          // Neon cyan links
        themePalette.setColor(QPalette::LinkVisited, QColor(255, 105, 180)); // Hot pink visited

        themePalette.setColor(QPalette::BrightText, QColor(255, 0, 255)); // Magenta alerts

        themePalette.setColor(QPalette::Light, QColor(0, 255, 255, 180));    // Cyan edges
        themePalette.setColor(QPalette::Midlight, QColor(57, 255, 20, 180)); // Greenish glow
        themePalette.setColor(QPalette::Mid, QColor(255, 20, 147, 180));     // Pink balance
        themePalette.setColor(QPalette::Dark, QColor(255, 0, 255, 120));     // Magenta deep
        themePalette.setColor(QPalette::Shadow, QColor(0, 0, 0));            // Keep shadow black

        qApp->setPalette(themePalette);

        m_iconBaseColor = QColor(0, 255, 255);  // Neon cyan
        m_iconHoverColor = QColor(255, 0, 255); // Magenta hover
        m_textColor = QColor(255, 221, 51);     // Neon yellow text for contrast

        break;

    case Theme::Shadlix:
        setSearchbar(
            "QLineEdit { background-color:#1a1033; color:#40e0d0; border:1px solid #40e0d0; }"
            "QLineEdit:focus { border:1px solid #2A82DA; }");
        themePalette.setColor(QPalette::Window, QColor(26, 16, 51));
        themePalette.setColor(QPalette::WindowText, QColor(147, 112, 219));
        themePalette.setColor(QPalette::Base, QColor(40, 20, 70));
        themePalette.setColor(QPalette::AlternateBase, QColor(60, 30, 100));
        themePalette.setColor(QPalette::ToolTipBase, QColor(30, 20, 60));
        themePalette.setColor(QPalette::ToolTipText, QColor(64, 224, 208));
        themePalette.setColor(QPalette::Text, QColor(64, 224, 208));
        themePalette.setColor(QPalette::Button, QColor(45, 25, 80));
        themePalette.setColor(QPalette::ButtonText, QColor(64, 224, 208));
        themePalette.setColor(QPalette::BrightText, QColor(255, 255, 255));
        themePalette.setColor(QPalette::Link, QColor(0, 191, 255));
        themePalette.setColor(QPalette::Highlight, QColor(147, 112, 219));
        themePalette.setColor(QPalette::HighlightedText, Qt::black);

        qApp->setPalette(themePalette);

        m_iconBaseColor = QColor(64, 224, 208);
        m_iconHoverColor = Qt::white;
        m_textColor = m_iconBaseColor;

        break;

    case Theme::ShadlixCave:
        setSearchbar(
            "QLineEdit { background-color:#0D3924; color:#39C591; border:1px solid #39C591; }"
            "QLineEdit:focus { border:1px solid #2A82DA; }");
        themePalette.setColor(QPalette::Window, QColor(27, 90, 63));
        themePalette.setColor(QPalette::WindowText, QColor(207, 225, 216));
        themePalette.setColor(QPalette::Base, QColor(22, 76, 50));
        themePalette.setColor(QPalette::AlternateBase, QColor(40, 112, 85));
        themePalette.setColor(QPalette::ToolTipBase, QColor(34, 99, 77));
        themePalette.setColor(QPalette::ToolTipText, QColor(57, 202, 144));
        themePalette.setColor(QPalette::Text, QColor(57, 202, 144));
        themePalette.setColor(QPalette::Button, QColor(27, 86, 58));
        themePalette.setColor(QPalette::ButtonText, QColor(57, 202, 144));
        themePalette.setColor(QPalette::BrightText, QColor(0, 230, 189));
        themePalette.setColor(QPalette::Link, QColor(0, 221, 198));
        themePalette.setColor(QPalette::Highlight, QColor(0, 221, 198));
        themePalette.setColor(QPalette::HighlightedText, QColor(207, 225, 216));

        qApp->setPalette(themePalette);

        m_iconBaseColor = QColor(57, 202, 144);
        m_iconHoverColor = QColor(0, 221, 198);
        m_textColor = QColor(207, 225, 216);

        break;
    }
}

void WindowThemes::ApplyThemeToDialog(QDialog* dialog) {
    dialog->setPalette(qApp->palette());
    dialog->setStyleSheet(qApp->styleSheet());

    QList<QLabel*> labels = dialog->findChildren<QLabel*>();
    for (auto* lbl : labels)
        lbl->setStyleSheet("color: " + m_textColor.name() + ";");

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

    widget->setPalette(qApp->palette());
    widget->setStyleSheet(qApp->styleSheet());

    // Labels
    QList<QLabel*> labels = widget->findChildren<QLabel*>();
    for (auto* lbl : labels)
        lbl->setStyleSheet("color: " + m_textColor.name() + ";");

    // Buttons
    QList<QPushButton*> buttons = widget->findChildren<QPushButton*>();
    for (auto* btn : buttons) {
        btn->setStyleSheet(QString("color:%1; background-color:%2;")
                               .arg(m_textColor.name())
                               .arg(qApp->palette().button().color().name()));
    }

    // LineEdits
    QList<QLineEdit*> edits = widget->findChildren<QLineEdit*>();
    for (auto* edit : edits) {
        edit->setStyleSheet(
            QString("color:%1; background-color:%2; border-radius:4px; padding:4px;")
                .arg(m_textColor.name())
                .arg(qApp->palette().base().color().name()));
    }
}
