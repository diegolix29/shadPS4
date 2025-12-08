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
#include <SDL3/SDL_messagebox.h>
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

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    IPC::Instance().Init();

    const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
    Config::load(user_dir / "config.toml");

    bool has_game_argument = false;
    bool has_emulator_argument = false;
    bool show_gui = false;
    bool no_ipc = false;
    bool waitForDebugger = false;

    std::string game_path;
    std::string emulator_path;
    std::vector<std::string> game_args{};
    std::vector<std::string> emulator_args{};
    std::optional<std::filesystem::path> game_folder;
    std::optional<std::filesystem::path> mods_folder;
    std::optional<int> waitPid;

    std::unordered_map<std::string, std::function<void(int&)>> arg_map = {
        {"-h",
         [&](int&) {
             std::cout << "Usage: shadps4 [options] <elf or eboot.bin path>\n"
                          "Options:\n"
                          "  -g, --game <path|ID>          Specify game path or ID to launch\n"
                          "  -e, --emulator <path|name>    Specify emulator executable path or "
                          "version name\n"
                          "  -d                            Alias for '-e default'\n"
                          "  -p, --patch <file>            Apply specified patch file\n"
                          "  -mf, --mods-folder <folder>   Specify mods folder path\n"
                          "  -i, --ignore-game-patch       Disable automatic game patch loading\n"
                          "  -f, --fullscreen <true|false> Set initial fullscreen state (does not "
                          "overwrite config)\n"
                          "  -s, --show-gui                Show GUI mode\n"
                          "  -I, --no-ipc                  Disable IPC subsystem\n"
                          "  --show-fps                    Enable FPS counter display at startup\n"
                          "  --add-game-folder <folder>    Add a new game folder to config\n"
                          "  --set-addon-folder <folder>   Set the addon folder in config\n"
                          "  --log-append                  Append logs instead of overwriting\n"
                          "  --override-root <folder>      Override game root directory\n"
                          "  --wait-for-debugger           Wait for debugger attach\n"
                          "  --wait-for-pid <pid>          Wait for process with given PID\n"
                          "  --config-clean                Use clean config (ignore all files)\n"
                          "  --config-global               Use only global config file\n"
                          "  -- ...                        Pass remaining args to the game ELF or "
                          "emulator\n"
                          "  -h, --help                    Show this help message\n";
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
        {"-d",
         [&](int&) {
             emulator_path = "default";
             has_emulator_argument = true;
         }},

        {"-f",
         [&](int& i) {
             if (++i >= argc) {
                 std::cerr << "Error: Missing argument for -f/--fullscreen\n";
                 exit(1);
             }
             std::string val(argv[i]);
             if (val == "true")
                 Config::setIsFullscreen(true);
             else if (val == "false")
                 Config::setIsFullscreen(false);
             else {
                 std::cerr << "Error: Invalid fullscreen argument, use 'true' or 'false'.\n";
                 exit(1);
             }
         }},
        {"--fullscreen", [&](int& i) { arg_map["-f"](i); }},

        {"-s", [&](int&) { show_gui = true; }},
        {"--show-gui", [&](int& i) { arg_map["-s"](i); }},
        {"-I", [&](int&) { no_ipc = true; }},
        {"--no-ipc", [&](int& i) { arg_map["-I"](i); }},

        {"-p",
         [&](int& i) {
             if (i + 1 < argc)
                 MemoryPatcher::patch_file = argv[++i];
             else {
                 std::cerr << "Error: Missing argument for -p/--patch\n";
                 exit(1);
             }
         }},
        {"--patch", [&](int& i) { arg_map["-p"](i); }},
        {"-i", [&](int&) { Core::FileSys::MntPoints::ignore_game_patches = true; }},
        {"--ignore-game-patch", [&](int& i) { arg_map["-i"](i); }},
        {"-im",
         [&](int&) {
             Core::FileSys::MntPoints::enable_mods = false;
             Core::FileSys::MntPoints::manual_mods_path.clear();
         }},
        {"--ignore-mods-path", [&](int& i) { arg_map["-im"](i); }},
        {"-mf",
         [&](int& i) {
             if (i + 1 < argc) {
                 std::filesystem::path dir(argv[++i]);
                 if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
                     std::cerr << "Error: Invalid mods folder: " << dir << "\n";
                     exit(1);
                 }
                 Core::FileSys::MntPoints::enable_mods = true;
                 Core::FileSys::MntPoints::manual_mods_path = dir;
                 std::cout << "Mods folder set: " << dir << "\n";
             } else {
                 std::cerr << "Error: Missing argument for -mf/--mods-folder\n";
                 exit(1);
             }
         }},
        {"--mods-folder", [&](int& i) { arg_map["-mf"](i); }},

        {"--config-clean", [&](int&) { Config::setConfigMode(Config::ConfigMode::Clean); }},
        {"--config-global", [&](int&) { Config::setConfigMode(Config::ConfigMode::Global); }},

        {"--add-game-folder",
         [&](int& i) {
             if (++i >= argc)
                 exit((std::cerr << "Error: Missing argument for --add-game-folder\n", 1));
             std::filesystem::path dir(argv[i]);
             if (!std::filesystem::exists(dir))
                 exit((std::cerr << "Error: Directory not found: " << dir << "\n", 1));
             Config::addGameDirectories(dir);
             Config::save(user_dir / "config.toml");
             std::cout << "Game folder saved.\n";
             exit(0);
         }},
        {"--set-addon-folder",
         [&](int& i) {
             if (++i >= argc)
                 exit((std::cerr << "Error: Missing argument for --set-addon-folder\n", 1));
             std::filesystem::path dir(argv[i]);
             if (!std::filesystem::exists(dir))
                 exit((std::cerr << "Error: Directory not found: " << dir << "\n", 1));
             Config::setAddonDirectories(dir);
             Config::save(user_dir / "config.toml");
             std::cout << "Addon folder saved.\n";
             exit(0);
         }},

        {"--log-append", [&](int&) { Common::Log::SetAppend(); }},
        {"--override-root",
         [&](int& i) {
             if (++i >= argc)
                 exit((std::cerr << "Error: Missing argument for --override-root\n", 1));
             std::filesystem::path folder(argv[i]);
             if (!std::filesystem::exists(folder) || !std::filesystem::is_directory(folder))
                 exit((std::cerr << "Error: Invalid folder: " << folder << "\n", 1));
             game_folder = folder;
         }},

        {"--wait-for-debugger", [&](int& i) { waitForDebugger = true; }},
        {"--wait-for-pid",
         [&](int& i) {
             if (++i >= argc) {
                 std::cerr << "Error: Missing argument for --wait-for-pid\n";
                 exit(1);
             }
             waitPid = std::stoi(argv[i]);
         }},
        {"--show-fps", [&](int& i) { Config::setShowFpsCounter(true); }}};

    if (argc == 1) {
        if (!SDL_ShowSimpleMessageBox(
                SDL_MESSAGEBOX_INFORMATION, "shadPS4",
                "This is a CLI application. Please use the QTLauncher for a GUI.", nullptr))
            std::cerr << "Could not display SDL message box! Error: " << SDL_GetError() << "\n";
        int dummy = 0; // one does not simply pass 0 directly
        arg_map.at("-h")(dummy);
        return -1;
    }

    for (int i = 1; i < argc; ++i) {
        std::string cur_arg = argv[i];
        auto it = arg_map.find(cur_arg);
        if (it != arg_map.end()) {
            it->second(i);
        } else if (std::string(argv[i]) == "--") {
            for (int j = i + 1; j < argc; ++j)
                (has_emulator_argument ? emulator_args : game_args).push_back(argv[j]);
            break;
        } else if (i == argc - 1 && !has_game_argument) {
            game_path = argv[i];
            has_game_argument = true;
        } else {
            std::cerr << "Unknown argument: " << cur_arg << "\n";
        }
    }

    if (has_emulator_argument && !has_game_argument) {
        game_path = emulator_path;
        has_game_argument = true;
        for (const auto& a : emulator_args)
            game_args.push_back(a);
    }

    std::filesystem::path eboot_path(game_path);
    if (!std::filesystem::exists(eboot_path)) {
        bool found = false;
        const int max_depth = 5;
        for (const auto& dir : Config::getGameDirectories()) {
            if (auto found_path = Common::FS::FindGameByID(dir, game_path, max_depth)) {
                eboot_path = *found_path;
                found = true;
                break;
            }
        }
        if (!found) {
            std::cerr << "Error: Game not found: " << game_path << "\n";
            return 1;
        }
    }

    if (!mods_folder.has_value()) {
        auto base_folder = eboot_path.parent_path();
        auto parent = base_folder.parent_path();
        auto game_folder_name = base_folder.filename().string();
        auto auto_mods_folder = parent / (game_folder_name + "-MODS");
        if (std::filesystem::exists(auto_mods_folder) &&
            std::filesystem::is_directory(auto_mods_folder)) {
            mods_folder = auto_mods_folder;
            Core::FileSys::MntPoints::enable_mods = true;
            std::cout << "Auto-detected mods folder: " << auto_mods_folder << "\n";
        }
    } else {
        std::cout << "Using manually specified mods folder: " << mods_folder->string() << "\n";
    }

    if (!Core::FileSys::MntPoints::enable_mods) {
        mods_folder.reset();
        Core::FileSys::MntPoints::manual_mods_path.clear();
    }

    if (waitPid)
        Core::Debugger::WaitForPid(*waitPid);

    Core::Emulator* emulator = Common::Singleton<Core::Emulator>::Instance();
    emulator->executableName = argv[0];
    emulator->waitForDebuggerBeforeRun = waitForDebugger;
    emulator->Run(eboot_path, game_args, game_folder);

    return 0;
}
