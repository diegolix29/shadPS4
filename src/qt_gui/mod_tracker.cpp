// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include "mod_tracker.h"

ModTracker::ModTracker(const QString& gameSerial, const QString& modsRoot)
    : gameSerial(gameSerial), modsRoot(modsRoot) {}

bool ModTracker::loadFromFile() {
    QString filePath = getTrackerFilePath();
    QFile file(filePath);

    if (!file.exists()) {
        return true;
    }

    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isNull() || !doc.isObject()) {
        return false;
    }

    QJsonObject root = doc.object();
    QJsonArray modsArray = root["mods"].toArray();

    mods.clear();

    for (const QJsonValue& value : modsArray) {
        if (!value.isObject()) {
            continue;
        }

        ModInfo modInfo = modInfoFromJson(value.toObject());
        if (modInfo.gameSerial == gameSerial) {
            mods[modInfo.name] = modInfo;
        }
    }

    updateConflicts();
    return true;
}

bool ModTracker::saveToFile() {
    QJsonObject root;
    QJsonArray modsArray;

    for (const auto& modInfo : mods) {
        modsArray.append(modInfoToJson(modInfo));
    }

    root["mods"] = modsArray;
    root["gameSerial"] = gameSerial;
    root["lastUpdated"] = QDateTime::currentDateTime().toString(Qt::ISODate);

    QJsonDocument doc(root);

    QString filePath = getTrackerFilePath();
    QFile file(filePath);

    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }

    file.write(doc.toJson());
    file.close();

    return true;
}

void ModTracker::addMod(const ModInfo& modInfo) {
    mods[modInfo.name] = modInfo;
    updateConflicts();
}

void ModTracker::removeMod(const QString& modName) {
    mods.remove(modName);
    updateConflicts();
}

void ModTracker::updateMod(const QString& modName, const ModInfo& modInfo) {
    mods[modName] = modInfo;
    updateConflicts();
}

ModInfo ModTracker::getMod(const QString& modName) const {
    return mods.value(modName);
}

QList<ModInfo> ModTracker::getAllMods() const {
    return mods.values();
}

QList<ModInfo> ModTracker::getActiveMods() const {
    QList<ModInfo> activeMods;
    for (const auto& modInfo : mods) {
        if (modInfo.isActive) {
            activeMods.append(modInfo);
        }
    }
    return activeMods;
}

void ModTracker::addFileToMod(const QString& modName, const QString& relativePath,
                              const QString& originalPath, const QString& backupPath) {
    if (!mods.contains(modName)) {
        return;
    }

    ModInfo& modInfo = mods[modName];
    modInfo.files.insert(relativePath);

    ModFileInfo fileInfo;
    fileInfo.relativePath = relativePath;
    fileInfo.originalPath = originalPath;
    fileInfo.backupPath = backupPath;
    fileInfo.installedAt = QDateTime::currentDateTime();

    QFileInfo originalFileInfo(originalPath);
    if (originalFileInfo.exists()) {
        fileInfo.fileSize = originalFileInfo.size();

        QFile file(originalPath);
        if (file.open(QIODevice::ReadOnly)) {
            QByteArray fileData = file.readAll();
            QCryptographicHash hash(QCryptographicHash::Sha256);
            hash.addData(fileData);
            fileInfo.checksum = hash.result().toHex();
            file.close();
        }
    }

    modInfo.fileDetails[relativePath] = fileInfo;
    updateConflicts();
}

void ModTracker::removeFileFromMod(const QString& modName, const QString& relativePath) {
    if (!mods.contains(modName)) {
        return;
    }

    ModInfo& modInfo = mods[modName];
    modInfo.files.remove(relativePath);
    modInfo.fileDetails.remove(relativePath);
    updateConflicts();
}

QStringList ModTracker::getModFiles(const QString& modName) const {
    if (!mods.contains(modName)) {
        return QStringList();
    }

    return mods[modName].files.values();
}

QStringList ModTracker::getConflictingFiles(const QString& modName) const {
    if (!mods.contains(modName)) {
        return QStringList();
    }

    const ModInfo& modInfo = mods[modName];
    QStringList conflicts;

    for (const QString& filePath : modInfo.files) {
        for (const auto& otherMod : mods) {
            if (otherMod.name != modName && otherMod.files.contains(filePath)) {
                conflicts.append(filePath);
                break;
            }
        }
    }

    return conflicts;
}

QSet<QString> ModTracker::findConflictingMods(const QString& modName) const {
    if (!mods.contains(modName)) {
        return QSet<QString>();
    }

    const ModInfo& modInfo = mods[modName];
    QSet<QString> conflictingMods;

    for (const QString& filePath : modInfo.files) {
        for (const auto& otherMod : mods) {
            if (otherMod.name != modName && otherMod.files.contains(filePath)) {
                conflictingMods.insert(otherMod.name);
            }
        }
    }

    return conflictingMods;
}

void ModTracker::setModActive(const QString& modName, bool active) {
    if (!mods.contains(modName)) {
        return;
    }

    ModInfo& modInfo = mods[modName];
    modInfo.isActive = active;
    if (active) {
        modInfo.lastActivated = QDateTime::currentDateTime();
    }

    updateConflicts();
}

