// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QDir>
#include <QFileDialog>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
#include "ui_version_dialog.h"
#include "version_dialog.h"

VersionDialog::VersionDialog(std::shared_ptr<CompatibilityInfoClass> compat_info, QWidget* parent)
    : QDialog(parent), ui(new Ui::VersionDialog), m_compat_info(std::move(compat_info)) {
    ui->setupUi(this);

    ui->currentShadPath->setText(QString::fromStdString(m_compat_info->GetShadPath()));

    LoadinstalledList();

    QStringList cachedVersions = LoadDownloadCache();
    if (!cachedVersions.isEmpty()) {
        PopulateDownloadTree(cachedVersions);
    } else {
        DownloadListVersion();
    }

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

    connect(ui->checkChangesVersionButton, &QPushButton::clicked, this,
            [this]() { LoadinstalledList(); });

    connect(ui->restoreExeButton, &QPushButton::clicked, this, [this]() { RestoreOriginalExe(); });
    connect(ui->installVersionButton, &QPushButton::clicked, this,
            [this]() { InstallSelectedVersionExe(); });

    connect(ui->addCustomVersionButton, &QPushButton::clicked, this, [this]() {
        QString exePath;

#ifdef Q_OS_WIN
        exePath = QFileDialog::getOpenFileName(this, tr("Select executable"), QDir::rootPath(),
                                               tr("Executable (*.exe)"));
#elif defined(Q_OS_LINUX)
    exePath = QFileDialog::getOpenFileName(this, tr("Select executable"), QDir::rootPath(),
                                            tr("Executable (*.AppImage)"));
#elif defined(Q_OS_MACOS)
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

        QString basePath = QString::fromStdString(m_compat_info->GetShadPath());
        QString newFolderPath = QDir(basePath).filePath(folderName);

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

        // Get the actual path of the invisible column folder
        QString fullPath = selectedItem->text(3);
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

    connect(ui->checkVersionDownloadButton, &QPushButton::clicked, this,
            [this]() { DownloadListVersion(); });
};

VersionDialog::~VersionDialog() {
    delete ui;
}

std::filesystem::path VersionDialog::GetActualExecutablePath() {
#ifdef __linux__
    if (const char* appimageEnv = std::getenv("APPIMAGE")) {
        return std::filesystem::path(appimageEnv);
    }
#endif
    return Common::FS::GetExecutablePath();
}

void VersionDialog::DownloadListVersion() {
    QNetworkAccessManager* manager = new QNetworkAccessManager(this);
    QUrl url("https://api.github.com/repos/shadps4-emu/shadPS4/tags");
    QNetworkRequest request(url);
    QNetworkReply* reply = manager->get(request);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (reply->error() == QNetworkReply::NoError) {
            QByteArray response = reply->readAll();
            QJsonDocument doc = QJsonDocument::fromJson(response);
            if (doc.isArray()) {
                QJsonArray tags = doc.array();
                ui->downloadTreeWidget->clear();

                QTreeWidgetItem* preReleaseItem = nullptr;
                QList<QTreeWidgetItem*> otherItems;
                bool foundPreRelease = false;

                //  > v.0.5.0
                auto isVersionGreaterThan_0_5_0 = [](const QString& tagName) -> bool {
                    QRegularExpression versionRegex(R"(v\.?(\d+)\.(\d+)\.(\d+))");
                    QRegularExpressionMatch match = versionRegex.match(tagName);
                    if (match.hasMatch()) {
                        int major = match.captured(1).toInt();
                        int minor = match.captured(2).toInt();
                        int patch = match.captured(3).toInt();

                        if (major > 0)
                            return true;
                        if (major == 0 && minor >= 5)
                            return true;
                        if (major == 0 && minor == 5 && patch > 0)
                            return true;
                    }
                    return false;
                };

                for (const QJsonValue& value : tags) {
                    QJsonObject tagObj = value.toObject();
                    QString tagName = tagObj["name"].toString();

                    if (tagName.startsWith("Pre-release", Qt::CaseInsensitive)) {
                        if (!foundPreRelease) {
                            preReleaseItem = new QTreeWidgetItem();
                            preReleaseItem->setText(0, "Pre-release");
                            foundPreRelease = true;
                        }
                        continue;
                    }
                    if (!isVersionGreaterThan_0_5_0(tagName)) {
                        continue;
                    }

                    QTreeWidgetItem* item = new QTreeWidgetItem();
                    item->setText(0, tagName);
                    otherItems.append(item);
                }

                // If you didn't find Pre-release, add it manually
                if (!foundPreRelease) {
                    preReleaseItem = new QTreeWidgetItem();
                    preReleaseItem->setText(0, "Pre-release");
                }

                // Add Pre-release first
                if (preReleaseItem) {
                    ui->downloadTreeWidget->addTopLevelItem(preReleaseItem);
                }

                // Add the others
                for (QTreeWidgetItem* item : otherItems) {
                    ui->downloadTreeWidget->addTopLevelItem(item);
                }

                // Assemble the current list
                QStringList versionList;
                for (int i = 0; i < ui->downloadTreeWidget->topLevelItemCount(); ++i) {
                    QTreeWidgetItem* item = ui->downloadTreeWidget->topLevelItem(i);
                    if (item)
                        versionList.append(item->text(0));
                }

                QStringList cachedVersions = LoadDownloadCache();
                if (cachedVersions == versionList) {
                    QMessageBox::information(this, tr("Version list update"),
                                             tr("No news, the version list is already updated."));
                } else {
                    SaveDownloadCache(versionList);
                    QMessageBox::information(
                        this, tr("Version list update"),
                        tr("The latest versions have been added to the list for download."));
                }

                // selects a version in downloadTreeWidget calls the download stream
                InstallSelectedVersion();
            }
        } else {
            QMessageBox::warning(this, tr("Error"),
                                 tr("Error accessing GitHub API") +
                                     QString(":\n%1").arg(reply->errorString()));
        }
        reply->deleteLater();
    });
}

