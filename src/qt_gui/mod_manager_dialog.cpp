// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <memory>
#include <QApplication>
#include <QButtonGroup>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QProcess>
#include <QProxyStyle>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSplitter>
#include <QStyleOption>
#include <QTextEdit>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

#include <QDesktopServices>
#include <QInputDialog>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTabWidget>
#include "common/path_util.h"
#include "core/file_sys/fs.h"
#include "mod_manager_dialog.h"
#include "mod_tracker.h"
#include "nexus_mods_api.h"

ModManagerDialog::ModManagerDialog(const QString& gamePath, const QString& gameSerial,
                                   QWidget* parent)
    : QDialog(parent), gamePath(gamePath), gameSerial(gameSerial), currentSortIndex(0),
      isGridView(true), searchBox(nullptr), sortComboBox(nullptr), gridViewBtn(nullptr),
      listViewBtn(nullptr), refreshBtn(nullptr), closeBtn(nullptr), installModBtn(nullptr),
      uninstallModBtn(nullptr), titleLabel(nullptr), modListWidget(nullptr), modScrollArea(nullptr),
      modListContainer(nullptr), modListLayout(nullptr), viewModeGroup(nullptr),
      listAvailable(nullptr), listActive(nullptr) {
    setWindowTitle(QString("Browse %1 Mods").arg(gameSerial));
    setMinimumSize(900, 700);
    resize(1200, 800);

    QString baseGamePath = gamePath;
    QString updatePath = gamePath + "-UPDATE";
    QString patchPath = gamePath + "-patch";

    if (!Core::FileSys::MntPoints::manual_mods_path.empty()) {
        overlayRoot = QString::fromStdString(Core::FileSys::MntPoints::manual_mods_path.string());
    } else {
        overlayRoot = findModFolder(gamePath);
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

    modTracker = std::make_unique<ModTracker>(gameSerial, modsRoot);
    modTracker->loadFromFile();

    setupUI();

    m_nexusApi = new NexusModsApi(this);
    m_imgNam = new QNetworkAccessManager(this);
    connect(m_nexusApi, &NexusModsApi::searchResultsReady, this,
            &ModManagerDialog::onNexusSearchResultsReady);
    connect(m_nexusApi, &NexusModsApi::modFilesReady, this,
            &ModManagerDialog::onNexusModFilesReady);
    connect(m_nexusApi, &NexusModsApi::downloadLinkReady, this,
            &ModManagerDialog::onNexusDownloadLinkReady);
    connect(m_nexusApi, &NexusModsApi::errorOccurred, this, &ModManagerDialog::onNexusError);
    connect(m_nexusApi, &NexusModsApi::apiKeyValidated, this,
            &ModManagerDialog::onNexusApiKeyValidated);
    if (m_nexusApi->autoDetectApiKey()) {
        m_nexusApi->validateApiKey();
    } else {
        updateNexusKeyStatus();
    }
    applyDarkTheme();
    scanAvailableMods();
    scanActiveMods();
    updateModDisplay();
}

void ModManagerDialog::setupUI() {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    mainLayout->setSpacing(10);

    setupHeader();
    mainLayout->addWidget(titleLabel);

    auto* tabs = new QTabWidget(this);
    tabs->setDocumentMode(true);

    auto* localTab = new QWidget();
    auto* localLayout = new QVBoxLayout(localTab);
    localLayout->setContentsMargins(0, 0, 0, 0);
    setupModList();
    localLayout->addWidget(modScrollArea);
    tabs->addTab(localTab, "Installed Mods");

    setupNexusTab(tabs);

    mainLayout->addWidget(tabs);
}

void ModManagerDialog::setupHeader() {
    auto* titleContainer = new QWidget(this);
    auto* titleLayout = new QVBoxLayout(titleContainer);

    auto* titleLabel = new QLabel(QString("Browse %1 Mods").arg(gameSerial), this);
    titleLabel->setAlignment(Qt::AlignCenter);
    QFont font = titleLabel->font();
    font.setPointSize(18);
    font.setBold(true);
    titleLabel->setFont(font);

    auto* headerLayout = new QHBoxLayout();
    searchBox = new QLineEdit(this);
    searchBox->setPlaceholderText("Search mods...");
    searchBox->setMinimumWidth(300);
    searchBox->setClearButtonEnabled(true);
    sortComboBox = new QComboBox(this);
    sortComboBox->addItem("Date Added", Qt::DescendingOrder);
    sortComboBox->addItem("Name", Qt::AscendingOrder);
    sortComboBox->addItem("Author", Qt::AscendingOrder);
    sortComboBox->addItem("Size", Qt::DescendingOrder);
    sortComboBox->setMinimumWidth(120);
    gridViewBtn = new QPushButton("Grid", this);
    gridViewBtn->setCheckable(true);
    gridViewBtn->setChecked(true);

    listViewBtn = new QPushButton("List", this);
    listViewBtn->setCheckable(true);

    viewModeGroup = new QButtonGroup(this);
    viewModeGroup->addButton(gridViewBtn, 0);
    viewModeGroup->addButton(listViewBtn, 1);
    viewModeGroup->setExclusive(true);
    refreshBtn = new QPushButton("Refresh", this);
    installModBtn = new QPushButton("Install Mod", this);
    closeBtn = new QPushButton("Close", this);

    headerLayout->addWidget(searchBox);
    headerLayout->addWidget(sortComboBox);
    headerLayout->addWidget(gridViewBtn);
    headerLayout->addWidget(listViewBtn);
    headerLayout->addStretch();
    headerLayout->addWidget(refreshBtn);
    auto* openFolderBtn = new QPushButton("Open Folder");
    openFolderBtn->setStyleSheet(R"(
        QPushButton {
            background-color: #2d2d2d;
            color: #ffffff;
            border: 1px solid #404040;
            border-radius: 4px;
            padding: 8px 16px;
            font-weight: bold;
        }
        QPushButton:hover {
            background-color: #404040;
            border-color: #0078d4;
        }
    )");

    connect(openFolderBtn, &QPushButton::clicked, this, [this]() {
        QString folderPath = availablePath;
        if (!QDir(folderPath).exists()) {
            QDir().mkpath(folderPath);
        }
        QUrl url = QUrl::fromLocalFile(folderPath);
        if (!QDesktopServices::openUrl(url)) {
            QMessageBox::warning(this, "Error", "Could not open the mods folder.");
        }
    });

    headerLayout->addWidget(openFolderBtn);
    headerLayout->addWidget(installModBtn);
    headerLayout->addWidget(closeBtn);

    connect(searchBox, &QLineEdit::textChanged, this, &ModManagerDialog::onSearchTextChanged);
    connect(sortComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ModManagerDialog::onSortOrderChanged);
    connect(viewModeGroup, QOverload<int>::of(&QButtonGroup::idClicked), this,
            &ModManagerDialog::onViewModeChanged);
    connect(refreshBtn, &QPushButton::clicked, this, &ModManagerDialog::onRefreshMods);
    connect(installModBtn, &QPushButton::clicked, this, &ModManagerDialog::installModFromDisk);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);

    titleLayout->addWidget(titleLabel);
    titleLayout->addLayout(headerLayout);

    this->titleLabel = titleContainer;
}

void ModManagerDialog::setupModList() {
    modScrollArea = new QScrollArea(this);
    modScrollArea->setWidgetResizable(true);
    modScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    modScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    modListContainer = new QWidget(this);
    modListLayout = new QVBoxLayout(modListContainer);
    modListLayout->setSpacing(10);
    modListLayout->setAlignment(Qt::AlignTop);

    modScrollArea->setWidget(modListContainer);
}

void ModManagerDialog::setupFooter() {}

