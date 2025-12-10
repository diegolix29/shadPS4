// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QProgressBar>
#include <QRegularExpression>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <common/path_util.h>

#include "common/config.h"
#include "qt_gui/compatibility_info.h"
#include "qt_gui/main_window.h"
#include "ui_version_dialog.h"
#include "version_dialog.h"

VersionDialog::VersionDialog(std::shared_ptr<CompatibilityInfoClass> compat_info, QWidget* parent)
    : QDialog(parent), ui(new Ui::VersionDialog), m_compat_info(std::move(compat_info)) {
    ui->setupUi(this);
    setAcceptDrops(true);

    ui->installedTreeWidget->setSortingEnabled(true);
    ui->installedTreeWidget->header()->setSortIndicatorShown(true);
    ui->installedTreeWidget->header()->setSectionsClickable(true);

    ui->downloadTreeWidget->setSortingEnabled(true);
    ui->downloadTreeWidget->header()->setSortIndicatorShown(true);
    ui->downloadTreeWidget->header()->setSectionsClickable(true);

    ui->currentShadPath->setText(QString::fromStdString(m_compat_info->GetShadPath()));

    LoadinstalledList();

    DownloadListVersion();

    connect(ui->browse_shad_path, &QPushButton::clicked, this, [this]() {
        QString initial_path = QString::fromStdString(m_compat_info->GetShadPath());

        QString shad_folder_path_string =
            QFileDialog::getExistingDirectory(this, tr("Select the shadPS4 folder"), initial_path);

        auto folder_path = Common::FS::PathFromQString(shad_folder_path_string);
        if (!folder_path.empty()) {
            ui->currentShadPath->setText(shad_folder_path_string);
            m_compat_info->SetShadPath(shad_folder_path_string.toStdString());
        }
    });

    connect(ui->clear_shad_path, &QPushButton::clicked, this, [this]() {
        ui->currentShadPath->clear();
        m_compat_info->SetShadPath("");

        QMessageBox::information(this, tr("Path cleared"),
                                 tr("The path to save versions has been cleared."));
    });

    connect(ui->checkChangesVersionButton, &QPushButton::clicked, this,
            [this]() { LoadinstalledList(); });

    connect(ui->installVersionButton, &QPushButton::clicked, this,
            [this]() { InstallSelectedVersionExe(); });
    connect(ui->uninstallQtButton, &QPushButton::clicked, this,
            &VersionDialog::UninstallSelectedVersion);
    connect(ui->installPkgButton, &QPushButton::clicked, this, [this]() { InstallPkgWithV7(); });
    connect(ui->addCustomVersionButton, &QPushButton::clicked, this, [this]() {
        QString exePath;

#if defined(Q_OS_WIN)
        exePath = QFileDialog::getOpenFileName(this, tr("Select executable"), QDir::rootPath(),
                                               tr("Executable (*.exe)"));
#elif defined(Q_OS_LINUX)
    exePath = QFileDialog::getOpenFileName(this, tr("Select executable"), QDir::rootPath(),
                                         tr("Executable (*.AppImage)"));
#elif defined(Q_OS_MAC)
    exePath = QFileDialog::getOpenFileName(this, tr("Select executable"), QDir::rootPath(),
                                         tr("Executable (*.*)"));
#endif

        if (exePath.isEmpty())
            return;

        bool ok;
        QString folderName =
            QInputDialog::getText(this, tr("Version name"),
                                  tr("Enter the name of this version as it appears in the list."),
                                  QLineEdit::Normal, "", &ok);
        if (!ok || folderName.trimmed().isEmpty())
            return;

        folderName = folderName.trimmed();

        QString uiChoice = QInputDialog::getItem(this, tr("UI type"), tr("Select UI type:"),
                                                 QStringList{tr("Qt"), tr("SDL")}, 0, false, &ok);
        if (!ok)
            return;

        QString uiSuffix = (uiChoice.compare("SDL", Qt::CaseInsensitive) == 0) ? "SDL" : "QT";
        QString finalFolderName = QString("%1_%2_%3").arg(folderName, QString("Custom"), uiSuffix);

        QString basePath = QString::fromStdString(m_compat_info->GetShadPath());
        QString newFolderPath = QDir(basePath).filePath(finalFolderName);

        QDir dir;
        if (dir.exists(newFolderPath)) {
            QMessageBox::warning(this, tr("Error"), tr("A folder with that name already exists."));
            return;
        }

        if (!dir.mkpath(newFolderPath)) {
            QMessageBox::critical(this, tr("Error"), tr("Failed to create folder."));
            return;
        }

        QFileInfo exeInfo(exePath);
        QString targetFilePath = QDir(newFolderPath).filePath(exeInfo.fileName());

        if (!QFile::copy(exePath, targetFilePath)) {
            QMessageBox::critical(this, tr("Error"), tr("Failed to copy executable."));
            return;
        }

        QMessageBox::information(this, tr("Success"), tr("Version added successfully."));
        LoadinstalledList();
    });

    connect(ui->deleteVersionButton, &QPushButton::clicked, this, [this]() {
        QTreeWidgetItem* selectedItem = ui->installedTreeWidget->currentItem();
        if (!selectedItem) {
            QMessageBox::warning(this, tr("Notice"),
                                 tr("No version selected from the Installed list."));
            return;
        }

        QString fullPath = selectedItem->text(5);
        if (fullPath.isEmpty()) {
            QMessageBox::critical(this, tr("Error"), tr("Failed to determine the folder path."));
            return;
        }
        QString folderName = QDir(fullPath).dirName();
        auto reply = QMessageBox::question(this, tr("Delete version"),
                                           tr("Do you want to delete the version") +
                                               QString(" \"%1\" ?").arg(folderName),
                                           QMessageBox::Yes | QMessageBox::No);
        if (reply == QMessageBox::Yes) {
            QDir dirToRemove(fullPath);
            if (dirToRemove.exists()) {
                if (!dirToRemove.removeRecursively()) {
                    QMessageBox::critical(this, tr("Error"),
                                          tr("Failed to delete folder.") +
                                              QString("\n \"%1\"").arg(folderName));
                    return;
                }
            }
            LoadinstalledList();
        }
    });
};

