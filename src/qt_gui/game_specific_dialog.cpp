// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fstream>
#include <QDesktopServices>
#include <QDialogButtonBox>

#include <toml.hpp>
#include "common/config.h"
#include "core/file_sys/fs.h"
#include "core/libraries/audio/audioout.h"

#include "common/path_util.h"
#include "game_specific_dialog.h"
#include "ui_game_specific_dialog.h"

GameSpecificDialog::GameSpecificDialog(std::shared_ptr<CompatibilityInfoClass> compat_info,
                                       QWidget* parent, const std::string& serial)
    : QDialog(parent), ui(new Ui::GameSpecificDialog()),
      m_compat_info(std::move(compat_info)), m_serial(serial) {
    ui->setupUi(this);
    setWindowFlags(windowFlags() & ~Qt::WindowCloseButtonHint);

    m_config_path = std::filesystem::path(GetUserPath(Common::FS::PathType::CustomConfigs)) /
                    (serial + ".toml");

    LoadValuesFromConfig();

    connect(ui->buttonBox, &QDialogButtonBox::accepted, this, [this]() {
        UpdateSettings();
        Config::save(m_config_path);
        accept();
    });

    connect(ui->buttonBox, &QDialogButtonBox::rejected, this, &QWidget::close);
}

GameSpecificDialog::~GameSpecificDialog() = default;

void GameSpecificDialog::VolumeSliderChange(int value) {
    ui->volumeText->setText(QString::number(value) + "%");
}

void GameSpecificDialog::OnCursorStateChanged(s16 index) {
    if (index == -1)
        return;
    if (index == Config::HideCursorState::Idle) {
        ui->idleTimeoutGroupBox->show();
    } else {
        if (!ui->idleTimeoutGroupBox->isHidden()) {
            ui->idleTimeoutGroupBox->hide();
        }
    }
}