void ModManagerDialog::applyDarkTheme() {
    QString darkStyle = R"(
        QDialog {
            background-color: #1e1e1e;
            color: #ffffff;
        }
        QLineEdit {
            background-color: #2d2d2d;
            color: #ffffff;
            border: 1px solid #404040;
            border-radius: 4px;
            padding: 6px;
            selection-background-color: #0078d4;
        }
        QComboBox {
            background-color: #2d2d2d;
            color: #ffffff;
            border: 1px solid #404040;
            border-radius: 4px;
            padding: 6px;
        }
        QComboBox::drop-down {
            border: none;
            width: 20px;
        }
        QComboBox::down-arrow {
            image: none;
            border-left: 4px solid transparent;
            border-right: 4px solid transparent;
            border-top: 4px solid #ffffff;
        }
        QPushButton {
            background-color: #2d2d2d;
            color: #ffffff;
            border: 1px solid #404040;
            border-radius: 4px;
            padding: 8px 16px;
            font-weight: bold;
        }
        QPushButton:hover {
            background-color: #404040;
            border-color: #0078d4;
        }
        QPushButton:checked {
            background-color: #0078d4;
            border-color: #0078d4;
        }
        QScrollArea {
            background-color: #1e1e1e;
            border: 1px solid #404040;
            border-radius: 4px;
        }
        QListWidget {
            background-color: #2d2d2d;
            color: #ffffff;
            border: 1px solid #404040;
            border-radius: 4px;
        }
        QListWidget::item {
            background-color: #2d2d2d;
            border-bottom: 1px solid #404040;
            padding: 8px;
        }
        QListWidget::item:selected {
            background-color: #0078d4;
        }
        QListWidget::item:hover {
            background-color: #404040;
        }
    )";

    setStyleSheet(darkStyle);
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

QString ModManagerDialog::findModFolder(const QString& gamePath) const {
    QStringList modSuffixes = {"-mods", "-MODS", "-Mods"};

    for (const QString& suffix : modSuffixes) {
        QString modPath = gamePath + suffix;
        if (QDir(modPath).exists()) {
            return modPath;
        }
    }
    return gamePath + "-MODS";
}

void ModManagerDialog::onSearchTextChanged(const QString& text) {
    if (!searchBox)
        return;
    currentSearchText = text.toLower();
    filterMods();
    updateModDisplay();
}

void ModManagerDialog::onSortOrderChanged(int index) {
    if (!sortComboBox)
        return;
    currentSortIndex = index;
    sortMods();
    updateModDisplay();
}

void ModManagerDialog::onViewModeChanged() {
    if (!viewModeGroup)
        return;
    isGridView = (viewModeGroup->checkedId() == 0);
    updateModDisplay();
}

void ModManagerDialog::onModItemClicked(QListWidgetItem* item) {
    Q_UNUSED(item);
}

void ModManagerDialog::onModItemDoubleClicked(QListWidgetItem* item) {
    onToggleModActive(item);
}

void ModManagerDialog::onToggleModActive(QListWidgetItem* item) {
    if (!item || !modTracker)
        return;

    QString modName = item->data(Qt::UserRole).toString();
    ModInfo modInfo = modTracker->getMod(modName);

    if (modInfo.isActive) {
        deactivateSelected();
    } else {
        activateSelected();
    }
}

void ModManagerDialog::onRefreshMods() {
    greyedOutMods.clear();

    scanAvailableMods();
    scanActiveMods();
    filterMods();
    sortMods();
    updateModDisplay();
}

void ModManagerDialog::updateModDisplay() {
    if (!modListLayout || !modListContainer) {
        return;
    }

    greyedOutMods.clear();
    while (QLayoutItem* item = modListLayout->takeAt(0)) {
        if (item->widget()) {
            item->widget()->hide();
            item->widget()->setParent(nullptr);
            item->widget()->deleteLater();
        }
        if (item->layout()) {
            while (QLayoutItem* nestedItem = item->layout()->takeAt(0)) {
                if (nestedItem->widget()) {
                    nestedItem->widget()->hide();
                    nestedItem->widget()->setParent(nullptr);
                    nestedItem->widget()->deleteLater();
                }
                delete nestedItem;
            }
        }
        delete item;
    }
    modListContainer->hide();
    QApplication::processEvents();

    if (isGridView) {
        auto* gridLayout = new QGridLayout();
        gridLayout->setSpacing(10);
        gridLayout->setContentsMargins(5, 5, 5, 5);
        int columns = 3;
        int row = 0, col = 0;

        for (const ModInfo& modInfo : filteredMods) {
            auto* modWidget = createModItem(modInfo);
            if (modWidget) {
                gridLayout->addWidget(modWidget, row, col);

                col++;
                if (col >= columns) {
                    col = 0;
                    row++;
                }
            }
        }

        modListLayout->addLayout(gridLayout);
    } else {
        for (const ModInfo& modInfo : filteredMods) {
            auto* modWidget = createModListItem(modInfo);
            if (modWidget) {
                modListLayout->addWidget(modWidget);
            }
        }
    }

    modListLayout->addStretch();
    modListContainer->show();
    modListContainer->updateGeometry();
    modListContainer->repaint();
}

void ModManagerDialog::filterMods() {
    filteredMods.clear();

    for (const ModInfo& modInfo : allMods) {
        bool matches = true;

        if (!currentSearchText.isEmpty()) {
            matches = modInfo.name.toLower().contains(currentSearchText) ||
                      modInfo.author.toLower().contains(currentSearchText) ||
                      modInfo.description.toLower().contains(currentSearchText);
        }

        if (matches) {
            filteredMods.append(modInfo);
        }
    }
}

void ModManagerDialog::sortMods() {
    if (filteredMods.isEmpty())
        return;

    std::sort(filteredMods.begin(), filteredMods.end(), [this](const ModInfo& a, const ModInfo& b) {
        switch (currentSortIndex) {
        case 0:
            return a.installedAt > b.installedAt;
        case 1:
            return a.name.toLower() < b.name.toLower();
        case 2:
            return a.author.toLower() < b.author.toLower();
        case 3: {
            qint64 sizeA = getModSizeBytes(a.name);
            qint64 sizeB = getModSizeBytes(b.name);
            return sizeA > sizeB;
        }
        default:
            return a.name.toLower() < b.name.toLower();
        }
    });
}