VersionDialog::~VersionDialog() {
    delete ui;
}

std::filesystem::path VersionDialog::GetActualExecutablePath() {
#if defined(Q_OS_LINUX)
    if (const char* appimageEnv = std::getenv("APPIMAGE")) {
        return std::filesystem::path(appimageEnv);
    }
#endif
    return Common::FS::GetExecutablePath();
}

void VersionDialog::DownloadListVersion() {
    QNetworkAccessManager* manager = new QNetworkAccessManager(this);

    const QString mainRepoUrl = "https://api.github.com/repos/shadps4-emu/shadPS4/tags";
    const QString forkRepoUrl = "https://api.github.com/repos/diegolix29/shadPS4/releases";

    QNetworkReply* mainReply = manager->get(QNetworkRequest(QUrl(mainRepoUrl)));
    QNetworkReply* forkReply = manager->get(QNetworkRequest(QUrl(forkRepoUrl)));

    auto processReplies = [this, mainReply, forkReply]() {
        if (!mainReply->isFinished() || !forkReply->isFinished())
            return;

        QList<QJsonObject> mainTags;
        QList<QJsonObject> forkReleases;

        if (mainReply->error() == QNetworkReply::NoError) {
            QJsonDocument doc = QJsonDocument::fromJson(mainReply->readAll());
            if (doc.isArray()) {
                for (const QJsonValue& val : doc.array())
                    mainTags.append(val.toObject());
            }
        }

        if (forkReply->error() == QNetworkReply::NoError) {
            QJsonDocument doc = QJsonDocument::fromJson(forkReply->readAll());
            if (doc.isArray()) {
                for (const QJsonValue& val : doc.array())
                    forkReleases.append(val.toObject());
            }
        }

        mainReply->deleteLater();
        forkReply->deleteLater();

        ui->downloadTreeWidget->clear();

        QList<QTreeWidgetItem*> mainItems;
        QList<QTreeWidgetItem*> forkItems;

        for (const QJsonObject& tagObj : mainTags) {
            QString tagName = tagObj["name"].toString();
            if (tagName.isEmpty())
                continue;

            QTreeWidgetItem* item = new QTreeWidgetItem();
            item->setText(0, tagName);
            item->setText(1, "[Official]");
            mainItems.append(item);
        }

        for (const QJsonObject& releaseObj : forkReleases) {
            bool isPrerelease = releaseObj["prerelease"].toBool();
            QString tagName = releaseObj["tag_name"].toString();
            QString name = releaseObj["name"].toString();

            if (name.isEmpty() && tagName.isEmpty())
                continue;

            QString display_name = name.isEmpty() ? tagName : name;

            QTreeWidgetItem* item = new QTreeWidgetItem();
            item->setText(0, display_name);
            item->setText(1, "[Fork]");
            item->setData(0, Qt::UserRole, tagName);
            forkItems.append(item);
        }

        auto sortItems = [](QList<QTreeWidgetItem*>& items) {
            std::sort(items.begin(), items.end(), [](QTreeWidgetItem* a, QTreeWidgetItem* b) {
                return a->text(0) > b->text(0);
            });
        };
        sortItems(mainItems);
        sortItems(forkItems);

        QTreeWidgetItem* mainHeader = new QTreeWidgetItem(QStringList() << "Official Releases");
        for (auto* item : mainItems)
            mainHeader->addChild(item);
        ui->downloadTreeWidget->addTopLevelItem(mainHeader);

        QTreeWidgetItem* forkHeader = new QTreeWidgetItem(QStringList() << "Fork Releases");
        for (auto* item : forkItems)
            forkHeader->addChild(item);
        ui->downloadTreeWidget->addTopLevelItem(forkHeader);

        ui->downloadTreeWidget->collapseAll();

        QStringList allTags;
        for (auto* item : mainItems)
            allTags << item->text(0);
        for (auto* item : forkItems)
            allTags << item->text(0);

        SaveDownloadCache(allTags);

        InstallSelectedVersion();
    };

    connect(mainReply, &QNetworkReply::finished, this, processReplies);
    connect(forkReply, &QNetworkReply::finished, this, processReplies);
}

