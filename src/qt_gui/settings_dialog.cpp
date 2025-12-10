// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <vector>
#include <QCompleter>
#include <QDirIterator>
#include <QFileDialog>
#include <QHoverEvent>
#include <QMessageBox>
#include <SDL3/SDL.h>
#ifdef _WIN32
#include <Shlobj.h>
#include <windows.h>
#endif
#include <fmt/format.h>

#include "common/config.h"
#include "common/logging/log.h"
#include "common/scm_rev.h"
#include "core/libraries/audio/audioout.h"
#include "qt_gui/compatibility_info.h"
#ifdef ENABLE_DISCORD_RPC
#include "common/discord_rpc_handler.h"
#include "common/singleton.h"
#endif
#ifdef ENABLE_UPDATER
#include "check_update.h"
#endif
#include <QDesktopServices>
#include <toml.hpp>
#include "background_music_player.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "log_presets_dialog.h"
#include "sdl_event_wrapper.h"
#include "settings_dialog.h"
#include "ui_settings_dialog.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_presenter.h"

extern std::unique_ptr<Vulkan::Presenter> presenter;

QStringList languageNames = {"Arabic",
                             "Czech",
                             "Danish",
                             "Dutch",
                             "English (United Kingdom)",
                             "English (United States)",
                             "Finnish",
                             "French (Canada)",
                             "French (France)",
                             "German",
                             "Greek",
                             "Hungarian",
                             "Indonesian",
                             "Italian",
                             "Japanese",
                             "Korean",
                             "Norwegian (Bokmaal)",
                             "Polish",
                             "Portuguese (Brazil)",
                             "Portuguese (Portugal)",
                             "Romanian",
                             "Russian",
                             "Simplified Chinese",
                             "Spanish (Latin America)",
                             "Spanish (Spain)",
                             "Swedish",
                             "Thai",
                             "Traditional Chinese",
                             "Turkish",
                             "Ukrainian",
                             "Vietnamese"};

const QVector<int> languageIndexes = {21, 23, 14, 6, 18, 1, 12, 22, 2, 4,  25, 24, 29, 5,  0, 9,
                                      15, 16, 17, 7, 26, 8, 11, 20, 3, 13, 27, 10, 19, 30, 28};
QMap<QString, QString> channelMap;
QMap<QString, QString> logTypeMap;
QMap<QString, QString> screenModeMap;
QMap<QString, QString> presentModeMap;
QMap<QString, QString> chooseHomeTabMap;
QMap<QString, QString> micMap;

int backgroundImageOpacitySlider_backup;
int bgm_volume_backup;
int volume_slider_backup;
int fps_backup;

static std::vector<QString> m_physical_devices;

