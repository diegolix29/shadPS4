//  SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#include "ipc.h"

#include <iostream>
#include <string>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "common/memory_patcher.h"
#include "common/thread.h"
#include "common/types.h"

/**
 * Protocol summary:
 * - IPC is enabled by setting the SHADPS4_ENABLE_IPC environment variable to "true"
 * - Input will be stdin & output stderr
 * - Strings are sent as UTF8
 * - Each communication line is terminated by a newline character ('\n')
 * - Each command parameter will be separated by a newline character ('\n'),
 *   variadic commands will start sending the number of parameters after the cmd word.
 *   Any ('\n') in the parameter must be escaped by a backslash ('\\n')
 * - Numbers can be sent with any base. Must prefix the number with '0x' for hex,
 *   '0b' for binary, or '0' for octal. Decimal numbers
 *   will be sent without any prefix.
 * - Output will be started by (';')
 * - The IPC server(this) will send a block started by
 *   #IPC_ENABLED
 *   and ended by
 *   #IPC_END
 *   In between, it will send the current capabilities and commands before the emulator start
 * - The IPC client(e.g., launcher) will send RUN then START to conintue the execution
 **/

/**
 * Command list:
 * - CAPABILITIES:
 *   - ENABLE_MEMORY_PATCH: enables PATCH_MEMORY command
 * - INPUT CMD:
 *   - RUN: start the emulator execution
 *   - START: start the game execution
 *   - PATCH_MEMORY(
 *       modName: str, offset: str, value: str,
 *       target: str, size: str, isOffset: number, littleEndian: number,
 *       patchMask: number, maskOffset: number
 *     ): add a memory patch, check @ref MemoryPatcher::PatchMemory for details
 * - OUTPUT CMD:
 *   - N/A
 **/

void IPC::Init() {
    const char* enabledEnv = std::getenv("SHADPS4_ENABLE_IPC");
    enabled = enabledEnv && strcmp(enabledEnv, "true") == 0;
    if (!enabled) {
        return;
    }

    input_thread = std::jthread([this] {
        Common::SetCurrentThreadName("IPC Read thread");
        this->InputLoop();
    });

    std::cerr << ";#IPC_ENABLED\n";
    std::cerr << ";ENABLE_MEMORY_PATCH\n";
    std::cerr << ";#IPC_END\n";
    std::cerr.flush();

    const auto ok = run_semaphore.try_acquire_for(std::chrono::seconds(5));
    if (!ok) {
        std::cerr << "IPC: Failed to acquire run semaphore, closing process.\n";
        exit(1);
    }
}

void IPC::InputLoop() {
    auto next_str = [&] -> const std::string& {
        static std::string line_buffer;
        do {
            std::getline(std::cin, line_buffer, '\n');
        } while (!line_buffer.empty() && line_buffer.back() == '\\');
        return line_buffer;
    };
    auto next_u64 = [&] -> u64 {
        auto& str = next_str();
        return std::stoull(str, nullptr, 0);
    };

    while (true) {
        auto& cmd = next_str();
        if (cmd.empty()) {
            continue;
        }
        if (cmd == "RUN") {
            run_semaphore.release();
        } else if (cmd == "START") {
            start_semaphore.release();
        } else if (cmd == "PATCH_MEMORY") {
        } else if (cmd == "CHEAT_ENABLE") {
        } else if (cmd == "LOAD_CHEATS") {
            std::string activatedCheatsFile = m_cheatsDir + "/activated/cheats.json";

            QFile file(QString::fromStdString(activatedCheatsFile));
            if (!file.open(QIODevice::ReadOnly)) {
                std::cerr << "IPC: Failed to open activated cheats file\n";
                continue;
            }

            QByteArray jsonData = file.readAll();
            file.close();

            QJsonDocument doc = QJsonDocument::fromJson(jsonData);
            if (!doc.isObject()) {
                std::cerr << "IPC: Activated cheats file invalid JSON\n";
                continue;
            }

            QJsonObject root = doc.object();
            QJsonObject enabled = root["enabled"].toObject();

            std::string gameKey = m_gameSerial + "_" + m_gameVersion;
            QJsonObject gameEntry = enabled[QString::fromStdString(gameKey)].toObject();

            for (auto it = gameEntry.begin(); it != gameEntry.end(); ++it) {
                QJsonArray selectedCheats = it.value().toArray();

                for (const QJsonValue& val : selectedCheats) {
                    std::string cheatName = val.toString().toStdString();
                    applyCheatByName(cheatName);
                }
            }
        } else {
            std::cerr << "UNKNOWN CMD: " << cmd << std::endl;
        }
    }
}

void IPC::applyCheatByName(const std::string& cheatName) {
    if (m_gameSerial.empty() || m_gameVersion.empty() || m_cheatsDir.empty()) {
        std::cerr << "IPC: Game info or cheats dir not set, cannot apply cheat\n";
        return;
    }

    std::string cheatFile =
        m_cheatsDir + "/" + m_gameSerial + "_" + m_gameVersion + "_default.json";

    QFile file(QString::fromStdString(cheatFile));
    if (!file.open(QIODevice::ReadOnly)) {
        std::cerr << "IPC: Failed to open cheat mod file: " << cheatFile << "\n";
        return;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        std::cerr << "IPC: Cheat mod file JSON invalid\n";
        return;
    }

    QJsonObject root = doc.object();
    QJsonArray mods = root["mods"].toArray();

    for (const QJsonValue& modVal : mods) {
        QJsonObject modObj = modVal.toObject();
        if (modObj["name"].toString().toStdString() == cheatName) {
            QJsonArray patches = modObj["patches"].toArray();

            for (const QJsonValue& patchVal : patches) {
                QJsonObject patch = patchVal.toObject();

                std::string modNameStr = cheatName;
                std::string offsetStr = patch["offset"].toString().toStdString();
                std::string valueStr = patch["value"].toString().toStdString();
                std::string targetStr = "";
                std::string sizeStr = patch["size"].toString().toStdString();
                bool isOffset = true;
                bool littleEndian = patch["littleEndian"].toBool();

                MemoryPatcher::PatchMemory(modNameStr, offsetStr, valueStr, targetStr, sizeStr,
                                           isOffset, littleEndian);
            }
            break; // found cheat, no need to continue
        }
    }
}