void VersionDialog::InstallSelectedVersion() {
    connect(
        ui->downloadTreeWidget, &QTreeWidget::itemClicked, this,
        [this](QTreeWidgetItem* item, int) {
            if (m_compat_info->GetShadPath().empty()) {
                QMessageBox::warning(this, tr("Select the shadPS4 folder"),
                                     tr("First you need to choose a location to save the versions "
                                        "in\n'Path to save versions'"));
                return;
            }

            QString versionName = item->text(0);
            QString platform;
            bool fetchSDL = ui->sdlBuildCheckBox->isChecked();

#if defined(Q_OS_WIN)
            platform = fetchSDL ? "win64-sdl" : "win64-qt";
#elif defined(Q_OS_LINUX)
            platform = fetchSDL ? "linux-sdl" : "linux-qt";
#elif defined(Q_OS_MAC)
            platform = fetchSDL ? "macos-sdl" : "macos-qt";
#endif

            QString sourceType = item->text(1);
            QString sourceLabel =
                sourceType.contains("Fork", Qt::CaseInsensitive) ? "Fork" : "Official";
            QString apiUrl;

            QString tag_name = item->data(0, Qt::UserRole).toString();
            if (sourceType.contains("Fork", Qt::CaseInsensitive)) {
                QString releaseIdentifier = tag_name.isEmpty() ? versionName : tag_name;

                apiUrl = QString("https://api.github.com/repos/diegolix29/shadPS4/releases/tags/%1")
                             .arg(releaseIdentifier);
            } else {
                if (versionName == "Pre-release") {
                    apiUrl = "https://api.github.com/repos/diegolix29/shadPS4/releases";
                } else {
                    apiUrl =
                        QString("https://api.github.com/repos/shadps4-emu/shadPS4/releases/tags/%1")
                            .arg(versionName);
                }
            }

            if (QMessageBox::question(
                    this, tr("Download"),
                    tr("Do you want to download the version: %1?").arg(versionName),
                    QMessageBox::Yes | QMessageBox::No) == QMessageBox::No)
                return;

            QNetworkAccessManager* manager = new QNetworkAccessManager(this);
            QNetworkReply* reply = manager->get(QNetworkRequest(QUrl(apiUrl)));

            connect(
                reply, &QNetworkReply::finished, this,
                [this, reply, platform, versionName, sourceLabel, fetchSDL]() {
                    if (reply->error() != QNetworkReply::NoError) {
                        QMessageBox::warning(this, tr("Error"), reply->errorString());
                        reply->deleteLater();
                        return;
                    }

                    QByteArray response = reply->readAll();
                    QJsonDocument doc = QJsonDocument::fromJson(response);
                    QJsonArray assets;
                    QJsonObject release;

                    if (versionName == "Pre-release") {
                        QJsonArray releases = doc.array();
                        for (const QJsonValue& val : releases) {
                            QJsonObject obj = val.toObject();
                            if (obj["prerelease"].toBool()) {
                                release = obj;
                                assets = obj["assets"].toArray();
                                break;
                            }
                        }
                    } else {
                        release = doc.object();
                        assets = release["assets"].toArray();
                    }

                    QString downloadUrl;
                    for (const QJsonValue& val : assets) {
                        QJsonObject obj = val.toObject();
                        if (obj["name"].toString().contains(platform)) {
                            downloadUrl = obj["browser_download_url"].toString();
                            break;
                        }
                    }

                    if (downloadUrl.isEmpty()) {
                        QMessageBox::warning(this, tr("Error"),
                                             tr("No files available for this platform."));
                        reply->deleteLater();
                        return;
                    }

                    QString userPath = QString::fromStdString(m_compat_info->GetShadPath());
                    QString fileName = "temp_download_update.zip";
                    QString destinationPath = userPath + "/" + fileName;

                    QNetworkAccessManager* downloadManager = new QNetworkAccessManager(this);
                    QNetworkReply* downloadReply =
                        downloadManager->get(QNetworkRequest(QUrl(downloadUrl)));

                    QDialog* progressDialog = new QDialog(this);
                    progressDialog->setWindowTitle(tr("Downloading %1").arg(versionName));
                    progressDialog->setFixedSize(400, 80);
                    QVBoxLayout* layout = new QVBoxLayout(progressDialog);
                    QProgressBar* progressBar = new QProgressBar(progressDialog);
                    progressBar->setRange(0, 100);
                    layout->addWidget(progressBar);
                    progressDialog->setLayout(layout);
                    progressDialog->show();

                    connect(downloadReply, &QNetworkReply::downloadProgress, progressBar,
                            [progressBar](qint64 r, qint64 t) {
                                if (t > 0)
                                    progressBar->setValue((r * 100) / t);
                            });

                    QFile* file = new QFile(destinationPath);
                    if (!file->open(QIODevice::WriteOnly)) {
                        file->deleteLater();
                        downloadReply->deleteLater();
                        return;
                    }

                    connect(downloadReply, &QNetworkReply::readyRead, file,
                            [file, downloadReply]() { file->write(downloadReply->readAll()); });

                    connect(
                        downloadReply, &QNetworkReply::finished, this,
                        [this, file, downloadReply, progressDialog, release, userPath, versionName,
                         sourceLabel, fetchSDL]() {
                            file->flush();
                            file->close();
                            file->deleteLater();
                            downloadReply->deleteLater();

                            QString releaseName = release["name"].toString();
                            if (releaseName.startsWith("shadps4 ", Qt::CaseInsensitive))
                                releaseName = releaseName.mid(8);
                            releaseName.replace(QRegularExpression("\\b[Cc]odename\\s+"), "");

                            QString folderName;
                            if (versionName == "Pre-release") {
                                folderName = release["tag_name"].toString();
                            } else {
                                QString datePart = release["published_at"].toString().left(10);
                                folderName = QString("%1 - %2").arg(releaseName, datePart);
                            }

                            QString uiSuffix = fetchSDL ? "SDL" : "QT";
                            folderName += QString("_%1_%2").arg(sourceLabel, uiSuffix);

                            QString destFolder = QDir(userPath).filePath(folderName);
                            QString scriptFilePath, scriptContent, process;
                            QStringList args;

#if defined(Q_OS_WIN)
                            scriptFilePath = userPath + "/extract_update.ps1";
                            scriptContent =
                                QString(
                                    "New-Item -ItemType Directory -Path \"%1\" -Force\n"
                                    "Expand-Archive -Path \"%2\" -DestinationPath \"%1\" -Force\n"
                                    "Remove-Item -Force \"%2\"\n"
                                    "Remove-Item -Force \"%3\"\n"
                                    "cls\n")
                                    .arg(destFolder, userPath + "/temp_download_update.zip",
                                         scriptFilePath);
                            process = "powershell.exe";
                            args << "-ExecutionPolicy" << "Bypass" << "-File" << scriptFilePath;
#elif defined(Q_OS_LINUX)
                            scriptFilePath = userPath + "/extract_update.sh";
                            scriptContent =
                                QString("#!/bin/bash\n"
                                        "mkdir -p \"%1\"\n"
                                        "if command -v unzip &> /dev/null; then\n"
                                        "    unzip -o \"%2\" -d \"%1\"\n"
                                        "elif command -v 7z &> /dev/null; then\n"
                                        "    7z x \"%2\" -o\"%1\" -y\n"
                                        "fi\n"
                                        "chmod +x \"%1\"/*.AppImage 2>/dev/null || true\n"
                                        "chmod +x \"%1\"/shadps4* 2>/dev/null || true\n"
                                        "rm \"%2\"\n"
                                        "rm \"%3\"\n"
                                        "clear\n")
                                    .arg(destFolder, userPath + "/temp_download_update.zip",
                                         scriptFilePath);
                            process = "bash";
                            args << scriptFilePath;
#elif defined(Q_OS_MAC)
                            scriptFilePath = userPath + "/extract_update.sh";
                            scriptContent =
                                QString("#!/bin/bash\n"
                                        "mkdir -p \"%1\"\n"
                                        "unzip -o \"%2\" -d \"%1\"\n"
                                        "find \"%1\" -name \"*.tar.gz\" -exec tar -xzf {} -C "
                                        "\"%1\" \\;\n"
                                        "chmod +x \"%1\"/shadps4.app/Contents/MacOS/shadps4 "
                                        "2>/dev/null || true\n"
                                        "rm \"%2\"\n"
                                        "rm \"%3\"\n"
                                        "clear\n")
                                    .arg(destFolder, userPath + "/temp_download_update.zip",
                                         scriptFilePath);
                            process = "bash";
                            args << scriptFilePath;
#endif

                            QFile scriptFile(scriptFilePath);
                            if (scriptFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
                                QTextStream out(&scriptFile);
#if defined(Q_OS_WIN)
                                scriptFile.write("\xEF\xBB\xBF");
#endif
                                out << scriptContent;
                                scriptFile.close();
#if defined(Q_OS_LINUX) || defined(Q_OS_MAC)
                                scriptFile.setPermissions(QFile::ExeUser | QFile::ReadUser |
                                                          QFile::WriteUser);
#endif
                                QProcess::startDetached(process, args);

                                QTimer::singleShot(
                                    4000, this,
                                    [this, destFolder, progressDialog, versionName, fetchSDL]() {
                                        progressDialog->close();
                                        progressDialog->deleteLater();

                                        QString executableName;
#if defined(Q_OS_WIN)
                                        executableName = "shadps4.exe";
#elif defined(Q_OS_LINUX)
                                        executableName = fetchSDL ? "shadps4-sdl" : "shadps4";
                                        QDir dir(destFolder);
                                        QStringList filters;
                                        filters << "*.AppImage" << "shadps4*";
                                        QStringList files =
                                            dir.entryList(filters, QDir::Files | QDir::Executable);
                                        if (!files.isEmpty())
                                            executableName = files.first();
#elif defined(Q_OS_MAC)
                                        executableName = "shadps4.app";
#endif
                                        QString finalExePath =
                                            QDir(destFolder).filePath(executableName);

                                        if (QFile::exists(finalExePath)) {
                                            QMessageBox::information(
                                                this, tr("Download Complete"),
                                                tr("Version %1 Downloaded please Install"
                                                   "it.\nPath: %2")
                                                    .arg(versionName, finalExePath));
                                        } else {
                                            QMessageBox::warning(
                                                this, tr("Error"),
                                                tr("Executable not found after extraction:\n%1")
                                                    .arg(finalExePath));
                                        }
                                        LoadinstalledList();
                                    });
                            }
                        });
                    reply->deleteLater();
                });
        });
}

