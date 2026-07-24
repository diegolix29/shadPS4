// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QToolTip>
#include "common/config.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/string_util.h"
#include "common/zar_fs.h"
#include "game_grid_frame.h"
#include "game_list_frame.h"
#include "game_list_utils.h"
#include "main_window.h"
#include "qt_gui/compatibility_info.h"

GameListFrame::GameListFrame(std::shared_ptr<GameInfoClass> game_info_get,
                             std::shared_ptr<CompatibilityInfoClass> compat_info_get,
                             std::shared_ptr<IpcClient> ipc_client, QWidget* parent)
    : QTableWidget(parent), m_game_info(game_info_get), m_compat_info(compat_info_get),
      m_ipc_client(ipc_client) {
    icon_size = Config::getIconSize();
    last_favorite = "";

    // Protection: Ensure this widget has a specific name so we can target it strictly if needed
    this->setObjectName("GameListFrame");

    this->setAttribute(Qt::WA_TranslucentBackground);
    this->setShowGrid(false);
    this->setEditTriggers(QAbstractItemView::NoEditTriggers);
    this->setSelectionBehavior(QAbstractItemView::SelectRows);
    this->setSelectionMode(QAbstractItemView::SingleSelection);
    this->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    this->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    this->verticalScrollBar()->installEventFilter(this);
    this->verticalScrollBar()->setSingleStep(20);
    this->horizontalScrollBar()->setSingleStep(20);
    this->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    this->verticalHeader()->setVisible(false);
    this->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this->horizontalHeader(), &QHeaderView::customContextMenuRequested, this,
            [this](const QPoint& pos) {
                QMenu menu(this);
                for (int i = 0; i < this->columnCount(); ++i) {
                    QString headerText = this->horizontalHeaderItem(i)->text();
                    QAction* action = menu.addAction(headerText);
                    action->setCheckable(true);
                    action->setChecked(!this->isColumnHidden(i));

                    connect(action, &QAction::toggled, this, [this, i](bool visible) {
                        this->setColumnHidden(i, !visible);
                        QList<int> hiddenCols;
                        for (int j = 0; j < this->columnCount(); ++j) {
                            if (this->isColumnHidden(j)) {
                                hiddenCols.append(j);
                            }
                        }
                        m_compat_info->SaveHiddenColumns(hiddenCols);
                    });
                }
                menu.exec(this->horizontalHeader()->mapToGlobal(pos));
            });

    this->horizontalHeader()->setHighlightSections(false);
    this->horizontalHeader()->setSortIndicatorShown(true);
    this->horizontalHeader()->setStretchLastSection(true);
    this->setContextMenuPolicy(Qt::CustomContextMenu);
    this->setColumnCount(10);
    this->setColumnWidth(1, 300); // Name
    this->setColumnWidth(2, 140); // Compatibility
    this->setColumnWidth(3, 120); // Serial
    this->setColumnWidth(4, 90);  // Region
    this->setColumnWidth(5, 90);  // Firmware
    this->setColumnWidth(6, 90);  // Size
    this->setColumnWidth(7, 90);  // Version
    this->setColumnWidth(8, 120); // Play Time

    QStringList headers;
    headers << tr("Icon") << tr("Name") << tr("Compatibility") << tr("Serial") << tr("Region")
            << tr("Firmware") << tr("Size") << tr("Version") << tr("Play Time") << tr("Path");
    this->setHorizontalHeaderLabels(headers);

    // Set initial theme color for headers
    QColor headerTextColor = Qt::white;
    if (auto* mw = qobject_cast<MainWindow*>(this->window())) {
        headerTextColor = mw->getThemes()->textColor();
    }
    this->horizontalHeader()->setStyleSheet(
        QString("QHeaderView::section { background-color: rgba(30, 30, 30, 200); color: %1; "
                "padding: 5px; "
                "border: none; border-right: 1px solid rgba(90, 170, 255, 80); border-bottom: 1px "
                "solid "
                "rgba(90, 170, 255, 80); } QHeaderView::section:hover { background-color: rgba(90, "
                "170, "
                "255, 100); }")
            .arg(headerTextColor.name()));

    this->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    this->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
    this->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
    this->horizontalHeader()->setSectionResizeMode(10, QHeaderView::Stretch);

    m_zar_container = new QWidget(this);
    QVBoxLayout* zarLayout = new QVBoxLayout(m_zar_container);
    zarLayout->setContentsMargins(0, 0, 0, 0);
    zarLayout->setSpacing(5);

    m_zar_title = new QLabel(tr("ZAR Files"), this);
    m_zar_title->setAlignment(Qt::AlignCenter);
    zarLayout->addWidget(m_zar_title);

    m_zar_list = new QListWidget(this);
    m_zar_list->setObjectName("ZarList");
    m_zar_list->setIconSize(QSize(64, 64));
    zarLayout->addWidget(m_zar_list);

    PopulateGameList();
    QList<int> hiddenCols = m_compat_info->LoadHiddenColumns();
    for (int col : hiddenCols) {
        if (col >= 0 && col < this->columnCount())
            this->setColumnHidden(col, true);
    }
    ApplyCustomBackground();
    RefreshZarBackgroundImage();

    connect(this, &QTableWidget::currentCellChanged, this, &GameListFrame::onCurrentCellChanged);

    this->horizontalHeader()->setSortIndicatorShown(true);
    this->horizontalHeader()->setSectionsClickable(true);
    QObject::connect(
        this->horizontalHeader(), &QHeaderView::sectionClicked, this, [this](int columnIndex) {
            if (ListSortedAsc) {
                SortNameDescending(columnIndex);
                this->horizontalHeader()->setSortIndicator(columnIndex, Qt::DescendingOrder);
                ListSortedAsc = false;
                sortColumn = columnIndex;
            } else {
                SortNameAscending(columnIndex);
                this->horizontalHeader()->setSortIndicator(columnIndex, Qt::AscendingOrder);
                ListSortedAsc = true;
                sortColumn = columnIndex;
            }
            this->clearContents();
            PopulateGameList(false);
        });

    connect(this, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        int changedFavorite = m_gui_context_menus.RequestGameMenu(
            pos, m_game_info->m_games, m_compat_info, m_ipc_client, this, true,
            [mw = QPointer<MainWindow>(qobject_cast<MainWindow*>(this->window()))](
                const QStringList& args) {
                if (mw)
                    mw->StartGameWithArgs(args);
            });
        PopulateGameList(false);
    });

    connect(this, &QTableWidget::cellClicked, this, [=, this](int row, int column) {
        if (column == 2 && m_game_info->m_games[row].compatibility.issue_number != "") {
            auto url_issues =
                "https://github.com/shadps4-compatibility/shadps4-game-compatibility/issues/";
            QDesktopServices::openUrl(
                QUrl(url_issues + m_game_info->m_games[row].compatibility.issue_number));
        } else if (column == 10) {
            last_favorite = m_game_info->m_games[row].serial;
            QString serialStr = QString::fromStdString(last_favorite);

            QList<QString> list = m_compat_info->LoadFavorites();
            bool isFavorite = list.contains(serialStr);

            if (isFavorite) {
                list.removeOne(serialStr);
            } else {
                list.append(serialStr);
            }

            m_compat_info->SaveFavorites(list);
            PopulateGameList(false);
            QList<int> hiddenCols = m_compat_info->LoadHiddenColumns();
            for (int col : hiddenCols) {
                if (col >= 0 && col < this->columnCount())
                    this->setColumnHidden(col, true);
            }
        }
    });
}

