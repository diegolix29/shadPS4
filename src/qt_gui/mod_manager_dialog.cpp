// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <memory>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "common/path_util.h"
#include "core/file_sys/fs.h"
#include "mod_manager_dialog.h"
#include "mod_tracker.h"

ModManagerDialog::ModManagerDialog(const QString& gamePath, const QString& gameSerial,
                                   QWidget* parent)
    : QDialog(parent), gamePath(gamePath), gameSerial(gameSerial) {
    setWindowTitle("Mod Manager");
    setMinimumSize(600, 380);

    QString baseGamePath = gamePath;
    QString updatePath = gamePath + "-UPDATE";
    QString patchPath = gamePath + "-patch";

    if (!Core::FileSys::MntPoints::manual_mods_path.empty()) {
        overlayRoot = QString::fromStdString(Core::FileSys::MntPoints::manual_mods_path.string());
    } else {
        overlayRoot = gamePath + "-MODS";
    }
    cleanupOverlayRootIfEmpty();

    QString modsRoot;
    Common::FS::PathToQString(modsRoot, Common::FS::GetUserPath(Common::FS::PathType::ModsFolder));
    modsRoot += "/" + gameSerial;

    availablePath = modsRoot + "/Available";
    activePath = modsRoot + "/Active";
    backupsRoot = modsRoot + "/Backups";

    QDir().mkpath(availablePath);
    QDir().mkpath(activePath);
    QDir().mkpath(backupsRoot);

    // Initialize ModTracker
    modTracker = std::make_unique<ModTracker>(gameSerial, modsRoot);
    modTracker->loadFromFile();

    auto* mainLayout = new QVBoxLayout(this);

    QLabel* lblGameTitle = new QLabel(QString("Game: %1").arg(gameSerial), this);
    lblGameTitle->setAlignment(Qt::AlignCenter);
    QFont font = lblGameTitle->font();
    font.setPointSize(14);
    font.setBold(true);
    lblGameTitle->setFont(font);
    mainLayout->addWidget(lblGameTitle);

    auto* layout = new QHBoxLayout();
    mainLayout->addLayout(layout);

    listAvailable = new QListWidget(this);
    listActive = new QListWidget(this);

    auto* leftColumn = new QVBoxLayout();
    auto* lblAvailable = new QLabel("Available Mods");
    lblAvailable->setAlignment(Qt::AlignCenter);

    leftColumn->addWidget(lblAvailable);
    leftColumn->addWidget(listAvailable);

    auto* rightColumn = new QVBoxLayout();
    auto* lblActive = new QLabel("Active Mods");
    lblActive->setAlignment(Qt::AlignCenter);

    rightColumn->addWidget(lblActive);
    rightColumn->addWidget(listActive);

    auto* buttons = new QVBoxLayout();
    auto* btnAdd = new QPushButton(">");
    auto* btnRemove = new QPushButton("<");
    auto* btnAddAll = new QPushButton(">>");
    auto* btnRemoveAll = new QPushButton("<<");
    auto* btnClose = new QPushButton("Close");
    auto* btnInstall = new QPushButton("Install Mod");
    auto* btnUninstall = new QPushButton("Uninstall Mod");

    leftColumn->addWidget(btnInstall);
    leftColumn->addWidget(btnUninstall);

    buttons->addWidget(btnAdd);
    buttons->addWidget(btnRemove);
    buttons->addWidget(btnAddAll);
    buttons->addWidget(btnRemoveAll);
    buttons->addStretch();
    buttons->addWidget(btnClose);

    layout->addLayout(leftColumn);
    layout->addLayout(buttons);
    layout->addLayout(rightColumn);

    scanAvailableMods();
    scanActiveMods();
    connect(btnInstall, &QPushButton::clicked, this, &ModManagerDialog::installModFromDisk);
    connect(btnUninstall, &QPushButton::clicked, this, &ModManagerDialog::removeAvailableMod);

    connect(btnAdd, &QPushButton::clicked, this, &ModManagerDialog::activateSelected);
    connect(btnRemove, &QPushButton::clicked, this, &ModManagerDialog::deactivateSelected);
    connect(btnAddAll, &QPushButton::clicked, this, &ModManagerDialog::activateAll);
    connect(btnRemoveAll, &QPushButton::clicked, this, &ModManagerDialog::deactivateAll);
    connect(btnClose, &QPushButton::clicked, this, &QDialog::close);
}