void VersionDialog::LoadinstalledList() {
    QString path = QString::fromStdString(m_compat_info->GetShadPath());
    QDir dir(path);
    if (!dir.exists() || path.isEmpty())
        return;

    ui->installedTreeWidget->clear();
    ui->installedTreeWidget->setColumnCount(6);

    QString currentExePath = QString::fromStdString(Config::getVersionPath());
    QString normalizedCurrentPath = QDir::fromNativeSeparators(currentExePath);

    QStringList folders = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    QRegularExpression versionRegex("^v(\\d+)\\.(\\d+)\\.(\\d+)$");

    QVector<QPair<QVector<int>, QString>> versionedDirs;
    QStringList otherDirs;

    for (const QString& folder : folders) {
        if (folder == "Pre-release") {
            otherDirs.append(folder);
            continue;
        }

        QRegularExpressionMatch match = versionRegex.match(folder.section(" - ", 0, 0));
        if (match.hasMatch()) {
            QVector<int> versionParts = {match.captured(1).toInt(), match.captured(2).toInt(),
                                         match.captured(3).toInt()};
            versionedDirs.append({versionParts, folder});
        } else {
            otherDirs.append(folder);
        }
    }

    std::sort(otherDirs.begin(), otherDirs.end());

    std::sort(versionedDirs.begin(), versionedDirs.end(), [](const auto& a, const auto& b) {
        if (a.first[0] != b.first[0])
            return a.first[0] > b.first[0]; // major
        if (a.first[1] != b.first[1])
            return a.first[1] > b.first[1]; // minor
        return a.first[2] > b.first[2];     // patch
    });

    auto extractSuffixes = [](const QString& folder, QString& outSource, QString& outUI,
                              QString& outBase) {
        outSource = "";
        outUI = "";
        outBase = folder;
        QRegularExpression suffRegex("_(Official|Fork|Custom)_(QT|SDL)$",
                                     QRegularExpression::CaseInsensitiveOption);
        QRegularExpressionMatch m = suffRegex.match(folder);
        if (m.hasMatch()) {
            outSource = m.captured(1);
            outUI = (m.captured(2).compare("SDL", Qt::CaseInsensitive) == 0) ? "SDL" : "Qt";
            outBase = folder.left(folder.length() - m.captured(0).length());
            return;
        }
        if (folder.endsWith("_SDL", Qt::CaseInsensitive)) {
            outUI = "SDL";
            outBase = folder.left(folder.length() - 4);
        } else if (folder.endsWith("_QT")) {
            outUI = "Qt";
            outBase = folder.left(folder.length() - 3);
        }
    };

    auto getExecutablePath = [path, &normalizedCurrentPath](const QString& folder,
                                                            const QString& uiType) -> bool {
        QString folderPath = QDir(path).filePath(folder);
        QDir dir(folderPath);

        QString exeName;
        bool isSDL = (uiType.compare("SDL", Qt::CaseInsensitive) == 0);

#if defined(Q_OS_WIN)
        exeName = isSDL ? "shadps4-sdl.exe" : "shadps4.exe";
#elif defined(Q_OS_LINUX)
        if (isSDL) {
            exeName = "shadps4-sdl";
        } else {
            QStringList candidates =
                dir.entryList(QStringList() << "*.AppImage", QDir::Files | QDir::Executable);
            if (!candidates.isEmpty()) {
                exeName = candidates.first();
            } else {
                exeName = "shadps4";
            }
        }
#elif defined(Q_OS_MAC)
        exeName = isSDL ? "shadps4-sdl" : "shadps4.app";
        if (exeName.endsWith(".app")) {
            QString innerExePath = dir.filePath(exeName + "/Contents/MacOS/shadps4");
            if (QFile::exists(innerExePath)) {
                QString normalizedInnerPath = QDir::fromNativeSeparators(innerExePath);
                return normalizedInnerPath == normalizedCurrentPath;
            }
        }
#endif
        QString fullExePath = QDir::fromNativeSeparators(dir.filePath(exeName));
        return fullExePath == normalizedCurrentPath;
    };

    for (const QString& folder : otherDirs) {
        QTreeWidgetItem* item = new QTreeWidgetItem(ui->installedTreeWidget);
        QString fullPath = QDir(path).filePath(folder);
        item->setText(5, fullPath);

        QString sourceLabel, uiLabel, baseName;
        extractSuffixes(folder, sourceLabel, uiLabel, baseName);

        if (baseName.startsWith("Pre-release-shadPS4")) {
            QStringList parts = baseName.split('-');
            item->setText(0, "Pre-release");
            QString shortHash;
            if (parts.size() >= 7) {
                shortHash = parts[6].left(7);
            } else {
                shortHash = "";
            }
            item->setText(1, shortHash);
            if (parts.size() >= 6) {
                QString date = QString("%1-%2-%3").arg(parts[3], parts[4], parts[5]);
                item->setText(2, date);
            } else {
                item->setText(2, "");
            }
        } else if (baseName.contains(" - ")) {
            QStringList parts = baseName.split(" - ");
            item->setText(0, parts.value(0));
            item->setText(1, parts.value(1));
            item->setText(2, parts.value(2));
        } else {
            item->setText(0, baseName);
            item->setText(1, "");
            item->setText(2, "");
        }

        item->setText(3, sourceLabel);
        item->setText(4, uiLabel);

        if (getExecutablePath(folder, uiLabel)) {
            item->setText(0, u8"\u2714" + item->text(0));
            ui->installedTreeWidget->setCurrentItem(item);
        }
    }

    for (const auto& pair : versionedDirs) {
        QTreeWidgetItem* item = new QTreeWidgetItem(ui->installedTreeWidget);
        QString fullPath = QDir(path).filePath(pair.second);
        item->setText(5, fullPath);

        QString sourceLabel, uiLabel, baseName;
        extractSuffixes(pair.second, sourceLabel, uiLabel, baseName);

        if (baseName.contains(" - ")) {
            QStringList parts = baseName.split(" - ");
            item->setText(0, parts.value(0));
            item->setText(1, parts.value(1));
            item->setText(2, parts.value(2));
        } else {
            item->setText(0, baseName);
            item->setText(1, "");
            item->setText(2, "");
        }

        item->setText(3, sourceLabel);
        item->setText(4, uiLabel);

        if (getExecutablePath(pair.second, uiLabel)) {
            item->setText(0, u8"\u2714" + item->text(0));
            ui->installedTreeWidget->setCurrentItem(item);
        }
    }

    connect(ui->installedTreeWidget, &QTreeWidget::itemSelectionChanged, this, [this]() {
        QTreeWidgetItem* selectedItem = ui->installedTreeWidget->currentItem();
        if (selectedItem) {
            QString exePath = selectedItem->text(5);
            if (!exePath.isEmpty())
                m_compat_info->SetSelectedShadExePath(exePath.toStdString());
        }
    });

    ui->installedTreeWidget->resizeColumnToContents(1);
    ui->installedTreeWidget->setColumnWidth(1, ui->installedTreeWidget->columnWidth(1) + 50);
}