SettingsDialog::SettingsDialog(std::shared_ptr<CompatibilityInfoClass> m_compat_info,
                               std::shared_ptr<IpcClient> ipc_client, QWidget* parent,
                               bool is_running, bool is_specific, std::string gsc_serial)
    : QDialog(parent), ui(new Ui::SettingsDialog), compat_info(std::move(m_compat_info)),
      m_ipc_client(std::move(ipc_client)), is_game_running(is_running),
      gs_serial(std::move(gsc_serial)) {
    ui->setupUi(this);
    ui->tabWidgetSettings->setUsesScrollButtons(false);

    initialHeight = this->height();
    const auto config_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);

    ui->buttonBox->button(QDialogButtonBox::StandardButton::Close)->setFocus();

    // Connect signals and slots
    channelMap = {{tr("Revert"), "Revert"}, {tr("BBFork"), "BBFork"}};
    logTypeMap = {{tr("async"), "async"}, {tr("sync"), "sync"}};
    screenModeMap = {{tr("Fullscreen (Borderless)"), "Fullscreen (Borderless)"},
                     {tr("Windowed"), "Windowed"},
                     {tr("Fullscreen"), "Fullscreen"}};
    presentModeMap = {{tr("Mailbox (Vsync)"), "Mailbox"},
                      {tr("Fifo (Vsync)"), "Fifo"},
                      {tr("Immediate (No Vsync)"), "Immediate"}};
    chooseHomeTabMap = {{tr("General"), "General"},   {tr("GUI"), "GUI"},
                        {tr("Graphics"), "Graphics"}, {tr("User"), "User"},
                        {tr("Input"), "Input"},       {tr("Paths"), "Paths"},
                        {tr("Log"), "Log"},           {tr("Debug"), "Debug"}};
    micMap = {{tr("None"), "None"}, {tr("Default Device"), "Default Device"}};

    if (m_physical_devices.empty()) {
        // Populate cache of physical devices.
        Vulkan::Instance instance(false, false);
        auto physical_devices = instance.GetPhysicalDevices();
        for (const vk::PhysicalDevice physical_device : physical_devices) {
            auto prop = physical_device.getProperties();
            QString name = QString::fromUtf8(prop.deviceName, -1);
            if (prop.apiVersion < Vulkan::TargetVulkanApiVersion) {
                name += tr(" * Unsupported Vulkan Version");
            }
            m_physical_devices.push_back(name);
        }
    }

    ui->graphicsAdapterBox->addItem(tr("Auto Select")); // -1, auto selection
    for (const auto& device : m_physical_devices) {
        ui->graphicsAdapterBox->addItem(device);
    }

    ui->consoleLanguageComboBox->addItems(languageNames);

    QCompleter* completer = new QCompleter(languageNames, this);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    ui->consoleLanguageComboBox->setCompleter(completer);

    ui->hideCursorComboBox->addItem(tr("Never"));
    ui->hideCursorComboBox->addItem(tr("Idle"));
    ui->hideCursorComboBox->addItem(tr("Always"));

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

    ui->micComboBox->addItem(micMap.key("None"), "None");
    ui->micComboBox->addItem(micMap.key("Default Device"), "Default Device");
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    int count = 0;
    SDL_AudioDeviceID* devices = SDL_GetAudioRecordingDevices(&count);
    if (devices) {
        for (int i = 0; i < count; ++i) {
            SDL_AudioDeviceID devId = devices[i];
            const char* name = SDL_GetAudioDeviceName(devId);
            if (name) {
                QString qname = QString::fromUtf8(name);
                ui->micComboBox->addItem(qname, QString::number(devId));
            }
        }
        SDL_free(devices);
    } else {
        qDebug() << "Erro SDL_GetAudioRecordingDevices:" << SDL_GetError();
    }

    InitializeEmulatorLanguages();
    onAudioDeviceChange(true);
    LoadValuesFromConfig();

    defaultTextEdit = tr("Point your mouse at an option to display its description.");
    ui->descriptionText->setText(defaultTextEdit);

    connect(ui->buttonBox, &QDialogButtonBox::rejected, this, &QWidget::close);

    connect(ui->buttonBox, &QDialogButtonBox::clicked, this,
            [this, config_dir](QAbstractButton* button) {
                if (button == ui->buttonBox->button(QDialogButtonBox::Save)) {
                    is_saving = true;
                    UpdateSettings();
                    Config::save(config_dir / "config.toml");
                    QWidget::close();

                } else if (button == ui->buttonBox->button(QDialogButtonBox::Apply)) {
                    UpdateSettings();
                    Config::save(config_dir / "config.toml");

                } else if (button == ui->buttonBox->button(QDialogButtonBox::RestoreDefaults)) {
                    Config::setDefaultValues();
                    Config::save(config_dir / "config.toml");
                    LoadValuesFromConfig();

                } else if (button == ui->buttonBox->button(QDialogButtonBox::Close)) {
                    ui->backgroundImageOpacitySlider->setValue(backgroundImageOpacitySlider_backup);
                    emit BackgroundOpacityChanged(backgroundImageOpacitySlider_backup);

                    ui->fpsSlider->setValue(fps_backup);
                    Config::setFpsLimit(fps_backup);

                    ui->horizontalVolumeSlider->setValue(volume_slider_backup);
                    Config::setVolumeSlider(volume_slider_backup, true);

                    ui->BGMVolumeSlider->setValue(bgm_volume_backup);
                    BackgroundMusicPlayer::getInstance().setVolume(bgm_volume_backup);

                    SyncRealTimeWidgetstoConfig();
                    for (auto* widget : QApplication::topLevelWidgets()) {
                        if (auto* dlg = qobject_cast<SettingsDialog*>(widget)) {
                            dlg->close();
                            dlg->deleteLater();
                        }
                    }
                }
                if (Common::Log::IsActive()) {
                    Common::Log::Filter filter;
                    filter.ParseFilterString(Config::getLogFilter());
                    Common::Log::SetGlobalFilter(filter);
                }
            });

    ui->buttonBox->button(QDialogButtonBox::Save)->setText(tr("Save"));
    ui->buttonBox->button(QDialogButtonBox::Apply)->setText(tr("Apply"));
    ui->buttonBox->button(QDialogButtonBox::RestoreDefaults)->setText(tr("Restore Defaults"));
    ui->buttonBox->button(QDialogButtonBox::Close)->setText(tr("Close"));

    connect(ui->tabWidgetSettings, &QTabWidget::currentChanged, this,
            [this]() { ui->buttonBox->button(QDialogButtonBox::Close)->setFocus(); });

    {
#ifdef ENABLE_UPDATER
#if (QT_VERSION < QT_VERSION_CHECK(6, 7, 0))
        connect(ui->updateCheckBox, &QCheckBox::stateChanged, this,
                [](int state) { Config::setAutoUpdate(state == Qt::Checked); });

        connect(ui->changelogCheckBox, &QCheckBox::stateChanged, this,
                [](int state) { Config::setAlwaysShowChangelog(state == Qt::Checked); });
#else
        connect(ui->updateCheckBox, &QCheckBox::checkStateChanged, this,
                [](Qt::CheckState state) { Config::setAutoUpdate(state == Qt::Checked); });

        connect(ui->changelogCheckBox, &QCheckBox::checkStateChanged, this,
                [](Qt::CheckState state) { Config::setAlwaysShowChangelog(state == Qt::Checked); });
#endif

        connect(ui->updateComboBox, &QComboBox::currentTextChanged, this,
                [this](const QString& channel) {
                    if (channelMap.contains(channel)) {
                        Config::setUpdateChannel(channelMap.value(channel).toStdString());
                    }
                });

        connect(ui->checkUpdateButton, &QPushButton::clicked, this, []() {
            auto checkUpdate = new CheckUpdate(true);
            checkUpdate->exec();
        });

#else
        ui->updaterGroupBox->setVisible(false);
#endif
        connect(ui->updateCompatibilityButton, &QPushButton::clicked, this,
                [this, compat_info = this->compat_info]() {
                    if (compat_info) {
                        compat_info->UpdateCompatibilityDatabase(this, true);
                        emit CompatibilityChanged();
                    }
                });

#if (QT_VERSION < QT_VERSION_CHECK(6, 7, 0))
        connect(ui->enableCompatibilityCheckBox, &QCheckBox::stateChanged, this,
                [this, m_compat_info](int state) {
#else
        connect(ui->enableCompatibilityCheckBox, &QCheckBox::checkStateChanged, this,
                [this, m_compat_info](Qt::CheckState state) {
#endif
                    bool enabled = (state == Qt::Checked);
                    Config::setCompatibilityEnabled(enabled);

                    if (enabled && m_compat_info) {
                        m_compat_info->LoadCompatibilityFile();
                    }
                    emit CompatibilityChanged();
                });
    }

    {
        connect(ui->backgroundImageOpacitySlider, &QSlider::valueChanged, this,
                [this](int value) { emit BackgroundOpacityChanged(value); });

        connect(ui->BGMVolumeSlider, &QSlider::valueChanged, this,
                [](int value) { BackgroundMusicPlayer::getInstance().setVolume(value); });

        connect(ui->chooseHomeTabComboBox, &QComboBox::currentTextChanged, this,
                [](const QString& hometab) { Config::setChooseHomeTab(hometab.toStdString()); });

#if (QT_VERSION < QT_VERSION_CHECK(6, 7, 0))
        connect(ui->showBackgroundImageCheckBox, &QCheckBox::stateChanged, this, [](int state) {
#else
        connect(ui->showBackgroundImageCheckBox, &QCheckBox::checkStateChanged, this,
                [](Qt::CheckState state) {
#endif
            Config::setShowBackgroundImage(state == Qt::Checked);
        });
    }

    {
        connect(ui->OpenCustomTrophyLocationButton, &QPushButton::clicked, this, []() {
            QString userPath;
            Common::FS::PathToQString(userPath,
                                      Common::FS::GetUserPath(Common::FS::PathType::CustomTrophy));
            QDesktopServices::openUrl(QUrl::fromLocalFile(userPath));
        });
    }

    {
        connect(ui->hideCursorComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](s16 index) { OnCursorStateChanged(index); });
    }

    connect(ui->GenAudioComboBox, &QComboBox::currentTextChanged, this,
            [this](const QString& device) { Config::setMainOutputDevice(device.toStdString()); });

    connect(ui->DsAudioComboBox, &QComboBox::currentTextChanged, this,
            [this](const QString& device) { Config::setPadSpkOutputDevice(device.toStdString()); });

#if (QT_VERSION < QT_VERSION_CHECK(6, 7, 0))
    connect(ui->FSRCheckBox, &QCheckBox::stateChanged, this,
            [](int state) { Config::setFsrEnabled(state == Qt::Checked); });

    connect(ui->RCASCheckBox, &QCheckBox::stateChanged, this,
            [](int state) { Config::setRcasEnabled(state == Qt::Checked); });
#else
    connect(ui->FSRCheckBox, &QCheckBox::checkStateChanged, this,
            [](int state) { Config::setFsrEnabled(state == Qt::Checked); });

    connect(ui->RCASCheckBox, &QCheckBox::checkStateChanged, this,
            [](int state) { Config::setRcasEnabled(state == Qt::Checked); });
#endif
    ui->fpsLimiterCheckBox->setChecked(Config::isFpsLimiterEnabled());
    ui->fpsSpinBox->setEnabled(Config::isFpsLimiterEnabled());
    ui->fpsSlider->setEnabled(Config::isFpsLimiterEnabled());

    connect(ui->fpsLimiterCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
        Config::setFpsLimiterEnabled(checked);
        ui->fpsSpinBox->setEnabled(checked);
        ui->fpsSlider->setEnabled(checked);
    });

    connect(ui->fpsSlider, &QSlider::valueChanged, ui->fpsSpinBox, &QSpinBox::setValue);
    connect(ui->fpsSpinBox, qOverload<int>(&QSpinBox::valueChanged), ui->fpsSlider,
            &QSlider::setValue);

    connect(ui->fpsSlider, &QSlider::valueChanged, this, [this](int value) {
        FPSChange(value);
        Config::setFpsLimit(value);
    });

    connect(ui->RCASSlider, &QSlider::valueChanged, this,
            &SettingsDialog::OnRcasAttenuationChanged);

    connect(ui->RCASSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            &SettingsDialog::OnRcasAttenuationSpinBoxChanged);

    if (Config::getGameRunning()) {
        connect(ui->RCASSlider, &QSlider::valueChanged, this,
                [this](int value) { m_ipc_client->setRcasAttenuation(value); });

#if (QT_VERSION < QT_VERSION_CHECK(6, 7, 0))
        connect(ui->FSRCheckBox, &QCheckBox::stateChanged, this,
                [this](int state) { m_ipc_client->setFsr(state); });

        connect(ui->RCASCheckBox, &QCheckBox::stateChanged, this,
                [this](int state) { m_ipc_client->setRcas(state); });
#else
        connect(ui->FSRCheckBox, &QCheckBox::checkStateChanged, this,
                [this](Qt::CheckState state) { m_ipc_client->setFsr(state); });

        connect(ui->RCASCheckBox, &QCheckBox::checkStateChanged, this,
                [this](Qt::CheckState state) { m_ipc_client->setRcas(state); });
#endif
    }

    // PATH TAB
    {
        connect(ui->addFolderButton, &QPushButton::clicked, this, [this]() {
            QString file_path_string =
                QFileDialog::getExistingDirectory(this, tr("Games directories"));
            auto file_path = Common::FS::PathFromQString(file_path_string);
            if (!file_path.empty() && Config::addGameDirectories(file_path, true)) {
                QListWidgetItem* item = new QListWidgetItem(file_path_string);
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(Qt::Checked);
                ui->gameFoldersListWidget->addItem(item);
            }
        });

        connect(ui->gameFoldersListWidget, &QListWidget::itemSelectionChanged, this, [this]() {
            ui->removeFolderButton->setEnabled(
                !ui->gameFoldersListWidget->selectedItems().isEmpty());
        });

        connect(ui->logPresetsButton, &QPushButton::clicked, this, [this]() {
            LogPresetsDialog dlg(compat_info, this);
            connect(&dlg, &LogPresetsDialog::PresetChosen, this,
                    [this](const QString& filter) { ui->logFilterLineEdit->setText(filter); });
            dlg.exec();
        });

        connect(ui->removeFolderButton, &QPushButton::clicked, this, [this]() {
            QListWidgetItem* selected_item = ui->gameFoldersListWidget->currentItem();
            QString item_path_string = selected_item ? selected_item->text() : QString();
            if (!item_path_string.isEmpty()) {
                auto file_path = Common::FS::PathFromQString(item_path_string);
                Config::removeGameDirectories(file_path);
                delete selected_item;
            }
        });

        connect(ui->horizontalVolumeSlider, &QSlider::valueChanged, this, [this](int value) {
            VolumeSliderChange(value);

            if (Config::getGameRunning())
                m_ipc_client->adjustVol(value);
        });

        connect(ui->browseButton, &QPushButton::clicked, this, [this]() {
            const auto save_data_path = Config::GetSaveDataPath();
            QString initial_path;
            Common::FS::PathToQString(initial_path, save_data_path);

            QString save_data_path_string =
                QFileDialog::getExistingDirectory(this, tr("Directory to save data"), initial_path);

            auto file_path = Common::FS::PathFromQString(save_data_path_string);
            if (!file_path.empty()) {
                Config::setSaveDataPath(file_path);
                ui->currentSaveDataPath->setText(save_data_path_string);
            }
        });

        connect(ui->browseSysModulesButton, &QPushButton::clicked, this, [this]() {
            const auto sysModulesPath = Config::getSysModulesPath();
            QString initial_path;
            Common::FS::PathToQString(initial_path, sysModulesPath);

            QString sysModulesPathString = QFileDialog::getExistingDirectory(
                this, tr("Select the System Modules folder"), initial_path);

            auto file_path = Common::FS::PathFromQString(sysModulesPathString);
            if (!file_path.empty()) {
                Config::setSysModulesPath(file_path);
                ui->currentSysModulesPath->setText(sysModulesPathString);
            }
        });

        connect(ui->folderButton, &QPushButton::clicked, this, [this]() {
            const auto dlc_folder_path = Config::getAddonDirectory();
            QString initial_path;
            Common::FS::PathToQString(initial_path, dlc_folder_path);

            QString dlc_folder_path_string =
                QFileDialog::getExistingDirectory(this, tr("Select the DLC folder"), initial_path);

            auto file_path = Common::FS::PathFromQString(dlc_folder_path_string);
            if (!file_path.empty()) {
                Config::setAddonDirectories(file_path);
                ui->currentDLCFolder->setText(dlc_folder_path_string);
            }
        });

        connect(ui->PortableUserButton, &QPushButton::clicked, this, []() {
            auto portable_dir = std::filesystem::current_path() / "user";
            QString userDirQString;
            Common::FS::PathToQString(userDirQString, portable_dir);

            std::filesystem::path global_dir;
#if _WIN32
            TCHAR appdata[MAX_PATH] = {0};
            SHGetFolderPath(NULL, CSIDL_APPDATA, NULL, 0, appdata);
            global_dir = std::filesystem::path(appdata) / "shadPS4";
#elif __APPLE__
    global_dir = std::filesystem::path(getenv("HOME")) / "Library" / "Application Support" / "shadPS4";
#else
    const char* xdg_data_home = getenv("XDG_DATA_HOME");
    if (xdg_data_home && *xdg_data_home)
        global_dir = std::filesystem::path(xdg_data_home) / "shadPS4";
    else
        global_dir = std::filesystem::path(getenv("HOME")) / ".local" / "share" / "shadPS4";
#endif

            if (std::filesystem::exists(portable_dir)) {
                QMessageBox::StandardButton reply = QMessageBox::question(
                    nullptr, tr("Portable folder exists"),
                    tr("%1 already exists. Overwrite with global folder data?").arg(userDirQString),
                    QMessageBox::Yes | QMessageBox::No);
                if (reply == QMessageBox::Yes && std::filesystem::exists(global_dir)) {
                    std::filesystem::copy(global_dir, portable_dir,
                                          std::filesystem::copy_options::recursive |
                                              std::filesystem::copy_options::overwrite_existing);
                    std::filesystem::remove_all(global_dir);
                    QMessageBox::information(nullptr, tr("Portable User Folder Updated"),
                                             tr("Portable folder overwritten with global data."));
                }
                return;
            }

            if (std::filesystem::exists(global_dir)) {
                std::filesystem::copy(global_dir, portable_dir,
                                      std::filesystem::copy_options::recursive);
                std::filesystem::remove_all(global_dir);
                QMessageBox::information(
                    nullptr, tr("Portable User Folder Created"),
                    tr("Moved data from global folder to:\n%1").arg(userDirQString));
            } else {
                std::filesystem::create_directories(portable_dir);
                QMessageBox::information(
                    nullptr, tr("Portable User Folder Created"),
                    tr("%1 successfully created - Relaunch Emulator to Activate")
                        .arg(userDirQString));
            }
        });

        connect(ui->GlobalUserButton, &QPushButton::clicked, this, []() {
            std::filesystem::path global_dir;
#if _WIN32
            TCHAR appdata[MAX_PATH] = {0};
            SHGetFolderPath(NULL, CSIDL_APPDATA, NULL, 0, appdata);
            global_dir = std::filesystem::path(appdata) / "shadPS4";
#elif __APPLE__
    global_dir = std::filesystem::path(getenv("HOME")) / "Library" / "Application Support" / "shadPS4";
#else
    const char* xdg_data_home = getenv("XDG_DATA_HOME");
    if (xdg_data_home && *xdg_data_home)
        global_dir = std::filesystem::path(xdg_data_home) / "shadPS4";
    else
        global_dir = std::filesystem::path(getenv("HOME")) / ".local" / "share" / "shadPS4";
#endif

            auto portable_dir = std::filesystem::current_path() / "user";
            QString userDirQString;
            Common::FS::PathToQString(userDirQString, global_dir);

            if (std::filesystem::exists(global_dir)) {
                QMessageBox::StandardButton reply = QMessageBox::question(
                    nullptr, tr("Global folder exists"),
                    tr("%1 already exists. Overwrite with portable folder data?")
                        .arg(userDirQString),
                    QMessageBox::Yes | QMessageBox::No);
                if (reply == QMessageBox::Yes && std::filesystem::exists(portable_dir)) {
                    std::filesystem::copy(portable_dir, global_dir,
                                          std::filesystem::copy_options::recursive |
                                              std::filesystem::copy_options::overwrite_existing);
                    std::filesystem::remove_all(portable_dir);
                    QMessageBox::information(nullptr, tr("Global User Folder Updated"),
                                             tr("Global folder overwritten with portable data."));
                }
                return;
            }

            if (std::filesystem::exists(portable_dir)) {
                std::filesystem::copy(portable_dir, global_dir,
                                      std::filesystem::copy_options::recursive);
                std::filesystem::remove_all(portable_dir);
                QMessageBox::information(
                    nullptr, tr("Global User Folder Created"),
                    tr("Moved data from portable folder to:\n%1").arg(userDirQString));
            } else {
                std::filesystem::create_directories(global_dir);
                QMessageBox::information(
                    nullptr, tr("Global User Folder Created"),
                    tr("%1 successfully created - Relaunch Emulator to Activate")
                        .arg(userDirQString));
            }
        });

        connect(ui->CreateUserButton, &QPushButton::clicked, this, []() {
            auto portable_dir = std::filesystem::current_path() / "user";
            QString userDirQString;
            Common::FS::PathToQString(userDirQString, portable_dir);

            if (std::filesystem::exists(portable_dir)) {
                QMessageBox::information(nullptr, tr("Cannot create portable user folder"),
                                         tr("%1 already exists").arg(userDirQString));
                return;
            }

            std::filesystem::create_directories(portable_dir);
            QMessageBox::information(
                nullptr, tr("Portable user folder created"),
                tr("%1 successfully created - Relaunch Emulator to Configure").arg(userDirQString));
        });
    }

    // DEBUG TAB
    {
        connect(ui->OpenLogLocationButton, &QPushButton::clicked, this, []() {
            QString userPath;
            Common::FS::PathToQString(userPath,
                                      Common::FS::GetUserPath(Common::FS::PathType::LogDir));
            QDesktopServices::openUrl(QUrl::fromLocalFile(userPath));
        });
    }

    // Descriptions
    {
        // General
        ui->consoleLanguageGroupBox->installEventFilter(this);
        ui->emulatorLanguageGroupBox->installEventFilter(this);
        ui->showSplashCheckBox->installEventFilter(this);
        ui->discordRPCCheckbox->installEventFilter(this);
        ui->gameVolumeGroup->installEventFilter(this);
        ui->separateUpdatesCheckBox->installEventFilter(this);

#ifdef ENABLE_UPDATER
        ui->updaterGroupBox->installEventFilter(this);
#endif

        // GUI
        ui->GUIBackgroundImageGroupBox->installEventFilter(this);
        ui->GUIMusicGroupBox->installEventFilter(this);
        ui->enableCompatibilityCheckBox->installEventFilter(this);
        ui->checkCompatibilityOnStartupCheckBox->installEventFilter(this);
        ui->updateCompatibilityButton->installEventFilter(this);

        // User
        auto names = Config::getUserNames();
        ui->userName1LineEdit->installEventFilter(this);
        ui->userName2LineEdit->installEventFilter(this);
        ui->userName3LineEdit->installEventFilter(this);
        ui->userName4LineEdit->installEventFilter(this);

        ui->disableTrophycheckBox->installEventFilter(this);
        ui->OpenCustomTrophyLocationButton->installEventFilter(this);
        ui->label_Trophy->installEventFilter(this);
        ui->trophyKeyLineEdit->installEventFilter(this);

        // Input
        ui->hideCursorGroupBox->installEventFilter(this);
        ui->idleTimeoutGroupBox->installEventFilter(this);
        ui->backgroundControllerCheckBox->installEventFilter(this);
        ui->motionControlsCheckBox->installEventFilter(this);
        ui->micComboBox->installEventFilter(this);

        // Graphics
        ui->graphicsAdapterGroupBox->installEventFilter(this);
        ui->windowSizeGroupBox->installEventFilter(this);
        ui->presentModeGroupBox->installEventFilter(this);
        ui->heightDivider->installEventFilter(this);
        ui->nullGpuCheckBox->installEventFilter(this);
        ui->enableHDRCheckBox->installEventFilter(this);
        ui->chooseHomeTabGroupBox->installEventFilter(this);
        ui->gameSizeCheckBox->installEventFilter(this);
        ui->enableAutoBackupCheckBox->installEventFilter(this);

        // Paths
        ui->gameFoldersGroupBox->installEventFilter(this);
        ui->gameFoldersListWidget->installEventFilter(this);
        ui->addFolderButton->installEventFilter(this);
        ui->removeFolderButton->installEventFilter(this);
        ui->saveDataGroupBox->installEventFilter(this);
        ui->currentSaveDataPath->installEventFilter(this);
        ui->currentDLCFolder->installEventFilter(this);
        ui->currentSysModulesPath->installEventFilter(this);
        ui->browseButton->installEventFilter(this);
        ui->folderButton->installEventFilter(this);
        ui->PortableUserFolderGroupBox->installEventFilter(this);

        // Log
        ui->logTypeGroupBox->installEventFilter(this);
        ui->logFilter->installEventFilter(this);
        ui->enableLoggingCheckBox->installEventFilter(this);
        ui->separateLogFilesCheckbox->installEventFilter(this);
        ui->OpenLogLocationButton->installEventFilter(this);

        // Debug
        ui->debugDump->installEventFilter(this);
        ui->vkValidationCheckBox->installEventFilter(this);
        ui->vkSyncValidationCheckBox->installEventFilter(this);
        ui->rdocCheckBox->installEventFilter(this);
        ui->cacheCheckBox->installEventFilter(this);
        ui->cacheArchiveCheckBox->installEventFilter(this);
        ui->crashDiagnosticsCheckBox->installEventFilter(this);
        ui->guestMarkersCheckBox->installEventFilter(this);
        ui->hostMarkersCheckBox->installEventFilter(this);
        ui->collectShaderCheckBox->installEventFilter(this);
        ui->copyGPUBuffersCheckBox->installEventFilter(this);

        // Experimental
        ui->isDevKitCheckBox->installEventFilter(this);
        ui->isNeoModeCheckBox->installEventFilter(this);
        ui->separateLogFilesCheckbox->installEventFilter(this);
        ui->DMACheckBox->installEventFilter(this);
        ui->HotkeysCheckBox->installEventFilter(this);
        ui->HomeHotkeysCheckBox->installEventFilter(this);
        ui->isDevKitCheckBox->installEventFilter(this);
        ui->isNeoModeCheckBox->installEventFilter(this);
        ui->connectedNetworkCheckBox->installEventFilter(this);
        ui->isPSNSignedInCheckBox->installEventFilter(this);
        ui->ReadbacksLinearCheckBox->installEventFilter(this);
        ui->ReadbackSpeedComboBox->installEventFilter(this);
        ui->MemorySpinBox->installEventFilter(this);
    }

    SdlEventWrapper::Wrapper::wrapperActive = true;
    if (!is_game_running) {
        SDL_InitSubSystem(SDL_INIT_EVENTS);
        Polling = QtConcurrent::run(&SettingsDialog::pollSDLevents, this);
    } else {
        SdlEventWrapper::Wrapper* DeviceEventWrapper = SdlEventWrapper::Wrapper::GetInstance();
        QObject::connect(DeviceEventWrapper, &SdlEventWrapper::Wrapper::audioDeviceChanged, this,
                         &SettingsDialog::onAudioDeviceChange);
    }
}

void SettingsDialog::closeEvent(QCloseEvent* event) {
    if (!is_saving) {
        ui->backgroundImageOpacitySlider->setValue(backgroundImageOpacitySlider_backup);
        emit BackgroundOpacityChanged(backgroundImageOpacitySlider_backup);
        ui->horizontalVolumeSlider->setValue(volume_slider_backup);
        Config::setVolumeSlider(volume_slider_backup, true);
        ui->BGMVolumeSlider->setValue(bgm_volume_backup);
        BackgroundMusicPlayer::getInstance().setVolume(bgm_volume_backup);
        ui->fpsSlider->setValue(fps_backup);
        ui->fpsSpinBox->setValue(fps_backup);

        Config::setFpsLimit(fps_backup);
    }
    SdlEventWrapper::Wrapper::wrapperActive = false;
    if (!is_game_running) {
        SDL_Event quitLoop{};
        quitLoop.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quitLoop);
        Polling.waitForFinished();
        SDL_QuitSubSystem(SDL_INIT_EVENTS);
    }
    QDialog::closeEvent(event);
}