void VersionDialog::InstallSelectedVersion() {
    connect(
        ui->downloadTreeWidget, &QTreeWidget::itemClicked, this,
        [this](QTreeWidgetItem* item, int) {
            if (m_compat_info->GetShadPath().empty()) {

                QMessageBox::StandardButton reply;
                reply = QMessageBox::warning(this, tr("Select the shadPS4 folder"),
                                             tr("First you need to choose a location to save the "
                                                "versions in\n'Path to save versions'"));
                return;
            }
            QString versionName = item->text(0);
            QString apiUrl;
            QString platform;

#ifdef Q_OS_WIN
            platform = "win64-qt";
#elif defined(Q_OS_LINUX)
            platform = "linux-qt";
#elif defined(Q_OS_MAC)
            platform = "macos-qt";
#endif
            if (versionName == "Pre-release") {
                apiUrl = "https://api.github.com/repos/shadps4-emu/shadPS4/releases";
            } else {
                apiUrl = QString("https://api.github.com/repos/shadps4-emu/"
                                 "shadPS4/releases/tags/%1")
                             .arg(versionName);
            }

            { // Menssage yes/no
                QMessageBox::StandardButton reply;
                reply = QMessageBox::question(this, tr("Download"),
                                              tr("Do you want to download the version:") +
                                                  QString(" %1 ?").arg(versionName),
                                              QMessageBox::Yes | QMessageBox::No);
                if (reply == QMessageBox::No)
                    return;
            }

            QNetworkAccessManager* manager = new QNetworkAccessManager(this);
            QNetworkRequest request(apiUrl);
            QNetworkReply* reply = manager->get(request);

            connect(reply, &QNetworkReply::finished, this, [this, reply, platform, versionName]() {
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
                    QString name = obj["name"].toString();
                    if (name.contains(platform)) {
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
                QNetworkRequest downloadRequest(downloadUrl);
                QNetworkReply* downloadReply = downloadManager->get(downloadRequest);

                QDialog* progressDialog = new QDialog(this);
                progressDialog->setWindowTitle(tr("Downloading %1").arg(versionName));
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
                    QMessageBox::warning(this, tr("Error"), tr("Could not save file."));
                    file->deleteLater();
                    downloadReply->deleteLater();
                    return;
                }

                connect(downloadReply, &QNetworkReply::readyRead, this,
                        [file, downloadReply]() { file->write(downloadReply->readAll()); });

                connect(
                    downloadReply, &QNetworkReply::finished, this,
                    [this, file, downloadReply, progressDialog, release, userPath, versionName]() {
                        file->flush();
                        file->close();
                        file->deleteLater();
                        downloadReply->deleteLater();

                        QString releaseName = release["name"].toString();

                        // Remove "shadPS4 " from the beginning, if it exists
                        if (releaseName.startsWith("shadps4 ", Qt::CaseInsensitive)) {
                            releaseName = releaseName.mid(8);
                        }

                        // Remove "codename" if it exists
                        releaseName.replace(QRegularExpression("\\b[Cc]odename\\s+"), "");

                        QString folderName;
                        if (versionName == "Pre-release") {
                            folderName = release["tag_name"].toString();
                        } else {
                            QString datePart = release["published_at"].toString().left(10);
                            folderName = QString("%1 - %2").arg(releaseName, datePart);
                        }

                        QString destFolder = QDir(userPath).filePath(folderName);

                        // extract ZIP
                        QString scriptFilePath;
                        QString scriptContent;
                        QStringList args;
                        QString process;

#ifdef Q_OS_WIN
                        scriptFilePath = userPath + "/extract_update.ps1";
                        scriptContent = QString("New-Item -ItemType Directory -Path \"%1\" "
                                                "-Force\n"
                                                "Expand-Archive -Path \"%2\" "
                                                "-DestinationPath \"%1\" -Force\n"
                                                "Remove-Item -Force \"%2\"\n"
                                                "Remove-Item -Force \"%3\"\n"
                                                "cls\n")
                                            .arg(destFolder)
                                            .arg(userPath + "/temp_download_update.zip")
                                            .arg(scriptFilePath);
                        process = "powershell.exe";
                        args << "-ExecutionPolicy" << "Bypass" << "-File" << scriptFilePath;
#elif defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
                    scriptFilePath = userPath + "/extract_update.sh";
                    scriptContent = QString(
                                        "#!/bin/bash\n"
                                        "mkdir -p \"%1\"\n"
                                        "unzip -o \"%2\" -d \"%1\"\n"
                                        "rm \"%2\"\n"
                                        "rm \"%3\"\n"
                                        "clear\n")
                                        .arg(destFolder)
                                        .arg(userPath + "/temp_download_update.zip")
                                        .arg(scriptFilePath);
                    process = "bash";
                    args << scriptFilePath;
#endif

                        QFile scriptFile(scriptFilePath);
                        if (scriptFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
                            QTextStream out(&scriptFile);
#ifdef Q_OS_WIN
                            scriptFile.write("\xEF\xBB\xBF"); // BOM
#endif
                            out << scriptContent;
                            scriptFile.close();
#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
                            scriptFile.setPermissions(QFile::ExeUser | QFile::ReadUser |
                                                      QFile::WriteUser);
#endif
                            QProcess::startDetached(process, args);

                            QTimer::singleShot(
                                4000, this, [this, folderName, progressDialog, versionName]() {
                                    progressDialog->close();
                                    progressDialog->deleteLater();
                                    QMessageBox::information(
                                        this, tr("Download"),
                                        tr("Version %1 has been downloaded").arg(versionName));
                                    VersionDialog::LoadinstalledList();
                                });

                        } else {
                            QMessageBox::warning(this, tr("Error"),
                                                 tr("Failed to create zip extraction script:") +
                                                     QString("\n%1").arg(scriptFilePath));
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
    ui->installedTreeWidget->setColumnCount(4);        // 4 = FullPath
    ui->installedTreeWidget->setColumnHidden(3, true); // FullPath invisible

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

    // add (Pre-release, Test Build...)
    for (const QString& folder : otherDirs) {
        QTreeWidgetItem* item = new QTreeWidgetItem(ui->installedTreeWidget);
        QString fullPath = QDir(path).filePath(folder);
        item->setText(3, fullPath); // FullPath invisible column

        if (folder.startsWith("Pre-release-shadPS4")) {
            QStringList parts = folder.split('-');
            item->setText(0, "Pre-release"); // Version
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
        } else if (folder.contains(" - ")) {
            QStringList parts = folder.split(" - ");
            item->setText(0, parts.value(0));
            item->setText(1, parts.value(1));
            item->setText(2, parts.value(2));
        } else {
            item->setText(0, folder); // only Version
            item->setText(1, "");
            item->setText(2, "");
        }
    }

    // add versions
    for (const auto& pair : versionedDirs) {
        QTreeWidgetItem* item = new QTreeWidgetItem(ui->installedTreeWidget);
        QString fullPath = QDir(path).filePath(pair.second);
        item->setText(3, fullPath); // FullPath invisible column

        if (pair.second.contains(" - ")) {
            QStringList parts = pair.second.split(" - ");
            item->setText(0, parts.value(0)); // Version
            item->setText(1, parts.value(1)); // Name
            item->setText(2, parts.value(2)); // Date
        } else {
            item->setText(0, pair.second); // only Version
            item->setText(1, "");
            item->setText(2, "");
        }
    }
    QString currentExePath = QString::fromStdString(m_compat_info->GetSelectedShadExePath());
    for (int i = 0; i < ui->installedTreeWidget->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = ui->installedTreeWidget->topLevelItem(i);
        QString fullPath = item->text(3); // invisible column
        if (!fullPath.isEmpty() && fullPath == currentExePath) {
            ui->installedTreeWidget->setCurrentItem(item);
            break;
        }
    }

    connect(ui->installedTreeWidget, &QTreeWidget::itemSelectionChanged, this, [this]() {
        QTreeWidgetItem* selectedItem = ui->installedTreeWidget->currentItem();
        if (selectedItem) {
            QString exePath = selectedItem->text(3); // invisible full path
            if (!exePath.isEmpty())
                m_compat_info->SetSelectedShadExePath(exePath.toStdString());
        }
    });

    ui->installedTreeWidget->resizeColumnToContents(1); // 1 = Name
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
                preReleaseItem->setText(0, "Pre-release");
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
    }

    if (preReleaseItem)
        ui->downloadTreeWidget->addTopLevelItem(preReleaseItem);
    for (QTreeWidgetItem* item : otherItems)
        ui->downloadTreeWidget->addTopLevelItem(item);

    InstallSelectedVersion();
}

void VersionDialog::InstallSelectedVersionExe() {
    QTreeWidgetItem* selectedItem = ui->installedTreeWidget->currentItem();
    if (!selectedItem) {
        QMessageBox::warning(this, tr("Notice"), tr("No version selected from Installed list."));
        return;
    }

    QString fullPath = selectedItem->text(3); // version folder path
    if (fullPath.isEmpty()) {
        QMessageBox::critical(this, tr("Error"), tr("Invalid version path."));
        return;
    }

    std::filesystem::path exePath = GetActualExecutablePath();
    if (exePath.empty()) {
        QMessageBox::critical(this, tr("Error"),
                              tr("Could not determine current executable path."));
        return;
    }

    QString destExe = QString::fromStdString(exePath.string());

    // The new source exe comes from the version folder
    QDir versionDir(fullPath);
#ifdef Q_OS_LINUX
    // look for .AppImage in the version folder
    QStringList candidates = versionDir.entryList(QStringList() << "*.AppImage", QDir::Files);
    if (candidates.isEmpty()) {
        QMessageBox::critical(this, tr("Error"),
                              tr("No AppImage found in version folder:\n%1").arg(fullPath));
        return;
    }
    QString sourceExe = versionDir.filePath(candidates.first());
#else
    QString sourceExe =
        QDir(fullPath).filePath(QString::fromStdString(exePath.filename().string()));
#endif

    QString backupExe = destExe + ".bak";

    if (QFile::exists(destExe)) {
        if (QFile::exists(backupExe)) {
            QFile::remove(backupExe);
        }
        if (!QFile::rename(destExe, backupExe)) {
            QMessageBox::critical(this, tr("Error"),
                                  tr("Failed to backup current executable:\n%1").arg(destExe));
            return;
        }
    }

    if (!QFile::copy(sourceExe, destExe)) {
        QMessageBox::critical(this, tr("Error"),
                              tr("Failed to copy executable to:\n%1").arg(destExe));
        return;
    }
    QFile::setPermissions(destExe,
                          QFileDevice::ExeUser | QFileDevice::ReadUser | QFileDevice::WriteUser);

    m_compat_info->SetSelectedShadExePath(destExe.toStdString());

    QMessageBox::information(
        this, tr("Success"),
        tr("Switched to version: %1\nPrevious executable was backed up as .bak")
            .arg(selectedItem->text(0)));

    if (QMessageBox::question(this, tr("Run Version"), tr("Do you want to run this version now?"),
                              QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
        QProcess::startDetached(destExe);
    }
}

void VersionDialog::RestoreOriginalExe() {
    std::filesystem::path exePath = GetActualExecutablePath();
    if (exePath.empty()) {
        QMessageBox::critical(this, tr("Error"),
                              tr("Could not determine current executable path."));
        return;
    }

    QString destExe = QString::fromStdString(exePath.string());
    QString backupExe = destExe + ".bak";

    if (!QFile::exists(backupExe)) {
        QMessageBox::warning(this, tr("Restore"),
                             tr("No backup executable found at:\n%1").arg(backupExe));
        return;
    }

    if (QFile::exists(destExe)) {
        QFile::remove(destExe);
    }

    if (!QFile::rename(backupExe, destExe)) {
        QMessageBox::critical(this, tr("Error"),
                              tr("Failed to restore backup:\n%1").arg(backupExe));
        return;
    }

    QFile::setPermissions(destExe,
                          QFileDevice::ExeUser | QFileDevice::ReadUser | QFileDevice::WriteUser);

    m_compat_info->SetSelectedShadExePath(destExe.toStdString());

    QMessageBox::information(this, tr("Restored"),
                             tr("Original executable has been restored:\n%1").arg(destExe));
}