void GameSpecificDialog::LoadValuesFromConfig() {
    if (!std::filesystem::exists(m_config_path)) {
        return;
    }

    const toml::value data = toml::parse(m_config_path);

    // === General ===
    ui->enableAutoBackupCheckBox->setChecked(
        toml::find_or<bool>(data, "General", "enableAutoBackup", false));
    ui->discordRPCCheckbox->setChecked(
        toml::find_or<bool>(data, "General", "enableDiscordRPC", false));

    connect(ui->horizontalVolumeSlider, &QSlider::valueChanged, this, [this](int value) {
        VolumeSliderChange(value);
        Config::setVolumeSlider(value);
        Libraries::AudioOut::AdjustVol();
    });
    int gameVolume = Config::getVolumeSlider();
    ui->horizontalVolumeSlider->setValue(gameVolume);
    ui->volumeText->setText(QString::number(ui->horizontalVolumeSlider->sliderPosition()) + "%");
    ui->connectedNetworkCheckBox->setChecked(
        toml::find_or<bool>(data, "General", "isConnectedToNetwork", false));
    ui->isDevKitCheckBox->setChecked(toml::find_or<bool>(data, "General", "isDevKit", false));
    ui->isNeoModeCheckBox->setChecked(toml::find_or<bool>(data, "General", "isPS4Pro", false));
    ui->isPSNSignedInCheckBox->setChecked(
        toml::find_or<bool>(data, "General", "isPSNSignedIn", false));
    ui->disableTrophycheckBox->setChecked(
        toml::find_or<bool>(data, "General", "isTrophyPopupDisabled", false));
    ui->logFilterLineEdit->setText(
        QString::fromStdString(toml::find_or<std::string>(data, "General", "logFilter", "")));
    ui->logTypeComboBox->setCurrentText(
        QString::fromStdString(toml::find_or<std::string>(data, "General", "logType", "sync")));
    ui->screenTipBox->setChecked(toml::find_or<bool>(data, "General", "screenTipDisable", false));
    ui->showSplashCheckBox->setChecked(toml::find_or<bool>(data, "General", "showSplash", false));
    QString side = QString::fromStdString(Config::sideTrophy());
    connect(ui->OpenLogLocationButton, &QPushButton::clicked, this, []() {
        QString userPath;
        Common::FS::PathToQString(userPath, Common::FS::GetUserPath(Common::FS::PathType::LogDir));
        QDesktopServices::openUrl(QUrl::fromLocalFile(userPath));
    });

    ui->radioButton_Left->setChecked(side == "left");
    ui->radioButton_Right->setChecked(side == "right");
    ui->radioButton_Top->setChecked(side == "top");
    ui->radioButton_Bottom->setChecked(side == "bottom");

    ui->popUpDurationSpinBox->setValue(Config::getTrophyNotificationDuration());

    // === Input ===

    ui->hideCursorComboBox->addItem(tr("Never"));
    ui->hideCursorComboBox->addItem(tr("Idle"));
    ui->hideCursorComboBox->addItem(tr("Always"));

    {
        connect(ui->hideCursorComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](s16 index) { OnCursorStateChanged(index); });
    }

    ui->backgroundControllerCheckBox->setChecked(
        toml::find_or<bool>(data, "Input", "backgroundControllerInput", true));
    ui->idleTimeoutSpinBox->setValue(toml::find_or<int>(data, "Input", "cursorHideTimeout", 5));
    ui->hideCursorComboBox->setCurrentIndex(toml::find_or<int>(data, "Input", "cursorState", 1));
    ui->motionControlsCheckBox->setChecked(
        toml::find_or<bool>(data, "Input", "isMotionControlsEnabled", true));
    ui->specialPadClassSpinBox->setValue(toml::find_or<int>(data, "Input", "specialPadClass", 1));
    ui->useSpecialPadCheckBox->setChecked(
        toml::find_or<bool>(data, "Input", "useSpecialPad", false));
    ui->useUnifiedInputConfigCheckBox->setChecked(
        toml::find_or<bool>(data, "Input", "useUnifiedInputConfig", true));
    ui->showSplashCheckBox->setChecked(toml::find_or<bool>(data, "General", "showSplash", false));

    // === GPU ===
    ui->enableHDRCheckBox->setChecked(toml::find_or<bool>(data, "GPU", "allowHDR", false));
    ui->copyGPUBuffersCheckBox->setChecked(
        toml::find_or<bool>(data, "GPU", "copyGPUBuffers", false));
    ui->DMACheckBox->setChecked(toml::find_or<bool>(data, "GPU", "directMemoryAccess", false));
    ui->dumpShadersCheckBox->setChecked(toml::find_or<bool>(data, "GPU", "dumpShaders", false));
    ui->fpsSpinBox->setValue(toml::find_or<int>(data, "GPU", "fpsLimit", 60));
    ui->fpsLimiterCheckBox->setChecked(toml::find_or<bool>(data, "GPU", "fpsLimiterEnabled", true));
    ui->FSRCheckBox->setChecked(toml::find_or<bool>(data, "GPU", "fsrEnabled", true));
    ui->displayModeComboBox->setCurrentText(QString::fromStdString(
        toml::find_or<std::string>(data, "GPU", "FullscreenMode", "Windowed")));
    ui->MemoryComboBox->setCurrentText(
        QString::fromStdString(toml::find_or<std::string>(data, "GPU", "memoryAlloc", "\u0001")));
    ui->presentModeComboBox->setCurrentText(
        QString::fromStdString(toml::find_or<std::string>(data, "GPU", "presentMode", "Mailbox")));
    ui->RCASSpinBox->setValue(toml::find_or<double>(data, "GPU", "rcas_attenuation", 0.25));
    ui->RCASCheckBox->setChecked(toml::find_or<bool>(data, "GPU", "rcasEnabled", true));
    ui->ReadbacksLinearCheckBox->setChecked(
        toml::find_or<bool>(data, "GPU", "readbackLinearImages", false));
    ui->ReadbackSpeedComboBox->setCurrentIndex(toml::find_or<int>(data, "GPU", "readbackSpeed", 3));
    ui->widthSpinBox->setValue(toml::find_or<int>(data, "GPU", "screenWidth", 1280));
    ui->heightSpinBox->setValue(toml::find_or<int>(data, "GPU", "screenHeight", 720));
    ui->SkipsCheckBox->setChecked(toml::find_or<bool>(data, "GPU", "shaderSkipsEnabled", true));
    ui->vblankSpinBox->setValue(toml::find_or<int>(data, "GPU", "vblankDivider", 1));

    // === Vulkan ===
    ui->crashDiagnosticsCheckBox->setChecked(
        toml::find_or<bool>(data, "Vulkan", "crashDiagnostic", false));
    ui->hostMarkersCheckBox->setChecked(toml::find_or<bool>(data, "Vulkan", "guestMarkers", false));
    ui->hostMarkersCheckBox->setChecked(toml::find_or<bool>(data, "Vulkan", "hostMarkers", false));
    ui->rdocCheckBox->setChecked(toml::find_or<bool>(data, "Vulkan", "rdocEnable", false));
    ui->vkValidationCheckBox->setChecked(toml::find_or<bool>(data, "Vulkan", "validation", false));
    ui->vkSyncValidationCheckBox->setChecked(
        toml::find_or<bool>(data, "Vulkan", "validation_sync", false));

    // === Debug ===
    ui->collectShaderCheckBox->setChecked(
        toml::find_or<bool>(data, "Debug", "CollectShader", false));
    ui->debugDump->setChecked(toml::find_or<bool>(data, "Debug", "DebugDump", false));
    ui->enableLoggingCheckBox->setChecked(toml::find_or<bool>(data, "Debug", "logEnabled", false));
}

