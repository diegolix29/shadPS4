//  SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include "common/singleton.h"

#include <semaphore>
#include <thread>

class IPC {
    bool enabled{false};
    std::jthread input_thread{};

    std::binary_semaphore run_semaphore{0};
    std::binary_semaphore start_semaphore{0};

    std::string m_gameSerial;
    std::string m_gameVersion;
    std::string m_cheatsDir;

public:
    static IPC& Instance() {
        return *Common::Singleton<IPC>::Instance();
    }

    void Init();

    operator bool() const {
        return enabled;
    }

    [[nodiscard]] bool IsEnabled() const {
        return enabled;
    }

    void WaitForStart() {
        start_semaphore.acquire();
    }

    void SetGameSerial(const std::string& serial) {
        m_gameSerial = serial;
    }
    void SetGameVersion(const std::string& version) {
        m_gameVersion = version;
    }
    void SetCheatsDir(const std::string& cheatsDir) {
        m_cheatsDir = cheatsDir;
    }

private:
    [[noreturn]] void InputLoop();

    void applyCheatByName(const std::string& cheatName);
};
