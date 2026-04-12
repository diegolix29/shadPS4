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
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QString>
#include <QTabWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include "mod_tracker.h"
#include "nexus_mods_api.h"

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

    void onNexusSearchClicked();
    void onNexusSearchReturnPressed();
    void onNexusSearchResultsReady(const QList<NexusModResult>& results, int totalCount);
    void onNexusModFilesReady(int modId, const QList<NexusFileInfo>& files);
    void onNexusDownloadLinkReady(int modId, int fileId, const QString& url);
    void onNexusError(const QString& message);
    void onNexusApiKeyValidated(bool valid, const QString& username);
    void onNexusSetApiKeyClicked();
    void onNexusResultItemClicked(int modId, const QString& gameSlug);
    void onNexusThumbnailDownloaded(QNetworkReply* reply, QLabel* imageLabel);
    void onNexusPagePrev();
    void onNexusPageNext();

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
    QString findModFolder(const QString& gamePath) const;
    QString resolveOriginalFile(const QString& rel) const;
    bool needsDvdrootPrefix(const QString& modName) const;
    bool ExtractArchive(const QString& archivePath, const QString& outputPath);

    void setupNexusTab(QTabWidget* tabs);
    void updateNexusKeyStatus();
    void populateNexusResults(const QList<NexusModResult>& results, int totalCount);
    QWidget* createNexusResultCard(const NexusModResult& mod);
    void loadNexusThumbnail(const QString& url, QLabel* label);
    void downloadAndInstallNexusMod(const QString& gameSlug, int modId, int fileId,
                                    const QString& fileName);

    QString gameSerial;
    QString gamePath;
    QSet<QString> greyedOutMods;

    QString modsRoot;
    QString availablePath;
    QString backupsRoot;
    QString activePath;
    QString overlayRoot;

    QLineEdit* searchBox{nullptr};
    QComboBox* sortComboBox{nullptr};
    QPushButton* gridViewBtn{nullptr};
    QPushButton* listViewBtn{nullptr};
    QPushButton* refreshBtn{nullptr};
    QPushButton* closeBtn{nullptr};
    QPushButton* installModBtn{nullptr};
    QPushButton* uninstallModBtn{nullptr};
    QWidget* titleLabel{nullptr};

    QListWidget* modListWidget{nullptr};
    QScrollArea* modScrollArea{nullptr};
    QWidget* modListContainer{nullptr};
    QVBoxLayout* modListLayout{nullptr};

    QButtonGroup* viewModeGroup{nullptr};
    QList<ModInfo> allMods;
    QList<ModInfo> filteredMods;
    QString currentSearchText;
    int currentSortIndex{0};
    bool isGridView{true};

    QListWidget* listAvailable{nullptr};
    QListWidget* listActive{nullptr};

    std::unique_ptr<ModTracker> modTracker;

    NexusModsApi* m_nexusApi{nullptr};
    QNetworkAccessManager* m_imgNam{nullptr};

    QLabel* m_nexusKeyStatusLabel{nullptr};
    QPushButton* m_nexusSetKeyBtn{nullptr};
    QLineEdit* m_nexusSearchBox{nullptr};
    QPushButton* m_nexusSearchBtn{nullptr};
    QWidget* m_nexusResultsContainer{nullptr};
    QVBoxLayout* m_nexusResultsLayout{nullptr};
    QScrollArea* m_nexusScrollArea{nullptr};
    QLabel* m_nexusStatusLabel{nullptr};
    QPushButton* m_nexusPrevBtn{nullptr};
    QPushButton* m_nexusNextBtn{nullptr};
    QLabel* m_nexusPageLabel{nullptr};

    int m_nexusOffset{0};
    int m_nexusLimit{20};
    int m_nexusTotalCount{0};
    QString m_nexusCurrentQuery;
    QString m_nexusCurrentSlug;

    QHash<int, QString> m_pendingFileSelectSlug;
};