void SettingsDialog::LoadValuesFromConfig() {

    std::filesystem::path userdir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
    std::error_code error;
    if (std::filesystem::exists(userdir / "config.toml", error)) {
        Config::load(userdir / "config.toml");
    } else {
        return;
    }

    try {
        std::ifstream ifs;
        ifs.exceptions(std::ifstream::failbit | std::ifstream::badbit);
        const toml::value data = toml::parse(userdir / "config.toml");
    } catch (std::exception& ex) {
        fmt::print("Got exception trying to load config file. Exception: {}\n", ex.what());
        return;
    }

    const toml::value data = toml::parse(userdir / "config.toml");
    const QVector<int> languageIndexes = {21, 23, 14, 6, 18, 1, 12, 22, 2, 4,  25, 24, 29, 5,  0, 9,
                                          15, 16, 17, 7, 26, 8, 11, 20, 3, 13, 27, 10, 19, 30, 28};

    const auto save_data_path = Config::GetSaveDataPath();
    QString save_data_path_string;
    Common::FS::PathToQString(save_data_path_string, save_data_path);
    ui->currentSaveDataPath->setText(save_data_path_string);

    const auto dlc_folder_path = Config::getAddonDirectory();
    QString dlc_folder_path_string;
    Common::FS::PathToQString(dlc_folder_path_string, dlc_folder_path);
    ui->currentDLCFolder->setText(dlc_folder_path_string);

    const auto sys_modules_path = Config::getSysModulesPath();
    QString sys_modules_path_string;
    Common::FS::PathToQString(sys_modules_path_string, sys_modules_path);
    ui->currentSysModulesPath->setText(sys_modules_path_string);

    ui->consoleLanguageComboBox->setCurrentIndex(
        std::distance(languageIndexes.begin(),
                      std::find(languageIndexes.begin(), languageIndexes.end(),
                                toml::find_or<int>(data, "Settings", "consoleLanguage", 6))) %
        languageIndexes.size());
    {
        std::string locale = toml::find_or<std::string>(data, "GUI", "emulatorLanguage", "en_US");
        int index = 0;
        if (languages.contains(locale)) {
            index = languages[locale];
        } else {
            index = 0; // fallback to English (first in list)
        }
        ui->emulatorLanguageComboBox->setCurrentIndex(index);
    }

    ui->hideCursorComboBox->setCurrentIndex(toml::find_or<int>(data, "Input", "cursorState", 1));
    OnCursorStateChanged(toml::find_or<int>(data, "Input", "cursorState", 1));
    ui->idleTimeoutSpinBox->setValue(toml::find_or<int>(data, "Input", "cursorHideTimeout", 5));

    QString micValue = QString::fromStdString(Config::getMicDevice());
    int micIndex = ui->micComboBox->findData(micValue);
    if (micIndex != -1) {
        ui->micComboBox->setCurrentIndex(micIndex);
    } else {
        ui->micComboBox->setCurrentIndex(0);
    }
    // First options is auto selection -1, so gpuId on the GUI will always have to subtract 1
    // when setting and add 1 when getting to select the correct gpu in Qt
    ui->graphicsAdapterBox->setCurrentIndex(toml::find_or<int>(data, "Vulkan", "gpuId", -1) + 1);
    ui->widthSpinBox->setValue(toml::find_or<int>(data, "GPU", "screenWidth", 1280));
    ui->heightSpinBox->setValue(toml::find_or<int>(data, "GPU", "screenHeight", 720));
    ui->vblankSpinBox->setValue(toml::find_or<int>(data, "GPU", "vblankFrequency", 60));
    ui->dumpShadersCheckBox->setChecked(toml::find_or<bool>(data, "GPU", "dumpShaders", false));
    ui->nullGpuCheckBox->setChecked(toml::find_or<bool>(data, "GPU", "nullGpu", false));
    ui->enableHDRCheckBox->setChecked(toml::find_or<bool>(data, "GPU", "allowHDR", false));
    ui->enableAutoBackupCheckBox->setChecked(
        toml::find_or<bool>(data, "General", "enableAutoBackup", false));
    ui->playBGMCheckBox->setChecked(toml::find_or<bool>(data, "General", "playBGM", false));
    ui->ReadbacksLinearCheckBox->setChecked(
        toml::find_or<bool>(data, "GPU", "readbackLinearImages", false));
    ui->separateUpdatesCheckBox->setChecked(
        toml::find_or<bool>(data, "General", "separateUpdateEnabled", false));
    ui->DMACheckBox->setChecked(toml::find_or<bool>(data, "GPU", "directMemoryAccess", false));
    ui->HotkeysCheckBox->setChecked(
        toml::find_or<bool>(data, "General", "DisableHardcodedHotkeys", false));
    ui->HomeHotkeysCheckBox->setChecked(
        toml::find_or<bool>(data, "General", "UseHomeButtonForHotkeys", false));
    ui->screenTipBox->setChecked(toml::find_or<bool>(data, "General", "screenTipDisable", false));
    ui->ReadbackSpeedComboBox->setCurrentIndex(static_cast<int>(Config::readbackSpeed()));

    ui->SkipsCheckBox->setChecked(toml::find_or<bool>(data, "GPU", "shaderSkipsEnabled", false));
    ui->MemorySpinBox->setValue(
        toml::find_or<int>(data, "General", "extraDmemInMbytes", Config::getExtraDmemInMbytes()));
    ui->disableTrophycheckBox->setChecked(
        toml::find_or<bool>(data, "General", "isTrophyPopupDisabled", false));
    ui->popUpDurationSpinBox->setValue(Config::getTrophyNotificationDuration());

    QString side = QString::fromStdString(Config::sideTrophy());

    ui->radioButton_Left->setChecked(side == "left");
    ui->radioButton_Right->setChecked(side == "right");
    ui->radioButton_Top->setChecked(side == "top");
    ui->radioButton_Bottom->setChecked(side == "bottom");

    ui->BGMVolumeSlider->setValue(toml::find_or<int>(data, "General", "BGMvolume", 50));
    int gameVolume = Config::getVolumeSlider();
    ui->horizontalVolumeSlider->setValue(gameVolume);
    ui->volumeText->setText(QString::number(ui->horizontalVolumeSlider->sliderPosition()) + "%");
    ui->fpsSlider->setValue(Config::getFpsLimit());
    ui->fpsSpinBox->setValue(Config::getFpsLimit());
    ui->fpsLimiterCheckBox->setChecked(Config::isFpsLimiterEnabled());
    ui->discordRPCCheckbox->setChecked(
        toml::find_or<bool>(data, "General", "enableDiscordRPC", true));

    std::string fullScreenMode =
        toml::find_or<std::string>(data, "GPU", "FullscreenMode", "Windowed");
    QString translatedText_FullscreenMode =
        screenModeMap.key(QString::fromStdString(fullScreenMode));
    ui->displayModeComboBox->setCurrentText(translatedText_FullscreenMode);

    std::string presentMode = toml::find_or<std::string>(data, "GPU", "presentMode", "Mailbox");
    QString translatedText_PresentMode = presentModeMap.key(QString::fromStdString(presentMode));
    ui->presentModeComboBox->setCurrentText(translatedText_PresentMode);

    ui->gameSizeCheckBox->setChecked(toml::find_or<bool>(data, "GUI", "loadGameSizeEnabled", true));
    ui->showSplashCheckBox->setChecked(toml::find_or<bool>(data, "General", "showSplash", false));
    QString translatedText_logType = logTypeMap.key(QString::fromStdString(Config::getLogType()));
    if (!translatedText_logType.isEmpty()) {
        ui->logTypeComboBox->setCurrentText(translatedText_logType);
    }
    ui->logFilterLineEdit->setText(
        QString::fromStdString(toml::find_or<std::string>(data, "General", "logFilter", "")));
    auto names = Config::getUserNames();
    ui->userName1LineEdit->setText(QString::fromStdString(names[0]));
    ui->userName2LineEdit->setText(QString::fromStdString(names[1]));
    ui->userName3LineEdit->setText(QString::fromStdString(names[2]));
    ui->userName4LineEdit->setText(QString::fromStdString(names[3]));

    ui->trophyKeyLineEdit->setText(
        QString::fromStdString(toml::find_or<std::string>(data, "Keys", "TrophyKey", "")));
    ui->trophyKeyLineEdit->setEchoMode(QLineEdit::Password);
    ui->debugDump->setChecked(toml::find_or<bool>(data, "Debug", "DebugDump", false));
    ui->separateLogFilesCheckbox->setChecked(
        toml::find_or<bool>(data, "Debug", "isSeparateLogFilesEnabled", false));
    ui->vkValidationCheckBox->setChecked(toml::find_or<bool>(data, "Vulkan", "validation", false));
    ui->vkSyncValidationCheckBox->setChecked(
        toml::find_or<bool>(data, "Vulkan", "validation_sync", false));
    ui->rdocCheckBox->setChecked(toml::find_or<bool>(data, "Vulkan", "rdocEnable", false));
    ui->cacheCheckBox->setChecked(
        toml::find_or<bool>(data, "Vulkan", "pipelineCacheEnable", false));
    ui->cacheArchiveCheckBox->setChecked(
        toml::find_or<bool>(data, "Vulkan", "pipelineCacheArchive", false));
    ui->crashDiagnosticsCheckBox->setChecked(
        toml::find_or<bool>(data, "Vulkan", "crashDiagnostic", false));
    ui->guestMarkersCheckBox->setChecked(
        toml::find_or<bool>(data, "Vulkan", "guestMarkers", false));
    ui->hostMarkersCheckBox->setChecked(toml::find_or<bool>(data, "Vulkan", "hostMarkers", false));
    ui->copyGPUBuffersCheckBox->setChecked(
        toml::find_or<bool>(data, "GPU", "copyGPUBuffers", false));
    ui->collectShaderCheckBox->setChecked(
        toml::find_or<bool>(data, "Debug", "CollectShader", false));
    ui->enableCompatibilityCheckBox->setChecked(
        toml::find_or<bool>(data, "General", "compatibilityEnabled", false));
    ui->enableLoggingCheckBox->setChecked(toml::find_or<bool>(data, "Debug", "logEnabled", true));

    ui->GenAudioComboBox->setCurrentText(
        QString::fromStdString(toml::find_or<std::string>(data, "Audio", "mainOutputDevice", "")));
    ui->DsAudioComboBox->setCurrentText(QString::fromStdString(
        toml::find_or<std::string>(data, "Audio", "padSpkOutputDevice", "")));

    ui->checkCompatibilityOnStartupCheckBox->setChecked(
        toml::find_or<bool>(data, "General", "checkCompatibilityOnStartup", false));

    ui->FSRCheckBox->setChecked(toml::find_or<bool>(data, "GPU", "fsrEnabled", true));
    ui->RCASCheckBox->setChecked(toml::find_or<bool>(data, "GPU", "rcasEnabled", true));
    ui->RCASSlider->setMinimum(0);
    ui->RCASSlider->setMaximum(3000);
    ui->RCASSlider->setValue(Config::getRcasAttenuation());
    ui->RCASSpinBox->setValue(ui->RCASSlider->value() / 1000.0);
    ui->connectedNetworkCheckBox->setChecked(
        toml::find_or<bool>(data, "General", "isConnectedToNetwork", false));
    ui->isPSNSignedInCheckBox->setChecked(
        toml::find_or<bool>(data, "General", "isPSNSignedIn", false));

#ifdef ENABLE_UPDATER
    ui->updateCheckBox->setChecked(toml::find_or<bool>(data, "General", "autoUpdate", false));
    ui->changelogCheckBox->setChecked(
        toml::find_or<bool>(data, "General", "alwaysShowChangelog", false));

    QString updateChannel = QString::fromStdString(Config::getUpdateChannel());

    if (updateChannel != "Full-Souls" && updateChannel != "BBFork" && updateChannel != "PRTBB") {
        updateChannel = "Full-Souls";
    }

    ui->updateComboBox->setCurrentText(channelMap.key(updateChannel));

#endif
    // Load special pad settings
    if (data.contains("Input")) {
        const auto& in = toml::find(data, "Input");

        for (int p = 1; p <= 4; ++p) {
            std::string classKey = fmt::format("specialPadClass{}", p);
            std::string useKey = fmt::format("useSpecialPad{}", p);

            if (in.contains(classKey))
                Config::setSpecialPadClass(p, toml::find<int>(in, classKey), true);

            if (in.contains(useKey))
                Config::setUseSpecialPad(p, toml::find<bool>(in, useKey), true);
        }

        // Update UI matrix
        for (int p = 1; p <= 4; ++p) {
            int cls = Config::getSpecialPadClass(p);
            bool use = Config::getUseSpecialPad(p);

            for (int c = 1; c <= 4; ++c)
                specialPadChecks[c - 1][p - 1]->setChecked(false);

            if (use && cls >= 1 && cls <= 4)
                specialPadChecks[cls - 1][p - 1]->setChecked(true);
        }
    }

    std::string chooseHomeTab =
        toml::find_or<std::string>(data, "General", "chooseHomeTab", "General");
    QString translatedText = chooseHomeTabMap.key(QString::fromStdString(chooseHomeTab));
    if (translatedText.isEmpty()) {
        translatedText = tr("General");
    }
    ui->chooseHomeTabComboBox->setCurrentText(translatedText);

    QStringList tabNames = {tr("General"), tr("GUI"),   tr("Graphics"), tr("User"),
                            tr("Input"),   tr("Paths"), tr("Log"),      tr("Debug")};
    int indexTab = tabNames.indexOf(translatedText);
    if (indexTab == -1)
        indexTab = 0;
    ui->tabWidgetSettings->setCurrentIndex(indexTab);

    ui->motionControlsCheckBox->setChecked(
        toml::find_or<bool>(data, "Input", "isMotionControlsEnabled", true));
    ui->backgroundControllerCheckBox->setChecked(
        toml::find_or<bool>(data, "Input", "backgroundControllerInput", false));
    ui->isDevKitCheckBox->setChecked(toml::find_or<bool>(data, "General", "isDevKit", false));

    ui->isNeoModeCheckBox->setChecked(toml::find_or<bool>(data, "General", "isPS4Pro", false));

    ui->removeFolderButton->setEnabled(!ui->gameFoldersListWidget->selectedItems().isEmpty());
    ui->backgroundImageOpacitySlider->setValue(Config::getBackgroundImageOpacity());
    ui->showBackgroundImageCheckBox->setChecked(Config::getShowBackgroundImage());
    SyncRealTimeWidgetstoConfig();

    backgroundImageOpacitySlider_backup = Config::getBackgroundImageOpacity();
    bgm_volume_backup = Config::getBGMvolume();
    volume_slider_backup = Config::getVolumeSlider();
    fps_backup = Config::getFpsLimit();
}

