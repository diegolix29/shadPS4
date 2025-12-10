// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QDialog>
#include "qt_gui/compatibility_info.h"

namespace Ui {
class VersionDialog;
}

class VersionDialog : public QDialog {
    Q_OBJECT

public:
    explicit VersionDialog(std::shared_ptr<CompatibilityInfoClass> compat_info,
                           QWidget* parent = nullptr);
    ~VersionDialog();

    void DownloadListVersion();
    void InstallSelectedVersion();
    void InstallPkgWithV7();

private:
    Ui::VersionDialog* ui;
    std::shared_ptr<CompatibilityInfoClass> m_compat_info;

    static std::filesystem::path GetActualExecutablePath();
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void LoadinstalledList();
    QStringList LoadDownloadCache();
    void SaveDownloadCache(const QStringList& versions);
    void PopulateDownloadTree(const QStringList& versions);
    void InstallSelectedVersionExe();
    void UninstallSelectedVersion();
};
