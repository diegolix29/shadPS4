// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "functional"
#include "iostream"
#include "string"
#include "system_error"
#include "unordered_map"

#include <fmt/core.h>
#include "common/config.h"
#include "common/logging/backend.h"
#include "common/memory_patcher.h"
#include "common/path_util.h"
#include "core/debugger.h"
#include "core/file_sys/fs.h"
#include "core/ipc/ipc.h"
#include "emulator.h"

#ifdef _WIN32
#include <windows.h>
#endif

// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include <fmt/core.h>
#include "common/config.h"
#include "common/logging/backend.h"
#include "common/memory_patcher.h"
#include "common/path_util.h"
#include "core/debugger.h"
#include "core/file_sys/fs.h"
#include "core/ipc/ipc.h"
#include "emulator.h"

#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    IPC::Instance().Init();

    // Load configurations
    const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
    Config::load(user_dir / "config.toml");

    bool has_game_argument = false;
    std::string game_path;
    std::vector<std::string> game_args{};
    std::optional<std::filesystem::path> game_folder;

    // Emulator args
    bool has_emulator_argument = false;
    std::string emulator_path;
    std::vector<std::string> emulator_args{};

    bool waitForDebugger = false;
    std::optional<int> waitPid;

    // Map of argument strings to lambda functions
    std::unordered_map<std::string, std::function<void(int&)>> arg_map = {
        {"-h",
         [&](int&) {
             std::cout
                 << "Usage: shadps4 [options] <elf or eboot.bin path>\n"
                    "Options:\n"
                    "  -g, --game <path|ID>          Specify game path to launch\n"
                    " -- ...                         Parameters passed to the game ELF. "
                    "Needs to be at the end of the line, and everything after \"--\" is a "
                    "game argument.\n"
                    "  -e, --emulator <path>         Specify emulator executable path\n"
                    "  -p, --patch <patch_file>      Apply specified patch file\n"
                    "  -i, --ignore-game-patch       Disable automatic loading of game patch\n"
                    "  -f, --fullscreen <true|false> Specify window initial fullscreen "
                    "state. Does not overwrite the config file.\n"
                    "  --add-game-folder <folder>    Adds a new game folder to the config.\n"
                    "  --set-addon-folder <folder>   Sets the addon folder to the config.\n"
                    "  --log-append                  Append log output to file instead of "
                    "overwriting it.\n"
                    "  --override-root <folder>      Override the game root folder. Default is the "
                    "parent of game path\n"
                    "  --wait-for-debugger           Wait for debugger to attach\n"
                    "  --wait-for-pid <pid>          Wait for process with specified PID to stop\n"
                    "  --config-clean                Run the emulator with the default config "
                    "values, ignores the config file(s) entirely.\n"
                    "  --config-global               Run the emulator with the base config file "
                    "only, ignores game specific configs.\n"
                    "  -h, --help                    Display this help message\n";
             exit(0);
         }},
        {"--help", [&](int& i) { arg_map["-h"](i); }},

        {"-g",
         [&](int& i) {
             if (i + 1 < argc) {
                 game_path = argv[++i];
                 has_game_argument = true;
             } else {
                 std::cerr << "Error: Missing argument for -g/--game\n";
                 exit(1);
             }
         }},
        {"--game", [&](int& i) { arg_map["-g"](i); }},

        {"-e",
         [&](int& i) {
             if (i + 1 < argc) {
                 emulator_path = argv[++i];
                 has_emulator_argument = true;
             } else {
                 std::cerr << "Error: Missing argument for -e/--emulator\n";
                 exit(1);
             }
         }},
        {"--emulator", [&](int& i) { arg_map["-e"](i); }},

        {"-p",
         [&](int& i) {
             if (i + 1 < argc) {
                 MemoryPatcher::patchFile = argv[++i];
             } else {
                 std::cerr << "Error: Missing argument for -p/--patch\n";
                 exit(1);
             }
         }},
        {"--patch", [&](int& i) { arg_map["-p"](i); }},

        {"-i", [&](int&) { Core::FileSys::MntPoints::ignore_game_patches = true; }},
        {"--ignore-game-patch", [&](int& i) { arg_map["-i"](i); }},

        {"-f",
         [&](int& i) {
             if (++i >= argc) {
                 std::cerr << "Error: Missing argument for -f/--fullscreen\n";
                 exit(1);
             }
             std::string f_param(argv[i]);
             bool is_fullscreen;
             if (f_param == "true") {
                 is_fullscreen = true;
             } else if (f_param == "false") {
                 is_fullscreen = false;
             } else {
                 std::cerr
                     << "Error: Invalid argument for -f/--fullscreen. Use 'true' or 'false'.\n";
                 exit(1);
             }
             Config::setIsFullscreen(is_fullscreen);
         }},
        {"--fullscreen", [&](int& i) { arg_map["-f"](i); }},

        {"--add-game-folder",
         [&](int& i) {
             if (++i >= argc) {
                 std::cerr << "Error: Missing argument for --add-game-folder\n";
                 exit(1);
             }
             std::filesystem::path config_path(argv[i]);
             std::error_code discard;
             if (!std::filesystem::exists(config_path, discard)) {
                 std::cerr << "Error: Directory does not exist: " << config_path << "\n";
                 exit(1);
             }
             Config::addGameDirectories(config_path);
             Config::save(user_dir / "config.toml");
             std::cout << "Game folder successfully saved.\n";
             exit(0);
         }},

        {"--set-addon-folder",
         [&](int& i) {
             if (++i >= argc) {
                 std::cerr << "Error: Missing argument for --set-addon-folder\n";
                 exit(1);
             }
             std::filesystem::path config_path(argv[i]);
             std::error_code discard;
             if (!std::filesystem::exists(config_path, discard)) {
                 std::cerr << "Error: Directory does not exist: " << config_path << "\n";
                 exit(1);
             }
             Config::setAddonDirectories(config_path);
             Config::save(user_dir / "config.toml");
             std::cout << "Addon folder successfully saved.\n";
             exit(0);
         }},

        {"--log-append", [&](int&) { Common::Log::SetAppend(); }},
        {"--config-clean", [&](int&) { Config::setConfigMode(Config::ConfigMode::Clean); }},
        {"--config-global", [&](int&) { Config::setConfigMode(Config::ConfigMode::Global); }},

        {"--override-root",
         [&](int& i) {
             if (++i >= argc) {
                 std::cerr << "Error: Missing argument for --override-root\n";
                 exit(1);
             }
             std::filesystem::path folder(argv[i]);
             if (!std::filesystem::exists(folder) || !std::filesystem::is_directory(folder)) {
                 std::cerr << "Error: Folder does not exist: " << folder << "\n";
                 exit(1);
             }
             game_folder = folder;
         }},

        {"--wait-for-debugger", [&](int&) { waitForDebugger = true; }},
        {"--wait-for-pid",
         [&](int& i) {
             if (++i >= argc) {
                 std::cerr << "Error: Missing argument for --wait-for-pid\n";
                 exit(1);
             }
             waitPid = std::stoi(argv[i]);
         }},
    };

    if (argc == 1) {
        int dummy = 0;
        arg_map.at("-h")(dummy);
        return -1;
    }

    // Parse command-line arguments
    for (int i = 1; i < argc; ++i) {
        std::string cur_arg = argv[i];
        auto it = arg_map.find(cur_arg);
        if (it != arg_map.end()) {
            it->second(i);
        } else if (i == argc - 1 && !has_game_argument) {
            game_path = argv[i];
            has_game_argument = true;
        } else if (std::string(argv[i]) == "--") {
            for (int j = i + 1; j < argc; ++j) {
                if (has_emulator_argument)
                    emulator_args.push_back(argv[j]);
                else
                    game_args.push_back(argv[j]);
            }
            break;
        } else if (i + 1 < argc && std::string(argv[i + 1]) == "--") {
            if (!has_game_argument) {
                game_path = argv[i];
                has_game_argument = true;
            }
            break;
        } else {
            std::cerr << "Unknown argument: " << cur_arg << "\n";
        }
    }

    // If emulator path is provided but no game path, treat emulator as game
    if (has_emulator_argument && !has_game_argument) {
        game_path = emulator_path;
        has_game_argument = true;
        for (const auto& arg : emulator_args)
            game_args.push_back(arg);
    }

    // Validate game path
    std::filesystem::path eboot_path(game_path);
    if (!std::filesystem::exists(eboot_path)) {
        bool game_found = false;
        const int max_depth = 5;
        for (const auto& install_dir : Config::getGameDirectories()) {
            if (auto found_path = Common::FS::FindGameByID(install_dir, game_path, max_depth)) {
                eboot_path = *found_path;
                game_found = true;
                break;
            }
        }
        if (!game_found) {
            std::cerr << "Error: Game ID or file path not found: " << game_path << "\n";
            return 1;
        }
    }

    if (waitPid.has_value()) {
        Core::Debugger::WaitForPid(waitPid.value());
    }

    // Run the emulator
    Core::Emulator* emulator = Common::Singleton<Core::Emulator>::Instance();
    emulator->executableName = argv[0];
    emulator->waitForDebuggerBeforeRun = waitForDebugger;
    emulator->Run(eboot_path, game_args, game_folder);

    return 0;
}
