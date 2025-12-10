// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QCheckBox>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QUrl>
#include <QVBoxLayout>
#include "common/path_util.h"
#include "welcome_dialog.h"

#include <filesystem>
namespace fs = std::filesystem;

#include <vector>
#include <QCompleter>
#include <QDirIterator>
#include <QFileDialog>
#include <QHoverEvent>
#include <QMessageBox>
#include <SDL3/SDL.h>
#ifdef _WIN32
#include <Shlobj.h>
#include <windows.h>
#endif
#include <fmt/format.h>
#include "main_window_themes.h"
extern WindowThemes m_window_themes;

WelcomeDialog::WelcomeDialog(WindowThemes* themes, QWidget* parent)
    : QDialog(parent), m_themes(themes) {
    Theme th = static_cast<Theme>(Config::getMainWindowTheme());

    m_window_themes.SetWindowTheme(th, nullptr);
    m_window_themes.ApplyThemeToDialog(this);
    SetupUI();
    ApplyTheme();
    setWindowTitle("Welcome to BBFork Build - shadPs4");
    setWindowIcon(QIcon(":images/shadps4.ico"));
    resize(700, 600);
    setMinimumSize(600, 550);
}

void WelcomeDialog::ApplyTheme() {
    if (!m_themes)
        return;

    QString textColor = m_themes->textColor().name();
    QString baseColor = m_themes->iconBaseColor().name();
    QString hoverColor = m_themes->iconHoverColor().name();
    for (auto* label : findChildren<QLabel*>()) {
        label->setStyleSheet(QString("color: %1;").arg(textColor));
    }
    QString buttonStyle = QString(R"(
        QPushButton {
            background-color: %1;
            color: %2;
            border-radius: 5px;
            padding: 6px 12px;
        }
        QPushButton:hover {
            background-color: %3;
        }
    )")
                              .arg(baseColor, textColor, hoverColor);

    for (auto* button : findChildren<QPushButton*>()) {
        button->setStyleSheet(buttonStyle);
    }
    for (auto* check : findChildren<QCheckBox*>()) {
        check->setStyleSheet(QString("color: %1;").arg(textColor));
    }
    setAutoFillBackground(true);
}