void GameSpecificDialog::UpdateSettings() {
    toml::value data;

    // === General ===
    data["General"]["enableAutoBackup"] = ui->enableAutoBackupCheckBox->isChecked();
    data["General"]["enableDiscordRPC"] = ui->discordRPCCheckbox->isChecked();
    data["General"]["volumeSlider"] = ui->horizontalVolumeSlider->value();
    data["General"]["isConnectedToNetwork"] = ui->connectedNetworkCheckBox->isChecked();
    data["General"]["isDevKit"] = ui->isDevKitCheckBox->isChecked();
    data["General"]["isPS4Pro"] = ui->isNeoModeCheckBox->isChecked();
    data["General"]["isPSNSignedIn"] = ui->isPSNSignedInCheckBox->isChecked();
    data["General"]["logFilter"] = ui->logFilterLineEdit->text().toStdString();
    data["General"]["logType"] = ui->logTypeComboBox->currentText().toStdString();
    data["General"]["screenTipDisable"] = ui->screenTipBox->isChecked();
    data["General"]["showSplash"] = ui->showSplashCheckBox->isChecked();
    data["General"]["isTrophyPopupDisabled"] = ui->disableTrophycheckBox->isChecked();
    data["General"]["trophyNotificationDuration"] = ui->popUpDurationSpinBox->value();

    if (ui->radioButton_Top->isChecked())
        data["General"]["sideTrophy"] = "top";
    else if (ui->radioButton_Left->isChecked())
        data["General"]["sideTrophy"] = "left";
    else if (ui->radioButton_Right->isChecked())
        data["General"]["sideTrophy"] = "right";
    else if (ui->radioButton_Bottom->isChecked())
        data["General"]["sideTrophy"] = "bottom";

    // === Input ===
    data["Input"]["cursorState"] = ui->hideCursorComboBox->currentIndex();
    data["Input"]["cursorHideTimeout"] = ui->idleTimeoutSpinBox->value();
    data["Input"]["backgroundControllerInput"] = ui->backgroundControllerCheckBox->isChecked();
    data["Input"]["isMotionControlsEnabled"] = ui->motionControlsCheckBox->isChecked();
    data["Input"]["specialPadClass"] = ui->specialPadClassSpinBox->value();
    data["Input"]["useSpecialPad"] = ui->useSpecialPadCheckBox->isChecked();
    data["Input"]["useUnifiedInputConfig"] = ui->useUnifiedInputConfigCheckBox->isChecked();

    // === GPU ===
    data["GPU"]["allowHDR"] = ui->enableHDRCheckBox->isChecked();
    data["GPU"]["directMemoryAccess"] = ui->DMACheckBox->isChecked();
    data["GPU"]["dumpShaders"] = ui->dumpShadersCheckBox->isChecked();
    data["GPU"]["fpsLimit"] = ui->fpsSpinBox->value();
    data["GPU"]["fpsLimiterEnabled"] = ui->fpsLimiterCheckBox->isChecked();
    data["GPU"]["fsrEnabled"] = ui->FSRCheckBox->isChecked();
    data["GPU"]["FullscreenMode"] = ui->displayModeComboBox->currentText().toStdString();
    data["GPU"]["memoryAlloc"] = ui->MemoryComboBox->currentText().toStdString();
    data["GPU"]["presentMode"] = ui->presentModeComboBox->currentText().toStdString();
    data["GPU"]["rcas_attenuation"] = ui->RCASSpinBox->value();
    data["GPU"]["rcasEnabled"] = ui->RCASCheckBox->isChecked();
    data["GPU"]["readbackLinearImages"] = ui->ReadbacksLinearCheckBox->isChecked();
    data["GPU"]["readbackSpeed"] = ui->ReadbackSpeedComboBox->currentIndex();
    data["GPU"]["screenWidth"] = ui->widthSpinBox->value();
    data["GPU"]["screenHeight"] = ui->heightSpinBox->value();
    data["GPU"]["shaderSkipsEnabled"] = ui->SkipsCheckBox->isChecked();
    data["GPU"]["vblankDivider"] = ui->vblankSpinBox->value();

    // === Vulkan ===
    data["Vulkan"]["validation"] = ui->vkValidationCheckBox->isChecked();
    data["Vulkan"]["validation_sync"] = ui->vkSyncValidationCheckBox->isChecked();
    data["Vulkan"]["rdocEnable"] = ui->rdocCheckBox->isChecked();
    data["Vulkan"]["hostMarkers"] = ui->hostMarkersCheckBox->isChecked();
    data["Vulkan"]["guestMarkers"] = ui->guestMarkersCheckBox->isChecked();
    data["Vulkan"]["crashDiagnostic"] = ui->crashDiagnosticsCheckBox->isChecked();

    // === Debug ===
    data["Debug"]["CollectShader"] = ui->collectShaderCheckBox->isChecked();
    data["Debug"]["DebugDump"] = ui->debugDump->isChecked();
    data["Debug"]["logEnabled"] = ui->enableLoggingCheckBox->isChecked();
    data["GPU"]["copyGPUBuffers"] = ui->copyGPUBuffersCheckBox->isChecked();

    // finally save to the per-game file
    std::ofstream ofs(m_config_path);
    ofs << data;
}
