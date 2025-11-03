// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef ENABLE_QT_GUI

#pragma once

#include <functional>
#include <QColor>
#include <QFileInfo>
#include <QProcess>

#include "common/memory_patcher.h"

class IpcClient : public QObject {
    Q_OBJECT

signals:
    void LogEntrySent(QString entry, QColor textColor);

public:
    explicit IpcClient(QObject* parent = nullptr);
    void startGame(const QFileInfo& exe, const QStringList& args,
                   const QString& workDir = QString(), bool disable_ipc = false);
    void startGame();
    void stopGame();
    void restartGame();
    void pauseGame();
    void resumeGame();
    void stopEmulator();
    void restartEmulator();
    void toggleFullscreen();
    void adjustVol(int volume);
    void setFsr(bool enable);
    void setRcas(bool enable);
    void setRcasAttenuation(int value);
    void reloadInputs(std::string config);
    void setActiveController(std::string GUID);
    void sendMemoryPatches(std::string modNameStr, std::string offsetStr, std::string valueStr,
                           std::string targetStr, std::string sizeStr, bool isOffset,
                           bool littleEndian,
                           MemoryPatcher::PatchMask patchMask = MemoryPatcher::PatchMask::None,
                           int maskOffset = 0);
    std::function<void()> gameClosedFunc;
    std::function<void()> startGameFunc;
    std::function<void()> restartEmulatorFunc;

    QString lastGamePath;
    QStringList lastGameArgs;
    QStringList lastArgs;
    QString lastWorkDir;

    enum ParsingState { normal, args_counter, args };
    std::vector<std::string> parsedArgs;
    std::unordered_map<std::string, bool> supportedCapabilities{
        {"memory_patch", false},
        {"emu_control", false},
    };
    static std::shared_ptr<IpcClient> GetInstance();
    static void SetInstance(std::shared_ptr<IpcClient> instance);

private:
    void onStderr();
    void onStdout();
    void onProcessClosed();
    void writeLine(const QString& text);

    QProcess* process = nullptr;
    QByteArray buffer;
    bool pendingRestart = false;

    ParsingState parsingState;
    int argsCounter;
};
inline static std::shared_ptr<IpcClient> s_instance = nullptr;

#endif