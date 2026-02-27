// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <memory>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QString>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include "mod_tracker.h"

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
    void activateModByName(const QString& modName);
    void deactivateModByName(const QString& modName);
    void showModDetails(const QString& modName);
    void populateFileTree(QTreeWidgetItem* parentItem, const QString& basePath,
                          const QString& relativePath);
    QWidget* createModItem(const ModInfo& modInfo);
    QWidget* createModListItem(const ModInfo& modInfo);
    bool showScrollableConflictDialog(const QString& text);

private slots:
    void onSearchTextChanged(const QString& text);
    void onSortOrderChanged(int index);
    void onViewModeChanged();
    void onModItemClicked(QListWidgetItem* item);
    void onModItemDoubleClicked(QListWidgetItem* item);
    void onToggleModActive(QListWidgetItem* item);
    void onRefreshMods();

private:
    void setupUI();
    void setupHeader();
    void setupModList();
    void setupFooter();
    void applyDarkTheme();
    void updateModDisplay();
    void filterMods();
    void sortMods();
    QPixmap getModThumbnail(const QString& modName) const;
    QString getModSizeString(const QString& modName) const;
    qint64 getModSizeBytes(const QString& modName) const;
    QString getModTypeString(const QString& modName) const;
    void scanAvailableMods();
    void cleanupOverlayRootIfEmpty();
    void scanActiveMods();
    void installMod(const QString& modName);
    void uninstallMod(const QString& modName);
    void copyModToOverlayAndTrack(const QString& modName);
    void cleanupEmptyDirectories(const QString& path);
    bool modMatchesGame(const std::filesystem::path& modPath) const;
    QString findModThatContainsFile(const QString& relPath) const;

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

    QLineEdit* searchBox;
    QComboBox* sortComboBox;
    QPushButton* gridViewBtn;
    QPushButton* listViewBtn;
    QPushButton* refreshBtn;
    QPushButton* closeBtn;
    QPushButton* installModBtn;
    QPushButton* uninstallModBtn;
    QWidget* titleLabel;

    QListWidget* modListWidget;
    QScrollArea* modScrollArea;
    QWidget* modListContainer;
    QVBoxLayout* modListLayout;

    QButtonGroup* viewModeGroup;
    QList<ModInfo> allMods;
    QList<ModInfo> filteredMods;
    QString currentSearchText;
    int currentSortIndex;
    bool isGridView;

    QListWidget* listAvailable;
    QListWidget* listActive;

    std::unique_ptr<ModTracker> modTracker;
};
