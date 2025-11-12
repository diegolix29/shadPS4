// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <thread>
#include <unordered_map>
#include <input/input_handler.h>
#include "common/config.h"
#include "common/logging/backend.h"
#include "common/memory_patcher.h"
#include "core/debugger.h"
#include "core/file_sys/fs.h"
#include "core/ipc/ipc_client.h"
#include "core/libraries/audio/audioout.h"
#include "emulator.h"
#include "game_directory_dialog.h"
#include "iostream"
#include "main_window.h"
#include "system_error"
#include "video_core/renderer_vulkan/vk_presenter.h"

#include <QApplication>
#include <QSettings>
#include "welcome_dialog.h"

extern std::unique_ptr<Vulkan::Presenter> presenter;
std::shared_ptr<IpcClient> m_ipc_client;

// Custom message handler to ignore Qt logs
void customMessageHandler(QtMsgType, const QMessageLogContext&, const QString&) {}
void StopProgram() {
    exit(0);
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    QApplication a(argc, argv);
    QApplication::setDesktopFileName("net.shadps4.shadPS4");

    const QString iniPath = QCoreApplication::applicationDirPath() + "/startup.ini";
    QSettings earlySettings(iniPath, QSettings::IniFormat);
    bool skipWelcome = earlySettings.value("skip_welcome", false).toBool();

    if (!skipWelcome) {
        WelcomeDialog welcome;
        if (welcome.exec() == QDialog::Accepted) {
            skipWelcome = welcome.skipOnNextLaunch();
        }
    }

    const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
    Config::load(user_dir / "config.toml");

    bool has_command_line_argument = argc > 1;
    bool show_gui = false;
    bool has_game_argument = false;
    std::string game_path;
    std::vector<std::string> game_args{};
    std::optional<std::filesystem::path> game_folder;
    std::optional<std::filesystem::path> mods_folder;
    bool waitForDebugger = false;
    std::optional<int> waitPid;

    Core::Emulator* emulator = Common::Singleton<Core::Emulator>::Instance();
    emulator->executableName = argv[0];
    emulator->waitForDebuggerBeforeRun = waitForDebugger;

    std::unordered_map<std::string, std::function<void(int&)>> arg_map = {
        {"-h",
         [&](int&) {
             std::cout << "Usage: shadps4 [options]\n...";
             exit(0);
         }},
        {"--help", [&](int& i) { arg_map["-h"](i); }},
        {"-s", [&](int&) { show_gui = true; }},
        {"--show-gui", [&](int& i) { arg_map["-s"](i); }},
        {"-g",
         [&](int& i) {
             if (++i < argc) {
                 game_path = argv[i];
                 has_game_argument = true;
             } else
                 exit(1);
         }},
        {"--game", [&](int& i) { arg_map["-g"](i); }},
        {"-p",
         [&](int& i) {
             if (++i < argc)
                 MemoryPatcher::patchFile = argv[i];
             else
                 exit(1);
         }},
        {"--patch", [&](int& i) { arg_map["-p"](i); }},
        {"-i", [&](int&) { Core::FileSys::MntPoints::ignore_game_patches = true; }},
        {"--ignore-game-patch", [&](int& i) { arg_map["-i"](i); }},
        {"-mf",
         [&](int& i) {
             if (++i < argc) {
                 std::filesystem::path dir(argv[i]);
                 Core::FileSys::MntPoints::enable_mods = true;
                 Core::FileSys::MntPoints::manual_mods_path = dir;
                 mods_folder = dir;
             } else
                 exit(1);
         }},
        {"--mods-folder", [&](int& i) { arg_map["-mf"](i); }},
        {"-f",
         [&](int& i) {
             if (++i < argc) {
                 std::string f = argv[i];
                 Config::setIsFullscreen(f == "true");
             } else
                 exit(1);
         }},
        {"--fullscreen", [&](int& i) { arg_map["-f"](i); }},
        {"--add-game-folder",
         [&](int& i) {
             if (++i < argc) {
                 Config::addGameDirectories(argv[i]);
                 Config::save(user_dir / "config.toml");
                 exit(0);
             } else
                 exit(1);
         }},
        {"--log-append", [&](int& i) { Common::Log::SetAppend(); }},
        {"--config-clean", [&](int& i) { Config::setConfigMode(Config::ConfigMode::Clean); }},
        {"--config-global", [&](int& i) { Config::setConfigMode(Config::ConfigMode::Global); }},
        {"--wait-for-debugger", [&](int& i) { waitForDebugger = true; }},
        {"--wait-for-pid", [&](int& i) {
             if (++i < argc)
                 waitPid = std::stoi(argv[i]);
             else
                 exit(1);
         }}};

    for (int i = 1; i < argc; ++i) {
        std::string cur_arg = argv[i];
        auto it = arg_map.find(cur_arg);
        if (it != arg_map.end()) {
            it->second(i);
            continue;
        }
        if (!has_game_argument && !cur_arg.empty() && cur_arg[0] != '-') {
            game_path = cur_arg;
            has_game_argument = true;
            continue;
        }
        if (cur_arg == "--") {
            for (int j = i + 1; j < argc; j++) {
                game_args.push_back(argv[j]);
            }
            break;
        }
        std::cerr << "Unknown argument: " << cur_arg << "\n";
        return 1;
    }

    if (waitPid.has_value())
        Core::Debugger::WaitForPid(waitPid.value());

    if (Config::getGameDirectoriesEnabled().empty() && !has_command_line_argument) {
        GameDirectoryDialog dlg;
        dlg.exec();
    }

    // Ignore Qt logs
    qInstallMessageHandler(customMessageHandler);

    MainWindow* m_main_window = new MainWindow(nullptr);
    if ((has_command_line_argument && show_gui) || !has_command_line_argument) {
        m_main_window->Init();
    }

    if (has_command_line_argument && !has_game_argument) {
        std::cerr << "Error: Please provide a game path or ID.\n";
        exit(1);
    }

    if (waitPid.has_value()) {
        Core::Debugger::WaitForPid(waitPid.value());
    }

    const bool ipc_enabled = qEnvironmentVariableIsSet("SHADPS4_ENABLE_IPC");
    const bool mods_env_enabled = qEnvironmentVariableIsSet("SHADPS4_ENABLE_MODS") &&
                                  qEnvironmentVariable("SHADPS4_ENABLE_MODS") == "1";

    const bool ignorePatches = qEnvironmentVariableIsSet("SHADPS4_BASE_GAME") &&
                               qEnvironmentVariable("SHADPS4_BASE_GAME") == "1";

    if (!mods_folder.has_value()) {
        Core::FileSys::MntPoints::enable_mods = mods_env_enabled;
    }

    Core::FileSys::MntPoints::ignore_game_patches = ignorePatches;

    if (ipc_enabled) {
        std::cout << ";#IPC_ENABLED\n";
        std::cout << ";ENABLE_MEMORY_PATCH\n";
        std::cout << ";ENABLE_EMU_CONTROL\n";
        std::cout << ";#IPC_END\n";
        std::cout.flush();

        auto next_str = [&]() -> const std::string& {
            static std::string line_buffer;
            do {
                std::getline(std::cin, line_buffer);
            } while (!line_buffer.empty() && line_buffer.back() == '\\');
            return line_buffer;
        };

        auto next_u64 = [&]() -> u64 {
            auto& str = next_str();
            return std::stoull(str, nullptr, 0);
        };

        std::thread([emulator, ipc_client = m_ipc_client, &next_str, &next_u64]() {
            std::string cmd;
            while (std::getline(std::cin, cmd)) {
                if (cmd == "PATCH_MEMORY") {
                    std::string modName, offset, value, target, size, isOffset, littleEndian, mask,
                        maskOffset;
                    std::getline(std::cin, modName);
                    std::getline(std::cin, offset);
                    std::getline(std::cin, value);
                    std::getline(std::cin, target);
                    std::getline(std::cin, size);
                    std::getline(std::cin, isOffset);
                    std::getline(std::cin, littleEndian);
                    std::getline(std::cin, mask);
                    std::getline(std::cin, maskOffset);

                    MemoryPatcher::ApplyRuntimePatch(modName, offset, value, target, size,
                                                     isOffset == "1", littleEndian == "1",
                                                     std::stoi(mask), std::stoi(maskOffset));
                } else if (cmd == "PAUSE" || cmd == "RESUME") {
                    SDL_Event e;
                    e.type = SDL_EVENT_TOGGLE_PAUSE;
                    SDL_PushEvent(&e);
                } else if (cmd == "ADJUST_VOLUME") {
                    int value = static_cast<int>(std::stoull(next_str(), nullptr, 0));
                    Config::setVolumeSlider(value);
                    Libraries::AudioOut::AdjustVol();
                } else if (cmd == "SET_FSR") {
                    bool use_fsr = std::stoull(next_str(), nullptr, 0) != 0;
                    if (presenter)
                        presenter->GetFsrSettingsRef().enable = use_fsr;
                } else if (cmd == "SET_RCAS") {
                    bool use_rcas = std::stoull(next_str(), nullptr, 0) != 0;
                    if (presenter)
                        presenter->GetFsrSettingsRef().use_rcas = use_rcas;
                } else if (cmd == "SET_RCAS_ATTENUATION") {
                    int value = static_cast<int>(std::stoull(next_str(), nullptr, 0));
                    if (presenter)
                        presenter->GetFsrSettingsRef().rcasAttenuation =
                            static_cast<float>(value / 1000.0f);
                } else if (cmd == "RELOAD_INPUTS") {
                    std::string config = next_str();
                    Input::ParseInputConfig(config);
                } else if (cmd == "SET_ACTIVE_CONTROLLER") {
                    std::string active_controller = next_str();
                    GamepadSelect::SetSelectedGamepad(active_controller);
                    SDL_Event checkGamepad;
                    SDL_memset(&checkGamepad, 0, sizeof(checkGamepad));
                    checkGamepad.type = SDL_EVENT_CHANGE_CONTROLLER;
                    SDL_PushEvent(&checkGamepad);
                } else if (cmd == "STOP") {
                    if (!Config::getGameRunning())
                        continue;
                    if (ipc_client)
                        ipc_client->stopGame();
                    Config::setGameRunning(false);
                } else if (cmd == "RESTART") {
                    if (!Config::getGameRunning())
                        continue;
                    if (ipc_client)
                        ipc_client->restartGame();
                }
            }
        }).detach();
    }

    if (has_game_argument) {
        std::filesystem::path game_file_path(game_path);
        if (!std::filesystem::exists(game_file_path)) {
            bool found = false;
            const int max_depth = 5;
            for (const auto& dir : Config::getGameDirectories()) {
                if (auto p = Common::FS::FindGameByID(dir, game_path, max_depth)) {
                    game_file_path = *p;
                    found = true;
                    break;
                }
            }
            if (!found) {
                std::cerr << "Error: Game not found: " << game_path << "\n";
                return 1;
            }
        }
        emulator->Run(game_file_path.string(), game_args, game_folder);
        if (!show_gui)
            return 0;
    }

    m_main_window->show();
    return a.exec();
}