QStringList VersionDialog::LoadDownloadCache() {
    QString cachePath =
        QDir(QString::fromStdString(m_compat_info->GetShadPath())).filePath("cache.version");

    QStringList cachedVersions;
    QFile file(cachePath);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        while (!in.atEnd())
            cachedVersions.append(in.readLine().trimmed());
    }
    return cachedVersions;
}

void VersionDialog::SaveDownloadCache(const QStringList& versions) {
    QString cachePath =
        QDir(QString::fromStdString(m_compat_info->GetShadPath())).filePath("cache.version");
    QFile file(cachePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&file);
        for (const QString& v : versions)
            out << v << "\n";
    }
}

void VersionDialog::PopulateDownloadTree(const QStringList& versions) {
    ui->downloadTreeWidget->clear();

    QTreeWidgetItem* preReleaseItem = nullptr;
    QList<QTreeWidgetItem*> otherItems;
    bool foundPreRelease = false;

    for (const QString& tagName : versions) {
        if (tagName.startsWith("Pre-release", Qt::CaseInsensitive)) {
            if (!foundPreRelease) {
                preReleaseItem = new QTreeWidgetItem();
                preReleaseItem->setText(0, tagName);
                preReleaseItem->setText(1, "[Fork Pre-release]");
                foundPreRelease = true;
            }
            continue;
        }
        QTreeWidgetItem* item = new QTreeWidgetItem();
        item->setText(0, tagName);
        otherItems.append(item);
    }

    if (!foundPreRelease) {
        preReleaseItem = new QTreeWidgetItem();
        preReleaseItem->setText(0, "Pre-release");
        preReleaseItem->setText(1, "");
    }

    if (preReleaseItem)
        ui->downloadTreeWidget->addTopLevelItem(preReleaseItem);
    for (QTreeWidgetItem* item : otherItems)
        ui->downloadTreeWidget->addTopLevelItem(item);
}