void GameListFrame::onCurrentCellChanged(int currentRow, int currentColumn, int previousRow,
                                         int previousColumn) {
    QTableWidgetItem* item = this->item(currentRow, currentColumn);
    if (!item) {
        return;
    }
    m_current_item = item; // Store current item
    SetListBackgroundImage(item);
    PlayBackgroundMusic(item);
}

void GameListFrame::ApplyHiddenColumns() {
    QList<int> hiddenCols = m_compat_info->LoadHiddenColumns();
    for (int col = 0; col < this->columnCount(); ++col) {
        bool hiddenByConfig = false;
        if (col == 2)
            hiddenByConfig = !Config::getCompatibilityEnabled();
        if (col == 6)
            hiddenByConfig = !Config::GetLoadGameSizeEnabled();

        this->setColumnHidden(col, hiddenByConfig || hiddenCols.contains(col));
    }
}

void GameListFrame::PlayBackgroundMusic(QTableWidgetItem* item) {
    if (!item || !Config::getPlayBGM()) {
        BackgroundMusicPlayer::getInstance().stopMusic();
        return;
    }
    QString snd0path;
    Common::FS::PathToQString(snd0path, m_game_info->m_games[item->row()].snd0_path);
    BackgroundMusicPlayer::getInstance().playMusic(snd0path);
}

