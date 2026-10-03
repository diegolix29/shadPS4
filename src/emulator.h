// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/singleton.h"
#include "core/linker.h"
#include "input/controller.h"
#include "sdl_window.h"

namespace Core {

using HLEInitDef = void (*)(Core::Loader::SymbolsResolver* sym);

struct SysModules {
    std::string_view module_name;
    HLEInitDef callback;
};

class Emulator {
public:
    Emulator();
    ~Emulator();

    void Run(std::filesystem::path file, std::vector<std::string> args = {},
             std::optional<std::filesystem::path> game_folder = {},
             std::vector<std::pair<std::filesystem::path, std::string>> mounts = {},
             const std::vector<std::string>& env_vars = {});
    void UpdatePlayTime(const std::string& serial);

    void Restart(std::filesystem::path eboot_path, const std::vector<std::string>& guest_args = {},
                 std::filesystem::path game_root = {});

    /**
     * Launches a new emulator process with the supplied CLI arguments, then terminates this
     * process. The new process waits for this one to exit before initializing.
     */
    [[noreturn]] void Relaunch(std::vector<std::string> args);

    const char* executableName;
    bool waitForDebuggerBeforeRun{false};
    void Shutdown();

private:
    void LoadSystemModules(const std::string& game_serial);

    Core::MemoryManager* memory;
    Input::GameControllers* controllers;
    Core::Linker* linker;
    std::unique_ptr<Frontend::WindowSDL> window;
    std::chrono::steady_clock::time_point start_time;
    std::jthread play_time_thread;
};

} // namespace Core