void SettingsDialog::VolumeSliderChange(int value) {
    ui->volumeText->setText(QString::number(value) + "%");
}

void SettingsDialog::InitializeEmulatorLanguages() {
    QDirIterator it(QStringLiteral(":/translations"), QDirIterator::NoIteratorFlags);

    QVector<QPair<QString, QString>> languagesList;

    while (it.hasNext()) {
        QString locale = it.next();
        locale.truncate(locale.lastIndexOf(QLatin1Char{'.'}));
        locale.remove(0, locale.lastIndexOf(QLatin1Char{'/'}) + 1);
        const QString lang = QLocale::languageToString(QLocale(locale).language());
        const QString country = QLocale::territoryToString(QLocale(locale).territory());

        QString displayName = QStringLiteral("%1 (%2)").arg(lang, country);
        languagesList.append(qMakePair(locale, displayName));
    }

    std::sort(languagesList.begin(), languagesList.end(),
              [](const QPair<QString, QString>& a, const QPair<QString, QString>& b) {
                  return a.second < b.second;
              });

    int idx = 0;
    for (const auto& pair : languagesList) {
        const QString& locale = pair.first;
        const QString& displayName = pair.second;

        ui->emulatorLanguageComboBox->addItem(displayName, locale);
        languages[locale.toStdString()] = idx;
        idx++;
    }

    connect(ui->emulatorLanguageComboBox, &QComboBox::currentIndexChanged, this,
            &SettingsDialog::OnLanguageChanged);
}

