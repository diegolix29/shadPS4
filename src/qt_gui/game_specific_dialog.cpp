// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fstream>
#include <QDesktopServices>
#include <QDialogButtonBox>

#include <toml.hpp>
#include "common/config.h"
#include "common/logging/log.h"

#include "core/file_sys/fs.h"
#include "core/libraries/audio/audioout.h"
#include "log_presets_dialog.h"
#include "sdl_event_wrapper.h"

#include "common/path_util.h"
#include "game_specific_dialog.h"
#include "ui_game_specific_dialog.h"

#include <SDL3/SDL.h>

GameSpecificDialog::GameSpecificDialog(std::shared_ptr<CompatibilityInfoClass> compat_info,
                                       std::shared_ptr<IpcClient> ipc_client, QWidget* parent,
                                       const std::string& serial, bool is_running,
                                       std::string gsc_serial)
    : QDialog(parent), ui(new Ui::GameSpecificDialog()), m_compat_info(std::move(compat_info)),
      m_ipc_client(ipc_client), m_serial(serial), m_lastPadIndex(0) {
    ui->setupUi(this);

    connect(ui->fpsSlider, &QSlider::valueChanged, ui->fpsSpinBox, &QSpinBox::setValue);

    connect(ui->fpsSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), ui->fpsSlider,
            &QSlider::setValue);

    m_config_path = std::filesystem::path(GetUserPath(Common::FS::PathType::CustomConfigs)) /
                    (serial + ".toml");

    specialPadChecks[0][0] = ui->spc1_p1;
    specialPadChecks[0][1] = ui->spc1_p2;
    specialPadChecks[0][2] = ui->spc1_p3;
    specialPadChecks[0][3] = ui->spc1_p4;

    specialPadChecks[1][0] = ui->spc2_p1;
    specialPadChecks[1][1] = ui->spc2_p2;
    specialPadChecks[1][2] = ui->spc2_p3;
    specialPadChecks[1][3] = ui->spc2_p4;

    specialPadChecks[2][0] = ui->spc3_p1;
    specialPadChecks[2][1] = ui->spc3_p2;
    specialPadChecks[2][2] = ui->spc3_p3;
    specialPadChecks[2][3] = ui->spc3_p4;

    specialPadChecks[3][0] = ui->spc4_p1;
    specialPadChecks[3][1] = ui->spc4_p2;
    specialPadChecks[3][2] = ui->spc4_p3;
    specialPadChecks[3][3] = ui->spc4_p4;

    PopulateAudioDevices();
    LoadValuesFromConfig();

    connect(ui->buttonBox, &QDialogButtonBox::accepted, this, [this]() {
        UpdateSettings();
        accept();
    });

    connect(ui->RCASSlider, &QSlider::valueChanged, this,
            &GameSpecificDialog::OnRcasAttenuationChanged);

    connect(ui->RCASSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            &GameSpecificDialog::OnRcasAttenuationSpinBoxChanged);

    connect(ui->logPresetsButton, &QPushButton::clicked, this, [this]() {
        LogPresetsDialog dlg(m_compat_info, this);
        connect(&dlg, &LogPresetsDialog::PresetChosen, this,
                [this](const QString& filter) { ui->logFilterLineEdit->setText(filter); });
        dlg.exec();
    });

    connect(ui->buttonBox, &QDialogButtonBox::rejected, this, &QWidget::close);

    ui->ReadbackSpeedComboBox->installEventFilter(this);
    ui->MemorySpinBox->installEventFilter(this);
}
static QMap<QString, QString> screenModeMap = {
    {"Fullscreen (Borderless)", "Fullscreen (Borderless)"},
    {"Windowed", "Windowed"},
    {"Fullscreen", "Fullscreen"},
};
GameSpecificDialog::~GameSpecificDialog() = default;

bool GameSpecificDialog::eventFilter(QObject* obj, QEvent* event) {
    if ((obj == ui->ReadbackSpeedComboBox || obj == ui->MemorySpinBox) &&
        event->type() == QEvent::Wheel) {
        return true;
    }
    return QDialog::eventFilter(obj, event);
}

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

void GameSpecificDialog::OnRcasAttenuationChanged(int sliderValue) {
    ui->RCASSpinBox->blockSignals(true);
    ui->RCASSpinBox->setValue(sliderValue / 1000.0);
    ui->RCASSpinBox->blockSignals(false);

    Config::setRcasAttenuation(static_cast<float>(sliderValue));
}

