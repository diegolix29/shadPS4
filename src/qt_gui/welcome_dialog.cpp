// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QCheckBox>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
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

WelcomeDialog::WelcomeDialog(QWidget* parent) : QDialog(parent) {
    SetupUI();
    setWindowTitle("Welcome to ShadPS4-BBFork!");
    setWindowIcon(QIcon(":images/shadps4.ico"));
    setFixedSize(600, 400);
}

void WelcomeDialog::SetupUI() {
    auto* mainLayout = new QVBoxLayout(this);

    mainLayout->setAlignment(Qt::AlignCenter);

    auto* logo = new QLabel();
    logo->setPixmap(QPixmap(":images/shadps4.png")
                        .scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    logo->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(logo);

    auto* title = new QLabel("<h2>Welcome to ShadPS4-BBFork!</h2>");
    title->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(title);

    auto* desc =
        new QLabel("<b>Included Features & Hacks:</b><br>"
                   "<ul>"
                   "<li>A sound hack that prevents Bloodborne from losing audio. (originally made "
                   "by rainvmaker)</li>"
                   "<li>Automatic backups via a checkbox in the Graphics tab in Settings.</li>"
                   "<li>A PM4 Type 0 hack to avoid related issues. "
                   "<i>(Do not use this with the \"Copy Buffer\" checkbox under the Debug tab in "
                   "Settings.)</i></li>"
                   "<li>An RCAS bar in Settings to adjust FSR sharpness.</li>"
                   "<li>Several Hotkeys.</li>"
                   "<li>Water Flickering Hack(BloodBorne).</li>"
                   "<li>READBACKS OPTIMIZATION (Smooth no extra stutters anymore) Fast and Unsafe "
                   "are for BloodBorne.</li>"
                   "<li>Restart and Stop buttons working as the QTLauncher.</li>"
                   "<li>Keyboard and mouse custom button mapping for FromSoftware games.</li>"
                   "<li>An Experimental tab with all new features and both isDevKit and Neo Mode "
                   "(PS4 Pro Mode) checkboxes in Settings.</li>"
                   "<li>Safe Tiling and USB PRs developed for main Shad.</li>"
                   "</ul>"
                   "<br>"
                   "Please select your installation type:<br>"
                   "<b>Portable</b> — creates a <code>user</code> folder next to the executable "
                   "(recommended).<br>"
                   "<b>Global</b> — stores data in AppData.");

    desc->setAlignment(Qt::AlignCenter);
    desc->setWordWrap(true);
    mainLayout->addWidget(desc);

    mainLayout->addSpacing(10);

    auto* installLabel = new QLabel("Select your preferred installation type:");
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
    connect(m_skipCheck, &QCheckBox::stateChanged, this, [this](int state) {
        Q_UNUSED(state);
        m_skipNextLaunch = m_skipCheck->isChecked(); // <-- store the state internally too
        const QString iniPath = QCoreApplication::applicationDirPath() + "/startup.ini";
        QSettings settings(iniPath, QSettings::IniFormat);
        settings.setValue("skip_welcome", m_skipNextLaunch);
        settings.sync();
        qDebug() << "[WelcomeDialog] skip_welcome updated to:" << m_skipNextLaunch;
    });

    QPushButton* updateButton = new QPushButton(tr("Update"), this);
    updateButton->setEnabled(true); // initially grayed out
    updateButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    mainLayout->addWidget(updateButton, 0, Qt::AlignLeft);
    connect(updateButton, &QPushButton::clicked, this, [this]() {
        QSettings startupIni(QCoreApplication::applicationDirPath() + "/startup.ini",
                             QSettings::IniFormat);
        startupIni.setValue("skip_welcome", true);
        startupIni.sync();
        accept(); // Close and apply
    });
    auto* footer = new QHBoxLayout();
    footer->addStretch();
    auto* discord = new QLabel("<a href=\"https://discord.gg/jgpqB7gUxG\">"
                               "<img src=\":images/discord.png\" width=24 height=24>"
                               "</a>");
    discord->setOpenExternalLinks(true);
    footer->addWidget(discord);
    mainLayout->addLayout(footer);

    // --- Portable button ---
    connect(portableBtn, &QPushButton::clicked, this, [this]() {
        m_portableChosen = true;

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
        // If no global detected, do nothing (portable folder auto-created)
        QMessageBox::information(this, tr("Portable Folder Set"),
                                 tr("Portable Folder Successfully Set"));

        Common::FS::ResetUserPaths(true);

        accept();
    });

    // --- Global button ---
    connect(globalBtn, &QPushButton::clicked, this, [this]() {
        m_portableChosen = false;

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
            // Global exists → erase newly created portable folder
            if (fs::exists(portable_dir))
                fs::remove_all(portable_dir);
        } else {
            // Global doesn't exist → create it
            fs::create_directories(global_dir);

            // If portable exists, move it into global
            if (fs::exists(portable_dir)) {
                fs::copy(portable_dir, global_dir, fs::copy_options::recursive);
                fs::remove_all(portable_dir);
            }
        }

        QMessageBox::information(this, tr("Global Folder Set"),
                                 tr("Global Folder Successfully Set"));

        Common::FS::SetUserPath(Common::FS::PathType::UserDir, global_dir);
        Common::FS::ResetUserPaths(false);

        accept();
    });
}
