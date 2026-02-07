// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QSet>
#include <QString>

struct ModFileInfo {
    QString relativePath;
    QString originalPath;
    QString backupPath;
    qint64 fileSize;
    QString checksum;
    QDateTime installedAt;
};

struct ModInfo {
    QString name;
    QString version;
    QString author;
    QString description;
    QDateTime installedAt;
    QDateTime lastActivated;
    bool isActive;
    QSet<QString> files;
    QMap<QString, ModFileInfo> fileDetails;
    QSet<QString> conflicts;
    QString gameSerial;
};

class ModTracker {
public:
    explicit ModTracker(const QString& gameSerial, const QString& modsRoot);

    bool loadFromFile();
    bool saveToFile();

    void addMod(const ModInfo& modInfo);
    void removeMod(const QString& modName);
    void updateMod(const QString& modName, const ModInfo& modInfo);

    ModInfo getMod(const QString& modName) const;
    QList<ModInfo> getAllMods() const;
    QList<ModInfo> getActiveMods() const;

    void addFileToMod(const QString& modName, const QString& relativePath,
                      const QString& originalPath, const QString& backupPath = QString());
    void removeFileFromMod(const QString& modName, const QString& relativePath);

    QStringList getModFiles(const QString& modName) const;
    QStringList getConflictingFiles(const QString& modName) const;
    QSet<QString> findConflictingMods(const QString& modName) const;

    void setModActive(const QString& modName, bool active);
    bool isModActive(const QString& modName) const;

    void updateConflicts();
    QStringList getFilesOwnedByOtherMods(const QString& modName, const QStringList& files) const;

private:
    QString getTrackerFilePath() const;
    QJsonObject modInfoToJson(const ModInfo& modInfo) const;
    ModInfo modInfoFromJson(const QJsonObject& json) const;
    QJsonObject fileInfoToJson(const ModFileInfo& fileInfo) const;
    ModFileInfo fileInfoFromJson(const QJsonObject& json) const;

    QString gameSerial;
    QString modsRoot;
    QMap<QString, ModInfo> mods;
    QString trackerFileName = "mod_tracker.json";
};