void SettingsDialog::OnLanguageChanged(int index) {
    if (index == -1)
        return;

    QString locale = ui->emulatorLanguageComboBox->itemData(index).toString();

    emit LanguageChanged(locale.toStdString());

    QTimer::singleShot(0, this, [this]() { ui->retranslateUi(this); });
}

void SettingsDialog::OnCursorStateChanged(s16 index) {
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

void SettingsDialog::FPSChange(int value) {
    ui->fpsSpinBox->setValue(ui->fpsSlider->value());
}

int SettingsDialog::exec() {
    return QDialog::exec();
}

SettingsDialog::~SettingsDialog() {}

void SettingsDialog::updateNoteTextEdit(const QString& elementName) {
    QString text;

    // clang-format off
    // General
    if (elementName == "consoleLanguageGroupBox") {
        text = tr("Console Language:\\nSets the language that the PS4 game uses.\\nIt's recommended to set this to a language the game supports, which will vary by region.");
    } else if (elementName == "emulatorLanguageGroupBox") {
        text = tr("Emulator Language:\\nSets the language of the emulator's user interface.");
    } else if (elementName == "showSplashCheckBox") {
        text = tr("Show Splash Screen:\\nShows the game's splash screen (a special image) while the game is starting.");
    } else if (elementName == "discordRPCCheckbox") {
        text = tr("Enable Discord Rich Presence:\\nDisplays the emulator icon and relevant information on your Discord profile.");
    } else if (elementName == "userName") {
        text = tr("Username:\\nSets the PS4's account username, which may be displayed by some games.");
    } else if (elementName == "label_Trophy" || elementName == "trophyKeyLineEdit") {
        text = tr("Trophy Key:\\nKey used to decrypt trophies. Must be obtained from your jailbroken console.\\nMust contain only hex characters.");
    } else if (elementName == "logTypeGroupBox") {
        text = tr("Log Type:\\nSets whether to synchronize the output of the log window for performance. May have adverse effects on emulation.");
    } else if (elementName == "logFilter") {
        text = tr("Log Filter:\\nFilters the log to only print specific information.\\nExamples: \"Core:Trace\" \"Lib.Pad:Debug Common.Filesystem:Error\" \"*:Critical\"\\nLevels: Trace, Debug, Info, Warning, Error, Critical - in this order, a specific level silences all levels preceding it in the list and logs every level after it.");
    #ifdef ENABLE_UPDATER
    } else if (elementName == "updaterGroupBox") {
        text = tr("Update:\\nRelease: Official versions released every month that may be very outdated, but are more reliable and tested.\\nNightly: Development versions that have all the latest features and fixes, but may contain bugs and are less stable.");
#endif
    } else if (elementName == "GUIBackgroundImageGroupBox") {
        text = tr("Background Image:\\nControl the opacity of the game background image.");
    } else if (elementName == "GUIMusicGroupBox") {
        text = tr("Play Title Music:\\nIf a game supports it, enable playing special music when selecting the game in the GUI.");
    } else if (elementName == "enableHDRCheckBox") {
        text = tr("Enable HDR:\\nEnables HDR in games that support it.\\nYour monitor must have support for the BT2020 PQ color space and the RGB10A2 swapchain format.");
    } else if (elementName == "disableTrophycheckBox") {
        text = tr("Disable Trophy Pop-ups:\\nDisable in-game trophy notifications. Trophy progress can still be tracked using the Trophy Viewer (right-click the game in the main window).");
    } else if (elementName == "enableCompatibilityCheckBox") {
        text = tr("Display Compatibility Data:\\nDisplays game compatibility information in table view. Enable \"Update Compatibility On Startup\" to get up-to-date information.");
    } else if (elementName == "checkCompatibilityOnStartupCheckBox") {
        text = tr("Update Compatibility On Startup:\\nAutomatically update the compatibility database when shadPS4 starts.");
    } else if (elementName == "updateCompatibilityButton") {
        text = tr("Update Compatibility Database:\\nImmediately update the compatibility database.");
    }

    //User
    if (elementName == "OpenCustomTrophyLocationButton") {
        text = tr("Open the custom trophy images/sounds folder:\\nYou can add custom images to the trophies and an audio.\\nAdd the files to custom_trophy with the following names:\\ntrophy.wav OR trophy.mp3, bronze.png, gold.png, platinum.png, silver.png\\nNote: The sound will only work in QT versions.");
    }

    // Input
    if (elementName == "hideCursorGroupBox") {
        text = tr("Hide Cursor:\\nChoose when the cursor will disappear:\\nNever: You will always see the mouse.\\nidle: Set a time for it to disappear after being idle.\\nAlways: you will never see the mouse.");
    } else if (elementName == "idleTimeoutGroupBox") {
        text = tr("Hide Idle Cursor Timeout:\\nThe duration (seconds) after which the cursor that has been idle hides itself.");
    } else if (elementName == "backgroundControllerCheckBox") {
        text = tr("Enable Controller Background Input:\\nAllow shadPS4 to detect controller inputs when the game window is not in focus.");
    }

    // Graphics
    if (elementName == "graphicsAdapterGroupBox") {
        text = tr("Graphics Device:\\nOn multiple GPU systems, select the GPU the emulator will use from the drop down list,\\nor select \"Auto Select\" to automatically determine it.");
    } else if (elementName == "presentModeGroupBox") {
        text = tr("Present Mode:\\nConfigures how video output will be presented to your screen.\\n\\n"
                  "Mailbox: Frames synchronize with your screen's refresh rate. New frames will replace any pending frames. Reduces latency but may skip frames if running behind.\\n"
                  "Fifo: Frames synchronize with your screen's refresh rate. New frames will be queued behind pending frames. Ensures all frames are presented but may increase latency.\\n"
                  "Immediate: Frames immediately present to your screen when ready. May result in tearing.");
    } else if (elementName == "windowSizeGroupBox") {
        text = tr("Width/Height:\\nSets the size of the emulator window at launch, which can be resized during gameplay.\\nThis is different from the in-game resolution.");
    } else if (elementName == "heightDivider") {
        text = tr("Vblank Frequency:\\nThe frame rate at which the emulator refreshes at (60hz is the baseline, whether the game runs at 30 or 60fps). Changing this may have adverse effects, such as increasing the game speed, or breaking critical game functionality that does not expect this to change!");
    } else if (elementName == "dumpShadersCheckBox") {
        text = tr("Enable Shaders Dumping:\\nFor the sake of technical debugging, saves the games shaders to a folder as they render.");
    } else if (elementName == "nullGpuCheckBox") {
        text = tr("Enable Null GPU:\\nFor the sake of technical debugging, disables game rendering as if there were no graphics card.");
    }

    // Path
    if (elementName == "gameFoldersGroupBox" || elementName == "gameFoldersListWidget") {
        text = tr("Game Folders:\\nThe list of folders to check for installed games.");
    } else if (elementName == "addFolderButton") {
        text = tr("Add:\\nAdd a folder to the list.");
    } else if (elementName == "removeFolderButton") {
        text = tr("Remove:\\nRemove a folder from the list.");
    } else if (elementName == "PortableUserFolderGroupBox") {
        text = tr("Portable user folder:\\nStores shadPS4 settings and data that will be applied only to the shadPS4 build located in the current folder. Restart the app after creating the portable user folder to begin using it.");
    }

    // DLC Folder
    if (elementName == "dlcFolderGroupBox" || elementName == "currentDLCFolder") {
        text = tr("DLC Path:\\nThe folder where game DLC loaded from.");
    } else if (elementName == "folderButton") {
        text = tr("Browse:\\nBrowse for a folder to set as the DLC path.");
    }

    // Save Data
    if (elementName == "saveDataGroupBox" || elementName == "currentSaveDataPath") {
        text = tr("Save Data Path:\\nThe folder where game save data will be saved.");
    } else if (elementName == "browseButton") {
        text = tr("Browse:\\nBrowse for a folder to set as the save data path.");
    }

    // Debug
    if (elementName == "debugDump") {
        text = tr("Enable Debug Dumping:\\nSaves the import and export symbols and file header information of the currently running PS4 program to a directory.");
    } else if (elementName == "vkValidationCheckBox") {
        text = tr("Enable Vulkan Validation Layers:\\nEnables a system that validates the state of the Vulkan renderer and logs information about its internal state.\\nThis will reduce performance and likely change the behavior of emulation.\\nYou need the Vulkan SDK for this to work.");
    } else if (elementName == "vkSyncValidationCheckBox") {
        text = tr("Enable Vulkan Synchronization Validation:\\nEnables a system that validates the timing of Vulkan rendering tasks.\\nThis will reduce performance and likely change the behavior of emulation.\\nYou need the Vulkan SDK for this to work.");
    } else if (elementName == "rdocCheckBox") {
        text = tr("Enable RenderDoc Debugging:\\nIf enabled, the emulator will provide compatibility with Renderdoc to allow capture and analysis of the currently rendered frame.");
    } else if (elementName == "crashDiagnosticsCheckBox") {
        text = tr("Crash Diagnostics:\\nCreates a .yaml file with info about the Vulkan state at the time of crashing.\\nUseful for debugging 'Device lost' errors. If you have this enabled, you should enable Host AND Guest Debug Markers.\\nYou need Vulkan Validation Layers enabled and the Vulkan SDK for this to work.");
    } else if (elementName == "guestMarkersCheckBox") {
        text = tr("Guest Debug Markers:\\nInserts any debug markers the game itself has added to the command buffer.\\nIf you have this enabled, you should enable Crash Diagnostics.\\nUseful for programs like RenderDoc.");
    } else if (elementName == "hostMarkersCheckBox") {
        text = tr("Host Debug Markers:\\nInserts emulator-side information like markers for specific AMDGPU commands around Vulkan commands, as well as giving resources debug names.\\nIf you have this enabled, you should enable Crash Diagnostics.\\nUseful for programs like RenderDoc.");
    } else if (elementName == "copyGPUBuffersCheckBox") {
        text = tr("Copy GPU Buffers:\\nGets around race conditions involving GPU submits.\\nMay or may not help with PM4 type 0 crashes.");
    } else if (elementName == "collectShaderCheckBox") {
        text = tr("Collect Shaders:\\nYou need this enabled to edit shaders with the debug menu (Ctrl + F10).");
    } else if (elementName == "separateLogFilesCheckbox") {
        text = tr("Separate Log Files:\\nWrites a separate logfile for each game.");
    } else if (elementName == "enableLoggingCheckBox") {
        text = tr("Enable Logging:\\nEnables logging.\\nDo not change this if you do not know what you're doing!\\nWhen asking for help, make sure this setting is ENABLED.");
    } else if (elementName == "OpenLogLocationButton") {
        text = tr("Open Log Location:\\nOpen the folder where the log file is saved.");
    } else if (elementName == "micComboBox") {
        text = tr("Microphone:\\nNone: Does not use the microphone.\\nDefault Device: Will use the default device defined in the system.\\nOr manually choose the microphone to be used from the list.");
    } else if (elementName == "volumeSliderElement") {
        text = tr("Volume:\\nAdjust volume for games on a global level, range goes from 0-500% with the default being 100%.");
    } else if (elementName == "chooseHomeTabGroupBox") {
        text = tr("Default tab when opening settings:\\nChoose which tab will open, the default is General.");
    } else if (elementName == "gameSizeCheckBox") {
        text = tr("Show Game Size In List:\\nThere is the size of the game in the list.");
    } else if (elementName == "motionControlsCheckBox") {
        text = tr("Enable Motion Controls:\\nWhen enabled it will use the controller's motion control if supported.");
    } else if (elementName == "ReadbacksLinearCheckBox") {
        text = tr("Enable Readback Linear Images:\\nEnables async downloading of GPU modified linear images.\\nMight fix issues in some games.");
    } else if (elementName == "DMACheckBox") {
        text = tr("Enable Direct Memory Access:\\nEnables arbitrary memory access from the GPU to CPU memory.");
    } else if (elementName == "isNeoModeCheckBox") {
        text = tr("Enable PS4 Neo Mode:\\nAdds support for PS4 Pro emulation and memory size. Currently causes instability in a large number of tested games.");
    } else if (elementName == "isDevKitCheckBox") {
        text = tr("Enable Devkit Console Mode:\\nAdds support for Devkit console memory size.");
    } else if (elementName == "connectedNetworkCheckBox") {
        text = tr("Set Network Connected to True:\\nForces games to detect an active network connection. Actual online capabilities are not yet supported.");
    } else if (elementName == "isPSNSignedInCheckBox") {
        text = tr("Set PSN Signed-in to True:\\nForces games to detect an active PSN sign-in. Actual PSN capabilities are not supported."); 
    }
    // clang-format on
    ui->descriptionText->setText(text.replace("\\n", "\n"));
}

bool SettingsDialog::eventFilter(QObject* obj, QEvent* event) {
    if (event->type() == QEvent::Enter || event->type() == QEvent::Leave) {
        if (qobject_cast<QWidget*>(obj)) {
            bool hovered = (event->type() == QEvent::Enter);
            QString elementName = obj->objectName();

            if (hovered) {
                updateNoteTextEdit(elementName);
            } else {
                ui->descriptionText->setText(defaultTextEdit);
            }
            return true;
        }
    }
    if ((obj == ui->ReadbackSpeedComboBox || obj == ui->MemorySpinBox) &&
        event->type() == QEvent::Wheel) {
        return true;
    }
    return QDialog::eventFilter(obj, event);
}

void SettingsDialog::UpdateSettings() {

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
    Config::setPlayBGM(ui->playBGMCheckBox->isChecked());
    Config::setLoggingEnabled(ui->enableLoggingCheckBox->isChecked());
    Config::setAllowHDR(ui->enableHDRCheckBox->isChecked());
    Config::setEnableAutoBackup(ui->enableAutoBackupCheckBox->isChecked());
    Config::setLogType(logTypeMap.value(ui->logTypeComboBox->currentText()).toStdString());
    Config::setMicDevice(ui->micComboBox->currentData().toString().toStdString());
    Config::setLogFilter(ui->logFilterLineEdit->text().toStdString());
    Config::setUserName(0, ui->userName1LineEdit->text().toStdString());
    Config::setUserName(1, ui->userName2LineEdit->text().toStdString());
    Config::setUserName(2, ui->userName3LineEdit->text().toStdString());
    Config::setUserName(3, ui->userName4LineEdit->text().toStdString());
    Config::setTrophyKey(ui->trophyKeyLineEdit->text().toStdString());
    Config::setCursorState(ui->hideCursorComboBox->currentIndex());
    Config::setCursorHideTimeout(ui->idleTimeoutSpinBox->value());
    Config::setGpuId(ui->graphicsAdapterBox->currentIndex() - 1);
    Config::setBGMvolume(ui->BGMVolumeSlider->value());
    Config::setLanguage(languageIndexes[ui->consoleLanguageComboBox->currentIndex()]);
    Config::setEnableDiscordRPC(ui->discordRPCCheckbox->isChecked());
    Config::setWindowWidth(ui->widthSpinBox->value());
    Config::setWindowHeight(ui->heightSpinBox->value());
    Config::setVblankFreq(ui->vblankSpinBox->value());
    Config::setDumpShaders(ui->dumpShadersCheckBox->isChecked());
    Config::setNullGpu(ui->nullGpuCheckBox->isChecked());
    Config::setSeparateUpdateEnabled(ui->separateUpdatesCheckBox->isChecked());
    Config::setReadbackLinearImages(ui->ReadbacksLinearCheckBox->isChecked());
    Config::setDirectMemoryAccess(ui->DMACheckBox->isChecked());
    Config::setDisableHardcodedHotkeys(ui->HotkeysCheckBox->isChecked());
    Config::setUseHomeButtonForHotkeys(ui->HomeHotkeysCheckBox->isChecked());
    Config::setScreenTipDisable(ui->screenTipBox->isChecked());
    Config::setReadbackSpeed(
        static_cast<Config::ReadbackSpeed>(ui->ReadbackSpeedComboBox->currentIndex()));

    Config::setShaderSkipsEnabled(ui->SkipsCheckBox->isChecked());
    Config::setFpsLimit(ui->fpsSlider->value());
    Config::setFpsLimit(ui->fpsSpinBox->value());
    Config::setFpsLimiterEnabled(ui->fpsLimiterCheckBox->isChecked());

    Config::setExtraDmemInMbytes(ui->MemorySpinBox->value());
    Config::setLoadGameSizeEnabled(ui->gameSizeCheckBox->isChecked());
    Config::setShowSplash(ui->showSplashCheckBox->isChecked());
    Config::setDebugDump(ui->debugDump->isChecked());
    Config::setSeparateLogFilesEnabled(ui->separateLogFilesCheckbox->isChecked());
    Config::setVkValidation(ui->vkValidationCheckBox->isChecked());
    Config::setVkSyncValidation(ui->vkSyncValidationCheckBox->isChecked());
    Config::setRdocEnabled(ui->rdocCheckBox->isChecked());
    Config::setPipelineCacheEnabled(ui->cacheCheckBox->isChecked());
    Config::setPipelineCacheArchived(ui->cacheArchiveCheckBox->isChecked());
    Config::setVkHostMarkersEnabled(ui->hostMarkersCheckBox->isChecked());
    Config::setVkGuestMarkersEnabled(ui->guestMarkersCheckBox->isChecked());
    Config::setVkCrashDiagnosticEnabled(ui->crashDiagnosticsCheckBox->isChecked());
    Config::setCollectShaderForDebug(ui->collectShaderCheckBox->isChecked());
    Config::setCopyGPUCmdBuffers(ui->copyGPUBuffersCheckBox->isChecked());
    Config::setVolumeSlider(ui->horizontalVolumeSlider->value(), true);
    Config::setSysModulesPath(Common::FS::PathFromQString(ui->currentSysModulesPath->text()));

    Config::setAutoUpdate(ui->updateCheckBox->isChecked());
    Config::setAlwaysShowChangelog(ui->changelogCheckBox->isChecked());
    Config::setUpdateChannel(channelMap.value(ui->updateComboBox->currentText()).toStdString());
    Config::setChooseHomeTab(
        chooseHomeTabMap.value(ui->chooseHomeTabComboBox->currentText()).toStdString());
    Config::setCompatibilityEnabled(ui->enableCompatibilityCheckBox->isChecked());
    Config::setCheckCompatibilityOnStartup(ui->checkCompatibilityOnStartupCheckBox->isChecked());
    Config::setBackgroundImageOpacity(ui->backgroundImageOpacitySlider->value());
    emit BackgroundOpacityChanged(ui->backgroundImageOpacitySlider->value());
    Config::setShowBackgroundImage(ui->showBackgroundImageCheckBox->isChecked());
    Config::setFsrEnabled(ui->FSRCheckBox->isChecked());
    Config::setRcasEnabled(ui->RCASCheckBox->isChecked());
    Config::setRcasAttenuation(ui->RCASSpinBox->value());
    Config::setRcasAttenuation(ui->RCASSlider->value());
    Config::setIsConnectedToNetwork(ui->connectedNetworkCheckBox->isChecked());
    Config::setPSNSignedIn(ui->isPSNSignedInCheckBox->isChecked());

    std::vector<Config::GameDirectories> dirs_with_states;
    for (int i = 0; i < ui->gameFoldersListWidget->count(); i++) {
        QListWidgetItem* item = ui->gameFoldersListWidget->item(i);
        QString path_string = item->text();
        auto path = Common::FS::PathFromQString(path_string);
        bool enabled = (item->checkState() == Qt::Checked);

        dirs_with_states.push_back({path, enabled});
    }
    Config::setAllGameDirectories(dirs_with_states);
    for (int p = 1; p <= 4; ++p) {
        int cls = 0;

        for (int c = 1; c <= 4; ++c) {
            if (specialPadChecks[c - 1][p - 1]->isChecked()) {
                cls = c;
                break;
            }
        }

        Config::setSpecialPadClass(p, cls, true);

        Config::setUseSpecialPad(p, cls != 0, true);
    }

#ifdef ENABLE_DISCORD_RPC
    auto* rpc = Common::Singleton<DiscordRPCHandler::RPC>::Instance();
    if (Config::getEnableDiscordRPC()) {
        rpc->init();
        rpc->setStatusIdling();
    } else {
        rpc->shutdown();
    }
#endif

    BackgroundMusicPlayer::getInstance().setVolume(ui->BGMVolumeSlider->value());
}

void SettingsDialog::OnRcasAttenuationChanged(int sliderValue) {
    float attenuation = sliderValue / 1000.0f;

    ui->RCASSpinBox->blockSignals(true);
    ui->RCASSpinBox->setValue(attenuation);
    ui->RCASSpinBox->blockSignals(false);

    Config::setRcasAttenuation(sliderValue);
    if (presenter)
        presenter->GetFsrSettingsRef().rcasAttenuation = attenuation;
}

void SettingsDialog::OnRcasAttenuationSpinBoxChanged(double spinValue) {
    int intValue = static_cast<int>(std::lround(spinValue * 1000.0));

    ui->RCASSlider->blockSignals(true);
    ui->RCASSlider->setValue(intValue);
    ui->RCASSlider->blockSignals(false);

    Config::setRcasAttenuation(intValue);
    if (presenter)
        presenter->GetFsrSettingsRef().rcasAttenuation = static_cast<float>(spinValue);
}

void SettingsDialog::SyncRealTimeWidgetstoConfig() {
    ui->gameFoldersListWidget->clear();

    std::filesystem::path userdir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
    const toml::value data = toml::parse(userdir / "config.toml");

    if (data.contains("GUI")) {
        const toml::value& gui = data.at("GUI");
        const auto directories_array =
            toml::find_or<std::vector<std::u8string>>(gui, "Directories", {});

        std::vector<bool> directories_enabled;
        try {
            directories_enabled = Config::getGameDirectoriesEnabled();
        } catch (...) {
            // If it does not exist, assume that all are enabled.
            directories_enabled.resize(directories_array.size(), true);
        }

        if (directories_enabled.size() < directories_array.size()) {
            directories_enabled.resize(directories_array.size(), true);
        }

        std::vector<Config::GameDirectories> settings_directories_config;

        for (size_t i = 0; i < directories_array.size(); i++) {
            std::filesystem::path dir = directories_array[i];
            bool enabled = directories_enabled[i];

            settings_directories_config.push_back({dir, enabled});

            QString path_string;
            Common::FS::PathToQString(path_string, dir);

            QListWidgetItem* item = new QListWidgetItem(path_string);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(enabled ? Qt::Checked : Qt::Unchecked);
            ui->gameFoldersListWidget->addItem(item);
        }

        Config::setAllGameDirectories(settings_directories_config);
    }

    if (Config::getGameRunning() && m_ipc_client) {
        m_ipc_client->setFsr(Config::getFsrEnabled());
        m_ipc_client->setRcas(Config::getRcasEnabled());
        m_ipc_client->setRcasAttenuation(static_cast<int>(Config::getRcasAttenuation()));
    }
}

void SettingsDialog::pollSDLevents() {
    SDL_Event event;
    while (SdlEventWrapper::Wrapper::wrapperActive) {

        if (!SDL_WaitEvent(&event)) {
            return;
        }

        if (event.type == SDL_EVENT_QUIT) {
            return;
        }

        if (event.type == SDL_EVENT_AUDIO_DEVICE_ADDED) {
            onAudioDeviceChange(true);
        }

        if (event.type == SDL_EVENT_AUDIO_DEVICE_REMOVED) {
            onAudioDeviceChange(false);
        }
    }
}

void SettingsDialog::onAudioDeviceChange(bool isAdd) {
    ui->GenAudioComboBox->clear();
    ui->DsAudioComboBox->clear();

    // prevent device list from refreshing too fast when game not running
    if (!is_game_running && isAdd == false)
        QThread::msleep(100);

    int deviceCount;
    QStringList deviceList;
    SDL_AudioDeviceID* devices = SDL_GetAudioPlaybackDevices(&deviceCount);

    if (!devices) {
        LOG_ERROR(Lib_AudioOut, "Unable to retrieve audio device list {}", SDL_GetError());
        return;
    }

    for (int i = 0; i < deviceCount; ++i) {
        const char* name = SDL_GetAudioDeviceName(devices[i]);
        std::string name_string = std::string(name);
        deviceList.append(QString::fromStdString(name_string));
    }

    ui->GenAudioComboBox->addItem(tr("Default Device"), "Default Device");
    ui->GenAudioComboBox->addItems(deviceList);
    ui->GenAudioComboBox->setCurrentText(QString::fromStdString(Config::getMainOutputDevice()));

    ui->DsAudioComboBox->addItem(tr("Default Device"), "Default Device");
    ui->DsAudioComboBox->addItems(deviceList);
    ui->DsAudioComboBox->setCurrentText(QString::fromStdString(Config::getPadSpkOutputDevice()));

    SDL_free(devices);
}