QWidget* ModManagerDialog::createModItem(const ModInfo& modInfo) {
    auto* widget = new QWidget();
    widget->setMinimumSize(320, 260);
    widget->setMaximumSize(380, 280);
    widget->setStyleSheet(R"(
        QWidget {
            background-color: #2d2d2d;
            border: 1px solid #404040;
            border-radius: 12px;
            padding: 12px;
        }
        QWidget:hover {
            border-color: #0078d4;
        }
    )");

    auto* layout = new QVBoxLayout(widget);
    layout->setSpacing(4);
    auto* titleLabel = new QLabel(modInfo.name);
    titleLabel->setStyleSheet("QLabel { font-weight: bold; font-size: 14px; color: #ffffff; }");
    titleLabel->setWordWrap(true);
    titleLabel->setMinimumHeight(30);
    titleLabel->setMaximumHeight(40);
    auto* authorLabel =
        new QLabel(QString("by %1").arg(modInfo.author.isEmpty() ? "Unknown" : modInfo.author));
    authorLabel->setStyleSheet("QLabel { color: #cccccc; font-size: 11px; }");
    authorLabel->setMinimumHeight(15);
    auto* typeLabel = new QLabel(getModTypeString(modInfo.name));
    typeLabel->setStyleSheet("QLabel { color: #4CAF50; font-size: 10px; font-weight: bold; }");
    typeLabel->setWordWrap(true);
    typeLabel->setMinimumHeight(25);
    typeLabel->setMaximumHeight(35);
    auto* sizeLabel = new QLabel(getModSizeString(modInfo.name));
    sizeLabel->setStyleSheet("QLabel { color: #888888; font-size: 10px; }");
    sizeLabel->setMinimumHeight(15);
    auto* statusLabel = new QLabel(modInfo.isActive ? "Active" : "Inactive");
    statusLabel->setStyleSheet(
        modInfo.isActive ? "QLabel { color: #4CAF50; font-size: 10px; font-weight: bold; }"
                         : "QLabel { color: #ff9800; font-size: 10px; font-weight: bold; }");
    statusLabel->setMinimumHeight(15);
    auto* buttonLayout = new QHBoxLayout();
    buttonLayout->setSpacing(6);

    auto* previewBtn = new QPushButton("Details");
    previewBtn->setStyleSheet(R"(
        QPushButton {
            background-color: #0078d4;
            color: #ffffff;
            border: none;
            border-radius: 4px;
            padding: 4px 10px;
            font-size: 10px;
            min-height: 20px;
        }
        QPushButton:hover {
            background-color: #106ebe;
        }
    )");

    auto* downloadBtn = new QPushButton(modInfo.isActive ? "Deactivate" : "Activate");
    downloadBtn->setStyleSheet(modInfo.isActive ? R"(
        QPushButton {
            background-color: #f44336;
            color: #ffffff;
            border: none;
            border-radius: 4px;
            padding: 4px 10px;
            font-size: 10px;
            min-height: 20px;
        }
        QPushButton:hover {
            background-color: #d32f2f;
        }
    )"
                                                : R"(
        QPushButton {
            background-color: #4CAF50;
            color: #ffffff;
            border: none;
            border-radius: 4px;
            padding: 4px 10px;
            font-size: 10px;
            min-height: 20px;
        }
        QPushButton:hover {
            background-color: #45a049;
        }
    )");

    buttonLayout->addWidget(previewBtn);
    buttonLayout->addWidget(downloadBtn);
    buttonLayout->addStretch();
    connect(downloadBtn, &QPushButton::clicked, this, [this, modInfo]() {
        if (modInfo.isActive) {
            deactivateModByName(modInfo.name);
        } else {
            activateModByName(modInfo.name);
        }
    });
    connect(previewBtn, &QPushButton::clicked, this,
            [this, modInfo]() { showModDetails(modInfo.name); });
    layout->addWidget(titleLabel);
    layout->addWidget(authorLabel);
    layout->addWidget(typeLabel);
    layout->addWidget(sizeLabel);
    layout->addWidget(statusLabel);
    layout->addLayout(buttonLayout);
    widget->setProperty("modName", modInfo.name);

    return widget;
}

QWidget* ModManagerDialog::createModListItem(const ModInfo& modInfo) {
    auto* widget = new QWidget();
    widget->setMinimumHeight(60);
    widget->setMaximumHeight(80);
    widget->setStyleSheet(R"(
        QWidget {
            background-color: #2d2d2d;
            border: 1px solid #404040;
            border-radius: 4px;
            padding: 8px;
            margin: 2px;
        }
        QWidget:hover {
            border-color: #0078d4;
        }
    )");

    auto* layout = new QHBoxLayout(widget);
    auto* titleLabel = new QLabel(modInfo.name);
    titleLabel->setStyleSheet("QLabel { font-weight: bold; font-size: 14px; color: #ffffff; }");
    titleLabel->setWordWrap(true);
    titleLabel->setMinimumWidth(200);
    titleLabel->setMaximumWidth(250);
    auto* authorLabel =
        new QLabel(QString("by %1").arg(modInfo.author.isEmpty() ? "Unknown" : modInfo.author));
    authorLabel->setStyleSheet("QLabel { color: #cccccc; font-size: 12px; }");
    authorLabel->setMinimumWidth(120);
    authorLabel->setMaximumWidth(150);
    auto* typeLabel = new QLabel(getModTypeString(modInfo.name));
    typeLabel->setStyleSheet("QLabel { color: #4CAF50; font-size: 11px; font-weight: bold; }");
    typeLabel->setMinimumWidth(100);
    typeLabel->setMaximumWidth(150);
    auto* sizeLabel = new QLabel(getModSizeString(modInfo.name));
    sizeLabel->setStyleSheet("QLabel { color: #888888; font-size: 11px; }");
    sizeLabel->setMinimumWidth(60);
    sizeLabel->setMaximumWidth(80);
    auto* statusLabel = new QLabel(modInfo.isActive ? "Active" : "Inactive");
    statusLabel->setStyleSheet(
        modInfo.isActive ? "QLabel { color: #4CAF50; font-size: 11px; font-weight: bold; }"
                         : "QLabel { color: #ff9800; font-size: 11px; font-weight: bold; }");
    statusLabel->setMinimumWidth(60);
    statusLabel->setMaximumWidth(80);
    auto* previewBtn = new QPushButton("Details");
    previewBtn->setStyleSheet(R"(
        QPushButton {
            background-color: #0078d4;
            color: #ffffff;
            border: none;
            border-radius: 4px;
            padding: 4px 8px;
            font-size: 11px;
        }
        QPushButton:hover {
            background-color: #106ebe;
        }
    )");

    auto* downloadBtn = new QPushButton(modInfo.isActive ? "Deactivate" : "Activate");
    downloadBtn->setStyleSheet(modInfo.isActive ? R"(
        QPushButton {
            background-color: #f44336;
            color: #ffffff;
            border: none;
            border-radius: 4px;
            padding: 4px 8px;
            font-size: 11px;
        }
        QPushButton:hover {
            background-color: #d32f2f;
        }
    )"
                                                : R"(
        QPushButton {
            background-color: #4CAF50;
            color: #ffffff;
            border: none;
            border-radius: 4px;
            padding: 4px 8px;
            font-size: 11px;
        }
        QPushButton:hover {
            background-color: #45a049;
        }
    )");

    connect(downloadBtn, &QPushButton::clicked, this, [this, modInfo]() {
        if (modInfo.isActive) {
            deactivateModByName(modInfo.name);
        } else {
            activateModByName(modInfo.name);
        }
    });
    connect(previewBtn, &QPushButton::clicked, this,
            [this, modInfo]() { showModDetails(modInfo.name); });

    layout->addWidget(previewBtn);
    layout->addWidget(downloadBtn);
    layout->addStretch();
    widget->setProperty("modName", modInfo.name);

    return widget;
}

QPixmap ModManagerDialog::getModThumbnail(const QString& modName) const {
    if (modName.isEmpty()) {
        return QPixmap();
    }

    QString modPath = availablePath + "/" + modName;
    QStringList possibleNames = {"thumbnail.png", "preview.png", "image.png", "screenshot.png",
                                 "cover.jpg"};

    for (const QString& name : possibleNames) {
        QString imagePath = modPath + "/" + name;
        if (QFile::exists(imagePath)) {
            QPixmap pixmap(imagePath);
            if (!pixmap.isNull()) {
                return pixmap;
            }
        }
    }

    QPixmap pixmap(280, 120);
    pixmap.fill(QColor(64, 64, 64));
    return pixmap;
}

QString ModManagerDialog::getModSizeString(const QString& modName) const {
    if (modName.isEmpty()) {
        return "0 B";
    }

    QString modPath = availablePath + "/" + modName;
    if (!QDir(modPath).exists()) {
        modPath = activePath + "/" + modName;
        if (!QDir(modPath).exists()) {
            return "0 B";
        }
    }

    qint64 totalSize = getModSizeBytes(modName);

    if (totalSize < 1024) {
        return QString("%1 B").arg(totalSize);
    } else if (totalSize < 1024 * 1024) {
        return QString("%1 KB").arg(totalSize / 1024.0, 0, 'f', 1);
    } else if (totalSize < 1024 * 1024 * 1024) {
        return QString("%1 MB").arg(totalSize / (1024.0 * 1024.0), 0, 'f', 1);
    } else {
        return QString("%1 GB").arg(totalSize / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1);
    }
}

