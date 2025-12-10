// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_QT_GUI

#include <memory>
#include <span>
#include <QDialog>
#include <QGroupBox>
#include <QPushButton>

#include "common/config.h"
#include "common/path_util.h"
#include "core/ipc/ipc_client.h"
#include "qt_gui/compatibility_info.h"

namespace Ui {
class SettingsDialog;
}

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(std::shared_ptr<CompatibilityInfoClass> m_compat_info,
                            std::shared_ptr<IpcClient> ipc_client, QWidget* parent = nullptr,
                            bool is_game_running = false, bool is_game_specific = false,
                            std::string gsc_serial = "");
    ~SettingsDialog();

    bool eventFilter(QObject* obj, QEvent* event) override;
    void updateNoteTextEdit(const QString& groupName);

    int exec() override;

signals:
    void LanguageChanged(const std::string& locale);
    void CompatibilityChanged();
    void BackgroundOpacityChanged(int opacity);

private:
    void LoadValuesFromConfig();
    void UpdateSettings();
    void SyncRealTimeWidgetstoConfig();
    void pollSDLevents();
    void onAudioDeviceChange(bool isAdd);
    void InitializeEmulatorLanguages();
    void OnLanguageChanged(int index);
    void OnCursorStateChanged(s16 index);
    void closeEvent(QCloseEvent* event) override;
    void OnRcasAttenuationChanged(int value);
    void OnRcasAttenuationSpinBoxChanged(double value);
    void VolumeSliderChange(int value);
    void FPSChange(int value);
    void setDefaultValues();

    std::unique_ptr<Ui::SettingsDialog> ui;

    std::map<std::string, int> languages;
    std::shared_ptr<CompatibilityInfoClass> compat_info;
    std::shared_ptr<IpcClient> m_ipc_client;
    QCheckBox* specialPadChecks[4][4];

    QString defaultTextEdit;

    int initialHeight;
    std::string gs_serial;

    bool is_game_running = false;
    bool is_saving = false;
    QFuture<void> Polling;
};

#endif
