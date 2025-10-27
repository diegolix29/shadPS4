// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
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

    // Predefine lambdas so aliases can reuse them safely
    std::function<void(int&)> help_fn;
    std::function<void(int&)> game_fn;
    std::function<void(int&)> emulator_fn;
    std::function<void(int&)> patch_fn;
    std::function<void(int&)> fullscreen_fn;

    help_fn = [&](int&) {
        std::cout
            << "Usage: shadps4 [options] <elf or eboot.bin path>\n"
               "Options:\n"
               "  -g, --game <path|ID>          Specify game path to launch\n"
               "  -- ...                        Parameters passed to the game ELF. "
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
        std::exit(0);
    };

    game_fn = [&](int& i) {
        if (i + 1 < argc) {
            game_path = argv[++i];
            has_game_argument = true;
        } else {
            std::cerr << "Error: Missing argument for -g/--game\n";
            std::exit(1);
        }
    };

    emulator_fn = [&](int& i) {
        if (i + 1 < argc) {
            emulator_path = argv[++i];
            has_emulator_argument = true;
        } else {
            std::cerr << "Error: Missing argument for -e/--emulator\n";
            std::exit(1);
        }
    };

    patch_fn = [&](int& i) {
        if (i + 1 < argc) {
            MemoryPatcher::patchFile = std::string(argv[++i]);
        } else {
            std::cerr << "Error: Missing argument for -p/--patch\n";
            std::exit(1);
        }
    };

    fullscreen_fn = [&](int& i) {
        if (++i >= argc) {
            std::cerr << "Error: Missing argument for -f/--fullscreen\n";
            std::exit(1);
        }
        std::string f_param(argv[i]);
        if (f_param == "true") {
            Config::setIsFullscreen(true);
        } else if (f_param == "false") {
            Config::setIsFullscreen(false);
        } else {
            std::cerr << "Error: Invalid argument for -f/--fullscreen. Use 'true' or 'false'.\n";
            std::exit(1);
        }
    };

    // Build argument map (aliases reuse the pre-defined lambdas)
    std::unordered_map<std::string, std::function<void(int&)>> arg_map = {
        {"-h", help_fn},
        {"--help", help_fn},
        {"-g", game_fn},
        {"--game", game_fn},
        {"-e", emulator_fn},
        {"--emulator", emulator_fn},
        {"-p", patch_fn},
        {"--patch", patch_fn},
        {"-i", [&](int&) { Core::FileSys::MntPoints::ignore_game_patches = true; }},
        {"--ignore-game-patch", [&](int& i) { arg_map.at("-i")(i); }},
        {"-f", fullscreen_fn},
        {"--fullscreen", fullscreen_fn},

        {"--add-game-folder",
         [&](int& i) {
             if (++i >= argc) {
                 std::cerr << "Error: Missing argument for --add-game-folder\n";
                 std::exit(1);
             }
             std::filesystem::path config_path(argv[i]);
             std::error_code discard;
             if (!std::filesystem::exists(config_path, discard)) {
                 std::cerr << "Error: Directory does not exist: " << config_path << "\n";
                 std::exit(1);
             }
             Config::addGameDirectories(config_path);
             Config::save(user_dir / "config.toml");
             std::cout << "Game folder successfully saved.\n";
             std::exit(0);
         }},

        {"--set-addon-folder",
         [&](int& i) {
             if (++i >= argc) {
                 std::cerr << "Error: Missing argument for --set-addon-folder\n";
                 std::exit(1);
             }
             std::filesystem::path config_path(argv[i]);
             std::error_code discard;
             if (!std::filesystem::exists(config_path, discard)) {
                 std::cerr << "Error: Directory does not exist: " << config_path << "\n";
                 std::exit(1);
             }
             Config::setAddonDirectories(config_path);
             Config::save(user_dir / "config.toml");
             std::cout << "Addon folder successfully saved.\n";
             std::exit(0);
         }},

        {"--log-append", [&](int&) { Common::Log::SetAppend(); }},
        {"--config-clean", [&](int&) { Config::setConfigMode(Config::ConfigMode::Clean); }},
        {"--config-global", [&](int&) { Config::setConfigMode(Config::ConfigMode::Global); }},

        {"--override-root",
         [&](int& i) {
             if (++i >= argc) {
                 std::cerr << "Error: Missing argument for --override-root\n";
                 std::exit(1);
             }
             std::filesystem::path folder(argv[i]);
             if (!std::filesystem::exists(folder) || !std::filesystem::is_directory(folder)) {
                 std::cerr << "Error: Folder does not exist: " << folder << "\n";
                 std::exit(1);
             }
             game_folder = folder;
         }},

        {"--wait-for-debugger", [&](int&) { waitForDebugger = true; }},
        {"--wait-for-pid",
         [&](int& i) {
             if (++i >= argc) {
                 std::cerr << "Error: Missing argument for --wait-for-pid\n";
                 std::exit(1);
             }
             try {
                 waitPid = std::stoi(argv[i]);
             } catch (const std::exception& e) {
                 std::cerr << "Error: Invalid pid for --wait-for-pid: " << argv[i] << "\n";
                 std::exit(1);
             }
         }},
    };

    if (argc == 1) {
        int dummy = 0;
        help_fn(dummy);
        return -1;
    }

    // Parse command-line arguments
    for (int i = 1; i < argc; ++i) {
        std::string cur_arg = argv[i];

        // handle standalone "--" which sends remaining args to game or emulator
        if (cur_arg == "--") {
            for (int j = i + 1; j < argc; ++j) {
                if (has_emulator_argument)
                    emulator_args.push_back(argv[j]);
                else
                    game_args.push_back(argv[j]);
            }
            break;
        }

        auto it = arg_map.find(cur_arg);
        if (it != arg_map.end()) {
            it->second(i);
        } else if (i == argc - 1 && !has_game_argument) {
            // last non-option argument is the game path
            game_path = argv[i];
            has_game_argument = true;
        } else if (i + 1 < argc && std::string(argv[i + 1]) == "--") {
            // argument followed by -- means this argument is the game path
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
