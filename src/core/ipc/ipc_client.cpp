// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef ENABLE_QT_GUI
#include <QDir>
#include <QMessageBox>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTimer>

#include <core/file_sys/fs.h>
#include "common/config.h"
#include "common/logging/log.h"
#include "ipc_client.h"

IpcClient::IpcClient(QObject* parent) : QObject(parent) {}

void IpcClient::startGame(const QFileInfo& exe, const QStringList& args, const QString& workDir,
                          bool disable_ipc) {
    lastGamePath = exe.absoluteFilePath();
    lastArgs = args;
    lastWorkDir = workDir.isEmpty() ? exe.absolutePath() : workDir;
    if (process) {
        process->disconnect();
        process->deleteLater();
        process = nullptr;
    }
    process = new QProcess(this);

    connect(process, &QProcess::readyReadStandardError, this, [this] { onStderr(); });
    connect(process, &QProcess::readyReadStandardOutput, this, [this] { onStdout(); });
    connect(process, &QProcess::finished, this, [this] { onProcessClosed(); });

    process->setProcessChannelMode(QProcess::SeparateChannels);

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!disable_ipc) {
        env.insert("SHADPS4_ENABLE_IPC", "true");
    }
    env.insert("SHADPS4_ENABLE_MODS", Core::FileSys::MntPoints::enable_mods ? "1" : "0");
    env.insert("SHADPS4_BASE_GAME", Core::FileSys::MntPoints::ignore_game_patches ? "1" : "0");

    process->setProcessEnvironment(env);

    process->setWorkingDirectory(workDir.isEmpty() ? exe.absolutePath() : workDir);
    process->start(exe.absoluteFilePath(), args, QIODevice::ReadWrite);
}

void IpcClient::startGame() {
    writeLine("START");
}

void IpcClient::pauseGame() {
    writeLine("PAUSE");
}

void IpcClient::resumeGame() {
    writeLine("RESUME");
}

void IpcClient::stopEmulator() {
    writeLine("STOP");
}

void IpcClient::restartEmulator() {
    stopEmulator();
    pendingRestart = true;
}

void IpcClient::toggleFullscreen() {
    writeLine("TOGGLE_FULLSCREEN");
}

void IpcClient::adjustVol(int volume) {
    writeLine("ADJUST_VOLUME");
    writeLine(QString::number(volume));
}

void IpcClient::setFsr(bool enable) {
    writeLine("SET_FSR");
    writeLine(enable ? "1" : "0");
}

void IpcClient::setRcas(bool enable) {
    writeLine("SET_RCAS");
    writeLine(enable ? "1" : "0");
}

void IpcClient::setRcasAttenuation(int value) {
    writeLine("SET_RCAS_ATTENUATION");
    writeLine(QString::number(value));
}

void IpcClient::reloadInputs(std::string config) {
    writeLine("RELOAD_INPUTS");
    writeLine(QString::fromStdString(config));
}

void IpcClient::setActiveController(std::string GUID) {
    writeLine("SET_ACTIVE_CONTROLLER");
    writeLine(QString::fromStdString(GUID));
}

void IpcClient::sendMemoryPatches(std::string modNameStr, std::string offsetStr,
                                  std::string valueStr, std::string targetStr, std::string sizeStr,
                                  bool isOffset, bool littleEndian,
                                  MemoryPatcher::PatchMask patchMask, int maskOffset) {
    writeLine("PATCH_MEMORY");
    writeLine(QString::fromStdString(modNameStr));
    writeLine(QString::fromStdString(offsetStr));
    writeLine(QString::fromStdString(valueStr));
    writeLine(QString::fromStdString(targetStr));
    writeLine(QString::fromStdString(sizeStr));
    writeLine(isOffset ? "1" : "0");
    writeLine(littleEndian ? "1" : "0");
    writeLine(QString::number(static_cast<int>(patchMask)));
    writeLine(QString::number(maskOffset));
}

std::shared_ptr<IpcClient> IpcClient::GetInstance() {
    return s_instance;
}
void IpcClient::SetInstance(std::shared_ptr<IpcClient> instance) {
    s_instance = std::move(instance);
}