void GameListFrame::PopulateGameList(bool isInitialPopulation) {
    this->m_current_item = nullptr;
    ApplyHiddenColumns();

    this->setRowCount(m_game_info->m_games.size());
    ResizeIcons(icon_size);

    ApplyLastSorting(isInitialPopulation);

    PopulateZarList();

    for (int i = 0; i < m_game_info->m_games.size(); i++) {
        SetTableItem(i, 1, QString::fromStdString(m_game_info->m_games[i].name));
        SetTableItem(i, 3, QString::fromStdString(m_game_info->m_games[i].serial));
        SetRegionFlag(i, 4, QString::fromStdString(m_game_info->m_games[i].region));
        SetTableItem(i, 5, QString::fromStdString(m_game_info->m_games[i].fw));
        SetTableItem(i, 6, QString::fromStdString(m_game_info->m_games[i].size));
        SetTableItem(i, 7, QString::fromStdString(m_game_info->m_games[i].version));

        if (m_game_info->m_games[i].serial == last_favorite && !isInitialPopulation) {
            this->setCurrentCell(i, 0);
        }

        m_game_info->m_games[i].compatibility =
            m_compat_info->GetCompatibilityInfo(m_game_info->m_games[i].serial);
        SetCompatibilityItem(i, 2, m_game_info->m_games[i].compatibility);

        QString playTime = GetPlayTime(m_game_info->m_games[i].serial);
        if (playTime.isEmpty()) {
            m_game_info->m_games[i].play_time = "0:00:00";
            SetTableItem(i, 8, tr("Never Played"));
        } else {
            QStringList timeParts = playTime.split(':');
            int hours = timeParts[0].toInt();
            int minutes = timeParts[1].toInt();
            int seconds = timeParts[2].toInt();

            QString formattedPlayTime;
            if (hours > 0) {
                formattedPlayTime += QString("%1").arg(hours) + tr("h");
            }
            if (minutes > 0) {
                formattedPlayTime += QString("%1").arg(minutes) + tr("m");
            }

            formattedPlayTime = formattedPlayTime.trimmed();
            m_game_info->m_games[i].play_time = playTime.toStdString();
            if (formattedPlayTime.isEmpty()) {
                SetTableItem(i, 8, QString("%1").arg(seconds) + tr("s"));
            } else {
                SetTableItem(i, 8, formattedPlayTime);
            }
        }

        QString path;
        Common::FS::PathToQString(path, m_game_info->m_games[i].path);
        SetTableItem(i, 9, path);
    }
}