void GameSpecificDialog::OnRcasAttenuationSpinBoxChanged(double spinValue) {
    int intValue = static_cast<int>(std::lround(spinValue * 1000.0));

    ui->RCASSlider->blockSignals(true);
    ui->RCASSlider->setValue(intValue);
    ui->RCASSlider->blockSignals(false);

    Config::setRcasAttenuation(static_cast<float>(intValue));
}

void GameSpecificDialog::LoadValuesFromConfig() {

    ui->enableAutoBackupCheckBox->setChecked(Config::getEnableAutoBackup());
    ui->HotkeysCheckBox->setChecked(Config::DisableHardcodedHotkeys());
    ui->HomeHotkeysCheckBox->setChecked(Config::UseHomeButtonForHotkeys());
    ui->discordRPCCheckbox->setChecked(Config::getEnableDiscordRPC());

    connect(ui->horizontalVolumeSlider, &QSlider::valueChanged, this, [this](int value) {
        VolumeSliderChange(value);

        if (Config::getGameRunning())
            m_ipc_client->adjustVol(value);
    });
    ui->horizontalVolumeSlider->blockSignals(true);
    ui->horizontalVolumeSlider->setValue(Config::getVolumeSlider());
    ui->horizontalVolumeSlider->blockSignals(false);
    VolumeSliderChange(Config::getVolumeSlider());

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
    int idx = Config::getCursorState();
    if (idx >= 0 && idx < ui->hideCursorComboBox->count())
        ui->hideCursorComboBox->setCurrentIndex(idx);
    else
        ui->hideCursorComboBox->setCurrentIndex(0);
    ui->motionControlsCheckBox->setChecked(Config::getIsMotionControlsEnabled());
    ui->useUnifiedInputConfigCheckBox->setChecked(Config::GetUseUnifiedInputConfig());

    ui->enableHDRCheckBox->setChecked(Config::allowHDR());
    ui->copyGPUBuffersCheckBox->setChecked(Config::copyGPUCmdBuffers());
    ui->DMACheckBox->setChecked(Config::directMemoryAccess());
    ui->dumpShadersCheckBox->setChecked(Config::dumpShaders());
    ui->fpsSpinBox->setValue(Config::getFpsLimit());
    ui->fpsLimiterCheckBox->setChecked(Config::isFpsLimiterEnabled());
    ui->FSRCheckBox->setChecked(Config::getFsrEnabled());
    ui->displayModeComboBox->setCurrentText(QString::fromStdString(Config::getFullscreenMode()));
    ui->MemorySpinBox->setValue(Config::getExtraDmemInMbytes());
    ui->presentModeComboBox->setCurrentText(QString::fromStdString(Config::getPresentMode()));
    ui->RCASSlider->setMinimum(0);
    ui->RCASSlider->setMaximum(3000);
    ui->RCASSlider->setValue(Config::getRcasAttenuation());
    ui->RCASSpinBox->setValue(ui->RCASSlider->value() / 1000.0);
    ui->RCASCheckBox->setChecked(Config::getRcasEnabled());
    ui->ReadbacksLinearCheckBox->setChecked(Config::getReadbackLinearImages());
    ui->ReadbackSpeedComboBox->setCurrentIndex(static_cast<int>(Config::readbackSpeed()));
    ui->widthSpinBox->setValue(Config::getWindowWidth());
    ui->heightSpinBox->setValue(Config::getWindowHeight());
    ui->SkipsCheckBox->setChecked(Config::getShaderSkipsEnabled());
    ui->vblankSpinBox->setValue(Config::vblankFreq());

    ui->crashDiagnosticsCheckBox->setChecked(Config::getVkCrashDiagnosticEnabled());
    ui->guestMarkersCheckBox->setChecked(Config::getVkGuestMarkersEnabled());
    ui->hostMarkersCheckBox->setChecked(Config::getVkHostMarkersEnabled());
    ui->rdocCheckBox->setChecked(Config::isRdocEnabled());
    ui->cacheCheckBox->setChecked(Config::isPipelineCacheEnabled());
    ui->cacheArchiveCheckBox->setChecked(Config::isPipelineCacheArchived());
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
    } catch (const std::exception& e) {
        qWarning() << "Failed to parse config:" << QString::fromStdString(e.what());
        return;
    }

    QMap<QString, QString> presentModeMap = {
        {"Mailbox", tr("Mailbox (Vsync)")},
        {"Fifo", tr("Fifo (Vsync)")},
        {"Immediate", tr("Immediate (No Vsync)")},
    };
    ui->mainOutputDeviceComboBox->setCurrentText(QString::fromStdString(
        toml::find_or<std::string>(data, "General", "mainOutputDevice", "")));
    ui->padSpkOutputDeviceComboBox->setCurrentText(QString::fromStdString(
        toml::find_or<std::string>(data, "General", "padSpkOutputDevice", "")));

    // General
    if (data.contains("General")) {
        const auto& gen = toml::find(data, "General");
        if (gen.contains("enableAutoBackup"))
            ui->enableAutoBackupCheckBox->setChecked(toml::find<bool>(gen, "enableAutoBackup"));
        if (gen.contains("DisableHardcodedHotkeys"))
            ui->HotkeysCheckBox->setChecked(toml::find<bool>(gen, "DisableHardcodedHotkeys"));
        if (gen.contains("UseHomeButtonForHotkeys"))
            ui->HomeHotkeysCheckBox->setChecked(toml::find<bool>(gen, "UseHomeButtonForHotkeys"));
        if (gen.contains("enableDiscordRPC"))
            ui->discordRPCCheckbox->setChecked(toml::find<bool>(gen, "enableDiscordRPC"));
        if (gen.contains("volumeSlider")) {
            int vol = toml::find<int>(gen, "volumeSlider");
            ui->horizontalVolumeSlider->setValue(vol);
            ui->volumeText->setText(QString::number(vol) + "%");
        }
        if (gen.contains("extraDmemInMbytes"))
            ui->MemorySpinBox->setValue(toml::find<int>(gen, "extraDmemInMbytes"));
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
        if (gen.contains("trophyNotificationDuration")) {
            int duration = 0;
            try {
                duration = toml::find<int>(gen, "trophyNotificationDuration");
            } catch (...) {
                duration = static_cast<int>(toml::find<double>(gen, "trophyNotificationDuration"));
            }

            ui->popUpDurationSpinBox->setValue(duration);
        }
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
        for (int p = 1; p <= 4; ++p) {
            std::string classKey = fmt::format("specialPadClass{}", p);
            std::string useKey = fmt::format("useSpecialPad{}", p);

            if (in.contains(classKey))
                Config::setSpecialPadClass(p, toml::find<int>(in, classKey), true);

            if (in.contains(useKey))
                Config::setUseSpecialPad(p, toml::find<bool>(in, useKey), true);
        }
        for (int p = 1; p <= 4; ++p) {
            int cls = Config::getSpecialPadClass(p);
            bool use = Config::getUseSpecialPad(p);

            for (int c = 1; c <= 4; ++c) {
                specialPadChecks[c - 1][p - 1]->setChecked(false);
            }

            if (use && cls >= 1 && cls <= 4) {
                specialPadChecks[cls - 1][p - 1]->setChecked(true);
            }
        }
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
        if (gpu.contains("FullscreenMode"))
            ui->displayModeComboBox->setCurrentText(
                QString::fromStdString(toml::find<std::string>(gpu, "FullscreenMode")));

        if (gpu.contains("Fullscreen"))
            Config::setIsFullscreen(toml::find<bool>(gpu, "Fullscreen"));

        if (gpu.contains("presentMode")) {
            std::string present = toml::find<std::string>(gpu, "presentMode");
            ui->presentModeComboBox->setCurrentText(
                presentModeMap.value(QString::fromStdString(present)));
        }

        if (gpu.contains("rcas_attenuation")) {
            double value = 0.0;
            try {
                value = toml::find<double>(gpu, "rcas_attenuation");
            } catch (...) {
                value = static_cast<double>(toml::find<int>(gpu, "rcas_attenuation"));
            }
            ui->RCASSlider->setValue(static_cast<int>(std::lround(value)));
            ui->RCASSpinBox->setValue(value / 1000.0);
            Config::setRcasAttenuation(static_cast<int>(value));
        }

        if (gpu.contains("rcasEnabled"))
            ui->RCASCheckBox->setChecked(toml::find<bool>(gpu, "rcasEnabled"));
        if (gpu.contains("readbackLinearImages"))
            ui->ReadbacksLinearCheckBox->setChecked(toml::find<bool>(gpu, "readbackLinearImages"));
        if (gpu.contains("readbackSpeedMode"))
            ui->ReadbackSpeedComboBox->setCurrentIndex(toml::find<int>(gpu, "readbackSpeedMode"));
        if (gpu.contains("windowWidth"))
            ui->widthSpinBox->setValue(toml::find<int>(gpu, "windowWidth"));
        if (gpu.contains("windowHeight"))
            ui->heightSpinBox->setValue(toml::find<int>(gpu, "windowHeight"));
        if (gpu.contains("shaderSkipsEnabled"))
            ui->SkipsCheckBox->setChecked(toml::find<bool>(gpu, "shaderSkipsEnabled"));
        if (gpu.contains("vblankFrequency"))
            ui->vblankSpinBox->setValue(toml::find<int>(gpu, "vblankFrequency"));
    }

    // Audio
    if (data.contains("Audio")) {
        const auto& audio = toml::find(data, "Audio");
        if (audio.contains("mainOutputDevice"))
            ui->mainOutputDeviceComboBox->setCurrentText(
                QString::fromStdString(toml::find<std::string>(audio, "mainOutputDevice")));
        else
            ui->mainOutputDeviceComboBox->setCurrentText(
                QString::fromStdString(Config::getMainOutputDevice()));

        if (audio.contains("padSpkOutputDevice"))
            ui->padSpkOutputDeviceComboBox->setCurrentText(
                QString::fromStdString(toml::find<std::string>(audio, "padSpkOutputDevice")));
        else
            ui->padSpkOutputDeviceComboBox->setCurrentText(
                QString::fromStdString(Config::getPadSpkOutputDevice()));
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
        if (vk.contains("pipelineCacheEnable"))
            ui->cacheCheckBox->setChecked(toml::find<bool>(vk, "pipelineCacheEnable"));
        if (vk.contains("pipelineCacheArchive"))
            ui->cacheArchiveCheckBox->setChecked(toml::find<bool>(vk, "pipelineCacheArchive"));
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
        {"Mailbox", tr("Mailbox (Vsync)")},
        {"Fifo", tr("Fifo (Vsync)")},
        {"Immediate", tr("Immediate (No Vsync)")},
    };

    //  General
    if (ui->enableAutoBackupCheckBox->isChecked() != Config::getEnableAutoBackup())
        overrides["General"]["enableAutoBackup"] = ui->enableAutoBackupCheckBox->isChecked();

    if (ui->discordRPCCheckbox->isChecked() != Config::getEnableDiscordRPC())
        overrides["General"]["enableDiscordRPC"] = ui->discordRPCCheckbox->isChecked();

    if (ui->HotkeysCheckBox->isChecked() != Config::DisableHardcodedHotkeys())
        overrides["General"]["DisableHardcodedHotkeys"] = ui->HotkeysCheckBox->isChecked();

    if (ui->HomeHotkeysCheckBox->isChecked() != Config::UseHomeButtonForHotkeys())
        overrides["General"]["UseHomeButtonForHotkeys"] = ui->HomeHotkeysCheckBox->isChecked();

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

    if (ui->MemorySpinBox->value() != Config::getExtraDmemInMbytes())
        overrides["General"]["extraDmemInMbytes"] = ui->MemorySpinBox->value();

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

    for (int p = 1; p <= 4; ++p) {
        int assignedClass = 0;

        // Check each class row for this pad
        for (int c = 1; c <= 4; ++c) {
            if (specialPadChecks[c - 1][p - 1]->isChecked()) {
                assignedClass = c;
                break;
            }
        }

        std::string classKey = fmt::format("specialPadClass{}", p);
        std::string useKey = fmt::format("useSpecialPad{}", p);

        bool use = assignedClass != 0;

        // Save differences only
        if (assignedClass != Config::getSpecialPadClass(p))
            overrides["Input"][classKey] = assignedClass;

        if (use != Config::getUseSpecialPad(p))
            overrides["Input"][useKey] = use;
    }

    if (ui->useUnifiedInputConfigCheckBox->isChecked() != Config::GetUseUnifiedInputConfig())
        overrides["Input"]["useUnifiedInputConfig"] =
            ui->useUnifiedInputConfigCheckBox->isChecked();

    // Audio
    if (ui->mainOutputDeviceComboBox->currentText().toStdString() != Config::getMainOutputDevice())
        overrides["Audio"]["mainOutputDevice"] =
            ui->mainOutputDeviceComboBox->currentText().toStdString();

    if (ui->padSpkOutputDeviceComboBox->currentText().toStdString() !=
        Config::getPadSpkOutputDevice())
        overrides["Audio"]["padSpkOutputDevice"] =
            ui->padSpkOutputDeviceComboBox->currentText().toStdString();

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

    if (ui->fpsLimiterCheckBox->isChecked() != Config::isFpsLimiterEnabled())
        overrides["GPU"]["fpsLimiterEnabled"] = ui->fpsLimiterCheckBox->isChecked();

    if (ui->FSRCheckBox->isChecked() != Config::getFsrEnabled())
        overrides["GPU"]["fsrEnabled"] = ui->FSRCheckBox->isChecked();

    {
        QString comboText = ui->displayModeComboBox->currentText();
        std::string screen = screenModeMap.value(comboText, "Windowed").toStdString();

        if (screen != Config::getFullscreenMode())
            overrides["GPU"]["FullscreenMode"] = screen;

        bool shouldFullscreen = (screen != "Windowed");
        if (shouldFullscreen != Config::getIsFullscreen())
            overrides["GPU"]["Fullscreen"] = (screen != "Windowed");
    }

    QString key = presentModeMap.key(ui->presentModeComboBox->currentText(), "Fifo");
    if (key.toStdString() != Config::getPresentMode())
        overrides["GPU"]["presentMode"] = key.toStdString();

    {
        int current = static_cast<int>(Config::getRcasAttenuation());
        int newVal = ui->RCASSlider->value();

        if (newVal != current)
            overrides["GPU"]["rcas_attenuation"] = static_cast<double>(newVal);
    }

    if (ui->RCASCheckBox->isChecked() != Config::getRcasEnabled())
        overrides["GPU"]["rcasEnabled"] = ui->RCASCheckBox->isChecked();

    if (ui->ReadbacksLinearCheckBox->isChecked() != Config::getReadbackLinearImages())
        overrides["GPU"]["readbackLinearImages"] = ui->ReadbacksLinearCheckBox->isChecked();

    if (static_cast<Config::ReadbackSpeed>(ui->ReadbackSpeedComboBox->currentIndex()) !=
        Config::readbackSpeed()) {
        overrides["GPU"]["readbackSpeedMode"] = ui->ReadbackSpeedComboBox->currentIndex();
    }

    if (ui->widthSpinBox->value() != Config::getWindowWidth())
        overrides["GPU"]["windowWidth"] = ui->widthSpinBox->value();

    if (ui->heightSpinBox->value() != Config::getWindowHeight())
        overrides["GPU"]["windowHeight"] = ui->heightSpinBox->value();

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

    if (ui->cacheCheckBox->isChecked() != Config::isPipelineCacheEnabled())
        overrides["Vulkan"]["pipelineCacheEnable"] = ui->cacheCheckBox->isChecked();

    if (ui->cacheArchiveCheckBox->isChecked() != Config::isPipelineCacheArchived())
        overrides["Vulkan"]["pipelineCacheArchive"] = ui->cacheArchiveCheckBox->isChecked();

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

    std::error_code ec;
    if (overrides.as_table().empty()) {
        std::filesystem::remove(m_config_path, ec);
        return;
    }

    std::filesystem::create_directories(m_config_path.parent_path(), ec);

    std::filesystem::path tmp = m_config_path;
    tmp += ".tmp";

    {
        std::ofstream ofs(tmp, std::ios::trunc);
        if (!ofs || !(ofs << overrides)) {
            qWarning() << "Failed to write per-game config to temp file:"
                       << QString::fromStdString(tmp.string());
            std::filesystem::remove(tmp, ec);
            return;
        }
    }

    std::filesystem::rename(tmp, m_config_path, ec);
    if (ec) {
        qWarning() << "Failed to move temp config into place:"
                   << QString::fromStdString(ec.message());
        std::filesystem::remove(tmp, ec);
    }
}

void GameSpecificDialog::PopulateAudioDevices() {
    ui->mainOutputDeviceComboBox->clear();
    ui->padSpkOutputDeviceComboBox->clear();

    // Add default option
    ui->mainOutputDeviceComboBox->addItem(tr("Default Device"));
    ui->padSpkOutputDeviceComboBox->addItem(tr("Default Device"));

    // Enumerate playback devices (speakers) with SDL3
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    int count = 0;
    SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&count);
    if (devices) {
        for (int i = 0; i < count; ++i) {
            SDL_AudioDeviceID devId = devices[i];
            const char* name = SDL_GetAudioDeviceName(devId);
            if (name) {
                QString qname = QString::fromUtf8(name);
                ui->mainOutputDeviceComboBox->addItem(qname);
                ui->padSpkOutputDeviceComboBox->addItem(qname);
            }
        }
        SDL_free(devices);
    }
}