bool ModTracker::isModActive(const QString& modName) const {
    if (!mods.contains(modName)) {
        return false;
    }

    return mods[modName].isActive;
}

void ModTracker::updateConflicts() {
    for (auto& modInfo : mods) {
        modInfo.conflicts.clear();
    }

    for (auto it1 = mods.begin(); it1 != mods.end(); ++it1) {
        for (auto it2 = std::next(it1); it2 != mods.end(); ++it2) {
            QSet<QString> files1 = it1.value().files;
            QSet<QString> files2 = it2.value().files;
            QSet<QString> intersection = files1.intersect(files2);
            if (!intersection.isEmpty()) {
                mods[it1.key()].conflicts.insert(it2.key());
                mods[it2.key()].conflicts.insert(it1.key());
            }
        }
    }
}

QStringList ModTracker::getFilesOwnedByOtherMods(const QString& modName,
                                                 const QStringList& files) const {
    QStringList ownedByOthers;

    for (const QString& filePath : files) {
        bool ownedByOther = false;
        for (const auto& modInfo : mods) {
            if (modInfo.name != modName && modInfo.files.contains(filePath)) {
                ownedByOther = true;
                break;
            }
        }
        if (ownedByOther) {
            ownedByOthers.append(filePath);
        }
    }

    return ownedByOthers;
}

QString ModTracker::getTrackerFilePath() const {
    return QDir(modsRoot).filePath(trackerFileName);
}

QJsonObject ModTracker::modInfoToJson(const ModInfo& modInfo) const {
    QJsonObject json;
    json["name"] = modInfo.name;
    json["version"] = modInfo.version;
    json["author"] = modInfo.author;
    json["description"] = modInfo.description;
    json["installedAt"] = modInfo.installedAt.toString(Qt::ISODate);
    json["lastActivated"] = modInfo.lastActivated.toString(Qt::ISODate);
    json["isActive"] = modInfo.isActive;
    json["gameSerial"] = modInfo.gameSerial;

    QJsonArray filesArray;
    for (const QString& file : modInfo.files) {
        filesArray.append(file);
    }
    json["files"] = filesArray;

    QJsonObject fileDetailsObj;
    for (auto it = modInfo.fileDetails.begin(); it != modInfo.fileDetails.end(); ++it) {
        fileDetailsObj[it.key()] = fileInfoToJson(it.value());
    }
    json["fileDetails"] = fileDetailsObj;

    QJsonArray conflictsArray;
    for (const QString& conflict : modInfo.conflicts) {
        conflictsArray.append(conflict);
    }
    json["conflicts"] = conflictsArray;

    return json;
}

ModInfo ModTracker::modInfoFromJson(const QJsonObject& json) const {
    ModInfo modInfo;
    modInfo.name = json["name"].toString();
    modInfo.version = json["version"].toString();
    modInfo.author = json["author"].toString();
    modInfo.description = json["description"].toString();
    modInfo.installedAt = QDateTime::fromString(json["installedAt"].toString(), Qt::ISODate);
    modInfo.lastActivated = QDateTime::fromString(json["lastActivated"].toString(), Qt::ISODate);
    modInfo.isActive = json["isActive"].toBool();
    modInfo.gameSerial = json["gameSerial"].toString();

    QJsonArray filesArray = json["files"].toArray();
    for (const QJsonValue& value : filesArray) {
        modInfo.files.insert(value.toString());
    }

    QJsonObject fileDetailsObj = json["fileDetails"].toObject();
    for (auto it = fileDetailsObj.begin(); it != fileDetailsObj.end(); ++it) {
        modInfo.fileDetails[it.key()] = fileInfoFromJson(it.value().toObject());
    }

    QJsonArray conflictsArray = json["conflicts"].toArray();
    for (const QJsonValue& value : conflictsArray) {
        modInfo.conflicts.insert(value.toString());
    }

    return modInfo;
}

QJsonObject ModTracker::fileInfoToJson(const ModFileInfo& fileInfo) const {
    QJsonObject json;
    json["relativePath"] = fileInfo.relativePath;
    json["originalPath"] = fileInfo.originalPath;
    json["backupPath"] = fileInfo.backupPath;
    json["fileSize"] = fileInfo.fileSize;
    json["checksum"] = fileInfo.checksum;
    json["installedAt"] = fileInfo.installedAt.toString(Qt::ISODate);
    return json;
}

ModFileInfo ModTracker::fileInfoFromJson(const QJsonObject& json) const {
    ModFileInfo fileInfo;
    fileInfo.relativePath = json["relativePath"].toString();
    fileInfo.originalPath = json["originalPath"].toString();
    fileInfo.backupPath = json["backupPath"].toString();
    fileInfo.fileSize = json["fileSize"].toVariant().toLongLong();
    fileInfo.checksum = json["checksum"].toString();
    fileInfo.installedAt = QDateTime::fromString(json["installedAt"].toString(), Qt::ISODate);
    return fileInfo;
}
