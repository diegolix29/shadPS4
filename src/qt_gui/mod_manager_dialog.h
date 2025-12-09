// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <QDialog>
#include <QListWidget>
#include <QString>

class ModManagerDialog : public QDialog {
    Q_OBJECT

public:
    explicit ModManagerDialog(const QString& gamePath, const QString& gameSerial,
                              QWidget* parent = nullptr);

    void activateSelected();
    QStringList detectModConflicts(const QString& modInstallPath, const QString& incomingRootPath);
    void installModFromDisk();
    void removeAvailableMod();
    void deactivateSelected();
    QString resolveOriginalFolderForRestore(const QString& rel) const;
    QString normalizeExtractedMod(const QString& modPath);
    void restoreMod(const QString& modName);
    void activateAll();
    void deactivateAll();
    bool showScrollableConflictDialog(const QString& text);

private:
    void scanAvailableMods();
    void cleanupOverlayRootIfEmpty();
    void scanActiveMods();
    void installMod(const QString& modName);
    void uninstallMod(const QString& modName);
    bool modMatchesGame(const std::filesystem::path& modPath) const;
    QString findModThatContainsFile(const QString& relPath) const;
    void updateModListUI();

    QString resolveOriginalFile(const QString& rel) const;

    bool needsDvdrootPrefix(const QString& modName) const;
    bool ExtractArchive(const QString& archivePath, const QString& outputPath);

    QString gameSerial;
    QString gamePath;
    QSet<QString> greyedOutMods;

    QString modsRoot;
    QString availablePath;
    QString backupsRoot;
    QString activePath;
    QString overlayRoot;

    QListWidget* listAvailable;
    QListWidget* listActive;
};