void WelcomeDialog::SetupUI() {
    auto* mainLayout = new QVBoxLayout(this);

    auto* logo = new QLabel();
    logo->setPixmap(QPixmap(":images/shadps4.png")
                        .scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    logo->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(logo);

    auto* title = new QLabel("<h2>BBFork Build by Diegolix - Welcome</h2>"
                             "<b>Included Features & Hacks:</b><br>");
    title->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(title);

    auto* descLabel =
        new QLabel("<ul>"
                   "<li>A sound hack that prevents Bloodborne from losing audio. (originally made "
                   "by rainvmaker)</li>"
                   "<li>Automatic backups via a checkbox in the Graphics tab in Settings.</li>"
                   "<li>NEW Games Menu button to trigger Big Picture Mode.</li>"
                   "<li>NEW Cinematic Frame View for games like a Netflix Viewer.</li>"
                   "<li>NEW PKG button to install Games if you have them Packed.</li>"
                   "<li>A PM4 Type 0 hack to avoid related issues. "
                   "<i>(Do not use this with the \"Copy Buffer\" checkbox under the Debug tab in "
                   "Settings.)</i></li>"
                   "<li>Several NEW Hotkeys like Mute sound - and Trophy viewer while ingame.</li>"
                   "<li>Water Flickering Hack(Bloodborne).</li>"
                   "<li>READBACKS OPTIMIZATION (Smooth no extra stutters anymore) Fast and Unsafe "
                   "are for Bloodborne.</li>"
                   "<li>Restart and Stop buttons working as the QTLauncher.</li>"
                   "<li>Keyboard and mouse custom button mapping for FromSoftware games.</li>"
                   "<li>An Experimental tab with all new features and both isDevKit and Neo Mode "
                   "(PS4 Pro Mode) checkboxes in Settings.</li>"
                   "<li>Safe Tiling and USB PRs developed for main Shad.</li>"
                   "</ul>"
                   "<br>");
    descLabel->setWordWrap(true);

    auto* scroll = new QScrollArea();
    scroll->setWidget(descLabel);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMinimumHeight(200);
    scroll->setStyleSheet("background: transparent;");
    scroll->viewport()->setStyleSheet("background: transparent;");

    mainLayout->addWidget(scroll);

    mainLayout->addSpacing(10);

    auto* installLabel =
        new QLabel("Please select your installation type:<br>"
                   "<b>Portable</b> — creates a <code>user</code> folder next to the executable "
                   "(recommended).<br>"
                   "<b>Global</b> — stores data in AppData.<br>"
                   "Select your preferred installation type:");
    installLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(installLabel);

    auto* buttonLayout = new QHBoxLayout();
    auto* portableBtn = new QPushButton("Portable");
    auto* globalBtn = new QPushButton("Global");
    portableBtn->setMinimumWidth(120);
    globalBtn->setMinimumWidth(120);
    buttonLayout->addStretch();
    buttonLayout->addWidget(portableBtn);
    buttonLayout->addWidget(globalBtn);
    buttonLayout->addStretch();
    mainLayout->addLayout(buttonLayout);

    m_skipCheck = new QCheckBox("Don't show this screen again");
    m_skipCheck->setChecked(false);
    mainLayout->addWidget(m_skipCheck, 0, Qt::AlignLeft);

#if (QT_VERSION < QT_VERSION_CHECK(6, 7, 0))
    connect(m_skipCheck, &QCheckBox::stateChanged, this,
            [this](int) { m_skipNextLaunch = m_skipCheck->isChecked(); });
#else
    connect(m_skipCheck, &QCheckBox::checkStateChanged, this,
            [this](int) { m_skipNextLaunch = m_skipCheck->isChecked(); });
#endif

    QPushButton* updateButton = new QPushButton(tr("Close"), this);
    updateButton->setEnabled(true);
    updateButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    mainLayout->addWidget(updateButton, 0, Qt::AlignLeft);
    connect(updateButton, &QPushButton::clicked, this, [this]() {
        Config::setShowWelcomeDialog(!m_skipNextLaunch); // save the choice
        Config::save(Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "config.toml");
        accept();
    });
    auto* footer = new QHBoxLayout();
    footer->addStretch();
    auto* discord = new QLabel("<a href=\"https://discord.gg/jgpqB7gUxG\">"
                               "<img src=\":images/discord.png\" width=24 height=24>"
                               "</a>");
    discord->setOpenExternalLinks(true);
    footer->addWidget(discord);
    mainLayout->addLayout(footer);

    connect(portableBtn, &QPushButton::clicked, this, [this]() {
        m_portableChosen = true;
        m_userMadeChoice = true;

        auto portable_dir = fs::current_path() / "user";
        fs::path global_dir;
#if _WIN32
        TCHAR appdata[MAX_PATH] = {0};
        SHGetFolderPath(NULL, CSIDL_APPDATA, NULL, 0, appdata);
        global_dir = fs::path(appdata) / "shadPS4";
#elif __APPLE__
    global_dir = fs::path(getenv("HOME")) / "Library" / "Application Support" / "shadPS4";
#else
    const char* xdg_data_home = getenv("XDG_DATA_HOME");
    if (xdg_data_home && *xdg_data_home)
        global_dir = fs::path(xdg_data_home) / "shadPS4";
    else
        global_dir = fs::path(getenv("HOME")) / ".local" / "share" / "shadPS4";
#endif

        if (fs::exists(global_dir)) {
            QMessageBox::StandardButton reply = QMessageBox::question(
                this, tr("Global folder detected"),
                tr("Global folder already exists.\n\nMove its content to portable and erase "
                   "global?\n"
                   "Click No to just create a new user folder and leave global intact."),
                QMessageBox::Yes | QMessageBox::No);
            if (reply == QMessageBox::Yes) {
                if (fs::exists(portable_dir))
                    fs::remove_all(portable_dir);
                fs::copy(global_dir, portable_dir, fs::copy_options::recursive);
                fs::remove_all(global_dir);
            } else {
                if (!fs::exists(portable_dir))
                    fs::create_directories(portable_dir);
            }
        }
        QMessageBox::information(this, tr("Portable Folder Set"),
                                 tr("Portable Folder Successfully Set"));

        Config::setShowWelcomeDialog(false); // don’t show next launch
        Config::save(Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "config.toml");
        accept();
    });

    connect(globalBtn, &QPushButton::clicked, this, [this]() {
        m_portableChosen = false;
        m_userMadeChoice = true;

        auto portable_dir = fs::current_path() / "user";
        fs::path global_dir;
#if _WIN32
        TCHAR appdata[MAX_PATH] = {0};
        SHGetFolderPath(NULL, CSIDL_APPDATA, NULL, 0, appdata);
        global_dir = fs::path(appdata) / "shadPS4";
#elif __APPLE__
    global_dir = fs::path(getenv("HOME")) / "Library" / "Application Support" / "shadPS4";
#else
    const char* xdg_data_home = getenv("XDG_DATA_HOME");
    if (xdg_data_home && *xdg_data_home)
        global_dir = fs::path(xdg_data_home) / "shadPS4";
    else
        global_dir = fs::path(getenv("HOME")) / ".local" / "share" / "shadPS4";
#endif

        if (fs::exists(global_dir)) {
            if (fs::exists(portable_dir))
                fs::remove_all(portable_dir);
        } else {
            fs::create_directories(global_dir);

            if (fs::exists(portable_dir)) {
                fs::copy(portable_dir, global_dir, fs::copy_options::recursive);
                fs::remove_all(portable_dir);
            }
        }

        QMessageBox::information(this, tr("Global Folder Set"),
                                 tr("Global Folder Successfully Set"));

        Config::setShowWelcomeDialog(false); // don’t show next launch
        Config::save(Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "config.toml");
        accept();
    });
}
