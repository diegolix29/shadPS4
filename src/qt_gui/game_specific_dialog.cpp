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

    ui->enableAutoBackupCheckBox->setChecked(Config::getEnableAutoBackup());
    ui->discordRPCCheckbox->setChecked(Config::getEnableDiscordRPC());

    connect(ui->horizontalVolumeSlider, &QSlider::valueChanged, this, [this](int value) {
        VolumeSliderChange(value);
        Libraries::AudioOut::AdjustVol();
    });
    ui->horizontalVolumeSlider->setValue(Config::getVolumeSlider());
    ui->volumeText->setText(QString::number(Config::getVolumeSlider()) + "%");

    ui->connectedNetworkCheckBox->setChecked(Config::getIsConnectedToNetwork());
    ui->isDevKitCheckBox->setChecked(Config::isDevKitConsole());
    ui->isNeoModeCheckBox->setChecked(Config::isNeoModeConsole());
    ui->isPSNSignedInCheckBox->setChecked(Config::getPSNSignedIn());
    ui->disableTrophycheckBox->setChecked(Config::getisTrophyPopupDisabled());
    ui->logFilterLineEdit->setText(QString::fromStdString(Config::getLogFilter()));
    ui->logTypeComboBox->setCurrentText(QString::fromStdString(Config::getLogType()));
    ui->screenTipBox->setChecked(Config::getScreenTipDisable());
    ui->showSplashCheckBox->setChecked(Config::showSplash());

    QString side = QString::fromStdString(Config::sideTrophy());
    ui->radioButton_Left->setChecked(side == "left");
    ui->radioButton_Right->setChecked(side == "right");
    ui->radioButton_Top->setChecked(side == "top");
    ui->radioButton_Bottom->setChecked(side == "bottom");

    ui->popUpDurationSpinBox->setValue(static_cast<int>(Config::getTrophyNotificationDuration()));

    connect(ui->OpenLogLocationButton, &QPushButton::clicked, this, []() {
        QString userPath;
        Common::FS::PathToQString(userPath, Common::FS::GetUserPath(Common::FS::PathType::LogDir));
        QDesktopServices::openUrl(QUrl::fromLocalFile(userPath));
    });

    ui->hideCursorComboBox->clear();
    ui->hideCursorComboBox->addItem(tr("Never"));
    ui->hideCursorComboBox->addItem(tr("Idle"));
    ui->hideCursorComboBox->addItem(tr("Always"));

    connect(ui->hideCursorComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](s16 index) { OnCursorStateChanged(index); });

    ui->backgroundControllerCheckBox->setChecked(Config::getBackgroundControllerInput());
    ui->idleTimeoutSpinBox->setValue(Config::getCursorHideTimeout());
    ui->hideCursorComboBox->setCurrentIndex(Config::getCursorState());
    ui->motionControlsCheckBox->setChecked(Config::getIsMotionControlsEnabled());
    ui->specialPadClassSpinBox->setValue(Config::getSpecialPadClass());
    ui->useSpecialPadCheckBox->setChecked(Config::getUseSpecialPad());
    ui->useUnifiedInputConfigCheckBox->setChecked(Config::GetUseUnifiedInputConfig());

    ui->enableHDRCheckBox->setChecked(Config::allowHDR());
    ui->copyGPUBuffersCheckBox->setChecked(Config::copyGPUCmdBuffers());
    ui->DMACheckBox->setChecked(Config::directMemoryAccess());
    ui->dumpShadersCheckBox->setChecked(Config::dumpShaders());
    ui->fpsSpinBox->setValue(Config::getFpsLimit());
    ui->fpsLimiterCheckBox->setChecked(Config::fpsLimiterEnabled());
    ui->FSRCheckBox->setChecked(Config::getFsrEnabled());
    ui->displayModeComboBox->setCurrentText(QString::fromStdString(Config::getFullscreenMode()));
    ui->MemoryComboBox->setCurrentText(QString::fromStdString(Config::getMemoryAlloc()));
    ui->presentModeComboBox->setCurrentText(QString::fromStdString(Config::getPresentMode()));
    ui->RCASSpinBox->setValue(Config::getRcasAttenuation());
    ui->RCASCheckBox->setChecked(Config::getRcasEnabled());
    ui->ReadbacksLinearCheckBox->setChecked(Config::getReadbackLinearImages());
    ui->ReadbackSpeedComboBox->setCurrentIndex(static_cast<int>(Config::readbackSpeed()));
    ui->widthSpinBox->setValue(Config::getScreenWidth());
    ui->heightSpinBox->setValue(Config::getScreenHeight());
    ui->SkipsCheckBox->setChecked(Config::getShaderSkipsEnabled());
    ui->vblankSpinBox->setValue(Config::vblankFreq());

    ui->crashDiagnosticsCheckBox->setChecked(Config::getVkCrashDiagnosticEnabled());
    ui->guestMarkersCheckBox->setChecked(Config::getVkGuestMarkersEnabled());
    ui->hostMarkersCheckBox->setChecked(Config::getVkHostMarkersEnabled());
    ui->rdocCheckBox->setChecked(Config::isRdocEnabled());
    ui->vkValidationCheckBox->setChecked(Config::vkValidationEnabled());
    ui->vkSyncValidationCheckBox->setChecked(Config::vkValidationSyncEnabled());

    ui->collectShaderCheckBox->setChecked(Config::collectShadersForDebug());
    ui->debugDump->setChecked(Config::debugDump());
    ui->enableLoggingCheckBox->setChecked(Config::getLoggingEnabled());

    if (!std::filesystem::exists(m_config_path))
        return;

    toml::value data;
    try {
        data = toml::parse(m_config_path);
    } catch (const std::exception&) {
        return;
    }

    // General
    if (data.contains("General")) {
        const auto& gen = toml::find(data, "General");
        if (gen.contains("enableAutoBackup"))
            ui->enableAutoBackupCheckBox->setChecked(toml::find<bool>(gen, "enableAutoBackup"));
        if (gen.contains("enableDiscordRPC"))
            ui->discordRPCCheckbox->setChecked(toml::find<bool>(gen, "enableDiscordRPC"));
        if (gen.contains("volumeSlider")) {
            int vol = toml::find<int>(gen, "volumeSlider");
            ui->horizontalVolumeSlider->setValue(vol);
            ui->volumeText->setText(QString::number(vol) + "%");
        }
        if (gen.contains("isConnectedToNetwork"))
            ui->connectedNetworkCheckBox->setChecked(toml::find<bool>(gen, "isConnectedToNetwork"));
        if (gen.contains("isDevKit"))
            ui->isDevKitCheckBox->setChecked(toml::find<bool>(gen, "isDevKit"));
        if (gen.contains("isPS4Pro"))
            ui->isNeoModeCheckBox->setChecked(toml::find<bool>(gen, "isPS4Pro"));
        if (gen.contains("isPSNSignedIn"))
            ui->isPSNSignedInCheckBox->setChecked(toml::find<bool>(gen, "isPSNSignedIn"));
        if (gen.contains("isTrophyPopupDisabled"))
            ui->disableTrophycheckBox->setChecked(toml::find<bool>(gen, "isTrophyPopupDisabled"));
        if (gen.contains("logFilter"))
            ui->logFilterLineEdit->setText(
                QString::fromStdString(toml::find<std::string>(gen, "logFilter")));
        if (gen.contains("logType"))
            ui->logTypeComboBox->setCurrentText(
                QString::fromStdString(toml::find<std::string>(gen, "logType")));
        if (gen.contains("screenTipDisable"))
            ui->screenTipBox->setChecked(toml::find<bool>(gen, "screenTipDisable"));
        if (gen.contains("showSplash"))
            ui->showSplashCheckBox->setChecked(toml::find<bool>(gen, "showSplash"));
        if (gen.contains("sideTrophy")) {
            QString side = QString::fromStdString(toml::find<std::string>(gen, "sideTrophy"));
            ui->radioButton_Left->setChecked(side == "left");
            ui->radioButton_Right->setChecked(side == "right");
            ui->radioButton_Top->setChecked(side == "top");
            ui->radioButton_Bottom->setChecked(side == "bottom");
        }
        if (gen.contains("trophyNotificationDuration"))
            ui->popUpDurationSpinBox->setValue(toml::find<int>(gen, "trophyNotificationDuration"));
    }

    // Input
    if (data.contains("Input")) {
        const auto& in = toml::find(data, "Input");
        if (in.contains("backgroundControllerInput"))
            ui->backgroundControllerCheckBox->setChecked(
                toml::find<bool>(in, "backgroundControllerInput"));
        if (in.contains("cursorHideTimeout"))
            ui->idleTimeoutSpinBox->setValue(toml::find<int>(in, "cursorHideTimeout"));
        if (in.contains("cursorState"))
            ui->hideCursorComboBox->setCurrentIndex(toml::find<int>(in, "cursorState"));
        if (in.contains("isMotionControlsEnabled"))
            ui->motionControlsCheckBox->setChecked(toml::find<bool>(in, "isMotionControlsEnabled"));
        if (in.contains("specialPadClass"))
            ui->specialPadClassSpinBox->setValue(toml::find<int>(in, "specialPadClass"));
        if (in.contains("useSpecialPad"))
            ui->useSpecialPadCheckBox->setChecked(toml::find<bool>(in, "useSpecialPad"));
        if (in.contains("useUnifiedInputConfig"))
            ui->useUnifiedInputConfigCheckBox->setChecked(
                toml::find<bool>(in, "useUnifiedInputConfig"));
    }

    // GPU
    if (data.contains("GPU")) {
        const auto& gpu = toml::find(data, "GPU");
        if (gpu.contains("allowHDR"))
            ui->enableHDRCheckBox->setChecked(toml::find<bool>(gpu, "allowHDR"));
        if (gpu.contains("copyGPUBuffers"))
            ui->copyGPUBuffersCheckBox->setChecked(toml::find<bool>(gpu, "copyGPUBuffers"));
        if (gpu.contains("directMemoryAccess"))
            ui->DMACheckBox->setChecked(toml::find<bool>(gpu, "directMemoryAccess"));
        if (gpu.contains("dumpShaders"))
            ui->dumpShadersCheckBox->setChecked(toml::find<bool>(gpu, "dumpShaders"));
        if (gpu.contains("fpsLimit"))
            ui->fpsSpinBox->setValue(toml::find<int>(gpu, "fpsLimit"));
        if (gpu.contains("fpsLimiterEnabled"))
            ui->fpsLimiterCheckBox->setChecked(toml::find<bool>(gpu, "fpsLimiterEnabled"));
        if (gpu.contains("fsrEnabled"))
            ui->FSRCheckBox->setChecked(toml::find<bool>(gpu, "fsrEnabled"));
        if (gpu.contains("screenMode"))
            ui->displayModeComboBox->setCurrentText(
                QString::fromStdString(toml::find<std::string>(gpu, "screenMode")));

        if (gpu.contains("isFullscreen"))
            Config::setIsFullscreen(toml::find<bool>(gpu, "isFullscreen"));

        if (gpu.contains("memoryAlloc"))
            ui->MemoryComboBox->setCurrentText(
                QString::fromStdString(toml::find<std::string>(gpu, "memoryAlloc")));
        if (gpu.contains("presentMode")) {
            std::string present = toml::find<std::string>(gpu, "presentMode");
            QMap<QString, QString> presentModeMap = {
                {tr("Mailbox (Vsync)"), "Mailbox"},
                {tr("Fifo (Vsync)"), "Fifo"},
                {tr("Immediate (No Vsync)"), "Immediate"},
            };
            const auto presentText =
                presentModeMap.key(QString::fromStdString(present), tr("Fifo (Vsync)"));
            ui->presentModeComboBox->setCurrentText(presentText);
        }

        if (gpu.contains("rcas_attenuation"))
            ui->RCASSpinBox->setValue(toml::find<double>(gpu, "rcas_attenuation"));
        if (gpu.contains("rcasEnabled"))
            ui->RCASCheckBox->setChecked(toml::find<bool>(gpu, "rcasEnabled"));
        if (gpu.contains("readbackLinearImages"))
            ui->ReadbacksLinearCheckBox->setChecked(toml::find<bool>(gpu, "readbackLinearImages"));
        if (gpu.contains("readbackSpeed"))
            ui->ReadbackSpeedComboBox->setCurrentIndex(toml::find<int>(gpu, "readbackSpeed"));
        if (gpu.contains("screenWidth"))
            ui->widthSpinBox->setValue(toml::find<int>(gpu, "screenWidth"));
        if (gpu.contains("screenHeight"))
            ui->heightSpinBox->setValue(toml::find<int>(gpu, "screenHeight"));
        if (gpu.contains("shaderSkipsEnabled"))
            ui->SkipsCheckBox->setChecked(toml::find<bool>(gpu, "shaderSkipsEnabled"));
        if (gpu.contains("vblankFrequency"))
            ui->vblankSpinBox->setValue(toml::find<int>(gpu, "vblankFrequency"));
    }

    // Vulkan
    if (data.contains("Vulkan")) {
        const auto& vk = toml::find(data, "Vulkan");
        if (vk.contains("crashDiagnostic"))
            ui->crashDiagnosticsCheckBox->setChecked(toml::find<bool>(vk, "crashDiagnostic"));
        if (vk.contains("guestMarkers"))
            ui->guestMarkersCheckBox->setChecked(toml::find<bool>(vk, "guestMarkers"));
        if (vk.contains("hostMarkers"))
            ui->hostMarkersCheckBox->setChecked(toml::find<bool>(vk, "hostMarkers"));
        if (vk.contains("rdocEnable"))
            ui->rdocCheckBox->setChecked(toml::find<bool>(vk, "rdocEnable"));
        if (vk.contains("validation"))
            ui->vkValidationCheckBox->setChecked(toml::find<bool>(vk, "validation"));
        if (vk.contains("validation_sync"))
            ui->vkSyncValidationCheckBox->setChecked(toml::find<bool>(vk, "validation_sync"));
    }

    // Debug
    if (data.contains("Debug")) {
        const auto& dbg = toml::find(data, "Debug");
        if (dbg.contains("CollectShader"))
            ui->collectShaderCheckBox->setChecked(toml::find<bool>(dbg, "CollectShader"));
        if (dbg.contains("DebugDump"))
            ui->debugDump->setChecked(toml::find<bool>(dbg, "DebugDump"));
        if (dbg.contains("logEnabled"))
            ui->enableLoggingCheckBox->setChecked(toml::find<bool>(dbg, "logEnabled"));
    }
}