qint64 ModManagerDialog::getModSizeBytes(const QString& modName) const {
    if (modName.isEmpty()) {
        return 0;
    }

    QString modPath = availablePath + "/" + modName;
    if (!QDir(modPath).exists()) {
        modPath = activePath + "/" + modName;
        if (!QDir(modPath).exists()) {
            return 0;
        }
    }

    qint64 totalSize = 0;

    QDirIterator it(modPath, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (it.fileInfo().isFile()) {
            totalSize += it.fileInfo().size();
        }
    }

    return totalSize;
}

QString ModManagerDialog::getModTypeString(const QString& modName) const {
    if (modName.isEmpty()) {
        return "General";
    }

    QString modPath = availablePath + "/" + modName;
    if (!QDir(modPath).exists()) {
        modPath = activePath + "/" + modName;
        if (!QDir(modPath).exists()) {
            return "General";
        }
    }

    QDir modDir(modPath);

    QString basePath = modPath;
    if (modDir.exists("dvdroot_ps4")) {
        basePath = modPath + "/dvdroot_ps4";
        modDir = QDir(basePath);
    }

    QStringList folders = modDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    if (folders.isEmpty()) {
        return "Empty";
    }

    QStringList modFolders;
    for (const QString& folder : folders) {
        QString folderPath = basePath + "/" + folder;
        QDir folderDir(folderPath);
        QStringList files = folderDir.entryList(QDir::Files);
        if (!files.isEmpty()) {
            modFolders.append(folder);
            if (modFolders.size() >= 3) {
                break;
            }
        }
    }

    if (modFolders.isEmpty()) {
        return "No files";
    }

    return modFolders.join(", ");
}

void ModManagerDialog::scanAvailableMods() {
    if (!modTracker) {
        return;
    }

    allMods.clear();
    QDir dir(availablePath);
    QStringList modFolders = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    for (const QString& modName : modFolders) {
        ModInfo modInfo = modTracker->getMod(modName);
        if (modInfo.name.isEmpty()) {
            modInfo.name = modName;
            modInfo.gameSerial = gameSerial;
            modInfo.installedAt = QDateTime::currentDateTime();
            modInfo.isActive = false;
            modInfo.author = "Unknown";
            modInfo.description = "";
            modInfo.version = "1.0";
        }

        allMods.append(modInfo);
    }
    filteredMods = allMods;
}

void ModManagerDialog::cleanupOverlayRootIfEmpty() {
    QDir overlayDir(overlayRoot);
    if (!overlayDir.exists())
        return;

    QStringList entries = overlayDir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
    if (entries.isEmpty()) {
        overlayDir.removeRecursively();
    }
}

void ModManagerDialog::activateAll() {
    for (const ModInfo& modInfo : allMods) {
        if (!modInfo.isActive) {
            QString modName = modInfo.name;
            QString src = availablePath + "/" + modName;
            QString dst = activePath + "/" + modName;

            if (QDir(src).exists()) {
                copyModToOverlayAndTrack(modName);

                if (QDir(dst).exists())
                    QDir(dst).removeRecursively();

                QFile::rename(src, dst);

                modTracker->setModActive(modName, true);
                modTracker->saveToFile();
            }
        }
    }

    scanAvailableMods();
    scanActiveMods();
    filterMods();
    sortMods();
    updateModDisplay();
}

void ModManagerDialog::deactivateAll() {
    QList<ModInfo> activeMods;
    for (const ModInfo& modInfo : allMods) {
        if (modInfo.isActive) {
            activeMods.append(modInfo);
        }
    }

    for (const ModInfo& modInfo : activeMods) {
        QString modName = modInfo.name;

        restoreMod(modName);

        QSet<QString> modsToUnblock = greyedOutMods;
        for (const QString& m : modsToUnblock)
            greyedOutMods.remove(m);

        QString src = activePath + "/" + modName;
        QString dst = availablePath + "/" + modName;

        if (QDir(dst).exists())
            QDir(dst).removeRecursively();

        QFile::rename(src, dst);

        modTracker->setModActive(modName, false);
        modTracker->saveToFile();
    }

    scanAvailableMods();
    scanActiveMods();
    filterMods();
    sortMods();
    updateModDisplay();
}

void ModManagerDialog::activateModByName(const QString& modName) {
    QString src = availablePath + "/" + modName;
    QString dst = activePath + "/" + modName;

    if (!QDir(src).exists()) {
        QMessageBox::warning(this, "Mod Not Found",
                             QString("Mod '%1' not found in Available folder.").arg(modName));
        return;
    }

    QSet<QString> conflictingMods = modTracker->findConflictingMods(modName);
    QStringList activeConflicts;
    for (const QString& conflictingMod : conflictingMods) {
        ModInfo conflictInfo = modTracker->getMod(conflictingMod);
        if (conflictInfo.isActive) {
            activeConflicts.append(conflictingMod);
        }
    }

    if (!activeConflicts.isEmpty()) {
        QString msg = "This mod conflicts with following active mods:\n\n";
        for (const QString& conflictingMod : activeConflicts) {
            msg += QString("- %1\n").arg(conflictingMod);
            greyedOutMods.insert(conflictingMod);
        }

        msg += "\nActivating this mod will overwrite conflicting files. Continue?";
        if (!showScrollableConflictDialog(msg))
            return;
    }

    copyModToOverlayAndTrack(modName);

    if (QDir(dst).exists())
        QDir(dst).removeRecursively();

    QFile::rename(src, dst);

    modTracker->setModActive(modName, true);
    modTracker->saveToFile();

    scanAvailableMods();
    scanActiveMods();
    filterMods();
    sortMods();
    updateModDisplay();
}

void ModManagerDialog::deactivateModByName(const QString& modName) {
    QString src = activePath + "/" + modName;
    QString dst = availablePath + "/" + modName;

    if (!QDir(src).exists()) {
        QMessageBox::warning(this, "Mod Not Found",
                             QString("Mod '%1' not found in Active folder.").arg(modName));
        return;
    }

    restoreMod(modName);

    QSet<QString> modsToUnblock = greyedOutMods;
    for (const QString& m : modsToUnblock)
        greyedOutMods.remove(m);

    if (QDir(dst).exists())
        QDir(dst).removeRecursively();

    QFile::rename(src, dst);

    modTracker->setModActive(modName, false);
    modTracker->saveToFile();

    scanAvailableMods();
    scanActiveMods();
    filterMods();
    sortMods();
    updateModDisplay();
}

void ModManagerDialog::scanActiveMods() {
    if (!modTracker) {
        return;
    }

    QDir dir(activePath);
    QStringList modFolders = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    for (const QString& modName : modFolders) {
        ModInfo modInfo = modTracker->getMod(modName);
        if (modInfo.name.isEmpty()) {
            modInfo.name = modName;
            modInfo.gameSerial = gameSerial;
            modInfo.installedAt = QDateTime::currentDateTime();
            modInfo.isActive = true;
            modInfo.author = "Unknown";
            modInfo.description = "";
            modInfo.version = "1.0";
        }

        bool found = false;
        for (const ModInfo& existingMod : allMods) {
            if (existingMod.name == modInfo.name) {
                found = true;
                break;
            }
        }

        if (!found) {
            allMods.append(modInfo);
        }
    }

    filteredMods = allMods;
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

    if (!Core::FileSys::MntPoints::manual_mods_path.empty()) {
        searchOrder << QString::fromStdString(Core::FileSys::MntPoints::manual_mods_path.string());
    } else {
        QStringList modSuffixes = {"-mods", "-MODS", "-Mods"};
        for (const QString& suffix : modSuffixes) {
            searchOrder << (gamePath + suffix);
        }
    }

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
          gameSerial == "CUSA00207" || gameSerial == "CUSA00208" || gameSerial == "CUSA003027" ||
          gameSerial == "CUSA01322" || gameSerial == "CUSA01363" || gameSerial == "CUSA03014" ||
          gameSerial == "CUSA03023"))
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
}

