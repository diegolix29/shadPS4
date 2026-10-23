// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QProgressDialog>
#include <QThreadPool>

#include "common/path_util.h"
#include "compatibility_info.h"
#include "game_info.h"

// Maximum depth to search for games in subdirectories
const int max_recursion_depth = 5;

void ScanDirectoryRecursively(const QString& dir, QStringList& filePaths, int current_depth = 0) {
    // Stop recursion if we've reached the maximum depth
    if (current_depth >= max_recursion_depth) {
        return;
    }

    QDir directory(dir);
    QFileInfoList entries = directory.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);

    for (const auto& entry : entries) {
        QString folderName = entry.fileName();

        // Skip update/patch/mods folders
        if (folderName.endsWith("-UPDATE", Qt::CaseInsensitive) ||
            folderName.endsWith("-patch", Qt::CaseInsensitive) ||
            folderName.endsWith("-MODS", Qt::CaseInsensitive)) {
            continue;
        }

        // Check if this directory contains a PS4 game (has sce_sys/param.sfo)
        if (QFile::exists(entry.filePath() + "/sce_sys/param.sfo")) {
            filePaths.append(entry.absoluteFilePath());
        } else {
            // If not a game directory, recursively scan it with increased depth
            ScanDirectoryRecursively(entry.absoluteFilePath(), filePaths, current_depth + 1);
        }
    }
}

void ScanZarFiles(const QString& dir, QStringList& zarFilePaths, int current_depth = 0) {
    // Same depth limit as the folder scan; never walk into a game's own files.
    if (current_depth >= max_recursion_depth || QFile::exists(dir + "/sce_sys/param.sfo")) {
        return;
    }
    QDir directory(dir);
    QFileInfoList entries =
        directory.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);

    for (const auto& entry : entries) {
        if (entry.isFile() && entry.fileName().endsWith(".zar", Qt::CaseInsensitive)) {
            QString fileName = entry.fileName();
            // Skip update/patch/mods zar files (case-insensitive)
            if (fileName.endsWith("-update.zar", Qt::CaseInsensitive) ||
                fileName.endsWith("-UPDATE.zar", Qt::CaseInsensitive) ||
                fileName.endsWith("-Update.zar", Qt::CaseInsensitive) ||
                fileName.endsWith("-patch.zar", Qt::CaseInsensitive) ||
                fileName.endsWith("-PATCH.zar", Qt::CaseInsensitive) ||
                fileName.endsWith("-Patch.zar", Qt::CaseInsensitive) ||
                fileName.endsWith("-mods.zar", Qt::CaseInsensitive) ||
                fileName.endsWith("-MODS.zar", Qt::CaseInsensitive) ||
                fileName.endsWith("-Mods.zar", Qt::CaseInsensitive)) {
                continue;
            }
            zarFilePaths.append(entry.absoluteFilePath());
        } else if (entry.isDir()) {
            QString folderName = entry.fileName();
            // Skip update/patch/mods folders
            if (folderName.endsWith("-UPDATE", Qt::CaseInsensitive) ||
                folderName.endsWith("-patch", Qt::CaseInsensitive) ||
                folderName.endsWith("-MODS", Qt::CaseInsensitive)) {
                continue;
            }
            // Recursively scan subdirectories for .zar files
            ScanZarFiles(entry.absoluteFilePath(), zarFilePaths, current_depth + 1);
        }
    }
}

GameInfoClass::GameInfoClass() = default;
GameInfoClass::~GameInfoClass() = default;

GameInfoClass::LoadedGames GameInfoClass::LoadFromDisk(
    const std::vector<std::filesystem::path>& dirs) {
    QStringList filePaths;
    QStringList zarFilePaths;
    for (const auto& installLoc : dirs) {
        QString installDir;
        Common::FS::PathToQString(installDir, installLoc);
        ScanDirectoryRecursively(installDir, filePaths, 0);
        ScanZarFiles(installDir, zarFilePaths);
    }

    LoadedGames loaded;
    loaded.games = QtConcurrent::blockingMapped<QVector<GameInfo>>(
        filePaths, [](const QString& path) { return readGameInfo(Common::FS::PathFromQString(path)); });
    loaded.zar_games = QtConcurrent::blockingMapped<QVector<GameInfo>>(
        zarFilePaths,
        [](const QString& path) { return readGameInfo(Common::FS::PathFromQString(path)); });

    QtConcurrent::blockingMap(loaded.games, &GameListUtils::GetFolderSize);

    std::sort(loaded.games.begin(), loaded.games.end(), GameInfoClass::CompareStrings);
    std::sort(loaded.zar_games.begin(), loaded.zar_games.end(), GameInfoClass::CompareStrings);
    return loaded;
}

QFuture<GameInfoClass::LoadedGames> GameInfoClass::LoadFromDiskAsync() {
    // The outer task gets its own thread so the global pool stays free for the per-game work
    // it fans out (blockingMapped / blockingMap above).
    static QThreadPool* loader_pool = [] {
        auto* pool = new QThreadPool();
        pool->setMaxThreadCount(1);
        return pool;
    }();
    return QtConcurrent::run(loader_pool, &GameInfoClass::LoadFromDisk,
                             Config::getGameDirectories());
}

void GameInfoClass::SetGames(LoadedGames loaded) {
    m_games = std::move(loaded.games);
    m_zar_games = std::move(loaded.zar_games);
    // used to retrieve values after performing a search
    m_games_backup = m_games;
}

void GameInfoClass::GetGameInfo(QWidget* parent) {
    // Progress bar, please be patient :) The work runs on worker threads; the dialog's event loop
    // keeps the window painting meanwhile.
    QProgressDialog dialog(tr("Loading game list, please wait :3"), QString(), 0, 0, parent);
    dialog.setWindowTitle(tr("Loading..."));
    dialog.setCancelButton(nullptr);

    QFutureWatcher<LoadedGames> futureWatcher;
    connect(&futureWatcher, &QFutureWatcher<LoadedGames>::finished, &dialog,
            &QProgressDialog::reset);
    futureWatcher.setFuture(LoadFromDiskAsync());
    if (!futureWatcher.isFinished()) {
        dialog.exec();
    }
    futureWatcher.waitForFinished();
    SetGames(futureWatcher.result());
}