void GameSpecificDialog::UpdateSettings() {
    toml::value overrides = toml::table{};

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

    //  General
    if (ui->enableAutoBackupCheckBox->isChecked() != Config::getEnableAutoBackup())
        overrides["General"]["enableAutoBackup"] = ui->enableAutoBackupCheckBox->isChecked();

    if (ui->discordRPCCheckbox->isChecked() != Config::getEnableDiscordRPC())
        overrides["General"]["enableDiscordRPC"] = ui->discordRPCCheckbox->isChecked();

    if (ui->horizontalVolumeSlider->value() != Config::getVolumeSlider())
        overrides["General"]["volumeSlider"] = ui->horizontalVolumeSlider->value();

    if (ui->connectedNetworkCheckBox->isChecked() != Config::getIsConnectedToNetwork())
        overrides["General"]["isConnectedToNetwork"] = ui->connectedNetworkCheckBox->isChecked();

    if (ui->isDevKitCheckBox->isChecked() != Config::isDevKitConsole())
        overrides["General"]["isDevKit"] = ui->isDevKitCheckBox->isChecked();

    if (ui->isNeoModeCheckBox->isChecked() != Config::isNeoModeConsole())
        overrides["General"]["isPS4Pro"] = ui->isNeoModeCheckBox->isChecked();

    if (ui->isPSNSignedInCheckBox->isChecked() != Config::getPSNSignedIn())
        overrides["General"]["isPSNSignedIn"] = ui->isPSNSignedInCheckBox->isChecked();

    if (ui->disableTrophycheckBox->isChecked() != Config::getisTrophyPopupDisabled())
        overrides["General"]["isTrophyPopupDisabled"] = ui->disableTrophycheckBox->isChecked();

    if (ui->logFilterLineEdit->text().toStdString() != Config::getLogFilter())
        overrides["General"]["logFilter"] = ui->logFilterLineEdit->text().toStdString();

    if (logTypeMap.value(ui->logTypeComboBox->currentText()).toStdString() != Config::getLogType())
        overrides["General"]["logType"] =
            logTypeMap.value(ui->logTypeComboBox->currentText()).toStdString();

    if (ui->screenTipBox->isChecked() != Config::getScreenTipDisable())
        overrides["General"]["screenTipDisable"] = ui->screenTipBox->isChecked();

    if (ui->showSplashCheckBox->isChecked() != Config::showSplash())
        overrides["General"]["showSplash"] = ui->showSplashCheckBox->isChecked();

    // Trophy
    std::string side = Config::sideTrophy();
    if (ui->radioButton_Top->isChecked() && side != "top")
        overrides["General"]["sideTrophy"] = "top";
    else if (ui->radioButton_Left->isChecked() && side != "left")
        overrides["General"]["sideTrophy"] = "left";
    else if (ui->radioButton_Right->isChecked() && side != "right")
        overrides["General"]["sideTrophy"] = "right";
    else if (ui->radioButton_Bottom->isChecked() && side != "bottom")
        overrides["General"]["sideTrophy"] = "bottom";

    if (ui->popUpDurationSpinBox->value() != Config::getTrophyNotificationDuration())
        overrides["General"]["trophyNotificationDuration"] = ui->popUpDurationSpinBox->value();

    //  Input
    if (ui->backgroundControllerCheckBox->isChecked() != Config::getBackgroundControllerInput())
        overrides["Input"]["backgroundControllerInput"] =
            ui->backgroundControllerCheckBox->isChecked();

    if (ui->idleTimeoutSpinBox->value() != Config::getCursorHideTimeout())
        overrides["Input"]["cursorHideTimeout"] = ui->idleTimeoutSpinBox->value();

    if (ui->hideCursorComboBox->currentIndex() != Config::getCursorState())
        overrides["Input"]["cursorState"] = ui->hideCursorComboBox->currentIndex();

    if (ui->motionControlsCheckBox->isChecked() != Config::getIsMotionControlsEnabled())
        overrides["Input"]["isMotionControlsEnabled"] = ui->motionControlsCheckBox->isChecked();

    if (ui->specialPadClassSpinBox->value() != Config::getSpecialPadClass())
        overrides["Input"]["specialPadClass"] = ui->specialPadClassSpinBox->value();

    if (ui->useSpecialPadCheckBox->isChecked() != Config::getUseSpecialPad())
        overrides["Input"]["useSpecialPad"] = ui->useSpecialPadCheckBox->isChecked();

    if (ui->useUnifiedInputConfigCheckBox->isChecked() != Config::GetUseUnifiedInputConfig())
        overrides["Input"]["useUnifiedInputConfig"] =
            ui->useUnifiedInputConfigCheckBox->isChecked();

    //  GPU
    if (ui->enableHDRCheckBox->isChecked() != Config::allowHDR())
        overrides["GPU"]["allowHDR"] = ui->enableHDRCheckBox->isChecked();

    if (ui->copyGPUBuffersCheckBox->isChecked() != Config::copyGPUCmdBuffers())
        overrides["GPU"]["copyGPUBuffers"] = ui->copyGPUBuffersCheckBox->isChecked();

    if (ui->DMACheckBox->isChecked() != Config::directMemoryAccess())
        overrides["GPU"]["directMemoryAccess"] = ui->DMACheckBox->isChecked();

    if (ui->dumpShadersCheckBox->isChecked() != Config::dumpShaders())
        overrides["GPU"]["dumpShaders"] = ui->dumpShadersCheckBox->isChecked();

    if (ui->fpsSpinBox->value() != Config::getFpsLimit())
        overrides["GPU"]["fpsLimit"] = ui->fpsSpinBox->value();

    if (ui->fpsLimiterCheckBox->isChecked() != Config::fpsLimiterEnabled())
        overrides["GPU"]["fpsLimiterEnabled"] = ui->fpsLimiterCheckBox->isChecked();

    if (ui->FSRCheckBox->isChecked() != Config::getFsrEnabled())
        overrides["GPU"]["fsrEnabled"] = ui->FSRCheckBox->isChecked();

    {
        std::string screen =
            screenModeMap.value(ui->displayModeComboBox->currentText()).toStdString();

        if (screen != Config::getFullscreenMode())
            overrides["GPU"]["screenMode"] = screen;

        bool shouldFullscreen = (screen != "Windowed");
        if (shouldFullscreen != Config::getIsFullscreen())
            overrides["GPU"]["isFullscreen"] = shouldFullscreen;
    }

    if (ui->MemoryComboBox->currentText().toStdString() != Config::getMemoryAlloc())
        overrides["GPU"]["memoryAlloc"] = ui->MemoryComboBox->currentText().toStdString();

    {
        std::string present =
            presentModeMap.value(ui->presentModeComboBox->currentText()).toStdString();
        if (present != Config::getPresentMode())
            overrides["GPU"]["presentMode"] = present;
    }

    if (ui->RCASSpinBox->value() != Config::getRcasAttenuation())
        overrides["GPU"]["rcas_attenuation"] = ui->RCASSpinBox->value();

    if (ui->RCASCheckBox->isChecked() != Config::getRcasEnabled())
        overrides["GPU"]["rcasEnabled"] = ui->RCASCheckBox->isChecked();

    if (ui->ReadbacksLinearCheckBox->isChecked() != Config::getReadbackLinearImages())
        overrides["GPU"]["readbackLinearImages"] = ui->ReadbacksLinearCheckBox->isChecked();

    if (static_cast<Config::ReadbackSpeed>(ui->ReadbackSpeedComboBox->currentIndex()) !=
        Config::readbackSpeed()) {
        overrides["GPU"]["readbackSpeed"] = ui->ReadbackSpeedComboBox->currentIndex();
    }

    if (ui->widthSpinBox->value() != Config::getScreenWidth())
        overrides["GPU"]["screenWidth"] = ui->widthSpinBox->value();

    if (ui->heightSpinBox->value() != Config::getScreenHeight())
        overrides["GPU"]["screenHeight"] = ui->heightSpinBox->value();

    if (ui->SkipsCheckBox->isChecked() != Config::getShaderSkipsEnabled())
        overrides["GPU"]["shaderSkipsEnabled"] = ui->SkipsCheckBox->isChecked();

    if (ui->vblankSpinBox->value() != Config::vblankFreq())
        overrides["GPU"]["vblankFrequency"] = ui->vblankSpinBox->value();

    // Vulkan
    if (ui->crashDiagnosticsCheckBox->isChecked() != Config::getVkCrashDiagnosticEnabled())
        overrides["Vulkan"]["crashDiagnostic"] = ui->crashDiagnosticsCheckBox->isChecked();

    if (ui->guestMarkersCheckBox->isChecked() != Config::getVkGuestMarkersEnabled())
        overrides["Vulkan"]["guestMarkers"] = ui->guestMarkersCheckBox->isChecked();

    if (ui->hostMarkersCheckBox->isChecked() != Config::getVkHostMarkersEnabled())
        overrides["Vulkan"]["hostMarkers"] = ui->hostMarkersCheckBox->isChecked();

    if (ui->rdocCheckBox->isChecked() != Config::isRdocEnabled())
        overrides["Vulkan"]["rdocEnable"] = ui->rdocCheckBox->isChecked();

    if (ui->vkValidationCheckBox->isChecked() != Config::vkValidationEnabled())
        overrides["Vulkan"]["validation"] = ui->vkValidationCheckBox->isChecked();

    if (ui->vkSyncValidationCheckBox->isChecked() != Config::vkValidationSyncEnabled())
        overrides["Vulkan"]["validation_sync"] = ui->vkSyncValidationCheckBox->isChecked();

    // Debug
    if (ui->collectShaderCheckBox->isChecked() != Config::collectShadersForDebug())
        overrides["Debug"]["CollectShader"] = ui->collectShaderCheckBox->isChecked();

    if (ui->debugDump->isChecked() != Config::debugDump())
        overrides["Debug"]["DebugDump"] = ui->debugDump->isChecked();

    if (ui->enableLoggingCheckBox->isChecked() != Config::getLoggingEnabled())
        overrides["Debug"]["logEnabled"] = ui->enableLoggingCheckBox->isChecked();

    if (overrides.as_table().empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(m_config_path.parent_path(), ec);

    std::ofstream ofs(m_config_path, std::ios::trunc);
    if (!ofs) {
        qWarning() << "Failed to open per-game config for writing:"
                   << QString::fromStdString(m_config_path.string());
        return;
    }
    ofs << overrides;
}