void GameListFrame::PopulateZarList() {
    m_zar_list->clear();

    for (const auto& zar_game : m_game_info->m_zar_games) {
        QString name = QString::fromStdString(zar_game.name);
        QString path;
        Common::FS::PathToQString(path, zar_game.path);

        if (name.isEmpty()) {
            name = QFileInfo(path).fileName();
        }
        QListWidgetItem* item = new QListWidgetItem(name);
        item->setData(Qt::UserRole, QString::fromStdString(zar_game.path.string()));

        if (Common::FS::Zar::IsZarArchive(zar_game.path)) {
            std::filesystem::path icon_path = zar_game.path / "sce_sys" / "icon0.png";
            if (Common::FS::Zar::Exists(icon_path)) {
                auto file_handle = Common::FS::Zar::OpenFile(icon_path);
                if (file_handle) {
                    u64 file_size = file_handle->GetSize();
                    std::vector<u8> icon_data(file_size);
                    file_handle->Read(icon_data.data(), file_size);
                    QPixmap pixmap;
                    pixmap.loadFromData(icon_data.data(), file_size);
                    if (!pixmap.isNull()) {
                        item->setIcon(QIcon(
                            pixmap.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
                    }
                }
            }
        } else if (!zar_game.icon.isNull()) {
            item->setIcon(QIcon(QPixmap::fromImage(zar_game.icon)));
        } else {
            QString iconPath;
            Common::FS::PathToQString(iconPath, zar_game.icon_path);
            if (QFile::exists(iconPath)) {
                QPixmap pixmap(iconPath);
                if (!pixmap.isNull()) {
                    item->setIcon(QIcon(
                        pixmap.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
                }
            }
        }

        item->setToolTip(path);
        m_zar_list->addItem(item);
    }
}

void GameListFrame::ApplyCustomBackground() {
    QString customPath = QString::fromStdString(Config::getCustomBackgroundImage());
    if (customPath.isEmpty())
        return;

    QImage original_image(customPath);
    if (!original_image.isNull()) {
        const int opacity = Config::getIconBgOpacity();
        backgroundImage = m_game_list_utils.ChangeImageOpacity(
            original_image, original_image.rect(), opacity / 100.0f);
        m_last_opacity = opacity;
        m_current_game_path = customPath.toStdString();
        RefreshListBackgroundImage();
    }
}

void GameListFrame::SetListBackgroundImage(QTableWidgetItem* item) {
    // Disabled - using single full-screen background instead
    backgroundImage = QImage();
    m_last_opacity = -1;
    m_current_game_path.clear();
    RefreshListBackgroundImage();
    return;
}

bool GameListFrame::CompareWithFavorite(const GameInfo& a, const GameInfo& b, int columnIndex,
                                        bool ascending) {
    QString serialStr_a = QString::fromStdString(a.serial);
    QString serialStr_b = QString::fromStdString(b.serial);

    QList<QString> list = m_compat_info->LoadFavorites();
    bool isFavorite_a = list.contains(serialStr_a);
    bool isFavorite_b = list.contains(serialStr_b);

    if (isFavorite_a != isFavorite_b) {
        return isFavorite_a;
    } else if (ascending) {
        return CompareStringsAscending(a, b, columnIndex);
    } else {
        return CompareStringsDescending(a, b, columnIndex);
    }
}

void GameListFrame::RefreshListBackgroundImage() {
    QPalette palette;

    // Get theme-based background color
    QColor bgColor = QColor(30, 30, 30); // Default fallback
    if (auto* mw = qobject_cast<MainWindow*>(this->window())) {
        bgColor = mw->getThemes()->backgroundColor();
    }
    int opacity = Config::getBgOpacity();
    int alpha = opacity * 255 / 100;

    if (!backgroundImage.isNull() && Config::getShowBackgroundImage()) {
        QSize widgetSize = size();
        QPixmap scaledPixmap =
            QPixmap::fromImage(backgroundImage)
                .scaled(widgetSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        int x = (widgetSize.width() - scaledPixmap.width()) / 2;
        int y = (widgetSize.height() - scaledPixmap.height()) / 2;
        QPixmap finalPixmap(widgetSize);
        finalPixmap.fill(QColor(bgColor.red(), bgColor.green(), bgColor.blue(), alpha));
        QPainter painter(&finalPixmap);
        painter.drawPixmap(x, y, scaledPixmap);
        palette.setBrush(QPalette::Base, QBrush(finalPixmap));
    } else {
        // Apply theme-based background color with opacity
        QPixmap solidPixmap(size());
        solidPixmap.fill(QColor(bgColor.red(), bgColor.green(), bgColor.blue(), alpha));
        palette.setBrush(QPalette::Base, QBrush(solidPixmap));
    }
    QColor transparentColor = QColor(135, 206, 235, 40);
    palette.setColor(QPalette::Highlight, transparentColor);
    this->setPalette(palette);
}

void GameListFrame::resizeEvent(QResizeEvent* event) {
    QTableWidget::resizeEvent(event);
    RefreshListBackgroundImage();
}

void GameListFrame::RefreshZarBackgroundImage() {
    // Get theme-based background color from main window
    QColor zarBgColor = QColor(30, 30, 30); // Default fallback
    QColor textColor = Qt::white;           // Default text color
    if (auto* mw = qobject_cast<MainWindow*>(this->window())) {
        zarBgColor = mw->getThemes()->backgroundColor();
        textColor = mw->getThemes()->textColor();
    }
    int opacity = Config::getBgOpacity();
    int alpha = opacity * 200 / 100;

    m_zar_title->setStyleSheet(
        QString("color: %1; font-weight: bold; background-color: rgba(%2, %3, "
                "%4, %5); padding: 5px; border-radius: 5px; text-align: center;")
            .arg(textColor.name())
            .arg(zarBgColor.red())
            .arg(zarBgColor.green())
            .arg(zarBgColor.blue())
            .arg(alpha));

    m_zar_list->setStyleSheet(
        QString(
            "QListWidget { background-color: rgba(%1, %2, %3, %4); border: 1px solid rgba(90, 170, "
            "255, 150); border-radius: 5px; } QListWidget::item { padding: 5px; color: %5; } "
            "QListWidget::item:hover { background-color: rgba(90, 170, 255, 100); } "
            "QListWidget::item:selected { background-color: rgba(90, 170, 255, 150); }")
            .arg(zarBgColor.red())
            .arg(zarBgColor.green())
            .arg(zarBgColor.blue())
            .arg(alpha)
            .arg(textColor.name()));
}

void GameListFrame::RefreshHeaderColors() {
    QColor headerTextColor = Qt::white;
    QColor headerBgColor = QColor(30, 30, 30);
    if (auto* mw = qobject_cast<MainWindow*>(this->window())) {
        headerTextColor = mw->getThemes()->textColor();
        headerBgColor = mw->getThemes()->backgroundColor();
    }
    this->horizontalHeader()->setStyleSheet(
        QString("QHeaderView::section { background-color: rgba(%1, %2, %3, 200); color: %4; "
                "padding: 5px; "
                "border: none; border-right: 1px solid rgba(90, 170, 255, 80); border-bottom: 1px "
                "solid "
                "rgba(90, 170, 255, 80); } QHeaderView::section:hover { background-color: rgba(90, "
                "170, "
                "255, 100); }")
            .arg(headerBgColor.red())
            .arg(headerBgColor.green())
            .arg(headerBgColor.blue())
            .arg(headerTextColor.name()));
}

void GameListFrame::SortNameAscending(int columnIndex) {
    std::sort(m_game_info->m_games.begin(), m_game_info->m_games.end(),
              [this, columnIndex](const GameInfo& a, const GameInfo& b) {
                  return this->CompareWithFavorite(a, b, columnIndex, true);
              });
}

void GameListFrame::SortNameDescending(int columnIndex) {
    std::sort(m_game_info->m_games.begin(), m_game_info->m_games.end(),
              [this, columnIndex](const GameInfo& a, const GameInfo& b) {
                  return this->CompareWithFavorite(a, b, columnIndex, false);
              });
}

void GameListFrame::ApplyLastSorting(bool isInitialPopulation) {
    if (isInitialPopulation) {
        SortNameAscending(1);
        ResizeIcons(icon_size);
    } else if (ListSortedAsc) {
        SortNameAscending(sortColumn);
        ResizeIcons(icon_size);
    } else {
        SortNameDescending(sortColumn);
        ResizeIcons(icon_size);
    }
}

void GameListFrame::ResizeIcons(int iconSize) {
    for (int index = 0; auto& game : m_game_info->m_games) {
        // Create widget container like grid frame
        QWidget* widget = new QWidget(this);
        widget->setAttribute(Qt::WA_TranslucentBackground);
        widget->setStyleSheet("background: transparent;");
        QVBoxLayout* layout = new QVBoxLayout(widget);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        // Create image container
        QWidget* image_container = new QWidget(widget);
        image_container->setFixedSize(iconSize, iconSize);
        QVBoxLayout* image_layout = new QVBoxLayout(image_container);
        image_layout->setContentsMargins(0, 0, 0, 0);

        // Create label for icon
        QLabel* image_label = new QLabel(image_container);
        QImage scaledPixmap = game.icon.scaled(QSize(iconSize, iconSize), Qt::KeepAspectRatio,
                                               Qt::SmoothTransformation);
        image_label->setPixmap(QPixmap::fromImage(scaledPixmap));
        image_label->setAlignment(Qt::AlignCenter);
        image_label->setFixedSize(iconSize, iconSize);
        image_label->setAttribute(Qt::WA_TranslucentBackground);
        image_label->setStyleSheet("background: transparent;");

        image_layout->addWidget(image_label);
        layout->addWidget(image_container);

        // Add config icon like grid frame
        std::string serialStr = game.serial;
        bool hasGameConfig = std::filesystem::exists(
            Common::FS::GetUserPath(Common::FS::PathType::CustomConfigs) / (serialStr + ".toml"));

        QLabel* config_label = new QLabel(image_container);
        QPixmap configPixmap = QPixmap(":images/game_settings.png")
                                   .scaled(iconSize / 3.8, iconSize / 3.8, Qt::KeepAspectRatio,
                                           Qt::SmoothTransformation);

        QColor themeColor = QColor(90, 170, 255);
        if (auto* mw = qobject_cast<MainWindow*>(this->window())) {
            themeColor = mw->getThemes()->textColor();
        }
        QPainter painter(&configPixmap);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(configPixmap.rect(), themeColor);
        painter.end();

        config_label->setPixmap(configPixmap);
        config_label->setFixedSize(configPixmap.width() + 4, configPixmap.height() + 4);
        config_label->setAlignment(Qt::AlignCenter);
        config_label->move(2, 2);
        config_label->raise();
        config_label->setVisible(hasGameConfig);
        config_label->setObjectName("gameConfigIcon");

        QList<QString> list = m_compat_info->LoadFavorites();
        bool isFavorite = list.contains(QString::fromStdString(serialStr));

        if (isFavorite) {
            image_container->setStyleSheet(
                QString("QWidget { border: 2px solid rgba(%1, %2, %3, 200); border-radius: 5px; }")
                    .arg(themeColor.red())
                    .arg(themeColor.green())
                    .arg(themeColor.blue()));
        }
        widget->setLayout(layout);
        this->setCellWidget(index, 0, widget);

        this->verticalHeader()->resizeSection(index, iconSize);
        this->horizontalHeader()->resizeSection(0, iconSize);

        index++;
    }
    this->horizontalHeader()->setSectionResizeMode(8, QHeaderView::ResizeToContents);
}

void GameListFrame::SetCompatibilityItem(int row, int column, CompatibilityEntry entry) {
    QTableWidgetItem* item = new QTableWidgetItem();
    QWidget* widget = new QWidget(this);
    widget->setAttribute(Qt::WA_TranslucentBackground);

    widget->setStyleSheet(
        "background: transparent; QToolTip {background-color: black; color: white;}");

    QGridLayout* layout = new QGridLayout(widget);

    QColor color;
    QString status_explanation;

    switch (entry.status) {
    case CompatibilityStatus::Unknown:
        color = QStringLiteral("#000000");
        status_explanation = tr("Compatibility is untested");
        break;
    case CompatibilityStatus::Nothing:
        color = QStringLiteral("#212121");
        status_explanation = tr("Game does not initialize properly / crashes the emulator");
        break;
    case CompatibilityStatus::Boots:
        color = QStringLiteral("#828282");
        status_explanation = tr("Game boots, but only displays a blank screen");
        break;
    case CompatibilityStatus::Menus:
        color = QStringLiteral("#FF0000");
        status_explanation = tr("Game displays an image but does not go past the menu");
        break;
    case CompatibilityStatus::Ingame:
        color = QStringLiteral("#F2D624");
        status_explanation = tr("Game has game-breaking glitches or unplayable performance");
        break;
    case CompatibilityStatus::Playable:
        color = QStringLiteral("#47D35C");
        status_explanation =
            tr("Game can be completed with playable performance and no major glitches");
        break;
    }

    QString tooltip_string;

    if (entry.status == CompatibilityStatus::Unknown) {
        tooltip_string = status_explanation;
    } else {
        tooltip_string =
            "<p> <i>" + tr("Click to see details on github") + "</i>" + "<br>" +
            tr("Last updated") +
            QString(": %1 (%2)").arg(entry.last_tested.toString("yyyy-MM-dd"), entry.version) +
            "<br>" + status_explanation + "</p>";
    }

    QPixmap circle_pixmap(16, 16);
    circle_pixmap.fill(Qt::transparent);
    QPainter painter(&circle_pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(color);
    painter.setBrush(color);
    painter.drawEllipse({circle_pixmap.width() / 2.0, circle_pixmap.height() / 2.0}, 6.0, 6.0);

    QLabel* dotLabel = new QLabel("", widget);
    dotLabel->setPixmap(circle_pixmap);

    QLabel* label = new QLabel(m_compat_info->GetCompatStatusString(entry.status), widget);
    this->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);

    label->setStyleSheet(
        "color: white; font-size: 16px; font-weight: bold; background: transparent;");

    // Create shadow effect
    QGraphicsDropShadowEffect* shadowEffect = new QGraphicsDropShadowEffect();
    shadowEffect->setBlurRadius(5);               // Set the blur radius of the shadow
    shadowEffect->setColor(QColor(0, 0, 0, 160)); // Set the color and opacity of the shadow
    shadowEffect->setOffset(2, 2);                // Set the offset of the shadow

    label->setGraphicsEffect(shadowEffect); // Apply shadow effect to the QLabel

    layout->addWidget(dotLabel, 0, 0, -1, 1);
    layout->addWidget(label, 0, 1, 1, 1);
    layout->setAlignment(Qt::AlignLeft);
    widget->setLayout(layout);
    widget->setToolTip(tooltip_string);
    this->setItem(row, column, item);
    this->setCellWidget(row, column, widget);

    return;
}

void GameListFrame::SetTableItem(int row, int column, QString itemStr) {
    QTableWidgetItem* item = new QTableWidgetItem();
    QWidget* widget = new QWidget(this);
    widget->setAttribute(Qt::WA_TranslucentBackground);
    widget->setStyleSheet("background: transparent;");

    QVBoxLayout* layout = new QVBoxLayout(widget);
    QLabel* label = new QLabel(itemStr, widget);

    QColor effectiveColor = m_textColor.isValid() ? m_textColor : Qt::white;

    label->setStyleSheet(
        QString("color: %1; font-size: 16px; font-weight: bold; background: transparent;")
            .arg(effectiveColor.name()));

    QGraphicsDropShadowEffect* shadowEffect = new QGraphicsDropShadowEffect();
    shadowEffect->setBlurRadius(5);
    shadowEffect->setColor(QColor(0, 0, 0, 160));
    shadowEffect->setOffset(2, 2);
    label->setGraphicsEffect(shadowEffect);

    layout->addWidget(label);
    if (column != 8 && column != 1)
        layout->setAlignment(Qt::AlignCenter);
    widget->setLayout(layout);

    this->setItem(row, column, item);
    this->setCellWidget(row, column, widget);
}

void GameListFrame::SetRegionFlag(int row, int column, QString itemStr) {
    QTableWidgetItem* item = new QTableWidgetItem();
    QImage scaledPixmap;
    if (itemStr == "Japan") {
        scaledPixmap = QImage(":images/flag_jp.png");
    } else if (itemStr == "Europe") {
        scaledPixmap = QImage(":images/flag_eu.png");
    } else if (itemStr == "USA") {
        scaledPixmap = QImage(":images/flag_us.png");
    } else if (itemStr == "Asia") {
        scaledPixmap = QImage(":images/flag_china.png");
    } else if (itemStr == "World") {
        scaledPixmap = QImage(":images/flag_world.png");
    } else {
        scaledPixmap = QImage(":images/flag_unk.png");
    }

    QWidget* widget = new QWidget(this);
    widget->setAttribute(Qt::WA_TranslucentBackground);
    widget->setStyleSheet("background: transparent;");

    QVBoxLayout* layout = new QVBoxLayout(widget);
    QLabel* label = new QLabel(widget);
    label->setPixmap(QPixmap::fromImage(scaledPixmap));
    label->setStyleSheet("background: transparent;");

    layout->setAlignment(Qt::AlignCenter);
    layout->addWidget(label);
    widget->setLayout(layout);
    this->setItem(row, column, item);
    this->setCellWidget(row, column, widget);
}

void GameListFrame::SetThemeColors(const QColor& textColor) {
    m_textColor = textColor;

    QPalette pal = this->palette();
    pal.setColor(QPalette::HighlightedText, Qt::black);
    this->setPalette(pal);

    PopulateGameList(false);
}

QString GameListFrame::GetPlayTime(const std::string& serial) {
    QString playTime;
    const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
    QString filePath = QString::fromStdString((user_dir / "play_time.txt").string());

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return playTime;
    }

    while (!file.atEnd()) {
        QByteArray line = file.readLine();
        QString lineStr = QString::fromUtf8(line).trimmed();

        QStringList parts = lineStr.split(' ');
        if (parts.size() >= 2) {
            QString fileSerial = parts[0];
            QString time = parts[1];

            if (fileSerial == QString::fromStdString(serial)) {
                playTime = time;
                break;
            }
        }
    }

    file.close();
    return playTime;
}

void GameListFrame::keyPressEvent(QKeyEvent* event) {
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && currentRow() >= 0) {
        emit cellDoubleClicked(currentRow(), currentColumn());
        event->accept();
        return;
    }
    QTableWidget::keyPressEvent(event);
}

QTableWidgetItem* GameListFrame::GetCurrentItem() {
    return m_current_item;
}