void IpcClient::onStderr() {
    buffer.append(process->readAllStandardError());
    int idx;
    while ((idx = buffer.indexOf('\n')) != -1) {
        QByteArray line = buffer.left(idx);
        buffer.remove(0, idx + 1);

        if (!line.isEmpty() && line.back() == '\r') {
            line.chop(1);
        }

        if (!line.startsWith(";")) {
            LOG_ERROR(Tty, "{}", line.toStdString());
            continue;
        }

        const QString s = QString::fromUtf8(line.mid(1)).trimmed();
        if (s == "#IPC_ENABLED") {
            LOG_INFO(IPC, "IPC detected");
        } else if (s == "ENABLE_MEMORY_PATCH") {
            supportedCapabilities["memory_patch"] = true;
            LOG_INFO(IPC, "Feature detected: 'memory_patch'");
        } else if (s == "ENABLE_EMU_CONTROL") {
            supportedCapabilities["emu_control"] = true;
            LOG_INFO(IPC, "Feature detected: 'emu_control'");
        } else if (s == "#IPC_END") {
            for (const auto& [capability, supported] : supportedCapabilities) {
                if (not supported) {
                    LOG_WARNING(IPC,
                                "Feature: '{}' is not supported by the choosen emulator version",
                                capability);
                }
            }
            LOG_INFO(IPC, "Start emu");
            writeLine("RUN");
            startGameFunc();
        } else if (s == "RESTART") {
            parsingState = ParsingState::args_counter;
        }

        else if (parsingState == ParsingState::args_counter) {
            argsCounter = s.toInt();
            parsingState = ParsingState::args;
        }

        else if (parsingState == ParsingState::args) {
            parsedArgs.push_back(s.toStdString());
            if (parsedArgs.size() == argsCounter) {
                parsingState = ParsingState::normal;
                pendingRestart = true;
                stopEmulator();
            }
        }
    }
}

void IpcClient::onStdout() {
    QColor color;
    QByteArray data = process->readAllStandardOutput();
    QString dataString = QString::fromUtf8(data);
    QStringList entries = dataString.split('\n');

    for (QString& entry : entries) {
        if (entry.contains("<Warning>")) {
            color = Qt::yellow;
        } else if (entry.contains("<Error>")) {
            color = Qt::red;
        } else if (entry.contains("<Critical>")) {
            color = Qt::magenta;
        } else if (entry.contains("<Trace>")) {
            color = Qt::gray;
        } else if (entry.contains("<Debug>")) {
            color = Qt::cyan;
        } else {
            color = Qt::white;
        }

        QRegularExpression ansiRegex(
            R"(\x1B\[[0-9;]*[mK])"); // ANSI escape codes from UNIX terminals
        entry = entry.replace(ansiRegex, "");

        if (!entry.isEmpty())
            emit LogEntrySent(entry.trimmed(), color);
    }
}

void IpcClient::onProcessClosed() {
    gameClosedFunc();
    if (process) {
        process->disconnect();
        process->deleteLater();
        process = nullptr;
    }
    if (pendingRestart) {
        pendingRestart = false;
        restartEmulatorFunc();
    }
}

void IpcClient::writeLine(const QString& text) {
    if (process == nullptr) {
        return;
    }

    QByteArray data = text.toUtf8();
    data.append('\n');
    process->write(data);
    process->waitForBytesWritten(1000);
}

void IpcClient::stopGame() {
    if (!process)
        return;

    writeLine("STOP");

    if (!process->waitForFinished(100)) {
        LOG_WARNING(IPC, "Process did not close gracefully, terminating...");
        process->kill();
    }

    process->disconnect();
    process->deleteLater();
    process = nullptr;
}

void IpcClient::restartGame() {
    if (!process) {
        LOG_WARNING(IPC, "No running game to restart.");
        return;
    }

    if (lastGamePath.isEmpty()) {
        LOG_ERROR(IPC, "Cannot restart: no stored game path.");
        return;
    }

    LOG_INFO(IPC, "Restarting game manually...");

    pendingRestart = false;

    stopGame();

    QTimer::singleShot(500, [this]() {
        LOG_INFO(IPC, "Launching game again...");
        startGame(QFileInfo(lastGamePath), lastArgs, lastWorkDir);
    });
}
#endif