void VersionDialog::InstallSelectedVersionExe() {
    Config::setBootLauncher(true);

    QTreeWidgetItem* selectedItem = ui->installedTreeWidget->currentItem();
    if (!selectedItem) {
        QMessageBox::warning(this, tr("Notice"), tr("No version selected from Installed list."));
        return;
    }

    QString fullPath = selectedItem->text(5);
    if (fullPath.isEmpty()) {
        QMessageBox::critical(this, tr("Error"), tr("Invalid version path."));
        return;
    }

    QString uiType = selectedItem->text(4);
    bool isSDL = (uiType.compare("SDL", Qt::CaseInsensitive) == 0);

    bool isQT = (uiType.compare("Qt", Qt::CaseInsensitive) == 0);
    if (isSDL) {
        Config::setSdlInstalled(true);
        Config::setQTInstalled(false);
    } else if (isQT) {
        Config::setSdlInstalled(false);
        Config::setQTInstalled(true);
    }
    QDir versionDir(fullPath);
    QString executableName;

#if defined(Q_OS_WIN)
    executableName = "shadps4.exe";
#elif defined(Q_OS_LINUX)
    if (isSDL) {
        if (QFile::exists(versionDir.filePath("shadps4-sdl")))
            executableName = "shadps4-sdl";
        else
            executableName = "shadps4";
    } else {
        QStringList candidates = versionDir.entryList(QStringList() << "*.AppImage", QDir::Files);
        if (!candidates.isEmpty())
            executableName = candidates.first();
        else
            executableName = "shadps4";
    }
#elif defined(Q_OS_MAC)
    executableName = "shadps4.app";
#endif

#if defined(Q_OS_WIN) || defined(Q_OS_MAC)

    if (isSDL) {
        QString source;
        QString target;

#if defined(Q_OS_WIN)
        source = versionDir.filePath("shadps4.exe");
        target = versionDir.filePath("shadps4-sdl.exe");
#endif
#if defined(Q_OS_LINUX)
        source = versionDir.filePath("Shadps4-sdl.exe");
        target = versionDir.filePath("shadps4-sdl.exe");
#endif

#if defined(Q_OS_MAC)
        source = versionDir.filePath("shadps4");
        target = versionDir.filePath("shadps4-sdl");
#endif

        if (QFile::exists(source)) {
            if (QFile::exists(target))
                QFile::remove(target);

            if (!QFile::rename(source, target)) {
                QMessageBox::critical(
                    this, tr("Error"),
                    tr("Failed to rename SDL executable:\n%1 → %2").arg(source, target));
            } else {
                executableName = QFileInfo(target).fileName();
            }
        }
    }
#endif

    QString finalExeToRun = versionDir.filePath(executableName);

    if (!QFile::exists(finalExeToRun)) {
        QStringList filters;
#if defined(Q_OS_WIN)
        filters << "*.exe";
#elif defined(Q_OS_LINUX)
        filters << "*.AppImage" << "shadps4*";
#elif defined(Q_OS_MAC)
        filters << "*.app";
#endif
        QStringList entries = versionDir.entryList(filters, QDir::Files | QDir::Executable);
        if (!entries.isEmpty()) {
            finalExeToRun = versionDir.filePath(entries.first());
        } else {
            QMessageBox::critical(this, tr("Error"),
                                  tr("Executable does not exist:\n%1").arg(finalExeToRun));
            return;
        }
    }

    Config::setVersionPath(finalExeToRun.toStdString());
    Config::save(Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "config.toml");

    QMessageBox::information(this, tr("Success"),
                             tr("Version selected successfully:\n%1").arg(finalExeToRun));

    if (isQT) {
        QMessageBox::information(this, tr("Qt Version Selected"),
                                 tr("QT Version installed successfully. QT Check Enable\n"
                                    "You can use GUI for Launch games."));
    } else if (isSDL) {
        QMessageBox::information(
            this, tr("SDL Version Selected"),
            tr("SDL version installed successfully. SDL Check Enable\n"
               "You can use GUI for Launch games.-NOTE: Only Play/Stop buttons work."));
    }
    g_MainWindow->autoCheckLauncherBox();
}

void VersionDialog::UninstallSelectedVersion() {
    Config::setBootLauncher(false);

    QTreeWidgetItem* selectedItem = ui->installedTreeWidget->currentItem();
    if (!selectedItem) {
        QMessageBox::warning(this, tr("Notice"), tr("No version selected."));
        return;
    }

    QString uiType = selectedItem->text(4);
    QString nameColumn = selectedItem->text(1).trimmed();
    QString versionFolderPath = selectedItem->text(5);

    if (versionFolderPath.isEmpty()) {
        QMessageBox::critical(this, tr("Error"), tr("Invalid version path."));
        return;
    }

    bool isSDL = (uiType.compare("SDL", Qt::CaseInsensitive) == 0);
    QDir versionDir(versionFolderPath);
    QString exeName;

#if defined(Q_OS_WIN)
    exeName = "shadps4.exe";
#elif defined(Q_OS_LINUX)
    if (isSDL) {
        exeName = QFile::exists(versionDir.filePath("shadps4-sdl")) ? "shadps4-sdl" : "shadps4";
    } else {
        QStringList candidates = versionDir.entryList(QStringList() << "*.AppImage", QDir::Files);
        exeName = candidates.isEmpty() ? "shadps4" : candidates.first();
    }
#elif defined(Q_OS_MAC)
    exeName = "shadps4.app";
#endif

    QString fullExePath = versionDir.filePath(exeName);

    if (!QFile::exists(fullExePath)) {
        QStringList filters;
#if defined(Q_OS_WIN)
        filters << "*.exe";
#elif defined(Q_OS_LINUX)
        filters << "*.AppImage" << "shadps4*";
#elif defined(Q_OS_MAC)
        filters << "*.app";
#endif
        QStringList entries = versionDir.entryList(filters, QDir::Files | QDir::Executable);
        if (!entries.isEmpty())
            fullExePath = versionDir.filePath(entries.first());
    }

    if (QMessageBox::question(
            this, tr("Unregister Version"),
            tr("Are you sure you want to unregister this %1 version from the launcher?")
                .arg(uiType),
            QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {

        std::string currentConfigPath = Config::getVersionPath();
        QString currentPathQt =
            QDir::fromNativeSeparators(QString::fromStdString(currentConfigPath));
        QString targetPathQt = QDir::fromNativeSeparators(fullExePath);

        if (currentPathQt == targetPathQt) {
            Config::setVersionPath("");
            Config::save(Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "config.toml");
            m_compat_info->SetSelectedShadExePath("");
        }

        QString shadBase = QString::fromStdString(m_compat_info->GetShadPath());
        QString configName = nameColumn.isEmpty() ? selectedItem->text(0) : nameColumn;

        QString configFilePath = QDir(shadBase).filePath("user/config/" + configName + ".toml");
        QFile::remove(configFilePath);

        QDir configDir(QDir(shadBase).filePath("user/config/" + configName));
        if (configDir.exists())
            configDir.removeRecursively();
        Config::setSdlInstalled(false);
        Config::setQTInstalled(false);
        QMessageBox::information(this, tr("Version Unregistered"),
                                 tr("%1 version has been unregistered from the launcher "
                                    "configuration and its local config set cleared.")
                                     .arg(uiType));

        LoadinstalledList();
        g_MainWindow->autoCheckLauncherBox();
    }
}

void VersionDialog::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        const QList<QUrl> urls = event->mimeData()->urls();
        if (!urls.isEmpty()) {
            QString file = urls.first().toLocalFile();
#if defined(Q_OS_WIN)
            if (file.endsWith(".exe", Qt::CaseInsensitive))
                event->acceptProposedAction();
#elif defined(Q_OS_LINUX)
            if (file.endsWith(".AppImage", Qt::CaseInsensitive))
                event->acceptProposedAction();
#elif defined(Q_OS_MAC)
            event->acceptProposedAction();
#endif
        }
    }
}

