// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <memory>
#include <QDialog>
#include "qt_gui/compatibility_info.h"

namespace Ui {
class GameSpecificDialog;
}

class GameSpecificDialog : public QDialog {
    Q_OBJECT

public:
    explicit GameSpecificDialog(std::shared_ptr<CompatibilityInfoClass> compat_info,
                                QWidget* parent, const std::string& serial, bool is_running,
                                std::string gsc_serial);
    ~GameSpecificDialog();

private:
    void LoadValuesFromConfig();
    void UpdateSettings();
    void VolumeSliderChange(int value);
    void OnCursorStateChanged(s16 index);
    void OnRcasAttenuationChanged(int value);
    void OnRcasAttenuationSpinBoxChanged(double value);
    void PopulateAudioDevices();
    Ui::GameSpecificDialog* ui;
    std::shared_ptr<CompatibilityInfoClass> m_compat_info;
    std::string m_serial;
    std::filesystem::path m_config_path;

    // backups if you want cancel/restore logic like SettingsDialog
    int fps_backup{};
    int volume_slider_backup{};
};