void ModManagerDialog::copyModToOverlayAndTrack(const QString& modName) {
    QString src = availablePath + "/" + modName;
    if (!QDir(src).exists()) {
        return;
    }

    ModInfo modInfo;
    modInfo.name = modName;
    modInfo.gameSerial = gameSerial;
    modInfo.installedAt = QDateTime::currentDateTime();
    modInfo.isActive = false;

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

        if (QFile::exists(destPath)) {
            backupFile = modBackupRoot + "/" + rel;
            QDir().mkpath(QFileInfo(backupFile).absolutePath());

            if (QFile::exists(backupFile)) {
                QString stamped =
                    backupFile + "." + QString::number(QDateTime::currentSecsSinceEpoch());
                QFile::rename(backupFile, stamped);
            }

            if (QFile::copy(destPath, backupFile)) {
            } else {
            }
        }

        originalFile = resolveOriginalFile(rel);

        if (QFile::exists(destPath)) {
            QString owner = findModThatContainsFile(rel);
            if (!owner.isEmpty() && owner != modName) {
                greyedOutMods.insert(owner);
                updateModDisplay();
            }
        }

        if (QFile::exists(destPath))
            QFile::remove(destPath);

        if (!QFile::copy(it.filePath(), destPath)) {
        }

        modTracker->addFileToMod(modName, rel, originalFile, backupFile);
    }

    modTracker->saveToFile();

    if (!QDir(overlayRoot).exists()) {
        QDir().mkpath(overlayRoot);
    }
}

void ModManagerDialog::uninstallMod(const QString& modName) {
    QString path = availablePath + "/" + modName;

    if (QDir(path).exists()) {
        QDir(path).removeRecursively();
    }

    modTracker->removeMod(modName);
    modTracker->saveToFile();

    scanAvailableMods();
}

void ModManagerDialog::activateSelected() {
    activateAll();
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
        updateModDisplay();
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
    updateModDisplay();
}

void ModManagerDialog::removeAvailableMod() {
    QMessageBox::information(this, "Remove Mod",
                             "Remove functionality needs to be adapted for the new UI system.");
}

void ModManagerDialog::deactivateSelected() {
    deactivateAll();
}

void ModManagerDialog::cleanupEmptyDirectories(const QString& path) {
    QDir dir(path);
    if (!dir.exists()) {
        return;
    }

    QStringList allDirs;
    QDirIterator it(path, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        allDirs.append(it.filePath());
    }

    std::sort(allDirs.begin(), allDirs.end(), std::greater<QString>());

    int removedDirs = 0;
    for (const QString& dirPath : allDirs) {
        QDir currentDir(dirPath);

        if (currentDir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty()) {
            if (currentDir.rmdir(dirPath)) {
                removedDirs++;
            } else {
            }
        }
    }

    if (dir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty()) {
        if (dir.rmdir(path)) {
            removedDirs++;
        }
    }

    if (removedDirs > 0) {
    }
}