void VersionDialog::dropEvent(QDropEvent* event) {
    const QList<QUrl> urls = event->mimeData()->urls();
    if (urls.isEmpty())
        return;

    QString exePath = urls.first().toLocalFile();
    if (exePath.isEmpty())
        return;

    bool ok;
    QString folderName = QInputDialog::getText(
        this, tr("Version name"), tr("Enter the name of this version as it appears in the list."),
        QLineEdit::Normal, "", &ok);
    if (!ok || folderName.trimmed().isEmpty())
        return;

    folderName = folderName.trimmed();

    QString uiChoice = QInputDialog::getItem(this, tr("UI type"), tr("Select UI type:"),
                                             QStringList{tr("Qt"), tr("SDL")}, 0, false, &ok);
    if (!ok)
        return;

    QString uiSuffix = (uiChoice.compare("SDL", Qt::CaseInsensitive) == 0) ? "SDL" : "QT";
    QString finalFolderName = QString("%1_%2_%3").arg(folderName, QString("Custom"), uiSuffix);

    QString basePath = QString::fromStdString(m_compat_info->GetShadPath());
    QString newFolderPath = QDir(basePath).filePath(finalFolderName);

    QDir dir;
    if (dir.exists(newFolderPath)) {
        QMessageBox::warning(this, tr("Error"), tr("A folder with that name already exists."));
        return;
    }

    if (!dir.mkpath(newFolderPath)) {
        QMessageBox::critical(this, tr("Error"), tr("Failed to create folder."));
        return;
    }

    QFileInfo exeInfo(exePath);
    QString targetFilePath = QDir(newFolderPath).filePath(exeInfo.fileName());

    if (!QFile::copy(exePath, targetFilePath)) {
        QMessageBox::critical(this, tr("Error"), tr("Failed to copy executable."));
        return;
    }

    QMessageBox::information(this, tr("Success"), tr("Version added successfully."));
    LoadinstalledList();
}

