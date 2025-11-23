// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QVBoxLayout>

#include "common/path_util.h"
#include "core/file_sys/fs.h"
#include "mod_manager_dialog.h"

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

    QString modsRoot;
    Common::FS::PathToQString(modsRoot, Common::FS::GetUserPath(Common::FS::PathType::ModsFolder));
    modsRoot += "/" + gameSerial;

    availablePath = modsRoot + "/Available";
    activePath = modsRoot + "/Active";
    backupsRoot = modsRoot + "/Backups";

    QDir().mkpath(availablePath);
    QDir().mkpath(activePath);
    QDir().mkpath(backupsRoot);
    QDir().mkpath(overlayRoot);

    auto* layout = new QHBoxLayout(this);

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
    std::filesystem::path basePath = gamePath.toStdString();
    if (!std::filesystem::exists(gamePath.toStdString()))
        return false;

    for (auto& entry : std::filesystem::recursive_directory_iterator(modPath)) {
        if (!entry.is_regular_file())
            continue;

        std::filesystem::path relative = std::filesystem::relative(entry.path(), modPath);

        std::filesystem::path expected = basePath / relative;

        if (std::filesystem::exists(expected)) {
            return true;
        }
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

    QString modBackupRoot = backupsRoot + "/" + modName;
    QDir().mkpath(modBackupRoot);

    QDirIterator it(src, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (!it.fileInfo().isFile())
            continue;
        QString rel = QDir(src).relativeFilePath(it.filePath());

        QString gameFile = resolveOriginalFile(rel);

        if (needsDvdrootPrefix(modName)) {
            if (!rel.startsWith("dvdroot_ps4/")) {
                rel = "dvdroot_ps4/" + rel;
            }
        }

        QString destPath = overlayRoot + "/" + rel;
        QDir().mkpath(QFileInfo(destPath).absolutePath());

        if (QFile::exists(destPath)) {
            QString owner = findModThatContainsFile(rel);
            if (!owner.isEmpty() && owner != modName) {
                greyedOutMods.insert(owner);
                updateModListUI();
            }
        }

        if (!gameFile.isEmpty()) {
            QString backupFile = modBackupRoot + "/" + rel;
            QDir().mkpath(QFileInfo(backupFile).absolutePath());

            if (QFile::exists(backupFile)) {
                QString stamped =
                    backupFile + "." + QString::number(QDateTime::currentSecsSinceEpoch());
                QFile::rename(backupFile, stamped);
            }

            QFile::copy(gameFile, backupFile);
        }

        if (QFile::exists(destPath))
            QFile::remove(destPath);

        if (!QFile::copy(it.filePath(), destPath)) {
            qWarning() << "Failed to copy mod file" << it.filePath() << "to" << destPath;
        }
    }

    QString activeModDir = activePath + "/" + modName;
    if (!QDir(activeModDir).exists())
        QDir().mkpath(activeModDir);
}

void ModManagerDialog::uninstallMod(const QString& modName) {
    QString path = availablePath + "/" + modName;

    if (QDir(path).exists()) {
        QDir(path).removeRecursively();
    }

    scanAvailableMods();
}

void ModManagerDialog::activateSelected() {
    auto items = listAvailable->selectedItems();
    for (auto* item : items) {
        QString modName = item->text();

        installMod(modName);

        QString src = availablePath + "/" + modName;
        QString dst = activePath + "/" + modName;

        if (QDir(dst).exists())
            QDir(dst).removeRecursively();

        QFile::rename(src, dst);

        listActive->addItem(modName);
        delete listAvailable->takeItem(listAvailable->row(item));
    }
}

void ModManagerDialog::installModFromDisk() {
    QString path =
        QFileDialog::getOpenFileName(this, "Select Mod Folder or Archive", QString(),
                                     "Mods (*.zip *.rar *.7z *.tar *.gz *.tgz);;All Files (*.*)");

    if (path.isEmpty())
        return;

    QFileInfo info(path);

    if (info.isFile()) {
        QString modName = info.baseName();
        QString dst = availablePath + "/" + modName;

        if (QDir(dst).exists()) {
            QMessageBox::warning(this, "Mod Exists", "This mod already exists.");
            return;
        }

        QDir().mkpath(dst);

        if (!ExtractArchive(path, dst)) {
            QMessageBox::critical(this, "Extraction Failed", "Could not extract the archive.");
            return;
        }

        scanAvailableMods();
        return;
    }

    QString modName = info.fileName();
    QString dst = availablePath + "/" + modName;

    if (QDir(dst).exists()) {
        QMessageBox::warning(this, "Mod Exists", "This mod already exists.");
        return;
    }

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

        listAvailable->addItem(modName);
        delete listActive->takeItem(listActive->row(item));
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

void ModManagerDialog::restoreMod(const QString& modName) {
    QString modBackupRoot = backupsRoot + "/" + modName;
    if (!QDir(modBackupRoot).exists())
        return;

    QString activeModPath = activePath + "/" + modName;
    {
        QDirIterator modIt(activeModPath, QDirIterator::Subdirectories);
        while (modIt.hasNext()) {
            modIt.next();

            if (!modIt.fileInfo().isFile())
                continue;

            QString rel = QDir(activeModPath).relativeFilePath(modIt.filePath());
            QString overlayFile = overlayRoot + "/" + rel;

            if (QFile::exists(overlayFile)) {
                QFile::remove(overlayFile);
            }
        }
    }

    {
        QDirIterator backupIt(modBackupRoot, QDirIterator::Subdirectories);
        while (backupIt.hasNext()) {
            backupIt.next();

            if (!backupIt.fileInfo().isFile())
                continue;

            QString rel = QDir(modBackupRoot).relativeFilePath(backupIt.filePath());
            QString restorePath = resolveOriginalFolderForRestore(rel);

            QDir().mkpath(QFileInfo(restorePath).absolutePath());

            if (QFile::exists(restorePath))
                QFile::remove(restorePath);

            QFile::copy(backupIt.filePath(), restorePath);
        }
    }
    QDir(modBackupRoot).removeRecursively();
}
