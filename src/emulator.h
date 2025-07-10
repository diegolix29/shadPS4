// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <thread>
#include "common/elf_info.h"
#ifdef ENABLE_QT_GUI
#include <QString>
#endif
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

    void Run(std::filesystem::path file, const std::vector<std::string> args);
    void StopEmulation();
    void UpdatePlayTime(const std::string& serial) const;
    static Emulator& GetInstance();
    void Restart();
    std::atomic<bool> is_running{false};

private:
    void LoadSystemModules(const std::string& game_serial);
    Common::ElfInfo game_info;
    std::unique_ptr<Frontend::WindowSDL> window = nullptr;
#ifdef ENABLE_QT_GUI
    QString lastEbootPath;
    void saveLastEbootPath(const QString& path);
    QString getLastEbootPath() const;
#endif
    Core::MemoryManager* memory;
    Input::GameController* controller;
    Core::Linker* linker;
    std::chrono::steady_clock::time_point start_time;
};

} // namespace Core