QString ModManagerDialog::findModThatContainsFile(const QString& relPath) const {
    QDir dir(activePath);
    for (const QString& mod : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QString candidate = activePath + "/" + mod + "/" + relPath;
        if (QFile::exists(candidate))
            return mod;
    }
    return "";
}

void ModManagerDialog::updateModListUI() {
    for (int i = 0; i < listActive->count(); i++) {
        QListWidgetItem* it = listActive->item(i);
        QString name = it->text();
        bool blocked = greyedOutMods.contains(name);

        it->setFlags(blocked ? Qt::NoItemFlags : Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    }
}

void ModManagerDialog::scanAvailableMods() {
    listAvailable->clear();

    QDir dir(availablePath);
    for (const QString& mod : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        listAvailable->addItem(mod);
    }
}

void ModManagerDialog::cleanupOverlayRootIfEmpty() {
    QDir overlayDir(overlayRoot);
    if (!overlayDir.exists())
        return;

    // Check if there are any files or subfolders
    QStringList entries = overlayDir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
    if (entries.isEmpty()) {
        overlayDir.removeRecursively();
    }
}

void ModManagerDialog::activateAll() {
    QList<QString> allMods;
    for (int i = 0; i < listAvailable->count(); i++)
        allMods.append(listAvailable->item(i)->text());

    for (const QString& name : allMods) {
        installMod(name);
        listActive->addItem(name);
    }

    listAvailable->clear();
}

void ModManagerDialog::deactivateAll() {
    QList<QString> allMods;
    for (int i = 0; i < listActive->count(); i++)
        allMods.append(listActive->item(i)->text());

    for (const QString& name : allMods) {
        uninstallMod(name);
        listAvailable->addItem(name);
    }

    listActive->clear();
}

void ModManagerDialog::scanActiveMods() {
    listActive->clear();

    QDir dir(activePath);
    for (const QString& mod : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        listActive->addItem(mod);
}

bool ModManagerDialog::modMatchesGame(const std::filesystem::path& modPath) const {
    const std::filesystem::path basePath{gamePath.toStdString()};

    std::error_code ec;
    if (!std::filesystem::exists(basePath, ec))
        return false;

    std::filesystem::recursive_directory_iterator it(
        modPath, std::filesystem::directory_options::skip_permission_denied, ec);

    for (const auto& entry : it) {
        if (ec)
            continue;

        if (!entry.is_regular_file(ec))
            continue;

        const std::filesystem::path rel = entry.path().lexically_relative(modPath);
        if (rel.empty())
            continue;

        if (std::filesystem::exists(basePath / rel, ec))
            return true;
    }

    return false;
}

QString ModManagerDialog::resolveOriginalFile(const QString& rel) const {
    QStringList searchOrder;

    // 1. manual_mods_path
    if (!Core::FileSys::MntPoints::manual_mods_path.empty()) {
        searchOrder << QString::fromStdString(Core::FileSys::MntPoints::manual_mods_path.string());
    }

    searchOrder << (gamePath + "-MODS");

    searchOrder << (gamePath + "-patch");

    searchOrder << (gamePath + "-UPDATE");

    searchOrder << gamePath;

    for (const QString& dir : searchOrder) {
        QString candidate = dir + "/" + rel;
        if (QFile::exists(candidate))
            return candidate;
    }

    return QString();
}

bool ModManagerDialog::needsDvdrootPrefix(const QString& modName) const {
    if (!(gameSerial == "CUSA03173" || gameSerial == "CUSA00900" || gameSerial == "CUSA00299" ||
          gameSerial == "CUSA00207"))
        return false;

    static const QSet<QString> bloodborneRootFolders = {
        "action", "chr",    "event", "facegen", "map",   "menu",     "movie",
        "msg",    "mtd",    "obj",   "other",   "param", "paramdef", "parts",
        "remo",   "script", "sfx",   "shader",  "sound"};

    QString modRoot = availablePath + "/" + modName;

    QDir dir(modRoot);
    for (const QString& entry : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (bloodborneRootFolders.contains(entry))
            return true;
        if (entry == "dvdroot_ps4")
            return false;
    }

    return false;
}

bool ModManagerDialog::ExtractArchive(const QString& archivePath, const QString& outputPath) {
    QFileInfo info(archivePath);
    QString ext = info.suffix().toLower();

#ifdef _WIN32
    if (ext == "zip") {
        int result =
            QProcess::execute("powershell.exe", {"-NoProfile", "-Command",
                                                 "Expand-Archive -LiteralPath \"" + archivePath +
                                                     "\" "
                                                     "-DestinationPath \"" +
                                                     outputPath + "\" -Force"});
        return (result == 0);
    }

    {
        QStringList args;
        args << "x" << archivePath << QString("-o%1").arg(outputPath) << "-y";

        int result = QProcess::execute("7z", args);
        if (result == 0)
            return true;
    }

    return false;

#elif __APPLE__

    if (ext == "zip") {
        int result = QProcess::execute("unzip", {"-o", archivePath, "-d", outputPath});
        return (result == 0);
    }

    if (ext == "tar" || ext == "gz" || ext == "tgz") {
        int result = QProcess::execute("tar", {"-xf", archivePath, "-C", outputPath});
        return (result == 0);
    }

    {
        QStringList args;
        args << "x" << archivePath << QString("-o%1").arg(outputPath) << "-y";

        int result = QProcess::execute("7z", args);
        if (result == 0)
            return true;
    }

    return false;

#else

    if (ext == "zip") {
        int result = QProcess::execute("unzip", {"-o", archivePath, "-d", outputPath});
        if (result == 0)
            return true;
    }

    if (ext == "tar" || ext == "gz" || ext == "tgz") {
        int result = QProcess::execute("tar", {"-xf", archivePath, "-C", outputPath});
        if (result == 0)
            return true;
    }

    {
        QStringList args;
        args << "x" << archivePath << QString("-o%1").arg(outputPath) << "-y";

        if (QProcess::execute("7z", args) == 0)
            return true;

        // Try unrar
        if (QProcess::execute("unrar", args) == 0)
            return true;
    }

    return false;
#endif
}

void ModManagerDialog::installMod(const QString& modName) {
    QString src = availablePath + "/" + modName;
    if (!QDir(src).exists())
        return;

    // NOTE: installMod should ONLY extract to Available folder
    // The actual copying to overlay and tracking happens in activateSelected
    // This function should NOT copy files to overlay or track them

    qDebug() << "Mod" << modName << "installed to Available folder only";
    qDebug() << "Files will be copied to overlay and tracked when mod is activated";
}

void ModManagerDialog::copyModToOverlayAndTrack(const QString& modName) {
    QString src =
        availablePath + "/" + modName; // Files are still in availablePath during activation
    if (!QDir(src).exists()) {
        qWarning() << "Mod source not found:" << modName;
        return;
    }

    // Create ModInfo for tracking FIRST
    ModInfo modInfo;
    modInfo.name = modName;
    modInfo.gameSerial = gameSerial;
    modInfo.installedAt = QDateTime::currentDateTime();
    modInfo.isActive = false; // Will be set to true in activateSelected

    // Add mod to tracker before adding files
    modTracker->addMod(modInfo);

    QString modBackupRoot = backupsRoot + "/" + modName;
    QDir().mkpath(modBackupRoot);

    QDirIterator it(src, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (!it.fileInfo().isFile())
            continue;
        QString rel = QDir(src).relativeFilePath(it.filePath());

        if (needsDvdrootPrefix(modName)) {
            if (!rel.startsWith("dvdroot_ps4/")) {
                rel = "dvdroot_ps4/" + rel;
            }
        }

        QString destPath = overlayRoot + "/" + rel;
        QDir().mkpath(QFileInfo(destPath).absolutePath());

        QString backupFile;
        QString originalFile;

        // Check if file already exists in overlayRoot (this is what we need to backup)
        if (QFile::exists(destPath)) {
            backupFile = modBackupRoot + "/" + rel;
            QDir().mkpath(QFileInfo(backupFile).absolutePath());

            // If backup already exists, timestamp it
            if (QFile::exists(backupFile)) {
                QString stamped =
                    backupFile + "." + QString::number(QDateTime::currentSecsSinceEpoch());
                QFile::rename(backupFile, stamped);
            }

            // Backup the existing overlay file
            if (QFile::copy(destPath, backupFile)) {
                qDebug() << "Backed up existing overlay file:" << destPath << "->" << backupFile;
            } else {
                qWarning() << "Failed to backup overlay file:" << destPath;
            }
        }

        // Find the original game file for reference (but don't backup it)
        originalFile = resolveOriginalFile(rel);

        if (QFile::exists(destPath)) {
            QString owner = findModThatContainsFile(rel);
            if (!owner.isEmpty() && owner != modName) {
                greyedOutMods.insert(owner);
                updateModListUI();
            }
        }

        if (QFile::exists(destPath))
            QFile::remove(destPath);

        if (!QFile::copy(it.filePath(), destPath)) {
            qWarning() << "Failed to copy mod file" << it.filePath() << "to" << destPath;
        }

        // Track this file in ModTracker (now the mod exists)
        modTracker->addFileToMod(modName, rel, originalFile, backupFile);
    }

    // Save the updated mod info with all files
    modTracker->saveToFile();

    if (!QDir(overlayRoot).exists()) {
        QDir().mkpath(overlayRoot);
    }

    qDebug() << "Mod" << modName << "copied to overlay and tracked with"
             << modTracker->getModFiles(modName).size() << "files";
}

void ModManagerDialog::uninstallMod(const QString& modName) {
    QString path = availablePath + "/" + modName;

    if (QDir(path).exists()) {
        QDir(path).removeRecursively();
    }

    // Remove mod from tracker
    modTracker->removeMod(modName);
    modTracker->saveToFile();

    scanAvailableMods();
}

void ModManagerDialog::activateSelected() {
    auto items = listAvailable->selectedItems();
    for (auto* item : items) {
        QString modName = item->text();
        QString src = availablePath + "/" + modName;

        // Use ModTracker for better conflict detection
        QSet<QString> conflictingMods = modTracker->findConflictingMods(modName);

        if (!conflictingMods.isEmpty()) {
            QString msg = "This mod conflicts with the following active mods:\n\n";
            for (const QString& conflictingMod : conflictingMods) {
                ModInfo conflictInfo = modTracker->getMod(conflictingMod);
                if (conflictInfo.isActive) {
                    msg += QString("- %1\n").arg(conflictingMod);
                    greyedOutMods.insert(conflictingMod);
                }
            }

            msg += "\nActivating this mod will overwrite conflicting files. Continue?";
            if (!showScrollableConflictDialog(msg))
                continue;

            updateModListUI();
        }

        // Copy files to overlay and track them BEFORE renaming
        copyModToOverlayAndTrack(modName);

        QString dst = activePath + "/" + modName;
        if (QDir(dst).exists())
            QDir(dst).removeRecursively();

        QFile::rename(src, dst);

        // Mark mod as active in tracker
        modTracker->setModActive(modName, true);
        modTracker->saveToFile();

        listActive->addItem(modName);
        delete listAvailable->takeItem(listAvailable->row(item));
    }
}

QStringList ModManagerDialog::detectModConflicts(const QString& modInstallPath,
                                                 const QString& incomingRootPath) {
    QStringList conflicts;

    QDir incoming(incomingRootPath);
    QDir base(modInstallPath);

    QDirIterator it(incomingRootPath, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (!it.fileInfo().isFile())
            continue;

        QString rel = incoming.relativeFilePath(it.filePath());
        if (needsDvdrootPrefix(QFileInfo(incomingRootPath).fileName()) &&
            !rel.startsWith("dvdroot_ps4/")) {
            rel = "dvdroot_ps4/" + rel;
        }
        QString targetFile = modInstallPath + "/" + rel;

        if (QFile::exists(targetFile))
            conflicts << rel;
    }

    return conflicts;
}

void ModManagerDialog::installModFromDisk() {
    QString path;

    QMessageBox msgBox(this);
    msgBox.setWindowTitle("Select Mod");
    msgBox.setText("Are you installing a folder or an archive?");
    QPushButton* folderBtn = msgBox.addButton("Folder", QMessageBox::AcceptRole);
    QPushButton* archiveBtn = msgBox.addButton("Archive", QMessageBox::AcceptRole);

    msgBox.exec();

    if (msgBox.clickedButton() == folderBtn) {
        path = QFileDialog::getExistingDirectory(this, "Select Mod Folder", QString(),
                                                 QFileDialog::ShowDirsOnly |
                                                     QFileDialog::DontResolveSymlinks);
    } else if (msgBox.clickedButton() == archiveBtn) {
        path = QFileDialog::getOpenFileName(
            this, "Select Mod Archive", QString(),
            "Mods (*.zip *.rar *.7z *.tar *.gz *.tgz);;All Files (*.*)");
    } else {
        return;
    }

    if (path.isEmpty())
        return;

    QFileInfo info(path);

#ifdef _WIN32
    if (info.suffix().toLower() == "rar") {
        QMessageBox::information(
            this, "RAR Not Supported",
            "RAR archives are not supported for mod installation on Windows.\n"
            "Please unpack the RAR archive manually and install the mod as a folder, "
            "or use a different supported archive format (ZIP, 7Z, TAR, GZ, TGZ).");
        return;
    }
#endif

    QString modName = info.baseName();
    QString dst = availablePath + "/" + modName;

    if (QDir(dst).exists()) {
        QMessageBox::warning(this, "Mod Exists", "This mod already exists.");
        return;
    }

    if (info.isDir()) {
        QDir().mkpath(dst);
        QDirIterator it(path, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            if (!it.fileInfo().isFile())
                continue;

            QString rel = QDir(path).relativeFilePath(it.filePath());
            QString outFile = dst + "/" + rel;

            QDir().mkpath(QFileInfo(outFile).absolutePath());
            QFile::copy(it.filePath(), outFile);
        }

        normalizeExtractedMod(dst);
        scanAvailableMods();
        return;
    }

    QString tempExtract = availablePath + "/.__tmp_extract_" + modName;
    QDir().mkpath(tempExtract);

    if (!ExtractArchive(path, tempExtract)) {
        QMessageBox::warning(this, "Extraction Failed", "Unable to extract the mod archive.");
        QDir(tempExtract).removeRecursively();
        return;
    }

    normalizeExtractedMod(tempExtract);
    QDir().rename(tempExtract, dst);

    scanAvailableMods();
}

void ModManagerDialog::removeAvailableMod() {
    auto items = listAvailable->selectedItems();
    if (items.isEmpty())
        return;

    QString modName = items.first()->text();
    QString modPath = availablePath + "/" + modName;

    QDir dir(modPath);
    dir.removeRecursively();

    scanAvailableMods();
}

void ModManagerDialog::deactivateSelected() {
    auto items = listActive->selectedItems();
    for (auto* item : items) {
        QString modName = item->text();

        // Restore files BEFORE marking mod as inactive
        restoreMod(modName);

        QSet<QString> modsToUnblock = greyedOutMods;
        for (const QString& m : modsToUnblock)
            greyedOutMods.remove(m);

        updateModListUI();
        QString src = activePath + "/" + modName;
        QString dst = availablePath + "/" + modName;

        if (QDir(dst).exists())
            QDir(dst).removeRecursively();

        QFile::rename(src, dst);

        // Mark mod as inactive AFTER file operations are complete
        modTracker->setModActive(modName, false);
        modTracker->saveToFile();

        listAvailable->addItem(modName);
        delete listActive->takeItem(listActive->row(item));
    }
}

void ModManagerDialog::cleanupEmptyDirectories(const QString& path) {
    QDir dir(path);
    if (!dir.exists()) {
        return;
    }

    // First, collect all directories to avoid modifying the directory while iterating
    QStringList allDirs;
    QDirIterator it(path, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        allDirs.append(it.filePath());
    }

    // Sort in reverse order to process subdirectories first
    std::sort(allDirs.begin(), allDirs.end(), std::greater<QString>());

    int removedDirs = 0;
    for (const QString& dirPath : allDirs) {
        QDir currentDir(dirPath);

        // Check if directory is empty (no files and no subdirectories)
        if (currentDir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty()) {
            if (currentDir.rmdir(dirPath)) {
                qDebug() << "Removed empty directory:" << dirPath;
                removedDirs++;
            } else {
                qWarning() << "Failed to remove empty directory:" << dirPath;
            }
        }
    }

    // Finally, check if the root overlay directory itself is empty
    if (dir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty()) {
        if (dir.rmdir(path)) {
            qDebug() << "Removed empty overlay root directory:" << path;
            removedDirs++;
        }
    }

    if (removedDirs > 0) {
        qDebug() << "Cleaned up" << removedDirs << "empty directories from" << path;
    }
}

QString ModManagerDialog::resolveOriginalFolderForRestore(const QString& rel) const {
    QStringList searchOrder;

    if (!Core::FileSys::MntPoints::manual_mods_path.empty())
        searchOrder << QString::fromStdString(Core::FileSys::MntPoints::manual_mods_path.string());

    searchOrder << (gamePath + "-MODS");
    searchOrder << (gamePath + "-patch");
    searchOrder << (gamePath + "-UPDATE");
    searchOrder << gamePath;

    for (const QString& base : searchOrder) {
        QString dst = base + "/" + rel;
        if (QDir(QFileInfo(dst).absolutePath()).exists())
            return dst;
    }

    return gamePath + "/" + rel;
}
QString ModManagerDialog::normalizeExtractedMod(const QString& modPath) {
    QDir root(modPath);

    static const QSet<QString> gameRoots = {"dvdroot_ps4", "action", "chr",   "event",    "facegen",
                                            "map",         "menu",   "movie", "msg",      "mtd",
                                            "obj",         "other",  "param", "paramdef", "parts",
                                            "remo",        "script", "sfx",   "shader",   "sound"};

    while (true) {
        QStringList entries = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

        if (entries.isEmpty())
            break;

        if (entries.contains("dvdroot_ps4")) {
            break;
        }

        bool hasGameRoots = false;
        for (const QString& e : entries) {
            if (gameRoots.contains(e)) {
                hasGameRoots = true;
                break;
            }
        }

        if (hasGameRoots) {
            if (needsDvdrootPrefix(QFileInfo(modPath).fileName())) {
                QString dvdroot = modPath + "/dvdroot_ps4";
                QDir().mkpath(dvdroot);

                for (const QString& e : entries) {
                    QString s = modPath + "/" + e;
                    QString t = dvdroot + "/" + e;
                    QFileInfo fi(s);
                    if (fi.isDir())
                        QDir().rename(s, t);
                    else
                        QFile::rename(s, t);
                }
            }
            break;
        }

        if (entries.size() == 1) {
            QString wrapper = modPath + "/" + entries.first();
            QDir wrapperDir(wrapper);
            if (!wrapperDir.exists())
                break;

            for (const QString& e : wrapperDir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
                QString s = wrapper + "/" + e;
                QString t = modPath + "/" + e;
                QFileInfo fi(s);
                if (fi.isDir())
                    QDir().rename(s, t);
                else
                    QFile::rename(s, t);
            }
            wrapperDir.removeRecursively();
        } else {
            for (const QString& e : entries) {
                QString s = modPath + "/" + e;
                QString t = modPath + "/" + e;
                Q_UNUSED(t);
            }
            break;
        }
    }

    return modPath;
}

bool ModManagerDialog::showScrollableConflictDialog(const QString& text) {
    QDialog dlg(this);
    dlg.setWindowTitle("Mod Conflict Detected");
    dlg.setModal(true);
    dlg.setMinimumSize(600, 400);
    dlg.setMaximumSize(800, 600);

    auto* layout = new QVBoxLayout(&dlg);

    QLabel* label = new QLabel(text);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setAlignment(Qt::AlignTop | Qt::AlignLeft);

    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSize(9);
    label->setFont(mono);

    auto* scroll = new QScrollArea();
    scroll->setWidget(label);
    scroll->setWidgetResizable(true);

    layout->addWidget(scroll);

    auto* btnLayout = new QHBoxLayout();
    QPushButton* okBtn = new QPushButton("OK");
    QPushButton* cancelBtn = new QPushButton("Cancel");

    btnLayout->addStretch();
    btnLayout->addWidget(cancelBtn);
    btnLayout->addWidget(okBtn);

    layout->addLayout(btnLayout);

    QObject::connect(okBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    QObject::connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);

    return dlg.exec() == QDialog::Accepted;
}

void ModManagerDialog::restoreMod(const QString& modName) {
    qDebug() << "Starting restoreMod for:" << modName;

    ModInfo modInfo = modTracker->getMod(modName);
    if (modInfo.name.isEmpty()) {
        qDebug() << "Mod not found in tracker, using fallback method for:" << modName;

        QString modBackupRoot = backupsRoot + "/" + modName;
        if (!QDir(modBackupRoot).exists()) {
            QMessageBox::warning(
                this, "Mod Deactivation Warning",
                QString("No backup data found for mod '%1'.\nFiles may not be properly restored.")
                    .arg(modName));
            return;
        }

        QString activeModPath = activePath + "/" + modName;
        int filesRemoved = 0;
        int filesRestored = 0;

        QDirIterator modIt(activeModPath, QDirIterator::Subdirectories);
        while (modIt.hasNext()) {
            modIt.next();
            if (!modIt.fileInfo().isFile())
                continue;

            QString rel = QDir(activeModPath).relativeFilePath(modIt.filePath());
            QString overlayFile = overlayRoot + "/" + rel;
            if (QFile::exists(overlayFile)) {
                qDebug() << "Removing overlay file (fallback):" << overlayFile;
                if (QFile::remove(overlayFile)) {
                    filesRemoved++;
                }
            }
        }

        QDirIterator backupIt(modBackupRoot, QDirIterator::Subdirectories);
        while (backupIt.hasNext()) {
            backupIt.next();
            if (!backupIt.fileInfo().isFile())
                continue;

            QString rel = QDir(modBackupRoot).relativeFilePath(backupIt.filePath());
            QString restorePath = overlayRoot + "/" + rel;

            QDir().mkpath(QFileInfo(restorePath).absolutePath());

            if (QFile::exists(restorePath))
                QFile::remove(restorePath);

            if (QFile::copy(backupIt.filePath(), restorePath)) {
                filesRestored++;
            }
        }

        QDir(modBackupRoot).removeRecursively();

        QMessageBox::information(
            this, "Mod Deactivation Complete",
            QString("Mod '%1' deactivated (fallback method).\n%2 files removed, %3 files restored.")
                .arg(modName)
                .arg(filesRemoved)
                .arg(filesRestored));
        return;
    }

    QStringList modFiles = modTracker->getModFiles(modName);
    qDebug() << "Mod files to process:" << modFiles.size();

    if (modFiles.isEmpty()) {
        QMessageBox::warning(this, "Mod Deactivation Warning",
                             QString("No files found in tracker for mod '%1'.\nThe mod may not "
                                     "have been properly installed."));
        return;
    }

    int filesRemoved = 0;
    int filesRestored = 0;
    int filesKeptForOtherMods = 0;
    QStringList errors;

    for (const QString& relativePath : modFiles) {
        QString overlayFile = overlayRoot + "/" + relativePath;
        qDebug() << "Processing file:" << relativePath << "->" << overlayFile;

        QStringList otherModsNeedingFile;
        QList<ModInfo> allActiveMods = modTracker->getActiveMods();
        qDebug() << "Current active mods count:" << allActiveMods.size();

        for (const ModInfo& otherMod : allActiveMods) {
            if (otherMod.name != modName && otherMod.files.contains(relativePath)) {
                otherModsNeedingFile.append(otherMod.name);
                qDebug() << "File needed by other mod:" << otherMod.name;
            }
        }

        if (otherModsNeedingFile.isEmpty()) {
            qDebug() << "No other mods need file, removing/restoring:" << relativePath;
            ModFileInfo fileInfo = modInfo.fileDetails.value(relativePath);
            if (!fileInfo.backupPath.isEmpty() && QFile::exists(fileInfo.backupPath)) {
                QDir().mkpath(QFileInfo(overlayFile).absolutePath());
                if (QFile::exists(overlayFile)) {
                    qDebug() << "Removing overlay file before restore:" << overlayFile;
                    QFile::remove(overlayFile);
                }
                if (QFile::copy(fileInfo.backupPath, overlayFile)) {
                    filesRestored++;
                    qDebug() << "Restored from backup:" << fileInfo.backupPath;
                } else {
                    errors.append(QString("Failed to restore %1 from backup").arg(relativePath));
                }
            } else {
                if (QFile::exists(overlayFile)) {
                    qDebug() << "Removing overlay file (no backup):" << overlayFile;
                    if (QFile::remove(overlayFile)) {
                        filesRemoved++;
                    } else {
                        errors.append(QString("Failed to remove %1").arg(relativePath));
                    }
                }
            }
        } else {
            qDebug() << "File needed by other mods, keeping:" << relativePath;
            filesKeptForOtherMods++;
            ModInfo nextMod = modTracker->getMod(otherModsNeedingFile.first());
            QString modSourcePath = activePath + "/" + nextMod.name + "/" + relativePath;
            QString nextModBackupPath = backupsRoot + "/" + nextMod.name + "/" + relativePath;

            bool fileUpdated = false;
            if (QFile::exists(modSourcePath)) {
                QDir().mkpath(QFileInfo(overlayFile).absolutePath());
                if (QFile::exists(overlayFile)) {
                    QFile::remove(overlayFile);
                }
                if (QFile::copy(modSourcePath, overlayFile)) {
                    fileUpdated = true;
                }
            } else if (!nextModBackupPath.isEmpty() && QFile::exists(nextModBackupPath)) {
                QDir().mkpath(QFileInfo(overlayFile).absolutePath());
                if (QFile::exists(overlayFile)) {
                    QFile::remove(overlayFile);
                }
                if (QFile::copy(nextModBackupPath, overlayFile)) {
                    fileUpdated = true;
                }
            }

            if (!fileUpdated) {
                errors.append(QString("Failed to update %1 for mod %2")
                                  .arg(relativePath, otherModsNeedingFile.first()));
            }
        }
    }

    QString modBackupRoot = backupsRoot + "/" + modName;
    if (QDir(modBackupRoot).exists()) {
        QDir(modBackupRoot).removeRecursively();
    }

    cleanupEmptyDirectories(overlayRoot);
}
