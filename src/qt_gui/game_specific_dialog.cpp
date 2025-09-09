// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fstream>
#include <QDesktopServices>
#include <QDialogButtonBox>

#include <toml.hpp>
#include "common/config.h"
#include "core/file_sys/fs.h"
#include "core/libraries/audio/audioout.h"
#include "log_presets_dialog.h"

#include "common/path_util.h"
#include "game_specific_dialog.h"
#include "ui_game_specific_dialog.h"

GameSpecificDialog::GameSpecificDialog(std::shared_ptr<CompatibilityInfoClass> compat_info,
                                       QWidget* parent, const std::string& serial)
    : QDialog(parent), ui(new Ui::GameSpecificDialog()), m_compat_info(std::move(compat_info)),
      m_serial(serial) {
    ui->setupUi(this);
    connect(ui->fpsSlider, &QSlider::valueChanged, ui->fpsSpinBox, &QSpinBox::setValue);
    connect(ui->fpsSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), ui->fpsSlider,
            &QSlider::setValue);

    m_config_path = std::filesystem::path(GetUserPath(Common::FS::PathType::CustomConfigs)) /
                    (serial + ".toml");

    LoadValuesFromConfig();

    connect(ui->buttonBox, &QDialogButtonBox::accepted, this, [this]() {
        UpdateSettings();
        Config::save(m_config_path);
        accept();
    });
    connect(ui->logPresetsButton, &QPushButton::clicked, this, [this]() {
        auto dlg = new LogPresetsDialog(m_compat_info, this);
        connect(dlg, &LogPresetsDialog::PresetChosen, this,
                [this](const QString& filter) { ui->logFilterLineEdit->setText(filter); });
        dlg->exec();
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
    if (!std::filesystem::exists(m_config_path))
        return;

    toml::value data;
    try {
        data = toml::parse(m_config_path);
    } catch (const std::exception& e) {
        return;
    }
    QMap<QString, QString> logTypeMap = {
        {tr("async"), "async"},
        {tr("sync"), "sync"},
    };

    QMap<QString, QString> presentModeMap = {
        {tr("Mailbox (Vsync)"), "Mailbox"},
        {tr("Fifo (Vsync)"), "Fifo"},
        {tr("Immediate (No Vsync)"), "Immediate"},
    };

    QMap<QString, QString> screenModeMap = {
        {tr("Fullscreen (Borderless)"), "Fullscreen (Borderless)"},
        {tr("Windowed"), "Windowed"},
        {tr("Fullscreen"), "Fullscreen"},
    };
    // === General ===
    ui->enableAutoBackupCheckBox->setChecked(
        toml::find_or<bool>(data, "General", "enableAutoBackup", false));
    ui->discordRPCCheckbox->setChecked(
        toml::find_or<bool>(data, "General", "enableDiscordRPC", false));

    connect(ui->horizontalVolumeSlider, &QSlider::valueChanged, this, [this](int value) {
        VolumeSliderChange(value);
        Libraries::AudioOut::AdjustVol();
    });
    int gameVolume = toml::find_or<int>(data, "General", "volumeSlider", 100);
    ui->horizontalVolumeSlider->setValue(gameVolume);
    ui->volumeText->setText(QString::number(gameVolume) + "%");

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
    const auto logValue =
        QString::fromStdString(toml::find_or<std::string>(data, "General", "logType", "sync"));
    const auto logText = logTypeMap.key(logValue, tr("sync"));
    ui->logTypeComboBox->setCurrentText(logText);
    ui->screenTipBox->setChecked(toml::find_or<bool>(data, "General", "screenTipDisable", false));
    ui->showSplashCheckBox->setChecked(toml::find_or<bool>(data, "General", "showSplash", false));

    QString side =
        QString::fromStdString(toml::find_or<std::string>(data, "General", "sideTrophy", "right"));
    ui->radioButton_Left->setChecked(side == "left");
    ui->radioButton_Right->setChecked(side == "right");
    ui->radioButton_Top->setChecked(side == "top");
    ui->radioButton_Bottom->setChecked(side == "bottom");

    ui->popUpDurationSpinBox->setValue(
        toml::find_or<int>(data, "General", "trophyNotificationDuration", 5));

    connect(ui->OpenLogLocationButton, &QPushButton::clicked, this, []() {
        QString userPath;
        Common::FS::PathToQString(userPath, Common::FS::GetUserPath(Common::FS::PathType::LogDir));
        QDesktopServices::openUrl(QUrl::fromLocalFile(userPath));
    });

    // === Input ===
    ui->hideCursorComboBox->addItem(tr("Never"));
    ui->hideCursorComboBox->addItem(tr("Idle"));
    ui->hideCursorComboBox->addItem(tr("Always"));

    connect(ui->hideCursorComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](s16 index) { OnCursorStateChanged(index); });

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

    // === GPU ===
    ui->enableHDRCheckBox->setChecked(toml::find_or<bool>(data, "GPU", "allowHDR", false));
    ui->copyGPUBuffersCheckBox->setChecked(
        toml::find_or<bool>(data, "GPU", "copyGPUBuffers", false));
    ui->DMACheckBox->setChecked(toml::find_or<bool>(data, "GPU", "directMemoryAccess", false));
    ui->dumpShadersCheckBox->setChecked(toml::find_or<bool>(data, "GPU", "dumpShaders", false));
    const int fps = toml::find_or<int>(data, "GPU", "fpsLimit", 60);
    ui->fpsSpinBox->setValue(fps);
    ui->fpsLimiterCheckBox->setChecked(toml::find_or<bool>(data, "GPU", "fpsLimiterEnabled", true));
    ui->FSRCheckBox->setChecked(toml::find_or<bool>(data, "GPU", "fsrEnabled", true));
    const auto screenValue = QString::fromStdString(
        toml::find_or<std::string>(data, "GPU", "FullscreenMode", "Windowed"));
    const auto screenText = screenModeMap.key(screenValue, tr("Windowed"));
    ui->displayModeComboBox->setCurrentText(screenText);
    ui->MemoryComboBox->setCurrentText(
        QString::fromStdString(toml::find_or<std::string>(data, "GPU", "memoryAlloc", "medium")));

    const auto presentValue =
        QString::fromStdString(toml::find_or<std::string>(data, "GPU", "presentMode", "Fifo"));
    const auto presentText = presentModeMap.key(presentValue, tr("Fifo (Vsync)"));
    ui->presentModeComboBox->setCurrentText(presentText);
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
    ui->guestMarkersCheckBox->setChecked(
        toml::find_or<bool>(data, "Vulkan", "guestMarkers", false));
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
    QMap<QString, QString> logTypeMap;

    logTypeMap = {{tr("async"), "async"}, {tr("sync"), "sync"}};
    QMap<QString, QString> presentModeMap;
    presentModeMap = {{tr("Mailbox (Vsync)"), "Mailbox"},
                      {tr("Fifo (Vsync)"), "Fifo"},
                      {tr("Immediate (No Vsync)"), "Immediate"}};
    QMap<QString, QString> screenModeMap;

    screenModeMap = {{tr("Fullscreen (Borderless)"), "Fullscreen (Borderless)"},
                     {tr("Windowed"), "Windowed"},
                     {tr("Fullscreen"), "Fullscreen"}};
    Config::setIsFullscreen(screenModeMap.value(ui->displayModeComboBox->currentText()) !=
                            "Windowed");
    Config::setFullscreenMode(
        screenModeMap.value(ui->displayModeComboBox->currentText()).toStdString());
    Config::setPresentMode(
        presentModeMap.value(ui->presentModeComboBox->currentText()).toStdString());
    Config::setIsMotionControlsEnabled(ui->motionControlsCheckBox->isChecked());
    Config::setBackgroundControllerInput(ui->backgroundControllerCheckBox->isChecked());
    Config::setisTrophyPopupDisabled(ui->disableTrophycheckBox->isChecked());
    Config::setTrophyNotificationDuration(ui->popUpDurationSpinBox->value());

    if (ui->radioButton_Top->isChecked()) {
        Config::setSideTrophy("top");
    } else if (ui->radioButton_Left->isChecked()) {
        Config::setSideTrophy("left");
    } else if (ui->radioButton_Right->isChecked()) {
        Config::setSideTrophy("right");
    } else if (ui->radioButton_Bottom->isChecked()) {
        Config::setSideTrophy("bottom");
    }
    Config::setDevKitMode(ui->isDevKitCheckBox->isChecked());
    Config::setNeoMode(ui->isNeoModeCheckBox->isChecked());
    Config::setLoggingEnabled(ui->enableLoggingCheckBox->isChecked());
    Config::setAllowHDR(ui->enableHDRCheckBox->isChecked());
    Config::setEnableAutoBackup(ui->enableAutoBackupCheckBox->isChecked());
    Config::setLogType(logTypeMap.value(ui->logTypeComboBox->currentText()).toStdString());
    Config::setLogFilter(ui->logFilterLineEdit->text().toStdString());
    Config::setCursorState(ui->hideCursorComboBox->currentIndex());
    Config::setCursorHideTimeout(ui->idleTimeoutSpinBox->value());
    Config::setEnableDiscordRPC(ui->discordRPCCheckbox->isChecked());
    Config::setWindowWidth(ui->widthSpinBox->value());
    Config::setWindowHeight(ui->heightSpinBox->value());
    Config::setVblankFreq(ui->vblankSpinBox->value());
    Config::setDumpShaders(ui->dumpShadersCheckBox->isChecked());
    Config::setReadbackLinearImages(ui->ReadbacksLinearCheckBox->isChecked());
    Config::setDirectMemoryAccess(ui->DMACheckBox->isChecked());
    Config::isScreenTipDisable(ui->screenTipBox->isChecked());
    Config::setReadbackSpeed(
        static_cast<Config::ReadbackSpeed>(ui->ReadbackSpeedComboBox->currentIndex()));

    Config::setShaderSkipsEnabled(ui->SkipsCheckBox->isChecked());
    Config::setFpsLimit(ui->fpsSlider->value());
    Config::setFpsLimit(ui->fpsSpinBox->value());

    Config::setMemoryAlloc(ui->MemoryComboBox->currentText().toStdString());
    Config::setShowSplash(ui->showSplashCheckBox->isChecked());
    Config::setDebugDump(ui->debugDump->isChecked());
    Config::setVkValidation(ui->vkValidationCheckBox->isChecked());
    Config::setVkSyncValidation(ui->vkSyncValidationCheckBox->isChecked());
    Config::setRdocEnabled(ui->rdocCheckBox->isChecked());
    Config::setVkHostMarkersEnabled(ui->hostMarkersCheckBox->isChecked());
    Config::setVkGuestMarkersEnabled(ui->guestMarkersCheckBox->isChecked());
    Config::setVkCrashDiagnosticEnabled(ui->crashDiagnosticsCheckBox->isChecked());
    Config::setCollectShaderForDebug(ui->collectShaderCheckBox->isChecked());
    Config::setCopyGPUCmdBuffers(ui->copyGPUBuffersCheckBox->isChecked());
    Config::setVolumeSlider(ui->horizontalVolumeSlider->value());

    Config::setFsrEnabled(ui->FSRCheckBox->isChecked());
    Config::setRcasEnabled(ui->RCASCheckBox->isChecked());
    Config::setRcasAttenuation(ui->RCASSpinBox->value());
}