QString ModManagerDialog::resolveOriginalFolderForRestore(const QString& rel) const {
    QStringList searchOrder;

    if (!Core::FileSys::MntPoints::manual_mods_path.empty())
        searchOrder << QString::fromStdString(Core::FileSys::MntPoints::manual_mods_path.string());
    else {
        QStringList modSuffixes = {"-mods", "-MODS", "-Mods"};
        for (const QString& suffix : modSuffixes) {
            searchOrder << (gamePath + suffix);
        }
    }
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

    ModInfo modInfo = modTracker->getMod(modName);
    if (modInfo.name.isEmpty()) {

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

        QStringList otherModsNeedingFile;
        QList<ModInfo> allActiveMods = modTracker->getActiveMods();

        for (const ModInfo& otherMod : allActiveMods) {
            if (otherMod.name != modName && otherMod.files.contains(relativePath)) {
                otherModsNeedingFile.append(otherMod.name);
            }
        }

        if (otherModsNeedingFile.isEmpty()) {
            ModFileInfo fileInfo = modInfo.fileDetails.value(relativePath);
            if (!fileInfo.backupPath.isEmpty() && QFile::exists(fileInfo.backupPath)) {
                QDir().mkpath(QFileInfo(overlayFile).absolutePath());
                if (QFile::exists(overlayFile)) {
                    QFile::remove(overlayFile);
                }
                if (QFile::copy(fileInfo.backupPath, overlayFile)) {
                    filesRestored++;
                } else {
                    errors.append(QString("Failed to restore %1 from backup").arg(relativePath));
                }
            } else {
                if (QFile::exists(overlayFile)) {
                    if (QFile::remove(overlayFile)) {
                        filesRemoved++;
                    } else {
                        errors.append(QString("Failed to remove %1").arg(relativePath));
                    }
                }
            }
        } else {
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

void ModManagerDialog::showModDetails(const QString& modName) {
    QDialog detailsDialog(this);
    detailsDialog.setWindowTitle(QString("%1 - Details").arg(modName));
    detailsDialog.setModal(true);
    detailsDialog.setMinimumSize(800, 600);
    detailsDialog.resize(1000, 700);

    auto* mainLayout = new QVBoxLayout(&detailsDialog);
    mainLayout->setContentsMargins(15, 15, 15, 15);
    mainLayout->setSpacing(10);

    auto* titleLabel = new QLabel(QString("<h2>%1</h2>").arg(modName));
    titleLabel->setAlignment(Qt::AlignCenter);
    titleLabel->setStyleSheet("QLabel { color: #ffffff; font-weight: bold; margin-bottom: 10px; }");

    auto* contentSplitter = new QSplitter(Qt::Horizontal);

    auto* leftWidget = new QWidget();
    auto* leftLayout = new QVBoxLayout(leftWidget);

    auto* fileLabel = new QLabel("File Structure:");
    fileLabel->setStyleSheet("QLabel { color: #ffffff; font-weight: bold; font-size: 14px; }");

    auto* fileTree = new QTreeWidget();
    fileTree->setHeaderLabels({"Path", "Size", "Type"});
    fileTree->setStyleSheet(R"(
        QTreeWidget {
            background-color: #2d2d2d;
            color: #ffffff;
            border: 1px solid #404040;
            border-radius: 4px;
            selection-background-color: #0078d4;
        }
        QTreeWidget::item {
            height: 24px;
        }
        QTreeWidget::item:selected {
            background-color: #0078d4;
        }
    )");

    QString modPath = availablePath + "/" + modName;
    if (!QDir(modPath).exists()) {
        modPath = activePath + "/" + modName;
    }

    QString basePath = modPath;
    QDir modDir(modPath);
    if (modDir.exists("dvdroot_ps4")) {
        basePath = modPath + "/dvdroot_ps4";
        modDir = QDir(basePath);
    }

    QTreeWidgetItem* rootItem = new QTreeWidgetItem(fileTree, {modName, "", "Folder"});
    rootItem->setIcon(0, QIcon(":/icons/folder.png"));
    populateFileTree(rootItem, basePath, "");
    rootItem->setExpanded(true);

    fileTree->resizeColumnToContents(0);
    fileTree->resizeColumnToContents(1);
    fileTree->resizeColumnToContents(2);

    leftLayout->addWidget(fileLabel);
    leftLayout->addWidget(fileTree);

    auto* rightWidget = new QWidget();
    auto* rightLayout = new QVBoxLayout(rightWidget);

    auto* infoLabel = new QLabel("Mod Information:");
    infoLabel->setStyleSheet("QLabel { color: #ffffff; font-weight: bold; font-size: 14px; }");

    auto* modInfoText = new QTextEdit();
    modInfoText->setReadOnly(true);
    modInfoText->setMaximumHeight(200);
    modInfoText->setStyleSheet(R"(
        QTextEdit {
            background-color: #2d2d2d;
            color: #ffffff;
            border: 1px solid #404040;
            border-radius: 4px;
            padding: 8px;
        }
    )");

    ModInfo modInfo = modTracker->getMod(modName);
    QString infoText = QString("Name: %1\n").arg(modName);
    infoText += QString("Author: %1\n").arg(modInfo.author.isEmpty() ? "Unknown" : modInfo.author);
    infoText += QString("Version: %1\n").arg(modInfo.version);
    infoText += QString("Size: %1\n").arg(getModSizeString(modName));
    infoText += QString("Status: %1\n").arg(modInfo.isActive ? "Active" : "Inactive");
    infoText += QString("Installed: %1").arg(modInfo.installedAt.toString("yyyy-MM-dd hh:mm"));

    modInfoText->setPlainText(infoText);

    rightLayout->addWidget(infoLabel);
    rightLayout->addWidget(modInfoText);
    rightLayout->addStretch();

    contentSplitter->addWidget(leftWidget);
    contentSplitter->addWidget(rightWidget);
    contentSplitter->setStretchFactor(0, 2);
    contentSplitter->setStretchFactor(1, 1);

    auto* closeBtn = new QPushButton("Close");
    closeBtn->setStyleSheet(R"(
        QPushButton {
            background-color: #f44336;
            color: #ffffff;
            border: none;
            border-radius: 4px;
            padding: 8px 16px;
            font-weight: bold;
        }
        QPushButton:hover {
            background-color: #d32f2f;
        }
    )");

    connect(closeBtn, &QPushButton::clicked, &detailsDialog, &QDialog::accept);

    mainLayout->addWidget(titleLabel);
    mainLayout->addWidget(contentSplitter);
    mainLayout->addWidget(closeBtn);

    detailsDialog.setStyleSheet(R"(
        QDialog {
            background-color: #1e1e1e;
            color: #ffffff;
        }
    )");

    detailsDialog.exec();
}

void ModManagerDialog::populateFileTree(QTreeWidgetItem* parentItem, const QString& basePath,
                                        const QString& relativePath) {
    QString fullPath = basePath + "/" + relativePath;
    QDir dir(fullPath);

    QStringList entries = dir.entryList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
    std::sort(entries.begin(), entries.end());

    for (const QString& entry : entries) {
        QString entryPath = fullPath + "/" + entry;
        QString entryRelativePath = relativePath.isEmpty() ? entry : relativePath + "/" + entry;
        QFileInfo fileInfo(entryPath);

        QTreeWidgetItem* item = new QTreeWidgetItem(parentItem, {entry, "", ""});

        if (fileInfo.isDir()) {
            item->setIcon(0, QIcon(":/icons/folder.png"));
            item->setText(2, "Folder");
            populateFileTree(item, basePath, entryRelativePath);
        } else {
            item->setIcon(0, QIcon(":/icons/file.png"));
            item->setText(1, QString("%1 B").arg(fileInfo.size()));

            QString extension = fileInfo.suffix().toLower();
            if (extension == "png" || extension == "jpg" || extension == "jpeg" ||
                extension == "bmp") {
                item->setText(2, "Image");
            } else if (extension == "txt" || extension == "json" || extension == "xml") {
                item->setText(2, "Text");
            } else if (extension == "bin" || extension == "dat") {
                item->setText(2, "Binary");
            } else {
                item->setText(2, "File");
            }
        }
    }
}

void ModManagerDialog::setupNexusTab(QTabWidget* tabs) {
    auto* nexusTab = new QWidget();
    auto* outerLayout = new QVBoxLayout(nexusTab);
    outerLayout->setContentsMargins(10, 8, 10, 8);
    outerLayout->setSpacing(8);

    auto* keyBar = new QHBoxLayout();
    auto* keyIcon = new QLabel("");
    m_nexusKeyStatusLabel = new QLabel("Nexus API key: not set");
    m_nexusKeyStatusLabel->setStyleSheet("QLabel { color: #ff9800; }");
    m_nexusSetKeyBtn = new QPushButton("Set API Key");
    m_nexusSetKeyBtn->setFixedWidth(110);

    keyBar->addWidget(keyIcon);
    keyBar->addWidget(m_nexusKeyStatusLabel, 1);
    keyBar->addWidget(m_nexusSetKeyBtn);
    outerLayout->addLayout(keyBar);

    auto* searchBar = new QHBoxLayout();
    m_nexusSearchBox = new QLineEdit();
    m_nexusSearchBox->setPlaceholderText(
        "Search Nexus Mods  -  type a mod name just like on the Nexus website...");
    m_nexusSearchBox->setClearButtonEnabled(true);
    m_nexusSearchBtn = new QPushButton("Search");
    m_nexusSearchBtn->setFixedWidth(90);
    m_nexusSearchBtn->setStyleSheet(R"(
        QPushButton { background-color: #e8591a; color: #fff;
                      border: none; border-radius: 4px; padding: 7px 14px;
                      font-weight: bold; }
        QPushButton:hover { background-color: #c44d16; }
        QPushButton:disabled { background-color: #555; }
    )");

    searchBar->addWidget(m_nexusSearchBox, 1);
    searchBar->addWidget(m_nexusSearchBtn);
    outerLayout->addLayout(searchBar);

    auto* pageBar = new QHBoxLayout();
    m_nexusStatusLabel = new QLabel("Enter a search term above.");
    m_nexusStatusLabel->setStyleSheet("QLabel { color: #aaa; font-size: 11px; }");
    m_nexusPrevBtn = new QPushButton("< Prev");
    m_nexusNextBtn = new QPushButton("Next >");
    m_nexusPageLabel = new QLabel("");
    m_nexusPrevBtn->setVisible(false);
    m_nexusNextBtn->setVisible(false);
    m_nexusPrevBtn->setFixedWidth(70);
    m_nexusNextBtn->setFixedWidth(70);

    pageBar->addWidget(m_nexusStatusLabel, 1);
    pageBar->addWidget(m_nexusPrevBtn);
    pageBar->addWidget(m_nexusPageLabel);
    pageBar->addWidget(m_nexusNextBtn);
    outerLayout->addLayout(pageBar);

    m_nexusScrollArea = new QScrollArea();
    m_nexusScrollArea->setWidgetResizable(true);
    m_nexusScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_nexusResultsContainer = new QWidget();
    m_nexusResultsLayout = new QVBoxLayout(m_nexusResultsContainer);
    m_nexusResultsLayout->setSpacing(8);
    m_nexusResultsLayout->setAlignment(Qt::AlignTop);
    m_nexusScrollArea->setWidget(m_nexusResultsContainer);
    outerLayout->addWidget(m_nexusScrollArea, 1);

    connect(m_nexusSetKeyBtn, &QPushButton::clicked, this,
            &ModManagerDialog::onNexusSetApiKeyClicked);
    connect(m_nexusSearchBtn, &QPushButton::clicked, this, &ModManagerDialog::onNexusSearchClicked);
    connect(m_nexusSearchBox, &QLineEdit::returnPressed, this,
            &ModManagerDialog::onNexusSearchReturnPressed);
    connect(m_nexusPrevBtn, &QPushButton::clicked, this, &ModManagerDialog::onNexusPagePrev);
    connect(m_nexusNextBtn, &QPushButton::clicked, this, &ModManagerDialog::onNexusPageNext);

    tabs->addTab(nexusTab, " Browse Nexus");
}

void ModManagerDialog::updateNexusKeyStatus() {
    if (!m_nexusApi)
        return;
    if (m_nexusApi->hasApiKey()) {
        m_nexusKeyStatusLabel->setText("Nexus API key: [OK]  (connected)");
        m_nexusKeyStatusLabel->setStyleSheet("QLabel { color: #4CAF50; }");
        m_nexusSearchBtn->setEnabled(true);
    } else {
        m_nexusKeyStatusLabel->setText(
            "Nexus API key: not set  -  paste your key to enable search");
        m_nexusKeyStatusLabel->setStyleSheet("QLabel { color: #ff9800; }");
        m_nexusSearchBtn->setEnabled(false);
    }
}

void ModManagerDialog::onNexusSetApiKeyClicked() {
    bool ok = false;
    QString key = QInputDialog::getText(this, "Nexus Mods API Key",
                                        "Paste your Nexus Mods personal API key\n"
                                        "(get it at nexusmods.com -> your profile -> API Keys):",
                                        QLineEdit::Normal,
                                        m_nexusApi ? m_nexusApi->apiKey() : QString(), &ok);

    if (ok && !key.trimmed().isEmpty() && m_nexusApi) {
        m_nexusApi->setApiKey(key.trimmed());
        m_nexusStatusLabel->setText("Validating key...");
        m_nexusApi->validateApiKey();
    }
}

void ModManagerDialog::onNexusApiKeyValidated(bool valid, const QString& username) {
    if (valid) {
        m_nexusKeyStatusLabel->setText(
            QString("Nexus API key: [OK]  logged in as %1").arg(username));
        m_nexusKeyStatusLabel->setStyleSheet("QLabel { color: #4CAF50; }");
        if (m_nexusSearchBtn)
            m_nexusSearchBtn->setEnabled(true);
        m_nexusStatusLabel->setText("Key validated. Type a mod name and hit Search.");
    } else {
        m_nexusKeyStatusLabel->setText("Nexus API key: [X]  invalid  -  please check your key");
        m_nexusKeyStatusLabel->setStyleSheet("QLabel { color: #f44336; }");
        if (m_nexusSearchBtn)
            m_nexusSearchBtn->setEnabled(false);
    }
}

void ModManagerDialog::onNexusSearchClicked() {
    if (!m_nexusApi)
        return;
    QString query = m_nexusSearchBox->text().trimmed();
    if (query.isEmpty()) {
        m_nexusStatusLabel->setText("Please enter a search term.");
        return;
    }
    m_nexusCurrentQuery = query;
    m_nexusOffset = 0;
    m_nexusCurrentSlug = NexusModsApi::gameSlugForSerial(gameSerial);

    if (m_nexusCurrentSlug.isEmpty()) {
        m_nexusStatusLabel->setText("This game serial is not mapped to a Nexus slug. "
                                    "Enter the Nexus game slug manually in the field below.");
        m_nexusSearchBtn->setEnabled(true);
        return;
    }

    m_nexusStatusLabel->setText("Searching Nexus (latest/trending/updated)...");
    m_nexusSearchBtn->setEnabled(false);

    m_nexusApi->searchAllModes(m_nexusCurrentSlug, query);
}

void ModManagerDialog::onNexusSearchReturnPressed() {
    onNexusSearchClicked();
}

void ModManagerDialog::onNexusPagePrev() {}

void ModManagerDialog::onNexusPageNext() {}

void ModManagerDialog::onNexusSearchResultsReady(const QList<NexusModResult>& results,
                                                 int totalCount) {
    m_nexusTotalCount = totalCount;
    if (m_nexusSearchBtn)
        m_nexusSearchBtn->setEnabled(true);

    if (results.isEmpty()) {
        m_nexusStatusLabel->setText("No mods found for that query.");
        m_nexusPrevBtn->setEnabled(false);
        m_nexusNextBtn->setEnabled(false);
        while (QLayoutItem* item = m_nexusResultsLayout->takeAt(0)) {
            if (item->widget())
                item->widget()->deleteLater();
            delete item;
        }
        return;
    }

    populateNexusResults(results, totalCount);
}

void ModManagerDialog::populateNexusResults(const QList<NexusModResult>& results, int totalCount) {
    while (QLayoutItem* item = m_nexusResultsLayout->takeAt(0)) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }

    for (const NexusModResult& mod : results) {
        QWidget* card = createNexusResultCard(mod);
        m_nexusResultsLayout->addWidget(card);
    }
    m_nexusResultsLayout->addStretch();

    int page = m_nexusOffset / m_nexusLimit + 1;
    int totalPages = (totalCount + m_nexusLimit - 1) / m_nexusLimit;
    m_nexusStatusLabel->setText(
        QString("Found %1 mod(s)  -  page %2 / %3").arg(totalCount).arg(page).arg(totalPages));
    m_nexusPageLabel->setText(QString("%1 / %2").arg(page).arg(totalPages));

    m_nexusPrevBtn->setVisible(false);
    m_nexusNextBtn->setVisible(false);

    if (m_nexusScrollArea)
        m_nexusScrollArea->verticalScrollBar()->setValue(0);
}

QWidget* ModManagerDialog::createNexusResultCard(const NexusModResult& mod) {
    auto* card = new QWidget();
    card->setMinimumHeight(100);
    card->setMaximumHeight(130);
    card->setStyleSheet(R"(
        QWidget {
            background-color: #2a2a2a;
            border: 1px solid #3a3a3a;
            border-radius: 6px;
        }
        QWidget:hover { border-color: #e8591a; }
    )");

    auto* row = new QHBoxLayout(card);
    row->setContentsMargins(10, 8, 10, 8);
    row->setSpacing(12);

    auto* imgLabel = new QLabel();
    imgLabel->setFixedSize(120, 80);
    imgLabel->setAlignment(Qt::AlignCenter);
    imgLabel->setStyleSheet("QLabel { background-color: #1a1a1a; border-radius: 4px; }");
    imgLabel->setText("Loading...");
    if (!mod.pictureUrl.isEmpty())
        loadNexusThumbnail(mod.pictureUrl, imgLabel);

    auto* textCol = new QVBoxLayout();
    textCol->setSpacing(2);

    auto* nameLabel = new QLabel(mod.name.isEmpty() ? "(unnamed mod)" : mod.name);
    nameLabel->setStyleSheet("QLabel { font-weight: bold; font-size: 14px; color: #fff; }");
    nameLabel->setWordWrap(true);

    auto* authorLabel = new QLabel(QString("by %1   |  %2")
                                       .arg(mod.author.isEmpty() ? "Unknown" : mod.author)
                                       .arg(mod.gameSlug.isEmpty() ? gameSerial : mod.gameSlug));
    authorLabel->setStyleSheet("QLabel { color: #aaa; font-size: 11px; }");

    auto* summaryLabel = new QLabel(mod.summary);
    summaryLabel->setStyleSheet("QLabel { color: #ccc; font-size: 11px; }");
    summaryLabel->setWordWrap(true);
    summaryLabel->setMaximumHeight(36);

    auto* statsLabel = new QLabel(QString("[DL] %1   |  [OK] %2   |  v%3")
                                      .arg(mod.downloadCount)
                                      .arg(mod.endorsementCount)
                                      .arg(mod.version.isEmpty() ? "?" : mod.version));
    statsLabel->setStyleSheet("QLabel { color: #888; font-size: 10px; }");

    textCol->addWidget(nameLabel);
    textCol->addWidget(authorLabel);
    textCol->addWidget(summaryLabel);
    textCol->addWidget(statsLabel);
    textCol->addStretch();

    auto* btnCol = new QVBoxLayout();
    btnCol->setSpacing(4);

    auto* nexusBtn = new QPushButton("Open on Nexus");
    nexusBtn->setFixedWidth(130);
    nexusBtn->setStyleSheet(R"(
        QPushButton { background-color: #333; color: #ddd; border: 1px solid #555;
                      border-radius: 4px; padding: 5px; font-size: 11px; }
        QPushButton:hover { background-color: #444; }
    )");

    auto* installBtn = new QPushButton("[DL] Get Files");
    installBtn->setFixedWidth(130);
    installBtn->setStyleSheet(R"(
        QPushButton { background-color: #e8591a; color: #fff; border: none;
                      border-radius: 4px; padding: 5px; font-weight: bold; font-size: 11px; }
        QPushButton:hover { background-color: #c44d16; }
    )");

    btnCol->addStretch();
    btnCol->addWidget(nexusBtn);
    btnCol->addWidget(installBtn);
    btnCol->addStretch();

    row->addWidget(imgLabel);
    row->addLayout(textCol, 1);
    row->addLayout(btnCol);

    int modId = mod.modId;
    QString gameSlug = mod.gameSlug.isEmpty() ? m_nexusCurrentSlug : mod.gameSlug;
    QString nexusUrl = QString("https://www.nexusmods.com/%1/mods/%2").arg(gameSlug).arg(modId);

    connect(nexusBtn, &QPushButton::clicked, this,
            [nexusUrl]() { QDesktopServices::openUrl(QUrl(nexusUrl)); });
    connect(installBtn, &QPushButton::clicked, this,
            [this, modId, gameSlug]() { onNexusResultItemClicked(modId, gameSlug); });

    return card;
}

void ModManagerDialog::loadNexusThumbnail(const QString& url, QLabel* label) {
    if (!m_imgNam || url.isEmpty())
        return;
    QNetworkReply* reply = m_imgNam->get(QNetworkRequest(QUrl(url)));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, label]() { onNexusThumbnailDownloaded(reply, label); });
}

void ModManagerDialog::onNexusThumbnailDownloaded(QNetworkReply* reply, QLabel* imageLabel) {
    reply->deleteLater();
    if (!imageLabel)
        return;
    if (reply->error() != QNetworkReply::NoError) {
        imageLabel->setText("No image");
        return;
    }
    QPixmap px;
    px.loadFromData(reply->readAll());
    if (!px.isNull()) {
        imageLabel->setPixmap(
            px.scaled(imageLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        imageLabel->setText(QString());
    } else {
        imageLabel->setText("No image");
    }
}

void ModManagerDialog::onNexusResultItemClicked(int modId, const QString& gameSlug) {
    if (!m_nexusApi)
        return;
    m_pendingFileSelectSlug[modId] = gameSlug;
    m_nexusStatusLabel->setText("Fetching file list...");
    m_nexusApi->fetchModFiles(gameSlug, modId);
}

void ModManagerDialog::onNexusModFilesReady(int modId, const QList<NexusFileInfo>& files) {
    m_nexusStatusLabel->setText(QString("%1 mod(s) found  -  ready.").arg(m_nexusTotalCount));

    if (files.isEmpty()) {
        QMessageBox::information(this, "No Files",
                                 "This mod has no downloadable files listed on Nexus.");
        return;
    }

    QString gameSlug = m_pendingFileSelectSlug.value(modId, m_nexusCurrentSlug);

    QDialog dlg(this);
    dlg.setWindowTitle("Choose File to Download");
    dlg.setMinimumSize(600, 350);
    dlg.setStyleSheet("QDialog { background-color: #1e1e1e; color: #fff; }");

    auto* layout = new QVBoxLayout(&dlg);
    auto* label = new QLabel(QString("Select a file for mod #%1:").arg(modId));
    label->setStyleSheet("QLabel { color: #fff; font-weight: bold; }");
    layout->addWidget(label);

    auto* listW = new QListWidget();
    listW->setStyleSheet(R"(
        QListWidget { background-color: #2d2d2d; color: #fff;
                      border: 1px solid #404040; }
        QListWidget::item { padding: 6px; border-bottom: 1px solid #404040; }
        QListWidget::item:selected { background-color: #e8591a; }
    )");

    for (const NexusFileInfo& fi : files) {
        QString sizeStr = fi.sizeKb > 1024 ? QString("%1 MB").arg(fi.sizeKb / 1024.0, 0, 'f', 1)
                                           : QString("%1 KB").arg(fi.sizeKb);
        QString label2 = QString("[%1]  %2  v%3  (%4)")
                             .arg(fi.category)
                             .arg(fi.name)
                             .arg(fi.version.isEmpty() ? "?" : fi.version)
                             .arg(sizeStr);
        auto* item = new QListWidgetItem(label2);
        item->setData(Qt::UserRole, fi.fileId);
        item->setData(Qt::UserRole + 1, fi.name);
        listW->addItem(item);
    }

    layout->addWidget(listW, 1);

    auto* btnRow = new QHBoxLayout();
    auto* cancelBtn = new QPushButton("Cancel");
    auto* dlBtn = new QPushButton("Get Download Link");
    dlBtn->setStyleSheet(R"(
        QPushButton { background-color: #e8591a; color: #fff; border: none;
                      border-radius: 4px; padding: 7px 14px; font-weight: bold; }
        QPushButton:hover { background-color: #c44d16; }
    )");

    btnRow->addStretch();
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(dlBtn);
    layout->addLayout(btnRow);

    connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    connect(dlBtn, &QPushButton::clicked, &dlg, [&dlg, listW]() {
        if (listW->currentItem())
            dlg.accept();
    });
    connect(listW, &QListWidget::itemDoubleClicked, &dlg,
            [&dlg](QListWidgetItem*) { dlg.accept(); });

    if (dlg.exec() != QDialog::Accepted)
        return;

    QListWidgetItem* sel = listW->currentItem();
    if (!sel)
        return;

    int fileId = sel->data(Qt::UserRole).toInt();
    QString fileName = sel->data(Qt::UserRole + 1).toString();

    m_nexusStatusLabel->setText("Fetching download link...");
    m_pendingFileSelectSlug[fileId] = gameSlug;
    downloadAndInstallNexusMod(gameSlug, modId, fileId, fileName);
}

void ModManagerDialog::downloadAndInstallNexusMod(const QString& gameSlug, int modId, int fileId,
                                                  const QString& /*fileName*/) {
    if (!m_nexusApi)
        return;

    connect(m_nexusApi, &NexusModsApi::downloadLinkReady, this,
            &ModManagerDialog::onNexusDownloadLinkReady, Qt::UniqueConnection);

    m_nexusApi->fetchDownloadLink(gameSlug, modId, fileId);
}

void ModManagerDialog::onNexusDownloadLinkReady(int /*modId*/, int /*fileId*/, const QString& url) {
    m_nexusStatusLabel->setText("Download link ready  -  opening in browser.");
    if (url.isEmpty()) {
        QMessageBox::warning(this, "No Link",
                             "Nexus did not return a download link.\n"
                             "This sometimes happens for free-tier accounts.\n"
                             "The mod page will open in your browser instead.");
        return;
    }
    QDesktopServices::openUrl(QUrl(url));

    QMessageBox::information(this, "Download Started",
                             "The download has been opened in your browser.\n\n"
                             "Once the file is saved, use the  \"Install Mod\" button\n"
                             "on the  \"Installed Mods\" tab to add it to your game.");
}

void ModManagerDialog::onNexusError(const QString& message) {
    if (m_nexusStatusLabel)
        m_nexusStatusLabel->setText(QString("Error: %1").arg(message));
    if (m_nexusSearchBtn)
        m_nexusSearchBtn->setEnabled(true);
    QMessageBox::warning(this, "Nexus Mods Error", message);
}