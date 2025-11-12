// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QFuture>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QtNetwork>

#include "common/config.h"
#include "core/file_format/psf.h"

enum class CompatibilityStatus {
    Unknown,
    Nothing,
    Boots,
    Menus,
    Ingame,
    Playable,
};

// Prioritize different compatibility reports based on user's platform
enum class OSType {
#ifdef Q_OS_WIN
    Win32 = 0,
    Unknown,
    Linux,
    macOS,
#elif defined(Q_OS_LINUX)
    Linux = 0,
    Unknown,
    Win32,
    macOS,
#elif defined(Q_OS_MAC)
    macOS = 0,
    Unknown,
    Linux,
    Win32,
#endif
    // Fake enum to allow for iteration
    Last
};

struct CompatibilityEntry {
    CompatibilityStatus status;
    QString version;
    QDateTime last_tested;
    QString url;
    QString issue_number;
    QMap<QString, bool> enabledCheats;
};

class CompatibilityInfoClass : public QObject {
    Q_OBJECT
public:
    std::shared_ptr<CompatibilityInfoClass> m_compat_info;

    // Please think of a better alternative
    inline static const std::unordered_map<QString, CompatibilityStatus> LabelToCompatStatus = {
        {QStringLiteral("status-unknown"), CompatibilityStatus::Unknown},
        {QStringLiteral("status-nothing"), CompatibilityStatus::Nothing},
        {QStringLiteral("status-boots"), CompatibilityStatus::Boots},
        {QStringLiteral("status-menus"), CompatibilityStatus::Menus},
        {QStringLiteral("status-ingame"), CompatibilityStatus::Ingame},
        {QStringLiteral("status-playable"), CompatibilityStatus::Playable}};
    inline static const std::unordered_map<QString, OSType> LabelToOSType = {
        {QStringLiteral("os-linux"), OSType::Linux},
        {QStringLiteral("os-macOS"), OSType::macOS},
        {QStringLiteral("os-windows"), OSType::Win32},
    };

    inline static const std::unordered_map<OSType, QString> OSTypeToString = {
        {OSType::Linux, QStringLiteral("os-linux")},
        {OSType::macOS, QStringLiteral("os-macOS")},
        {OSType::Win32, QStringLiteral("os-windows")},
        {OSType::Unknown, QStringLiteral("os-unknown")}};

    CompatibilityInfoClass();
    ~CompatibilityInfoClass();
    void SetCompatibilityInfo(const std::string& serial, const CompatibilityEntry& entry);
    void UpdateCompatibilityDatabase(QWidget* parent = nullptr, bool forced = false);
    bool LoadCompatibilityFile();
    CompatibilityEntry GetCompatibilityInfo(const std::string& serial);
    const QString GetCompatStatusString(const CompatibilityStatus status);
    void ExtractCompatibilityInfo(QByteArray response);
    QList<QString> LoadPresets() const;
    void SavePresets(const QList<QString>& list);
    QList<QString> LoadFavorites() const;
    void SaveFavorites(const QList<QString>& list);
    QList<int> LoadHiddenColumns() const;
    void SaveHiddenColumns(const QList<int>& hiddenColumns);
    int LoadDisplayMode() const;
    void SaveDisplayMode(int mode);
    std::string GetShadPath() const {

        if (m_shadPath.empty()) {
            QSettings settings("shadPS4", "Emulator");
            QString path = settings.value("ShadPath", "").toString();
            return path.toStdString();
        }
        return m_shadPath;
    }

    void SetShadPath(const std::string& path) {
        m_shadPath = path;

        // Persist to disk
        QSettings settings("shadPS4", "Emulator");
        settings.setValue("ShadPath", QString::fromStdString(path));
        settings.sync();
    }
    QList<int> LoadDockWidgetSizes();
    void SaveDockWidgetSizes(const QList<int>& sizes);

    bool LoadShowLogSetting() const;
    void SaveShowLogSetting(bool show);

    void SetSkipWelcome(bool skip);

    bool GetSkipWelcome() const;

    std::string GetSelectedShadExePath() const {
        if (m_selectedShadExePath.empty()) {
            QSettings settings("shadPS4", "Emulator");
            QString path = settings.value("SelectedShadExePath", "").toString();
            return path.toStdString();
        }
        return m_selectedShadExePath;
    }

    void SetSelectedShadExePath(const std::string& path) {
        m_selectedShadExePath = path;

        // Persist to disk
        QSettings settings("shadPS4", "Emulator");
        settings.setValue("SelectedShadExePath", QString::fromStdString(path));
        settings.sync();
    }

private:
    QNetworkAccessManager* m_network_manager;
    QString m_compatibility_filename;
    QJsonObject m_compatibility_database;
    std::string m_shadPath;
    std::string m_selectedShadExePath;
};