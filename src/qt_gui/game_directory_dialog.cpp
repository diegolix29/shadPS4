// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "game_directory_dialog.h"

GameDirectoryDialog::GameDirectoryDialog() : m_gamesDirectory(nullptr) {
    auto layout = new QVBoxLayout(this);

    layout->addWidget(SetupGamesDirectory());
    layout->addWidget(SetupAddonsDirectory());
    layout->addWidget(SetupSysModulesDirectory());
    layout->addStretch();
    layout->addWidget(SetupDialogActions());

    setWindowTitle(tr("shadPS4 - Choose directory"));
    setWindowIcon(QIcon(":images/shadps4.ico"));
}

GameDirectoryDialog::~GameDirectoryDialog() {}

void GameDirectoryDialog::BrowseGamesDirectory() {
    auto path = QFileDialog::getExistingDirectory(this, tr("Games Directory"));

    if (!path.isEmpty()) {
        m_gamesDirectory->setText(QDir::toNativeSeparators(path));
    }
}

void GameDirectoryDialog::BrowseAddonsDirectory() {
    auto path = QFileDialog::getExistingDirectory(this, tr("DLC Directory"));

    if (!path.isEmpty()) {
        m_addonsDirectory->setText(QDir::toNativeSeparators(path));
    }
}

QWidget* GameDirectoryDialog::SetupGamesDirectory() {
    auto group = new QGroupBox(tr("Games Directory"));
    auto layout = new QHBoxLayout(group);
    m_gamesDirectory = new QLineEdit();
    QString directory;
    std::filesystem::path directory_path =
        Config::getGameDirectories().empty() ? "" : Config::getGameDirectories().front();
    Common::FS::PathToQString(directory, directory_path);
    m_gamesDirectory->setText(directory);
    m_gamesDirectory->setMinimumWidth(400);

    layout->addWidget(m_gamesDirectory);

    auto browse = new QPushButton(tr("Browse"));

    connect(browse, &QPushButton::clicked, this, &GameDirectoryDialog::BrowseGamesDirectory);

    layout->addWidget(browse);

    return group;
}

QWidget* GameDirectoryDialog::SetupSysModulesDirectory() {
    auto group = new QGroupBox(tr("Directory for system modules"));
    auto layout = new QHBoxLayout(group);

    // Input field.
    m_sysModulesDirectory = new QLineEdit();
    QString sysModulesDir;
    Common::FS::PathToQString(sysModulesDir, Config::getSysModulesPath());
    m_sysModulesDirectory->setText(sysModulesDir);
    m_sysModulesDirectory->setMinimumWidth(400);

    layout->addWidget(m_sysModulesDirectory);

    // Browse button.
    auto browse = new QPushButton(tr("Browse"));
    connect(browse, &QPushButton::clicked, [this]() {
        auto path = QFileDialog::getExistingDirectory(this, tr("Directory for system modules"));
        if (!path.isEmpty()) {
            m_sysModulesDirectory->setText(QDir::toNativeSeparators(path));
        }
    });
    layout->addWidget(browse);

    return group;
}

QWidget* GameDirectoryDialog::SetupAddonsDirectory() {
    auto group = new QGroupBox(tr("DLC Directory"));
    auto layout = new QHBoxLayout(group);

    // Input.
    m_addonsDirectory = new QLineEdit();
    QString install_dir;
    Common::FS::PathToQString(install_dir, Config::getAddonDirectory());
    m_addonsDirectory->setText(install_dir);
    m_addonsDirectory->setMinimumWidth(400);

    layout->addWidget(m_addonsDirectory);

    // Browse button.
    auto browse = new QPushButton(tr("Browse"));

    connect(browse, &QPushButton::clicked, this, &GameDirectoryDialog::BrowseAddonsDirectory);

    layout->addWidget(browse);

    return group;
}

QWidget* GameDirectoryDialog::SetupDialogActions() {
    auto actions = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);

    connect(actions, &QDialogButtonBox::accepted, this, &GameDirectoryDialog::Save);
    connect(actions, &QDialogButtonBox::rejected, this, &GameDirectoryDialog::reject);

    return actions;
}

void GameDirectoryDialog::Save() {
    // Check games directory.
    auto gamesDirectory = m_gamesDirectory->text();
    auto addonsDirectory = m_addonsDirectory->text();
    auto sysModulesDirectory = m_sysModulesDirectory->text();

    if (gamesDirectory.isEmpty() || !QDir(gamesDirectory).exists() ||
        !QDir::isAbsolutePath(gamesDirectory)) {
        QMessageBox::critical(this, tr("Error"), "The value for location for games is not valid.");
        return;
    }

    if (sysModulesDirectory.isEmpty() || !QDir::isAbsolutePath(sysModulesDirectory)) {
        QMessageBox::critical(this, tr("Error"), "If not set default place will be used");
        return;
    }

    if (addonsDirectory.isEmpty() || !QDir::isAbsolutePath(addonsDirectory)) {
        QMessageBox::critical(this, tr("Error"), "The value for location for DLC is not valid.");
        return;
    }
    QDir addonsDir(addonsDirectory);
    if (!addonsDir.exists()) {
        if (!addonsDir.mkpath(".")) {
            QMessageBox::critical(this, tr("Error"), "The DLC location could not be created.");
            return;
        }
    }
    Config::addGameDirectories(Common::FS::PathFromQString(gamesDirectory));
    Config::setAddonDirectories(Common::FS::PathFromQString(addonsDirectory));
    Config::setSysModulesPath(Common::FS::PathFromQString(sysModulesDirectory));
    const auto config_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
    Config::save(config_dir / "config.toml");
    accept();
}