void VersionDialog::InstallPkgWithV7() {
    if (m_compat_info->GetShadPath().empty()) {
        QMessageBox::warning(this, tr("Setup Required"),
                             tr("First you need to choose a location to save the "
                                "versions in (Path to save versions)."));

        QString initial_path = QString::fromStdString(m_compat_info->GetShadPath());
        QString shad_folder_path_string =
            QFileDialog::getExistingDirectory(this, tr("Select the shadPS4 folder"), initial_path);

        auto folder_path = Common::FS::PathFromQString(shad_folder_path_string);

        if (folder_path.empty()) {
            return;
        }

        m_compat_info->SetShadPath(shad_folder_path_string.toStdString());
        ui->currentShadPath->setText(shad_folder_path_string);
    }

    const QString targetReleasePrefix = "V7-shadPS4";
    QString userPath = QString::fromStdString(m_compat_info->GetShadPath());
    QString installerFolder = QDir(userPath).filePath("PKG_Installer_v7");
    QString installerExe;

#if defined(Q_OS_WIN)
    installerExe = QDir(installerFolder).filePath("extractor.exe");
    QString platform = "win64-qt";
#elif defined(Q_OS_LINUX)
    installerExe = QDir(installerFolder).filePath("extractor");
    QString platform = "linux-qt";
#elif defined(Q_OS_MAC)
    installerExe = QDir(installerFolder).filePath("extractor");
    QString platform = "macos-qt";
#endif

    if (QFile::exists(installerExe)) {
        QProcess::startDetached(installerExe, QStringList() << "--install-pkg");

        QMessageBox infoBox(this);
        infoBox.setWindowTitle(tr("PKG Installer Running"));
        infoBox.setIcon(QMessageBox::Information);
        infoBox.setText(tr("The existing PKG Installer has been launched with the "
                           "installation argument.\n"
                           "Please tap OK after your installation is complete.\n\n"
                           "The app will close the installer process automatically."));
        infoBox.setStandardButtons(QMessageBox::Ok);

        connect(&infoBox, &QMessageBox::accepted, this, []() {
#if defined(Q_OS_WIN)
            QProcess::startDetached("taskkill", QStringList() << "/IM" << "extractor.exe" << "/F");
#elif defined(Q_OS_LINUX) || defined(Q_OS_MAC)
                    QProcess::startDetached("pkill", QStringList() << "extractor");
#endif
        });

        infoBox.exec();
        this->close();
        return;
    }

    const QString forkRepoUrl = "https://api.github.com/repos/diegolix29/shadPS4/releases";

    QMessageBox::StandardButton reply =
        QMessageBox::question(this, tr("PKG Installer Download"),
                              tr("To run the PKG installer, we need to download the %1 build (%2 "
                                 ")in BBFork Repository.\n\nDo you want to proceed?")
                                  .arg(platform)
                                  .arg(targetReleasePrefix),
                              QMessageBox::Yes | QMessageBox::No);
    if (reply == QMessageBox::No)
        return;

    QNetworkAccessManager* manager = new QNetworkAccessManager(this);
    QNetworkReply* apiReply = manager->get(QNetworkRequest(QUrl(forkRepoUrl)));

    connect(
        apiReply, &QNetworkReply::finished, this,
        [this, apiReply, platform, targetReleasePrefix]() {
            if (apiReply->error() != QNetworkReply::NoError) {
                QMessageBox::warning(this, tr("Error"),
                                     tr("Failed to fetch release info: ") +
                                         apiReply->errorString());
                apiReply->deleteLater();
                return;
            }

            QJsonDocument doc = QJsonDocument::fromJson(apiReply->readAll());
            QJsonArray releases = doc.array();
            QString downloadUrl;

            for (const QJsonValue& releaseVal : releases) {
                QJsonObject releaseObj = releaseVal.toObject();
                if (releaseObj["tag_name"].toString().startsWith(targetReleasePrefix)) {
                    QJsonArray assets = releaseObj["assets"].toArray();
                    for (const QJsonValue& assetVal : assets) {
                        QJsonObject assetObj = assetVal.toObject();
                        QString name = assetObj["name"].toString();
                        if (name.contains(platform)) {
                            downloadUrl = assetObj["browser_download_url"].toString();
                            break;
                        }
                    }
                    break;
                }
            }
            apiReply->deleteLater();

            if (downloadUrl.isEmpty()) {
                QMessageBox::critical(
                    this, tr("Error"),
                    tr("Installation files not found for release '%1' or platform %2.")
                        .arg(targetReleasePrefix)
                        .arg(platform));
                return;
            }

            QString userPath = QString::fromStdString(m_compat_info->GetShadPath());
            QString downloadFileName = "temp_pkg_installer.zip";
            const QString destinationPath = userPath + "/" + downloadFileName;
            QString installerFolder = QDir(userPath).filePath("PKG_Installer_v7");

            QDir(installerFolder).removeRecursively();
            QFile::remove(destinationPath);

            QNetworkAccessManager* downloadManager = new QNetworkAccessManager(this);
            QNetworkReply* downloadReply = downloadManager->get(QNetworkRequest(QUrl(downloadUrl)));

            QDialog* progressDialog = new QDialog(this);
            progressDialog->setWindowTitle(
                tr("Downloading PKG Installer (%1)").arg(targetReleasePrefix));
            progressDialog->setFixedSize(400, 80);
            QVBoxLayout* layout = new QVBoxLayout(progressDialog);
            QProgressBar* progressBar = new QProgressBar(progressDialog);
            progressBar->setRange(0, 100);
            layout->addWidget(progressBar);
            progressDialog->setLayout(layout);
            progressDialog->show();

            connect(downloadReply, &QNetworkReply::downloadProgress, this,
                    [progressBar](qint64 bytesReceived, qint64 bytesTotal) {
                        if (bytesTotal > 0)
                            progressBar->setValue(
                                static_cast<int>((bytesReceived * 100) / bytesTotal));
                    });

            QFile* file = new QFile(destinationPath);
            if (!file->open(QIODevice::WriteOnly)) {
                QMessageBox::warning(this, tr("Error"),
                                     tr("Could not save temporary download file."));
                file->deleteLater();
                downloadReply->deleteLater();
                progressDialog->close();
                return;
            }

            connect(downloadReply, &QNetworkReply::readyRead, this,
                    [file, downloadReply]() { file->write(downloadReply->readAll()); });

            connect(
                downloadReply, &QNetworkReply::finished, this,
                [this, file, downloadReply, progressDialog, installerFolder, destinationPath,
                 userPath]() mutable {
                    file->flush();
                    file->close();
                    file->deleteLater();
                    downloadReply->deleteLater();
                    progressDialog->close();
                    progressDialog->deleteLater();

                    if (downloadReply->error() != QNetworkReply::NoError) {
                        QMessageBox::critical(this, tr("Download Failed"),
                                              downloadReply->errorString());
                        QFile::remove(destinationPath);
                        return;
                    }

#if defined(Q_OS_WIN)
                    QString psScript =
                        QString("New-Item -ItemType Directory -Path \"%1\" -Force\n"
                                "Expand-Archive -Path \"%2\" -DestinationPath \"%1\" -Force\n"
                                "Move-Item -Path \"%1/shadps4.exe\" -Destination "
                                "\"%1/extractor.exe\" -Force\n"
                                "Start-Process -FilePath \"%1/extractor.exe\" -ArgumentList "
                                "\"--install-pkg\"\n")
                            .arg(installerFolder)
                            .arg(destinationPath);
                    QString psFile = userPath + "/run_pkg_installer.ps1";
                    QFile f(psFile);
                    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
                        QTextStream out(&f);
                        f.write("\xEF\xBB\xBF");
                        out << psScript;
                        f.close();
                    }
                    QProcess::startDetached("powershell.exe", QStringList()
                                                                  << "-ExecutionPolicy" << "Bypass"
                                                                  << "-File" << psFile);
#else
                    QString shScript = QString("#!/bin/bash\n"
                                               "mkdir -p \"%1\"\n"
                                               "unzip -o \"%2\" -d \"%1\"\n"
                                               "cd \"%1\"\n"
                                               "mv -f Shadps4-qt.AppImage extractor\n"
                                               "chmod +x extractor 2>/dev/null || true\n"
                                               "nohup ./extractor --install-pkg &\n")
                                           .arg(installerFolder)
                                           .arg(destinationPath);

                    QString shFile = userPath + "/run_pkg_installer.sh";
                    QFile f(shFile);
                    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
                        QTextStream out(&f);
                        out << shScript;
                        f.close();
                        f.setPermissions(QFile::ExeUser | QFile::ReadUser | QFile::WriteUser);
                    }
                    QProcess::startDetached("/bin/bash", QStringList() << shFile);
#endif

                    QMessageBox infoBox(this);
                    infoBox.setWindowTitle(tr("PKG Installer Running"));
                    infoBox.setIcon(QMessageBox::Information);
                    infoBox.setText(tr("PKG installer is being called on the downloaded build.\n"
                                       "Please tap OK after your installation.\n\n"
                                       "The app and temporary downloaded build will be close and "
                                       "erased automatically."));
                    infoBox.setStandardButtons(QMessageBox::Ok);

                    connect(&infoBox, &QMessageBox::accepted, this,
                            [installerFolder, destinationPath]() {
#if defined(Q_OS_WIN)
                                QProcess::startDetached(
                                    "taskkill", QStringList() << "/IM" << "extractor.exe" << "/F");
#elif defined(Q_OS_LINUX) || defined(Q_OS_MAC)
                                    QProcess::startDetached("pkill", QStringList() << "extractor");
#endif
                                QDir(installerFolder).removeRecursively();
                                QFile::remove(destinationPath);
                            });

                    infoBox.exec();
                    this->close();
                });
        });
}