// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMetaObject>
#include <QObject>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressDialog>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QString>
#include <QStyle>
#include <QStyleFactory>
#include <QTextStream>
#include <QWidget>
#include <signal.h>
#include <zarchive/zarchivewriter.h>
#include "SDL3/SDL_events.h"
#include "emulator.h"
#include "mod_manager_dialog.h"

#include "about_dialog.h"
#include "cheats_patches.h"
#ifdef ENABLE_UPDATER
#include "check_update.h"
#endif
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/scm_rev.h"
#include "common/string_util.h"
#include "control_settings.h"
#include "core/emulator_state.h"
#include "core/ipc/ipc.h"

#include "core/libraries/audio/audioout.h"
#include "imgui/big_picture.h"
#include "version_dialog.h"

#include "core/debug_state.h"
#include "game_directory_dialog.h"
#include "hotkeys.h"
#include "input/controller.h"
#include "input/input_handler.h"
#include "kbm_gui.h"
#include "main_window.h"
#include "settings_dialog.h"

#ifdef ENABLE_DISCORD_RPC
#include "common/discord_rpc_handler.h"
#endif
namespace Storage {
extern std::atomic_bool shader_cache_paused_game;
extern std::atomic_bool shader_cache_error_shown;
} // namespace Storage
MainWindow* g_MainWindow = nullptr;
QProcess* MainWindow::emulatorProcess = nullptr;

QFlowLayout::QFlowLayout(QWidget* parent, int margin, int hSpacing, int vSpacing)
    : QLayout(parent), m_hSpace(hSpacing), m_vSpace(vSpacing) {
    setContentsMargins(margin, margin, margin, margin);
}

QFlowLayout::QFlowLayout(int margin, int hSpacing, int vSpacing)
    : m_hSpace(hSpacing), m_vSpace(vSpacing) {
    setContentsMargins(margin, margin, margin, margin);
}

QFlowLayout::~QFlowLayout() {
    QLayoutItem* item;
    while ((item = takeAt(0)))
        delete item;
}

void QFlowLayout::addItem(QLayoutItem* item) {
    m_itemList.append(item);
}

int QFlowLayout::horizontalSpacing() const {
    if (m_hSpace >= 0) {
        return m_hSpace;
    } else {
        return smartSpacing(QStyle::PM_LayoutHorizontalSpacing);
    }
}

int QFlowLayout::verticalSpacing() const {
    if (m_vSpace >= 0) {
        return m_vSpace;
    } else {
        return smartSpacing(QStyle::PM_LayoutVerticalSpacing);
    }
}

int QFlowLayout::count() const {
    return m_itemList.size();
}

QLayoutItem* QFlowLayout::itemAt(int index) const {
    return m_itemList.value(index);
}

QLayoutItem* QFlowLayout::takeAt(int index) {
    if (index >= 0 && index < m_itemList.size()) {
        return m_itemList.takeAt(index);
    }
    return nullptr;
}

Qt::Orientations QFlowLayout::expandingDirections() const {
    return {};
}

bool QFlowLayout::hasHeightForWidth() const {
    return true;
}

int QFlowLayout::heightForWidth(int width) const {
    return doLayout(QRect(0, 0, width, 0), true);
}

void QFlowLayout::setGeometry(const QRect& rect) {
    QLayout::setGeometry(rect);
    doLayout(rect, false);
}

QSize QFlowLayout::sizeHint() const {
    QWidget* p = qobject_cast<QWidget*>(parent());
    int w = p ? p->width() : 400;
    int h = hasHeightForWidth() ? heightForWidth(w) : minimumSize().height();
    return QSize(w, h);
}

QSize QFlowLayout::minimumSize() const {
    QSize size(0, 0);

    int maxItemHeight = 0;
    int totalWidth = 0;

    for (QLayoutItem* item : m_itemList) {
        QSize itemMin = item->minimumSize();

        totalWidth += itemMin.width();
        maxItemHeight = qMax(maxItemHeight, itemMin.height());
    }

    const QMargins margins = contentsMargins();
    size.setHeight(maxItemHeight + margins.top() + margins.bottom());
    size.setWidth(totalWidth + margins.left() + margins.right());

    return size;
}

int QFlowLayout::doLayout(const QRect& rect, bool testOnly) const {
    int left, top, right, bottom;
    getContentsMargins(&left, &top, &right, &bottom);
    QRect effectiveRect = rect.adjusted(left, top, -right, -bottom);

    int x = effectiveRect.x();
    int y = effectiveRect.y();
    int lineHeight = 0;

    int spaceX = horizontalSpacing();
    int spaceY = verticalSpacing();

    if (spaceX < 0)
        spaceX = 6;
    if (spaceY < 0)
        spaceY = 6;

    const int maxX = effectiveRect.right();

    for (QLayoutItem* item : m_itemList) {
        QWidget* wid = item->widget();
        QSize itemSize = item->sizeHint();

        int nextX = x + (lineHeight == 0 ? 0 : spaceX);

        if (nextX + itemSize.width() > maxX && lineHeight > 0) {
            x = effectiveRect.x();
            y += lineHeight + spaceY;
            nextX = x;
            lineHeight = 0;
        }

        if (!testOnly) {
            item->setGeometry(QRect(nextX, y, itemSize.width(), itemSize.height()));
        }

        x = nextX + itemSize.width();
        lineHeight = qMax(lineHeight, itemSize.height());
    }

    int totalHeight = (y + lineHeight + bottom);

    return totalHeight;
}

int QFlowLayout::smartSpacing(QStyle::PixelMetric pm) const {
    QObject* parent = this->parent();
    if (!parent) {
        return 6;
    } else if (parent->isWidgetType()) {
        QWidget* pw = static_cast<QWidget*>(parent);
        return pw->style()->pixelMetric(pm, nullptr, pw);
    } else {
        int s = static_cast<QLayout*>(parent)->spacing();
        return s >= 0 ? s : 6;
    }
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), ui(new Ui::MainWindow) {
    ui->setupUi(this);
    installEventFilter(this);
    setAttribute(Qt::WA_DeleteOnClose);
    g_MainWindow = this;
    m_ipc_client = std::make_shared<IpcClient>(this);
    IpcClient::SetInstance(m_ipc_client);
    initializeGamepad();
}

MainWindow::~MainWindow() {
    SaveWindowState();
    const auto config_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
    Config::saveMainWindow(config_dir / "config.toml");
    if (g_MainWindow == this)
        g_MainWindow = nullptr;
}

std::string MainWindow::GetRunningGameSerial() const {
    return runningGameSerial;
}

bool MainWindow::Init() {
    auto start = std::chrono::steady_clock::now();
    LoadTranslation();
    QApplication::setStyle(QStyleFactory::create(QStyleFactory::keys().first()));

    ui->toggleLabelsAct->setChecked(Config::getShowLabelsUnderIcons());
    ui->toggleColorFilterAct->setChecked(Config::getEnableColorFilter());

    AddUiWidgets();
    CreateActions();
    CreateRecentGameActions();
    ConfigureGuiFromSettings();
    CreateDockWindows(true);
    CreateConnects();
    SetLastUsedTheme();
    SetLastIconSizeBullet();
    toggleColorFilter();

    setMinimumSize(590, 455);
    std::string window_title = "";
    std::string remote_url(Common::g_scm_remote_url);
    std::string remote_host = Common::GetRemoteNameFromLink();
    if (Common::g_is_release) {
        if (remote_host == "shadps4-emu" || remote_url.length() == 0) {
            window_title = fmt::format("shadPS4 v{}", Common::g_version);
        } else {
            window_title = fmt::format("shadPS4 {}/v{}", remote_host, Common::g_version);
        }
    } else {
        if (remote_host == "shadps4-emu" || remote_url.length() == 0) {
            window_title = fmt::format("shadPS4 v{} {} {}", Common::g_version, Common::g_scm_branch,
                                       Common::g_scm_desc);
        } else {
            window_title = fmt::format("shadPS4 v{} {}/{} {}", Common::g_version, remote_host,
                                       Common::g_scm_branch, Common::g_scm_desc);
        }
    }
    setWindowTitle(QString::fromStdString(window_title));
    this->show();
    // load game list
    LoadGameLists();
    m_bigPicture =
        std::make_unique<BigPictureWidget>(m_game_info, m_compat_info, m_ipc_client, nullptr, this);
    m_hubMenu = std::make_unique<HubMenuWidget>(m_game_info, m_compat_info, m_ipc_client,
                                                &m_window_themes, this);
    if (Config::getEnableColorFilter()) {
        ui->playButton->installEventFilter(this);
        ui->pauseButton->installEventFilter(this);
        ui->stopButton->installEventFilter(this);
        ui->refreshButton->installEventFilter(this);
        ui->restartButton->installEventFilter(this);
        ui->settingsButton->installEventFilter(this);
        ui->fullscreenButton->installEventFilter(this);
        ui->controllerButton->installEventFilter(this);
        ui->keyboardButton->installEventFilter(this);
        ui->updaterButton->installEventFilter(this);
        ui->configureHotkeysButton->installEventFilter(this);
        ui->versionButton->installEventFilter(this);
        ui->bigPictureButton->installEventFilter(this);
        ui->hubMenuButton->installEventFilter(this);
        ui->cinemaButton->installEventFilter(this);
        ui->modManagerButton->installEventFilter(this);
        ui->launcherBox->installEventFilter(this);
        ui->zarBootButton->installEventFilter(this);
        ui->zarConvertButton->installEventFilter(this);
    }

    if (!Config::getEnableColorFilter()) {
        ui->playButton->removeEventFilter(this);
        ui->pauseButton->removeEventFilter(this);
        ui->stopButton->removeEventFilter(this);
        ui->refreshButton->removeEventFilter(this);
        ui->restartButton->removeEventFilter(this);
        ui->settingsButton->removeEventFilter(this);
        ui->fullscreenButton->removeEventFilter(this);
        ui->controllerButton->removeEventFilter(this);
        ui->keyboardButton->removeEventFilter(this);
        ui->updaterButton->removeEventFilter(this);
        ui->configureHotkeysButton->removeEventFilter(this);
        ui->versionButton->removeEventFilter(this);
        ui->bigPictureButton->removeEventFilter(this);
        ui->hubMenuButton->removeEventFilter(this);
        ui->cinemaButton->removeEventFilter(this);
        ui->modManagerButton->removeEventFilter(this);
        ui->launcherBox->removeEventFilter(this);
        ui->zarBootButton->removeEventFilter(this);
        ui->zarConvertButton->removeEventFilter(this);
    }

    QString savedStyle = QString::fromStdString(Config::getGuiStyle());
    if (!savedStyle.isEmpty()) {
        int idx = ui->styleSelector->findText(savedStyle, Qt::MatchFixedString);
        if (idx >= 0) {
            ui->styleSelector->setCurrentIndex(idx);
        } else {
            for (int i = 0; i < ui->styleSelector->count(); ++i) {
                if (ui->styleSelector->itemData(i).toString() == savedStyle) {
                    ui->styleSelector->setCurrentIndex(i);
                    break;
                }
            }
        }
    }
#ifdef ENABLE_UPDATER
    CheckUpdateMain(true);
#endif

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    statusBar.reset(new QStatusBar);
    this->setStatusBar(statusBar.data());
    // Update status bar
    int numGames = m_game_info->m_games.size();
    QString statusMessage = tr("Games: ") + QString::number(numGames) + " (" +
                            QString::number(duration.count()) + "ms)";
    statusBar->showMessage(statusMessage);

#ifdef ENABLE_DISCORD_RPC
    if (Config::getEnableDiscordRPC()) {
        auto* rpc = Common::Singleton<DiscordRPCHandler::RPC>::Instance();
        rpc->init();
        rpc->setStatusIdling();
    }
#endif
    connect(m_game_cinematic_frame.get(), &GameCinematicFrame::launchGameRequested, this,
            [this](int index) { StartGameByIndex(index, {}); });
    connect(
        m_bigPicture.get(), &BigPictureWidget::openModsManagerRequested, this, [this](int index) {
            if (index < 0 || index >= m_game_info->m_games.size()) {
                QMessageBox::warning(this, tr("Mod Manager"), tr("Invalid game index."));
                return;
            }

            const GameInfo& game = m_game_info->m_games[index];

            QString gamePathQString;
            Common::FS::PathToQString(gamePathQString, game.path);

            auto dlg =
                new ModManagerDialog(gamePathQString, QString::fromStdString(game.serial), this);
            dlg->exec();
            restoreBigPictureFocus();
        });

    connect(m_bigPicture.get(), &BigPictureWidget::openHotkeysRequested, this,
            &MainWindow::openHotkeysWindow);

    connect(m_bigPicture.get(), &BigPictureWidget::launchRequestedFromGameMenu, this,
            [this](int index) { StartGameByIndex(index, {}); });

    connect(m_bigPicture.get(), &BigPictureWidget::globalConfigRequested, this,
            &MainWindow::openSettingsWindow);

    connect(m_hubMenu.get(), &HubMenuWidget::launchRequestedFromHub, this,
            &MainWindow::onHubMenuLaunchGameRequested);

    connect(m_hubMenu.get(), &HubMenuWidget::globalConfigRequested, this,
            &MainWindow::openSettingsWindow);

    connect(m_hubMenu.get(), &HubMenuWidget::gameConfigRequested, this,
            &MainWindow::onHubMenuGameConfigRequested);

    connect(m_hubMenu.get(), &HubMenuWidget::openModsManagerRequested, this,
            &MainWindow::onHubMenuOpenModsManagerRequested);

    connect(m_hubMenu.get(), &HubMenuWidget::openCheatsRequested, this,
            &MainWindow::onOpenCheatsRequested);

    connect(m_hubMenu.get(), &HubMenuWidget::openFolderRequested, this,
            &MainWindow::onHubMenuOpenFolderRequested);

    connect(m_hubMenu.get(), &HubMenuWidget::deleteShadersRequested, this,
            &MainWindow::onDeleteShaderCacheRequested);

    connect(m_bigPicture.get(), &BigPictureWidget::gameConfigRequested, this, [this](int index) {
        auto& g = m_game_info->m_games[index];
        auto dlg = new GameSpecificDialog(m_compat_info, m_ipc_client, this, g.serial, false, "");
        dlg->exec();
        restoreBigPictureFocus();
    });
    if (Config::GamesMenuUI()) {
        m_bigPicture->toggle();
    }
    if (Config::HubMenuUI()) {
        m_hubMenu->toggle();
    }
    ApplyLastUsedStyle();

    return true;
}

void MainWindow::onHubMenuLaunchGameRequested(int index) {
    StartGameByIndex(index, {});
}

void MainWindow::onHubMenuGameConfigRequested(int index) {
    if (index < 0 || index >= m_game_info->m_games.size())
        return;

    auto& g = m_game_info->m_games[index];
    auto dlg = new GameSpecificDialog(m_compat_info, m_ipc_client, this, g.serial, false, "");
    dlg->exec();
    restoreHubFocus();
}

void MainWindow::onHubMenuOpenModsManagerRequested(int index) {
    if (index < 0 || index >= m_game_info->m_games.size()) {
        QMessageBox::warning(this, tr("Mod Manager"), tr("Invalid game index."));
        return;
    }

    const GameInfo& game = m_game_info->m_games[index];

    QString gamePathQString;
    Common::FS::PathToQString(gamePathQString, game.path);

    auto dlg = new ModManagerDialog(gamePathQString, QString::fromStdString(game.serial), this);
    dlg->exec();
    restoreHubFocus();
}

void MainWindow::onOpenCheatsRequested(int index) {
    if (index < 0 || index >= m_game_info->m_games.size())
        return;

    const GameInfo& game = m_game_info->m_games[index];

    QString gameName = QString::fromStdString(game.name);
    QString gameSerial = QString::fromStdString(game.serial);
    QString gameVersion = QString::fromStdString(game.version);
    QString gameSize = QString::fromStdString(game.size);

    QString iconPath;
    Common::FS::PathToQString(iconPath, game.icon_path);
    QPixmap gameImage(iconPath);

    auto* cheatsPatches = new CheatsPatches(gameName, gameSerial, m_ipc_client, gameVersion,
                                            gameSize, gameImage, this);

    cheatsPatches->setWindowFlags(Qt::Window | Qt::Dialog);
    cheatsPatches->setAttribute(Qt::WA_DeleteOnClose);

    connect(cheatsPatches, &QObject::destroyed, this, [this]() { restoreHubFocus(); });

    cheatsPatches->show();
}

void MainWindow::onHubMenuOpenFolderRequested(int index) {
    if (index < 0 || index >= m_game_info->m_games.size())
        return;

    QString path;
    Common::FS::PathToQString(path, m_game_info->m_games[index].path);
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void MainWindow::onDeleteShaderCacheRequested(int index) {
    if (index < 0 || index >= m_game_info->m_games.size())
        return;

    const auto& game = m_game_info->m_games[index];

    QString shaderCachePath;
    Common::FS::PathToQString(shaderCachePath,
                              Common::FS::GetUserPath(Common::FS::PathType::CacheDir) /
                                  (game.serial + ".zip"));

    if (!QFile::exists(shaderCachePath)) {
        QMessageBox::information(this, tr("Shader Cache"),
                                 tr("This game does not have any saved Shader Cache to delete."));
        return;
    }

    QString gameName = QString::fromStdString(game.name);
    if (QMessageBox::Yes ==
        QMessageBox::question(
            this, tr("Delete Shader Cache"),
            tr("Are you sure you want to delete %1's Shader Cache?").arg(gameName),
            QMessageBox::Yes | QMessageBox::No)) {
        QFile::remove(shaderCachePath);
    }
}

void MainWindow::onShaderCacheError(const QString& gameSerial) {
    QString shaderCachePath;

    Common::FS::PathToQString(shaderCachePath,
                              Common::FS::GetUserPath(Common::FS::PathType::CacheDir) /
                                  std::filesystem::path(gameSerial.toStdString() + ".zip"));

    QString gameName = gameSerial;

    for (const auto& game : m_game_info->m_games) {
        if (game.serial == gameSerial.toStdString()) {
            gameName = QString::fromStdString(game.name);
            break;
        }
    }
    QMessageBox::StandardButton reply =
        QMessageBox::question(this, tr("Shader Cache Invalid"),
                              tr("The shader cache for %1 is invalid.\n\n"
                                 "Do you want to delete the shader cache and continue?")
                                  .arg(gameName),
                              QMessageBox::Yes | QMessageBox::No);

    if (reply == QMessageBox::Yes) {
        QString shaderZipPath;
        Common::FS::PathToQString(shaderZipPath,
                                  Common::FS::GetUserPath(Common::FS::PathType::CacheDir) /
                                      std::filesystem::path(gameSerial.toStdString() + ".zip"));

        QString shaderDirPath;
        Common::FS::PathToQString(shaderDirPath,
                                  Common::FS::GetUserPath(Common::FS::PathType::CacheDir) /
                                      std::filesystem::path(gameSerial.toStdString()));

        if (QFile::exists(shaderZipPath)) {
            QFile::remove(shaderZipPath);
        }

        QDir shaderDir(shaderDirPath);
        if (shaderDir.exists()) {
            shaderDir.removeRecursively();
        }
    }
    Storage::DataBase::Instance().ResetShaderCacheState();
    if (Storage::shader_cache_paused_game.exchange(false)) {
        DebugState.ResumeGuestThreads();
    }
}

void MainWindow::openSettingsWindow() {
    SettingsDialog dlg(m_compat_info, m_ipc_client, this);
    dlg.exec();
    restoreBigPictureFocus();
    restoreHubFocus();
}

void MainWindow::createToolbarContextMenu(const QPoint& pos) {
    QMenu menu(this);
    menu.setTitle(tr("Customize Toolbar"));

    for (QWidget* container : m_toolbarContainers) {
        QString name;

        if (container->objectName() == "playPauseContainer") {
            name = tr("Play/Pause");
        } else {
            QLabel* label = container->property("buttonLabel").value<QLabel*>();
            name = label ? label->text() : container->objectName();
            if (name.isEmpty()) {

                if (qobject_cast<QFrame*>(container)) {
                    name = tr("Separator");
                } else if (qobject_cast<QCheckBox*>(container)) {
                    name = qobject_cast<QCheckBox*>(container)->text();
                } else if (qobject_cast<QComboBox*>(container->parentWidget())) {
                    name = tr("Style/Search/Size");
                } else {
                    name = tr("Unknown Widget");
                }
            }
        }

        QAction* action = menu.addAction(name);
        action->setCheckable(true);
        action->setChecked(container->isVisible());
        action->setData(QVariant::fromValue(container));
        connect(action, &QAction::toggled, this, &MainWindow::toggleToolbarWidgetVisibility);
    }

    QPoint menuPos = QCursor::pos();
    menu.exec(menuPos);
}

void MainWindow::toggleToolbarWidgetVisibility(bool checked) {
    QAction* action = qobject_cast<QAction*>(sender());
    if (!action)
        return;

    QWidget* container = action->data().value<QWidget*>();
    if (container) {
        container->setVisible(checked);

        if (!container->objectName().isEmpty()) {
            Config::setToolbarWidgetVisibility(container->objectName().toStdString(), checked);
        } else {
        }

        const auto config_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        Config::saveMainWindow(config_dir / "config.toml");

        if (ui->toolBar->widgetForAction(ui->toolBar->toggleViewAction()) &&
            ui->toolBar->widgetForAction(ui->toolBar->toggleViewAction())->parentWidget()) {
            ui->toolBar->widgetForAction(ui->toolBar->toggleViewAction())
                ->parentWidget()
                ->layout()
                ->invalidate();
        }
        ui->toolBar->updateGeometry();
    }
}

void MainWindow::restoreBigPictureFocus() {
    if (g_MainWindow && m_bigPicture->isVisible()) {
        m_bigPicture->show();
        m_bigPicture->raise();
        m_bigPicture->activateWindow();
        m_bigPicture->setFocus(Qt::OtherFocusReason);
    }
}

void MainWindow::restoreHubFocus() {
    if (g_MainWindow && m_hubMenu->isVisible()) {
        m_hubMenu->show();
        m_hubMenu->raise();
        m_hubMenu->activateWindow();
        m_hubMenu->setFocus(Qt::OtherFocusReason);
    }
}

void MainWindow::openHotkeysWindow() {
    Hotkeys dlg(m_ipc_client, Config::getGameRunning(), this);
    dlg.exec();
    restoreBigPictureFocus();
    restoreHubFocus();
}

void MainWindow::StartGameByIndex(int index, QStringList args) {
    if (index < 0 || index >= m_game_info->m_games.size())
        return;
    lastGamePath = m_game_info->m_games[index].path;
    runningGameSerial = m_game_info->m_games[index].serial;

    StartGameWithArgs(args, index);
}

void MainWindow::toggleColorFilter() {
    bool enableFilter = ui->toggleColorFilterAct->isChecked();
    Config::setEnableColorFilter(enableFilter);

    QColor baseColor;
    QColor hoverColor;

    if (enableFilter) {
        baseColor = m_window_themes.iconBaseColor();
        hoverColor = m_window_themes.iconHoverColor();
    } else {

        bool isLightTheme = (Config::getMainWindowTheme() == static_cast<int>(Theme::Light));
        baseColor = isLightTheme ? Qt::black : Qt::white;
        hoverColor = baseColor;
    }

    SetUiIcons(baseColor, hoverColor);

    if (m_game_list_frame) {
        if (enableFilter) {
            m_game_list_frame->SetThemeColors(m_window_themes.textColor());
        } else {
            m_game_list_frame->SetThemeColors(baseColor);
        }
    }

    if (m_game_cinematic_frame)
        m_game_cinematic_frame->update();

    const auto config_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
    Config::saveMainWindow(config_dir / "config.toml");
}

void MainWindow::CreateActions() {
    // create action group for icon size
    m_icon_size_act_group = new QActionGroup(this);
    m_icon_size_act_group->addAction(ui->setIconSizeTinyAct);
    m_icon_size_act_group->addAction(ui->setIconSizeSmallAct);
    m_icon_size_act_group->addAction(ui->setIconSizeMediumAct);
    m_icon_size_act_group->addAction(ui->setIconSizeLargeAct);

    // create action group for list mode
    m_list_mode_act_group = new QActionGroup(this);
    m_list_mode_act_group->addAction(ui->setlistModeListAct);
    m_list_mode_act_group->addAction(ui->setlistModeGridAct);
    m_list_mode_act_group->addAction(ui->setlistElfAct);
    if (ui->setlistModeCinematicAct) {
        m_list_mode_act_group->addAction(ui->setlistModeCinematicAct);
    }
    // create action group for themes
    m_theme_act_group = new QActionGroup(this);
    m_theme_act_group->addAction(ui->setThemeDark);
    m_theme_act_group->addAction(ui->setThemeLight);
    m_theme_act_group->addAction(ui->setThemeGreen);
    m_theme_act_group->addAction(ui->setThemeBlue);
    m_theme_act_group->addAction(ui->setThemeViolet);
    m_theme_act_group->addAction(ui->setThemeGruvbox);
    m_theme_act_group->addAction(ui->setThemeTokyoNight);
    m_theme_act_group->addAction(ui->setThemeOled);
    m_theme_act_group->addAction(ui->setThemeNeon);
    m_theme_act_group->addAction(ui->setThemeShadlix);
    m_theme_act_group->addAction(ui->setThemeShadlixCave);
    m_theme_act_group->addAction(ui->setThemeDeepPurple);
    m_theme_act_group->addAction(ui->setThemeQSS);
}

void MainWindow::toggleLabelsUnderIcons() {
    bool showLabels = ui->toggleLabelsAct->isChecked();
    Config::setShowLabelsUnderIcons(showLabels);
    UpdateToolbarLabels();
    if (Config::getGameRunning()) {
        UpdateToolbarButtons();
    }
    const auto config_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
    Config::saveMainWindow(config_dir / "config.toml");
}

void MainWindow::toggleFullscreen() {
    SDL_Event toggleFullscreenEvent;
    toggleFullscreenEvent.type = SDL_EVENT_TOGGLE_FULLSCREEN;
    SDL_PushEvent(&toggleFullscreenEvent);
}

QWidget* MainWindow::createButtonWithLabel(QPushButton* button, const QString& labelText,
                                           bool showLabel) {
    QWidget* container = new QWidget(this);
    QVBoxLayout* layout = new QVBoxLayout(container);
    layout->setAlignment(Qt::AlignCenter | Qt::AlignBottom);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(button);

    QLabel* label = new QLabel(labelText, this);
    label->setAlignment(Qt::AlignCenter | Qt::AlignBottom);
    label->setVisible(ui->toggleLabelsAct->isChecked());
    layout->addWidget(label);

    if (!ui->toggleLabelsAct->isChecked())
        button->setToolTip(labelText);
    else
        button->setToolTip("");

    container->setLayout(layout);
    container->setProperty("buttonLabel", QVariant::fromValue(label));
    return container;
}

QWidget* createSpacer(QWidget* parent) {
    QWidget* spacer = new QWidget(parent);
    spacer->setFixedWidth(15);
    spacer->setFixedHeight(15);
    return spacer;
}

void MainWindow::AddUiWidgets() {
    m_toolbarContainers.clear();

    auto createButtonWithLabel_wrapped = [this](QPushButton* button, const QString& labelText,
                                                bool showLabel) -> QWidget* {
        QWidget* container = new QWidget(this);
        QVBoxLayout* layout = new QVBoxLayout(container);
        layout->setAlignment(Qt::AlignCenter | Qt::AlignBottom);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(button);

        QLabel* label = new QLabel(labelText, this);
        label->setAlignment(Qt::AlignCenter | Qt::AlignBottom);
        label->setVisible(ui->toggleLabelsAct->isChecked());
        // Set initial theme color
        label->setStyleSheet(QString("color: %1; font-size: 11px;").arg(m_window_themes.textColor().name()));
        layout->addWidget(label);

        if (!ui->toggleLabelsAct->isChecked())
            button->setToolTip(labelText);
        else
            button->setToolTip("");

        container->setLayout(layout);
        container->setProperty("buttonLabel", QVariant::fromValue(label));
        container->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        return container;
    };

    bool showLabels = ui->toggleLabelsAct->isChecked();
    ui->toolBar->clear();

    ui->backgroundImageLabel = new QLabel(this);
    ui->backgroundImageLabel->setObjectName("backgroundImageLabel");
    ui->backgroundImageLabel->setAlignment(Qt::AlignCenter);
    ui->backgroundImageLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    QColor bgColor = m_window_themes.backgroundColor();
    ui->backgroundImageLabel->setStyleSheet(QString("QLabel#backgroundImageLabel {"
                                                    "  background-color: rgba(%1, %2, %3, %4);"
                                                    "  border: none;"
                                                    "}")
                                                .arg(bgColor.red())
                                                .arg(bgColor.green())
                                                .arg(bgColor.blue())
                                                .arg(Config::getIconBgOpacity()));

    QString defaultBackgroundPath = ":/images/default_background.jpg";
    QPixmap defaultPixmap(defaultBackgroundPath);
    if (!defaultPixmap.isNull()) {
        QPixmap scaledPixmap =
            defaultPixmap.scaled(size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        ui->backgroundImageLabel->setPixmap(scaledPixmap);
    }

    setCentralWidget(ui->backgroundImageLabel);

    QWidget* uiOverlay = new QWidget(ui->backgroundImageLabel);
    uiOverlay->setObjectName("uiOverlay");
    uiOverlay->setStyleSheet("QWidget#uiOverlay { background-color: transparent; }");
    uiOverlay->setGeometry(0, 0, ui->backgroundImageLabel->width(),
                           ui->backgroundImageLabel->height());

    QVBoxLayout* mainLayout = new QVBoxLayout(uiOverlay);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    mainLayout->setSpacing(0);

    ui->topControlBar = new QWidget(uiOverlay);
    ui->topControlBar->setStyleSheet("background-color: transparent; border: none;");
    ui->topControlBar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    ui->topControlBarLayout = new QHBoxLayout(ui->topControlBar);
    ui->topControlBarLayout->setContentsMargins(5, 5, 5, 5);
    ui->topControlBarLayout->setSpacing(10);
    ui->topControlBarLayout->setAlignment(Qt::AlignCenter);

    ui->launcherBox = new QCheckBox(tr("Use Selected Version"), uiOverlay);
    ui->launcherBox->setToolTip(tr("Let you Boot Game with selected Version"));
    ui->launcherBox->setChecked(Config::getBootLauncher());

    ui->toggleLogButton->setObjectName("ToggleLogButton");
    ui->installPkgButton->setObjectName("InstallPkgButton");
    ui->bpBootButton->setObjectName("BPBootButton");
    ui->zarBootButton->setObjectName("ZarBootButton");
    ui->zarConvertButton->setObjectName("ZarConvertButton");
    ui->themeButton->setObjectName("ThemeButton");
    ui->themeButton->setText(tr("Theme"));

    for (QPushButton* extraBtn : {ui->toggleLogButton, ui->installPkgButton, ui->bpBootButton,
                                  ui->zarBootButton, ui->zarConvertButton, ui->themeButton}) {
        const QColor toolbarGlowColor(90, 170, 255);
        extraBtn->setCursor(Qt::PointingHandCursor);
        extraBtn->setProperty("modernToolbarButton", true);
        HoverAnimator::Attach(extraBtn, toolbarGlowColor);
        QColor extraBtnBgColor = m_window_themes.backgroundColor();
        extraBtn->setStyleSheet(QString(
            "QPushButton { background-color: rgba(%1, %2, %3, 200); border: 1px solid rgba(90, "
            "170, 255, 150); border-radius: 5px; padding: 5px; }")
            .arg(extraBtnBgColor.red())
            .arg(extraBtnBgColor.green())
            .arg(extraBtnBgColor.blue()));
    }

    ui->launcherBox->setObjectName("launcherBox");
    QColor launcherBgColor = m_window_themes.backgroundColor();
    ui->launcherBox->setStyleSheet(QString(
        "QCheckBox { background-color: rgba(%1, %2, %3, 220); border: 1px solid rgba(90, 170, 255, "
        "150); border-radius: 5px; padding: 5px; }")
        .arg(launcherBgColor.red())
        .arg(launcherBgColor.green())
        .arg(launcherBgColor.blue()));

    QWidget* styleContainer = new QWidget(uiOverlay);
    styleContainer->setStyleSheet("background-color: transparent; border: none;");
    QHBoxLayout* styleLayout = new QHBoxLayout(styleContainer);
    styleLayout->setContentsMargins(0, 0, 0, 0);
    styleLayout->setSpacing(5);

    QLabel* styleLabel = new QLabel(tr("GUI Style:"), uiOverlay);
    QColor styleBgColor = m_window_themes.backgroundColor();
    styleLabel->setStyleSheet(QString("color: white; font-weight: bold; background-color: rgba(%1, %2, %3, "
                              "200); padding: 5px; border-radius: 5px;")
                              .arg(styleBgColor.red())
                              .arg(styleBgColor.green())
                              .arg(styleBgColor.blue()));
    styleLayout->addWidget(styleLabel);

    ui->styleSelector->setStyleSheet(QString(
        "QComboBox { background-color: rgba(%1, %2, %3, 200); color: white; padding: 5px; "
        "border-radius: 5px; border: 1px solid rgba(90, 170, 255, 150); } QComboBox::drop-down { "
        "border: none; } QComboBox QAbstractItemView { background-color: rgba(%1, %2, %3, 200); "
        "color: white; selection-background-color: rgba(90, 170, 255, 150); }")
        .arg(styleBgColor.red())
        .arg(styleBgColor.green())
        .arg(styleBgColor.blue()));
    styleLayout->addWidget(ui->styleSelector);
    styleContainer->setObjectName("styleContainer");

    ui->topControlBarLayout->addStretch();
    ui->topControlBarLayout->addWidget(styleContainer);
    ui->topControlBarLayout->addWidget(ui->mw_searchbar);
    ui->topControlBarLayout->addWidget(ui->bpBootButton);
    ui->topControlBarLayout->addWidget(ui->zarBootButton);
    ui->topControlBarLayout->addWidget(ui->zarConvertButton);
    ui->topControlBarLayout->addWidget(ui->installPkgButton);
    ui->topControlBarLayout->addWidget(ui->toggleLogButton);
    ui->topControlBarLayout->addWidget(ui->themeButton);
    ui->topControlBarLayout->addWidget(ui->launcherBox);
    ui->topControlBarLayout->addStretch();

    m_toolbarContainers.append(ui->installPkgButton);
    m_toolbarContainers.append(ui->toggleLogButton);
    m_toolbarContainers.append(ui->bpBootButton);
    m_toolbarContainers.append(ui->zarBootButton);
    m_toolbarContainers.append(ui->zarConvertButton);
    m_toolbarContainers.append(ui->themeButton);
    m_toolbarContainers.append(styleContainer);
    m_toolbarContainers.append(ui->mw_searchbar);
    m_toolbarContainers.append(ui->launcherBox);

    mainLayout->addWidget(ui->topControlBar);

    mainLayout->addStretch();

    QWidget* iconsWrapper = new QWidget(uiOverlay);
    iconsWrapper->setStyleSheet("background-color: transparent; border: none;");
    QHBoxLayout* iconsWrapperLayout = new QHBoxLayout(iconsWrapper);
    iconsWrapperLayout->setContentsMargins(0, 0, 0, 0);

    ui->bootIconsArea = new QWidget(iconsWrapper);
    ui->bootIconsArea->setObjectName("bootIconsArea");
    ui->bootIconsLayout = new QHBoxLayout(ui->bootIconsArea);
    ui->bootIconsLayout->setContentsMargins(10, 5, 10, 5);
    ui->bootIconsLayout->setSpacing(10);

    QColor bootIconsBgColor = m_window_themes.backgroundColor();
    ui->bootIconsArea->setStyleSheet(QString("QWidget#bootIconsArea {"
                                     "  background-color: rgba(%1, %2, %3, 220);"
                                     "  border-radius: 12px;"
                                     "  border: 1px solid rgba(90, 170, 255, 80);"
                                     "}")
                                     .arg(bootIconsBgColor.red())
                                     .arg(bootIconsBgColor.green())
                                     .arg(bootIconsBgColor.blue()));
    ui->bootIconsArea->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);

    const QColor toolbarGlowColor(90, 170, 255);
    auto addToolbarButton = [this, createButtonWithLabel_wrapped, showLabels,
                             toolbarGlowColor](QPushButton* button, const QString& text) {
        button->setCursor(Qt::PointingHandCursor);
        button->setProperty("modernToolbarButton", true);
        button->setStyleSheet("QPushButton { background-color: transparent; border: none; }");
        HoverAnimator::Attach(button, toolbarGlowColor);
        QWidget* container = createButtonWithLabel_wrapped(button, text, showLabels);
        container->setStyleSheet("background-color: transparent;");
        return container;
    };

    ui->playButton->setObjectName("playButton");
    ui->pauseButton->setObjectName("pauseButton");
    ui->stopButton->setObjectName("stopButton");
    ui->restartButton->setObjectName("restartButton");
    ui->settingsButton->setObjectName("settingsButton");
    ui->fullscreenButton->setObjectName("fullscreenButton");
    ui->controllerButton->setObjectName("controllerButton");
    ui->keyboardButton->setObjectName("keyboardButton");
    ui->configureHotkeysButton->setObjectName("configureHotkeysButton");
    ui->refreshButton->setObjectName("refreshButton");
    ui->updaterButton->setObjectName("updaterButton");
    ui->versionButton->setObjectName("versionButton");
    ui->modManagerButton->setObjectName("modManagerButton");
    ui->bigPictureButton->setObjectName("bigPictureButton");
    ui->hubMenuButton->setObjectName("hubMenuButton");
    ui->cinemaButton->setObjectName("cinemaButton");

    QSize initialIconSize(40, 40);
    for (QPushButton* btn :
         {ui->playButton, ui->pauseButton, ui->stopButton, ui->restartButton, ui->settingsButton,
          ui->fullscreenButton, ui->controllerButton, ui->keyboardButton,
          ui->configureHotkeysButton, ui->updaterButton, ui->refreshButton, ui->versionButton,
          ui->modManagerButton, ui->bigPictureButton, ui->hubMenuButton, ui->cinemaButton}) {
        btn->setIconSize(initialIconSize);
    }

    QWidget* playPauseStack = new QWidget(uiOverlay);
    playPauseStack->setStyleSheet("background-color: transparent; border: none;");
    QHBoxLayout* playPauseLayout = new QHBoxLayout(playPauseStack);
    playPauseLayout->setContentsMargins(0, 0, 0, 0);
    playPauseLayout->setSpacing(0);

    ui->playContainer = addToolbarButton(ui->playButton, tr("Play"));
    ui->pauseContainer = addToolbarButton(ui->pauseButton, tr("Pause"));

    ui->playContainer->setObjectName("playContainer");
    ui->pauseContainer->setObjectName("pauseContainer");

    playPauseLayout->addWidget(ui->playContainer);
    playPauseLayout->addWidget(ui->pauseContainer);
    playPauseStack->setLayout(playPauseLayout);

    playPauseStack->setObjectName("playPauseContainer");

    ui->pauseButton->setVisible(false);
    QLabel* pauseLabel = ui->pauseContainer->findChild<QLabel*>();
    if (pauseLabel) {
        pauseLabel->setVisible(false);
    }

    QWidget* stopContainer = addToolbarButton(ui->stopButton, tr("Stop"));
    QWidget* restartContainer = addToolbarButton(ui->restartButton, tr("Restart"));
    QWidget* settingsContainer = addToolbarButton(ui->settingsButton, tr("Settings"));
    QWidget* fullscreenContainer = addToolbarButton(ui->fullscreenButton, tr("Fullscreen"));
    QWidget* controllerContainer = addToolbarButton(ui->controllerButton, tr("Controllers"));
    QWidget* keyboardContainer = addToolbarButton(ui->keyboardButton, tr("Keyboard"));
    QWidget* hotkeysContainer = addToolbarButton(ui->configureHotkeysButton, tr("Hotkeys"));
    QWidget* updaterContainer = addToolbarButton(ui->updaterButton, tr("Update"));
    QWidget* refreshContainer = addToolbarButton(ui->refreshButton, tr("Refresh"));
    QWidget* versionContainer = addToolbarButton(ui->versionButton, tr("Version"));
    QWidget* modsContainer = addToolbarButton(ui->modManagerButton, tr("Mods"));
    QWidget* bigPictureContainer = addToolbarButton(ui->bigPictureButton, tr("BigPicture"));
    QWidget* gameHubContainer = addToolbarButton(ui->hubMenuButton, tr("GameHub"));
    QWidget* cinemaContainer = addToolbarButton(ui->cinemaButton, tr("Cinema"));

    stopContainer->setObjectName("stopContainer");
    restartContainer->setObjectName("restartContainer");
    settingsContainer->setObjectName("settingsContainer");
    fullscreenContainer->setObjectName("fullscreenContainer");
    controllerContainer->setObjectName("controllerContainer");
    keyboardContainer->setObjectName("keyboardContainer");
    hotkeysContainer->setObjectName("hotkeysContainer");
    updaterContainer->setObjectName("updaterContainer");
    refreshContainer->setObjectName("refreshContainer");
    versionContainer->setObjectName("versionContainer");
    modsContainer->setObjectName("modsContainer");
    bigPictureContainer->setObjectName("bigPictureContainer");
    gameHubContainer->setObjectName("gameHubContainer");
    cinemaContainer->setObjectName("cinemaContainer");

    ui->bootIconsLayout->addWidget(playPauseStack);
    ui->bootIconsLayout->addWidget(stopContainer);
    ui->bootIconsLayout->addWidget(restartContainer);
    ui->bootIconsLayout->addWidget(settingsContainer);
    ui->bootIconsLayout->addWidget(fullscreenContainer);
    ui->bootIconsLayout->addWidget(controllerContainer);
    ui->bootIconsLayout->addWidget(keyboardContainer);
    ui->bootIconsLayout->addWidget(hotkeysContainer);
    ui->bootIconsLayout->addWidget(updaterContainer);
    ui->bootIconsLayout->addWidget(refreshContainer);
    ui->bootIconsLayout->addWidget(versionContainer);
    ui->bootIconsLayout->addWidget(modsContainer);
    ui->bootIconsLayout->addWidget(bigPictureContainer);
    ui->bootIconsLayout->addWidget(gameHubContainer);
    ui->bootIconsLayout->addWidget(cinemaContainer);

    ui->bootIconsLayout->setAlignment(Qt::AlignCenter);

    iconsWrapperLayout->addStretch();
    iconsWrapperLayout->addWidget(ui->bootIconsArea);
    iconsWrapperLayout->addStretch();

    m_toolbarContainers.append(playPauseStack);
    m_toolbarContainers.append(stopContainer);
    m_toolbarContainers.append(restartContainer);
    m_toolbarContainers.append(settingsContainer);
    m_toolbarContainers.append(fullscreenContainer);
    m_toolbarContainers.append(controllerContainer);
    m_toolbarContainers.append(keyboardContainer);
    m_toolbarContainers.append(hotkeysContainer);
    m_toolbarContainers.append(updaterContainer);
    m_toolbarContainers.append(refreshContainer);
    m_toolbarContainers.append(versionContainer);
    m_toolbarContainers.append(modsContainer);
    m_toolbarContainers.append(bigPictureContainer);
    m_toolbarContainers.append(gameHubContainer);
    m_toolbarContainers.append(cinemaContainer);

    ui->bootIconsArea->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->bootIconsArea, &QWidget::customContextMenuRequested, this,
            &MainWindow::createToolbarContextMenu);

    mainLayout->addWidget(iconsWrapper);

    mainLayout->addSpacing(15);

    ui->gameRectangleContainer = new QWidget(uiOverlay);
    ui->gameRectangleLayout = new QVBoxLayout(ui->gameRectangleContainer);
    ui->gameRectangleLayout->setContentsMargins(15, 15, 15, 15);
    ui->gameRectangleLayout->setSpacing(10);

    ui->logDisplay = new QTextEdit(uiOverlay);
    ui->logDisplay->setObjectName("logDisplay");
    ui->logDisplay->setText(tr("Game Log"));
    ui->logDisplay->setReadOnly(true);
    ui->logDisplay->setMinimumHeight(150);
    ui->logDisplay->setMaximumHeight(200);

    QPalette logPalette = ui->logDisplay->palette();
    logPalette.setColor(QPalette::Base, Qt::black);
    ui->logDisplay->setPalette(logPalette);

    QColor gameRectBgColor = m_window_themes.backgroundColor();
    ui->gameRectangleContainer->setStyleSheet(QString("QWidget#gameRectangleContainer {"
                                              "  background-color: rgba(%1, %2, %3, 200);"
                                              "  border-radius: 15px;"
                                              "  border: 2px solid rgba(90, 170, 255, 150);"
                                              "}")
                                              .arg(gameRectBgColor.red())
                                              .arg(gameRectBgColor.green())
                                              .arg(gameRectBgColor.blue()));
    ui->gameRectangleContainer->setObjectName("gameRectangleContainer");
    ui->gameRectangleContainer->setMinimumHeight(75);

    ui->gameHeightSliderContainer = new QWidget(uiOverlay);
    ui->gameHeightSliderContainer->setStyleSheet("background-color: transparent; border: none;");
    ui->gameHeightSliderLayout = new QHBoxLayout(ui->gameHeightSliderContainer);
    ui->gameHeightSliderLayout->setContentsMargins(5, 5, 5, 5);
    ui->gameHeightSliderLayout->setSpacing(30);

    QWidget* bootIconsSliderWidget = new QWidget(uiOverlay);
    bootIconsSliderWidget->setStyleSheet("background-color: transparent; border: none;");
    QVBoxLayout* bootIconsSliderVLayout = new QVBoxLayout(bootIconsSliderWidget);
    bootIconsSliderVLayout->setContentsMargins(0, 0, 0, 0);
    bootIconsSliderVLayout->setSpacing(5);
    bootIconsSliderVLayout->setAlignment(Qt::AlignCenter);

    ui->bootIconsSizeSlider = new QSlider(Qt::Horizontal, uiOverlay);
    ui->bootIconsSizeSlider->setMinimum(20);
    ui->bootIconsSizeSlider->setMaximum(80);
    ui->bootIconsSizeSlider->setValue(40);
    ui->bootIconsSizeSlider->setFixedWidth(150);

    m_bootIconsLabel = new QLabel(tr("Icons Size"), uiOverlay);
    m_bootIconsLabel->setStyleSheet("color: white; font-weight: bold; font-size: 11px;");
    m_bootIconsLabel->setAlignment(Qt::AlignCenter);

    bootIconsSliderVLayout->addWidget(ui->bootIconsSizeSlider, 0, Qt::AlignCenter);
    bootIconsSliderVLayout->addWidget(m_bootIconsLabel, 0, Qt::AlignCenter);
    bootIconsSliderWidget->setLayout(bootIconsSliderVLayout);

    QWidget* gameHeightSliderWidget = new QWidget(uiOverlay);
    gameHeightSliderWidget->setStyleSheet("background-color: transparent; border: none;");
    QVBoxLayout* gameHeightSliderVLayout = new QVBoxLayout(gameHeightSliderWidget);
    gameHeightSliderVLayout->setContentsMargins(0, 0, 0, 0);
    gameHeightSliderVLayout->setSpacing(5);
    gameHeightSliderVLayout->setAlignment(Qt::AlignCenter);

    ui->gameContainerHeightSlider = new QSlider(Qt::Horizontal, uiOverlay);
    ui->gameContainerHeightSlider->setMinimum(150);
    ui->gameContainerHeightSlider->setMaximum(600);
    ui->gameContainerHeightSlider->setValue(300);
    ui->gameContainerHeightSlider->setFixedWidth(150);

    m_gameHeightLabel = new QLabel(tr("Container Height"), uiOverlay);
    m_gameHeightLabel->setStyleSheet("color: white; font-weight: bold; font-size: 11px;");
    m_gameHeightLabel->setAlignment(Qt::AlignCenter);

    gameHeightSliderVLayout->addWidget(ui->gameContainerHeightSlider, 0, Qt::AlignCenter);
    gameHeightSliderVLayout->addWidget(m_gameHeightLabel, 0, Qt::AlignCenter);
    gameHeightSliderWidget->setLayout(gameHeightSliderVLayout);

    QWidget* gameSizeSliderWidget = new QWidget(uiOverlay);
    gameSizeSliderWidget->setStyleSheet("background-color: transparent; border: none;");
    QVBoxLayout* gameSizeSliderVLayout = new QVBoxLayout(gameSizeSliderWidget);
    gameSizeSliderVLayout->setContentsMargins(0, 0, 0, 0);
    gameSizeSliderVLayout->setSpacing(5);
    gameSizeSliderVLayout->setAlignment(Qt::AlignCenter);

    m_gameSizeLabel = new QLabel(tr("Game Size"), uiOverlay);
    m_gameSizeLabel->setStyleSheet("color: white; font-weight: bold; font-size: 11px;");
    m_gameSizeLabel->setAlignment(Qt::AlignCenter);

    ui->sizeSlider->setFixedWidth(150);

    gameSizeSliderVLayout->addWidget(ui->sizeSlider, 0, Qt::AlignCenter);
    gameSizeSliderVLayout->addWidget(m_gameSizeLabel, 0, Qt::AlignCenter);
    gameSizeSliderWidget->setLayout(gameSizeSliderVLayout);

    ui->gameHeightSliderLayout->addStretch();
    ui->gameHeightSliderLayout->addWidget(bootIconsSliderWidget);
    ui->gameHeightSliderLayout->addWidget(gameHeightSliderWidget);
    ui->gameHeightSliderLayout->addWidget(gameSizeSliderWidget);

    ui->logOpacitySlider = new QSlider(Qt::Horizontal, uiOverlay);
    ui->logOpacitySlider->setMinimum(0);
    ui->logOpacitySlider->setMaximum(100);
    ui->logOpacitySlider->setValue(Config::getLogOpacity());

    ui->bgOpacitySlider = new QSlider(Qt::Horizontal, uiOverlay);
    ui->bgOpacitySlider->setMinimum(0);
    ui->bgOpacitySlider->setMaximum(100);
    ui->bgOpacitySlider->setValue(Config::getBgOpacity());

    ui->iconBgOpacitySlider = new QSlider(Qt::Horizontal, uiOverlay);
    ui->iconBgOpacitySlider->setMinimum(0);
    ui->iconBgOpacitySlider->setMaximum(255);
    ui->iconBgOpacitySlider->setValue(Config::getIconBgOpacity());

    QWidget* logOpacitySliderWidget = new QWidget(uiOverlay);
    logOpacitySliderWidget->setStyleSheet("background-color: transparent; border: none;");
    QVBoxLayout* logOpacitySliderVLayout = new QVBoxLayout(logOpacitySliderWidget);
    logOpacitySliderVLayout->setContentsMargins(0, 0, 0, 0);
    logOpacitySliderVLayout->setSpacing(5);
    logOpacitySliderVLayout->setAlignment(Qt::AlignCenter);

    m_logOpacityLabel = new QLabel(tr("Log Opacity"), uiOverlay);
    m_logOpacityLabel->setStyleSheet("color: white; font-weight: bold; font-size: 11px;");
    m_logOpacityLabel->setAlignment(Qt::AlignCenter);

    ui->logOpacitySlider->setFixedWidth(150);
    logOpacitySliderVLayout->addWidget(ui->logOpacitySlider, 0, Qt::AlignCenter);
    logOpacitySliderVLayout->addWidget(m_logOpacityLabel, 0, Qt::AlignCenter);
    logOpacitySliderWidget->setLayout(logOpacitySliderVLayout);

    QWidget* bgOpacitySliderWidget = new QWidget(uiOverlay);
    bgOpacitySliderWidget->setStyleSheet("background-color: transparent; border: none;");
    QVBoxLayout* bgOpacitySliderVLayout = new QVBoxLayout(bgOpacitySliderWidget);
    bgOpacitySliderVLayout->setContentsMargins(0, 0, 0, 0);
    bgOpacitySliderVLayout->setSpacing(5);
    bgOpacitySliderVLayout->setAlignment(Qt::AlignCenter);

    m_bgOpacityLabel = new QLabel(tr("BG Opacity"), uiOverlay);
    m_bgOpacityLabel->setStyleSheet("color: white; font-weight: bold; font-size: 11px;");
    m_bgOpacityLabel->setAlignment(Qt::AlignCenter);

    ui->bgOpacitySlider->setFixedWidth(150);
    bgOpacitySliderVLayout->addWidget(ui->bgOpacitySlider, 0, Qt::AlignCenter);
    bgOpacitySliderVLayout->addWidget(m_bgOpacityLabel, 0, Qt::AlignCenter);
    bgOpacitySliderWidget->setLayout(bgOpacitySliderVLayout);

    QWidget* iconBgOpacitySliderWidget = new QWidget(uiOverlay);
    iconBgOpacitySliderWidget->setStyleSheet("background-color: transparent; border: none;");
    QVBoxLayout* iconBgOpacitySliderVLayout = new QVBoxLayout(iconBgOpacitySliderWidget);
    iconBgOpacitySliderVLayout->setContentsMargins(0, 0, 0, 0);
    iconBgOpacitySliderVLayout->setSpacing(5);
    iconBgOpacitySliderVLayout->setAlignment(Qt::AlignCenter);

    m_iconBgOpacityLabel = new QLabel(tr("Icon BG Opacity"), uiOverlay);
    m_iconBgOpacityLabel->setStyleSheet("color: white; font-weight: bold; font-size: 11px;");
    m_iconBgOpacityLabel->setAlignment(Qt::AlignCenter);

    ui->iconBgOpacitySlider->setFixedWidth(150);
    iconBgOpacitySliderVLayout->addWidget(ui->iconBgOpacitySlider, 0, Qt::AlignCenter);
    iconBgOpacitySliderVLayout->addWidget(m_iconBgOpacityLabel, 0, Qt::AlignCenter);
    iconBgOpacitySliderWidget->setLayout(iconBgOpacitySliderVLayout);

    ui->gameHeightSliderLayout->addWidget(logOpacitySliderWidget);
    ui->gameHeightSliderLayout->addWidget(bgOpacitySliderWidget);
    ui->gameHeightSliderLayout->addWidget(iconBgOpacitySliderWidget);
    ui->gameHeightSliderLayout->addStretch();

    connect(ui->logOpacitySlider, &QSlider::valueChanged, this, [this](int value) {
        Config::setLogOpacity(value);
        const auto config_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        Config::save(config_dir / "config.toml", false);
        if (ui->logDisplay) {
            QColor logBgColor = m_window_themes.backgroundColor();
            ui->logDisplay->setStyleSheet(
                QString("QTextEdit#logDisplay { background-color: rgba(%1, %2, %3, %4); color: white; "
                        "border: 1px solid rgba(90, 170, 255, 150); }")
                    .arg(logBgColor.red())
                    .arg(logBgColor.green())
                    .arg(logBgColor.blue())
                    .arg(value * 255 / 100));
        }
    });

    connect(ui->bgOpacitySlider, &QSlider::valueChanged, this, [this](int value) {
        Config::setBgOpacity(value);
        const auto config_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        Config::save(config_dir / "config.toml", false);
        int alpha = value * 255 / 100;
        QColor gameRectBgColor = m_window_themes.backgroundColor();
        ui->gameRectangleContainer->setStyleSheet(
            QString("QWidget#gameRectangleContainer {"
                    "  background-color: rgba(%1, %2, %3, %4);"
                    "  border-radius: 15px;"
                    "  border: 2px solid rgba(90, 170, 255, 150);"
                    "}")
                .arg(gameRectBgColor.red())
                .arg(gameRectBgColor.green())
                .arg(gameRectBgColor.blue())
                .arg(alpha));
        // Update ZAR backgrounds with BG opacity
        if (m_game_list_frame) {
            m_game_list_frame->RefreshZarBackgroundImage();
        }
        if (m_game_grid_frame) {
            m_game_grid_frame->RefreshZarBackgroundImage();
            m_game_grid_frame->RefreshGridBackgroundImage();
        }
    });

    connect(ui->iconBgOpacitySlider, &QSlider::valueChanged, this, [this](int value) {
        Config::setIconBgOpacity(value);
        const auto config_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        Config::save(config_dir / "config.toml", false);
        UpdateIconBackgroundOpacity();
    });

    // Apply initial opacity values from config
    if (ui->logDisplay) {
        int logOpacity = Config::getLogOpacity();
        QColor logBgColor = m_window_themes.backgroundColor();
        ui->logDisplay->setStyleSheet(
            QString("QTextEdit#logDisplay { background-color: rgba(%1, %2, %3, %4); color: white; "
                    "border: 1px solid rgba(90, 170, 255, 150); }")
                .arg(logBgColor.red())
                .arg(logBgColor.green())
                .arg(logBgColor.blue())
                .arg(logOpacity * 255 / 100));
    }

    int bgOpacity = Config::getBgOpacity();
    int bgAlpha = bgOpacity * 255 / 100;
    QColor gameRectBgColorInit = m_window_themes.backgroundColor();
    ui->gameRectangleContainer->setStyleSheet(QString("QWidget#gameRectangleContainer {"
                                                      "  background-color: rgba(%1, %2, %3, %4);"
                                                      "  border-radius: 15px;"
                                                      "  border: 2px solid rgba(90, 170, 255, 150);"
                                                      "}")
                                                  .arg(gameRectBgColorInit.red())
                                                  .arg(gameRectBgColorInit.green())
                                                  .arg(gameRectBgColorInit.blue())
                                                  .arg(bgAlpha));

    UpdateIconBackgroundOpacity();
    ui->gameHeightSliderContainer->setLayout(ui->gameHeightSliderLayout);

    ui->gameRectangleLayout->addWidget(ui->gameHeightSliderContainer);
    mainLayout->addWidget(ui->gameRectangleContainer);

    mainLayout->addSpacing(10);
    mainLayout->addWidget(ui->logDisplay);

    mainLayout->addSpacing(35);

    uiOverlay->setLayout(mainLayout);

    for (QWidget* container : m_toolbarContainers) {
        if (!container->objectName().isEmpty()) {
            bool visible =
                Config::getToolbarWidgetVisibility(container->objectName().toStdString(), true);
            container->setVisible(visible);
        }
    }

    ui->playButton->setVisible(true);
    ui->pauseButton->setVisible(false);

    ui->styleSelector->clear();
    QStringList styles = QStyleFactory::keys();
    for (const QString& styleName : styles) {
        if (styleName.compare("windowsvista", Qt::CaseInsensitive) != 0)
            ui->styleSelector->addItem(styleName);
    }

    QDir qssDir(QString::fromStdString(
        Common::FS::GetUserPath(Common::FS::PathType::CustomThemes).string()));
    QStringList qssFiles = qssDir.entryList({"*.qss"}, QDir::Files);

    for (const QString& qssFile : qssFiles) {
        QString themeName = QFileInfo(qssFile).baseName() + " (QSS)";
        int index = ui->styleSelector->count();
        ui->styleSelector->addItem(themeName);
        ui->styleSelector->setItemData(index, qssDir.absoluteFilePath(qssFile));
    }

    QString savedStyle = QString::fromStdString(Config::getGuiStyle());
    if (!savedStyle.isEmpty()) {
        int idx = ui->styleSelector->findText(savedStyle, Qt::MatchFixedString);
        ui->styleSelector->setCurrentIndex(
            idx >= 0 ? idx : ui->styleSelector->findText(QApplication::style()->objectName()));
    } else {
        ui->styleSelector->setCurrentText(QApplication::style()->objectName());
    }
    m_isRepopulatingStyleSelector = false;
}

void MainWindow::UpdateToolbarButtons() {
    bool showLabels = ui->toggleLabelsAct->isChecked();

    QLabel* playLabel = ui->playContainer->findChild<QLabel*>();
    QLabel* pauseLabel = ui->pauseContainer->findChild<QLabel*>();

    if (Config::getGameRunning()) {
        ui->playButton->setVisible(false);
        ui->pauseButton->setVisible(true);

        if (is_paused) {
            ui->pauseButton->setIcon(ui->playButton->icon());
            ui->pauseButton->setToolTip(tr("Resume"));

            if (m_originalIcons.contains(ui->playButton)) {
                m_originalIcons[ui->pauseButton] = m_originalIcons[ui->playButton];
            }
        } else {
            QColor baseColor;
            QColor hoverColor;

            if (Config::getEnableColorFilter()) {
                baseColor = m_window_themes.iconBaseColor();
                hoverColor = m_window_themes.iconHoverColor();
            } else {
                baseColor = (Config::getMainWindowTheme() == static_cast<int>(Theme::Light))
                                ? Qt::black
                                : Qt::white;
                hoverColor = baseColor;
            }

            ui->pauseButton->setIcon(
                RecolorIcon(QIcon(":/images/pause_icon.png"), baseColor, hoverColor));
            ui->pauseButton->setToolTip(tr("Pause"));

            m_originalIcons[ui->pauseButton] = QIcon(":/images/pause_icon.png");
        }

        if (showLabels) {
            if (playLabel)
                playLabel->setVisible(false);
            if (pauseLabel) {
                pauseLabel->setText(is_paused ? tr("Resume") : tr("Pause"));
                pauseLabel->setVisible(true);
            }
        }
    } else {
        ui->playButton->setVisible(true);
        ui->pauseButton->setVisible(false);

        if (showLabels) {
            if (playLabel) {
                playLabel->setText(tr("Play"));
                playLabel->setVisible(true);
            }
            if (pauseLabel)
                pauseLabel->setVisible(false);
        }
    }
}

void MainWindow::UpdateToolbarLabels() {
    bool showLabels = ui->toggleLabelsAct->isChecked();

    for (QPushButton* button :
         {ui->stopButton, ui->restartButton, ui->settingsButton, ui->fullscreenButton,
          ui->controllerButton, ui->keyboardButton, ui->versionButton, ui->bigPictureButton,
          ui->hubMenuButton, ui->cinemaButton, ui->configureHotkeysButton, ui->updaterButton,
          ui->refreshButton, ui->modManagerButton}) {
        QLabel* label = button->parentWidget()->findChild<QLabel*>();
        if (label)
            label->setVisible(showLabels);
    }

    QLabel* playLabel = ui->playButton->parentWidget()->findChild<QLabel*>();
    QLabel* pauseLabel = ui->pauseButton->parentWidget()->findChild<QLabel*>();

    if (Config::getGameRunning()) {
        if (playLabel)
            playLabel->setVisible(false);
        if (pauseLabel) {
            pauseLabel->setText(is_paused ? tr("Resume") : tr("Pause"));
            pauseLabel->setVisible(showLabels);
        }
    } else {
        if (playLabel) {
            playLabel->setText(tr("Play"));
            playLabel->setVisible(showLabels);
        }
        if (pauseLabel)
            pauseLabel->setVisible(false);
    }

    const auto config_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
    Config::saveMainWindow(config_dir / "config.toml");
}

void MainWindow::contextMenuEvent(QContextMenuEvent* event) {
    if (ui->toolBar->geometry().contains(event->pos())) {

        createToolbarContextMenu(event->globalPos());
        event->accept();
        return;
    }
    QMainWindow::contextMenuEvent(event);
}

void MainWindow::CreateDockWindows(bool newDock) {
    if (newDock) {
        m_game_list_frame.reset(new GameListFrame(m_game_info, m_compat_info, m_ipc_client, this));
        m_game_list_frame->setObjectName("gamelist");

        m_game_grid_frame.reset(new GameGridFrame(m_game_info, m_compat_info, m_ipc_client, this));
        m_game_grid_frame->setObjectName("gamegridlist");

        m_elf_viewer.reset(new ElfViewer(this));
        m_elf_viewer->setObjectName("elflist");

        m_game_cinematic_frame.reset(
            new GameCinematicFrame(m_game_info, m_compat_info, m_ipc_client, this));
        m_game_cinematic_frame->setObjectName("cinematiclist");

        m_list_container = new QWidget(this);
        QHBoxLayout* listLayout = new QHBoxLayout(m_list_container);
        listLayout->setContentsMargins(0, 0, 0, 0);
        listLayout->setSpacing(0);

        m_game_list_frame->m_zar_splitter = new QSplitter(Qt::Horizontal, m_list_container);
        m_game_list_frame->m_zar_splitter->addWidget(m_game_list_frame.data());
        m_game_list_frame->m_zar_splitter->addWidget(m_game_list_frame->m_zar_container);
        m_game_list_frame->m_zar_splitter->setStretchFactor(0, 1);
        m_game_list_frame->m_zar_splitter->setStretchFactor(1, 0);
        m_game_list_frame->m_zar_splitter->setSizes({800, 200});
        listLayout->addWidget(m_game_list_frame->m_zar_splitter);

        m_grid_container = new QWidget(this);
        QHBoxLayout* gridLayout = new QHBoxLayout(m_grid_container);
        gridLayout->setContentsMargins(0, 0, 0, 0);
        gridLayout->setSpacing(0);

        m_game_grid_frame->m_zar_splitter = new QSplitter(Qt::Horizontal, m_grid_container);
        m_game_grid_frame->m_zar_splitter->addWidget(m_game_grid_frame.data());
        m_game_grid_frame->m_zar_splitter->addWidget(m_game_grid_frame->m_zar_container);
        m_game_grid_frame->m_zar_splitter->setStretchFactor(0, 1);
        m_game_grid_frame->m_zar_splitter->setStretchFactor(1, 0);
        m_game_grid_frame->m_zar_splitter->setSizes({800, 200});
        gridLayout->addWidget(m_game_grid_frame->m_zar_splitter);

        // Initialize backgrounds with slider opacity state
        m_game_list_frame->RefreshListBackgroundImage();
        m_game_grid_frame->RefreshGridBackgroundImage();
    }

    int table_mode = Config::getTableMode();
    int slider_pos = isTableList ? Config::getSliderPosition() : Config::getSliderPositionGrid();

    if (table_mode == 0) {
        m_game_grid_frame->hide();
        m_elf_viewer->hide();
        m_grid_container->hide();

        m_game_list_frame->show();
        if (!newDock) {
            m_game_list_frame->clearContents();
            m_game_list_frame->PopulateGameList();
        } else {
            m_game_list_frame->PopulateZarList();
        }
        m_game_list_frame->m_zar_list->setVisible(!m_game_info->m_zar_games.isEmpty());
        m_list_container->show();

        ui->gameRectangleLayout->addWidget(m_list_container);

        ui->sizeSlider->setEnabled(true);
        ui->sizeSlider->setSliderPosition(slider_pos);
        isTableList = true;

    } else if (table_mode == 1) {
        m_game_list_frame->hide();
        m_elf_viewer->hide();
        m_list_container->hide();

        m_game_grid_frame->show();
        if (!newDock) {
            if (m_game_grid_frame->item(0, 0) == nullptr) {
                m_game_grid_frame->clearContents();
                m_game_grid_frame->PopulateGameGrid(m_game_info->m_games, false);
            }
        } else {
            m_game_grid_frame->PopulateZarList();
        }
        m_game_grid_frame->m_zar_list->setVisible(!m_game_info->m_zar_games.isEmpty());
        m_grid_container->show();

        ui->gameRectangleLayout->addWidget(m_grid_container);

        ui->sizeSlider->setEnabled(true);
        ui->sizeSlider->setSliderPosition(slider_pos);
        isTableList = false;

    } else if (table_mode == 2) {
        m_game_list_frame->hide();
        m_game_grid_frame->hide();

        m_elf_viewer->show();
        ui->gameRectangleLayout->addWidget(m_elf_viewer.data());

        ui->sizeSlider->setEnabled(false);
        isTableList = false;
    }

    ui->welcomeAct->setCheckable(true);
    ui->welcomeAct->setChecked(Config::getShowWelcomeDialog());

    ui->bigPictureAct->setCheckable(true);
    ui->bigPictureAct->setChecked(Config::GamesMenuUI());

    ui->pauseOnUnfocusAct->setCheckable(true);
    ui->pauseOnUnfocusAct->setChecked(Config::getPauseOnUnfocus());

    bool showLog = m_compat_info->LoadShowLogSetting();
    ui->logDisplay->setVisible(showLog);
    ui->toggleLogButton->setText(showLog ? tr("Hide Log") : tr("Show Log"));
    ui->installPkgButton->setText(tr("Install PKG"));
    ui->bpBootButton->setText(tr("BP Boot"));
    ui->zarBootButton->setText(tr("Boot ZAR"));
    ui->zarConvertButton->setText(tr("Convert to ZAR"));

    disconnect(ui->toggleLogButton, nullptr, nullptr, nullptr);

    connect(ui->toggleLogButton, &QPushButton::clicked, this, [this]() {
        bool visible = ui->logDisplay->isVisible();
        ui->logDisplay->setVisible(!visible);
        ui->toggleLogButton->setText(visible ? tr("Show Log") : tr("Hide Log"));
        m_compat_info->SaveShowLogSetting(!visible);
    });

    QMenu* themeMenu = new QMenu(this);
    themeMenu->addAction(ui->setThemeDark);
    themeMenu->addAction(ui->setThemeLight);
    themeMenu->addAction(ui->setThemeGreen);
    themeMenu->addAction(ui->setThemeBlue);
    themeMenu->addAction(ui->setThemeViolet);
    themeMenu->addAction(ui->setThemeGruvbox);
    themeMenu->addAction(ui->setThemeTokyoNight);
    themeMenu->addAction(ui->setThemeOled);
    themeMenu->addAction(ui->setThemeNeon);
    themeMenu->addAction(ui->setThemeShadlix);
    themeMenu->addAction(ui->setThemeShadlixCave);
    themeMenu->addAction(ui->setThemeDeepPurple);
    themeMenu->addAction(ui->setThemeQSS);

    connect(ui->themeButton, &QPushButton::clicked, this, [this, themeMenu]() {
        themeMenu->exec(ui->themeButton->mapToGlobal(QPoint(0, ui->themeButton->height())));
    });
}

void MainWindow::LoadGameLists() {
    if (Config::getCompatibilityEnabled())
        m_compat_info->LoadCompatibilityFile();

    if (Config::getCheckCompatibilityOnStartup())
        m_compat_info->UpdateCompatibilityDatabase(this);

    m_game_info->GetGameInfo(this);

    qDebug() << "LoadGameLists: m_zar_games size:" << m_game_info->m_zar_games.size();

    if (isTableList) {
        m_game_list_frame->PopulateGameList();
        m_game_list_frame->m_zar_list->setVisible(!m_game_info->m_zar_games.isEmpty());
        qDebug() << "LoadGameLists: List view, zar list visible:"
                 << m_game_list_frame->m_zar_list->isVisible()
                 << "item count:" << m_game_list_frame->m_zar_list->count();
    } else {
        m_game_grid_frame->PopulateGameGrid(m_game_info->m_games, false);
        m_game_grid_frame->m_zar_list->setVisible(!m_game_info->m_zar_games.isEmpty());
        qDebug() << "LoadGameLists: Grid view, zar list visible:"
                 << m_game_grid_frame->m_zar_list->isVisible()
                 << "item count:" << m_game_grid_frame->m_zar_list->count();
    }
}

#ifdef ENABLE_UPDATER
void MainWindow::CheckUpdateMain(bool checkSave) {
    if (checkSave) {
        if (!Config::autoUpdate()) {
            return;
        }
    }
    auto checkUpdate = new CheckUpdate(false);
    checkUpdate->exec();
}
#endif

void MainWindow::toggleWelcomeScreenOnLaunch(bool enabled) {
    Config::setShowWelcomeDialog(enabled);
    ui->welcomeAct->setChecked(enabled);
    Config::save(Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "config.toml", false);
}

void MainWindow::onSetCustomBackground() {
    QString file =
        QFileDialog::getOpenFileName(this, tr("Select Background Image"), QDir::homePath(),
                                     tr("Images (*.png *.jpg *.jpeg *.bmp)"));

    if (!file.isEmpty()) {
        if (m_game_grid_frame) {
            m_game_grid_frame->SetCustomBackgroundImage(file);
        }
    }
}

void MainWindow::onClearCustomBackground() {
    if (m_game_grid_frame) {
        m_game_grid_frame->SetCustomBackgroundImage("");
    }
}

void MainWindow::CreateConnects() {

    auto applyThemeAndReconstruct = [this]() {
        SetLastIconSizeBullet();
        toggleColorFilter();
        ApplyLastUsedStyle();
        UpdateThemeBackgrounds();
        UpdateIconBackgroundOpacity();
        LoadGameLists();
    };

    connect(this, &MainWindow::WindowResized, this, &MainWindow::HandleResize);
    connect(ui->mw_searchbar, &QLineEdit::textChanged, this, &MainWindow::SearchGameTable);
    connect(ui->exitAct, &QAction::triggered, this, &QWidget::close);
    connect(ui->refreshGameListAct, &QAction::triggered, this, &MainWindow::RefreshGameTable);
    connect(ui->refreshButton, &QPushButton::clicked, this, &MainWindow::RefreshGameTable);
    connect(ui->showGameListAct, &QAction::triggered, this, &MainWindow::ShowGameList);
    connect(ui->toggleLabelsAct, &QAction::triggered, this, &MainWindow::toggleLabelsUnderIcons);
    connect(ui->toggleColorFilterAct, &QAction::triggered, this, &MainWindow::toggleColorFilter);
    connect(ui->fullscreenButton, &QPushButton::clicked, this, &MainWindow::toggleFullscreen);

    connect(ui->sizeSlider, &QSlider::valueChanged, this, [this](int value) {
        if (isTableList) {
            m_game_list_frame->icon_size = 48 + value;
            m_game_list_frame->ResizeIcons(48 + value);
            Config::setIconSize(48 + value);
            Config::setSliderPosition(value);
            m_game_list_frame->m_zar_list->setIconSize(QSize(48 + value, 48 + value));
        } else {
            m_game_grid_frame->icon_size = 69 + value;
            m_game_grid_frame->PopulateGameGrid(m_game_info->m_games, false);
            Config::setIconSizeGrid(69 + value);
            Config::setSliderPositionGrid(value);
            m_game_grid_frame->m_zar_list->setIconSize(QSize(69 + value, 69 + value));
        }
    });

    connect(ui->bootIconsSizeSlider, &QSlider::valueChanged, this, [this](int value) {
        QSize newSize(value, value);
        ui->playButton->setIconSize(newSize);
        ui->pauseButton->setIconSize(newSize);
        ui->stopButton->setIconSize(newSize);
        ui->restartButton->setIconSize(newSize);
        ui->settingsButton->setIconSize(newSize);
        ui->fullscreenButton->setIconSize(newSize);
        ui->controllerButton->setIconSize(newSize);
        ui->keyboardButton->setIconSize(newSize);
        ui->configureHotkeysButton->setIconSize(newSize);
        ui->updaterButton->setIconSize(newSize);
        ui->refreshButton->setIconSize(newSize);
        ui->versionButton->setIconSize(newSize);
        ui->modManagerButton->setIconSize(newSize);
        ui->bigPictureButton->setIconSize(newSize);
        ui->hubMenuButton->setIconSize(newSize);
        ui->cinemaButton->setIconSize(newSize);
    });

    connect(ui->gameContainerHeightSlider, &QSlider::valueChanged, this, [this](int value) {
        ui->gameRectangleContainer->setMinimumHeight(value);
        ui->gameRectangleContainer->setMaximumHeight(value);
    });

    ui->gameRectangleContainer->setMinimumHeight(300);
    ui->gameRectangleContainer->setMaximumHeight(300);

    connect(ui->shadFolderAct, &QAction::triggered, this, [this]() {
        QString userPath;
        Common::FS::PathToQString(userPath, Common::FS::GetUserPath(Common::FS::PathType::UserDir));
        QDesktopServices::openUrl(QUrl::fromLocalFile(userPath));
    });

    connect(ui->playButton, &QPushButton::clicked, this, [this]() { StartGameWithArgs({}); });
    connect(ui->stopButton, &QPushButton::clicked, this, &MainWindow::StopGame);
    connect(ui->pauseButton, &QPushButton::clicked, this, &MainWindow::PauseGame);
    connect(ui->restartButton, &QPushButton::clicked, this, &MainWindow::RestartGame);
    connect(m_game_grid_frame.get(), &QTableWidget::cellDoubleClicked, this,
            &MainWindow::StartGame);
    connect(m_game_list_frame.get(), &QTableWidget::cellDoubleClicked, this,
            &MainWindow::StartGame);

    connect(m_game_list_frame.get()->m_zar_list, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem* item) {
                QString gamePath = item->data(Qt::UserRole).toString();
                std::filesystem::path path = Common::FS::PathFromQString(gamePath);

                if (!std::filesystem::exists(path)) {
                    QMessageBox::critical(nullptr, tr("Run Game"), tr("ZAR file not found"));
                    return;
                }

                QString selectedVersion = QString::fromStdString(Config::getVersionPath());
                if (selectedVersion.isEmpty() || !QFile::exists(selectedVersion)) {
                    selectedVersion = QCoreApplication::applicationFilePath();
                }
                QFileInfo fileInfo(selectedVersion);
                if (!fileInfo.exists()) {
                    QMessageBox::critical(nullptr, "shadPS4",
                                          QString(tr("Could not find the emulator executable")));
                    return;
                }

                QStringList final_args;
                final_args << gamePath;

                Config::setGameRunning(true);
                lastGamePath = path;

                QString workDir = QDir::currentPath();
                m_ipc_client->startGame(fileInfo, final_args, workDir, false);
                m_ipc_client->setActiveController(GamepadSelect::GetSelectedGamepad());

                m_ipc_client->gameClosedFunc = [this]() {
                    QMetaObject::invokeMethod(this, [this]() {
                        Config::setGameRunning(false);
                        UpdateToolbarButtons();
                        setWindowState(Qt::WindowNoState);
                        show();
                    });
                };

                setWindowState(Qt::WindowMinimized);
            });

    connect(m_game_grid_frame.get()->m_zar_list, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem* item) {
                QString gamePath = item->data(Qt::UserRole).toString();
                std::filesystem::path path = Common::FS::PathFromQString(gamePath);

                if (!std::filesystem::exists(path)) {
                    QMessageBox::critical(nullptr, tr("Run Game"), tr("ZAR file not found"));
                    return;
                }

                QString selectedVersion = QString::fromStdString(Config::getVersionPath());
                if (selectedVersion.isEmpty() || !QFile::exists(selectedVersion)) {
                    selectedVersion = QCoreApplication::applicationFilePath();
                }
                QFileInfo fileInfo(selectedVersion);
                if (!fileInfo.exists()) {
                    QMessageBox::critical(nullptr, "shadPS4",
                                          QString(tr("Could not find the emulator executable")));
                    return;
                }

                QStringList final_args;
                final_args << gamePath;

                Config::setGameRunning(true);
                lastGamePath = path;

                QString workDir = QDir::currentPath();
                m_ipc_client->startGame(fileInfo, final_args, workDir, false);
                m_ipc_client->setActiveController(GamepadSelect::GetSelectedGamepad());

                m_ipc_client->gameClosedFunc = [this]() {
                    QMetaObject::invokeMethod(this, [this]() {
                        Config::setGameRunning(false);
                        UpdateToolbarButtons();
                        setWindowState(Qt::WindowNoState);
                        show();
                    });
                };

                setWindowState(Qt::WindowMinimized);
            });

    connect(m_game_grid_frame.get(), &QTableWidget::currentCellChanged, this,
            [this](int currentRow, int currentColumn, int previousRow, int previousColumn) {
                if (Config::getShowBackgroundImage()) {
                    if (currentRow >= 0 && currentColumn >= 0) {
                        int columnCnt = m_game_grid_frame->columnCount();
                        if (columnCnt > 0) {
                            int itemID = (currentRow * columnCnt) + currentColumn;
                            if (itemID >= 0 && itemID < m_game_info->m_games.size()) {
                                QString gamePic = QString::fromStdString(
                                    m_game_info->m_games[itemID].pic_path.string());
                                if (!gamePic.isEmpty()) {
                                    QPixmap pixmap(gamePic);
                                    if (!pixmap.isNull()) {
                                        QPixmap scaledPixmap =
                                            pixmap.scaled(ui->backgroundImageLabel->size(),
                                                          Qt::KeepAspectRatioByExpanding,
                                                          Qt::SmoothTransformation);
                                        ui->backgroundImageLabel->setPixmap(scaledPixmap);
                                    }
                                }
                            }
                        }
                    } else {
                        QString defaultBackgroundPath = ":/images/default_background.jpg";
                        QPixmap defaultPixmap(defaultBackgroundPath);
                        if (!defaultPixmap.isNull()) {
                            QPixmap scaledPixmap = defaultPixmap.scaled(
                                ui->backgroundImageLabel->size(), Qt::KeepAspectRatioByExpanding,
                                Qt::SmoothTransformation);
                            ui->backgroundImageLabel->setPixmap(scaledPixmap);
                        }
                    }
                }
            });

    connect(m_game_list_frame.get(), &QTableWidget::currentCellChanged, this,
            [this](int currentRow, int currentColumn, int previousRow, int previousColumn) {
                if (Config::getShowBackgroundImage()) {
                    if (currentRow >= 0 && currentRow < m_game_info->m_games.size()) {
                        QString gamePic = QString::fromStdString(
                            m_game_info->m_games[currentRow].pic_path.string());
                        if (!gamePic.isEmpty()) {
                            QPixmap pixmap(gamePic);
                            if (!pixmap.isNull()) {
                                QPixmap scaledPixmap = pixmap.scaled(
                                    ui->backgroundImageLabel->size(),
                                    Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
                                ui->backgroundImageLabel->setPixmap(scaledPixmap);
                            }
                        }
                    } else {
                        QString defaultBackgroundPath = ":/images/default_background.jpg";
                        QPixmap defaultPixmap(defaultBackgroundPath);
                        if (!defaultPixmap.isNull()) {
                            QPixmap scaledPixmap = defaultPixmap.scaled(
                                ui->backgroundImageLabel->size(), Qt::KeepAspectRatioByExpanding,
                                Qt::SmoothTransformation);
                            ui->backgroundImageLabel->setPixmap(scaledPixmap);
                        }
                    }
                }
            });

    connect(ui->configureAct, &QAction::triggered, this, [this]() {
        auto settingsDialog =
            new SettingsDialog(m_compat_info, m_ipc_client, this, Config::getGameRunning());

        connect(settingsDialog, &SettingsDialog::LanguageChanged, this,
                &MainWindow::OnLanguageChanged);

        connect(settingsDialog, &SettingsDialog::CompatibilityChanged, this,
                &MainWindow::RefreshGameTable);

        connect(settingsDialog, &SettingsDialog::accepted, this, &MainWindow::RefreshGameTable);

        connect(settingsDialog, &SettingsDialog::BackgroundOpacityChanged, this,
                [this](int opacity) {
                    Config::setBackgroundImageOpacity(opacity);
                    if (m_game_list_frame) {
                        QTableWidgetItem* current = m_game_list_frame->GetCurrentItem();
                        if (current) {
                            m_game_list_frame->SetListBackgroundImage(current);
                        }
                    }
                    if (m_game_grid_frame) {
                        if (m_game_grid_frame->IsValidCellSelected()) {
                            m_game_grid_frame->SetGridBackgroundImage(m_game_grid_frame->crtRow,
                                                                      m_game_grid_frame->crtColumn);
                        }
                    }
                });

        settingsDialog->exec();
    });

    connect(ui->styleSelector, &QComboBox::currentTextChanged, [this](const QString& styleName) {
        if (m_isRepopulatingStyleSelector) {
            return;
        }

        int idx = ui->styleSelector->currentIndex();
        QVariant data = ui->styleSelector->itemData(idx);

        Theme lastKnownTheme = static_cast<Theme>(Config::getMainWindowTheme());
        Theme themeToReload = lastKnownTheme;

        if (styleName.endsWith("(QSS)") && data.isValid()) {

            QString qssFilePath = data.toString();
            QFileInfo qssFileInfo(qssFilePath);
            QString qssBaseName = qssFileInfo.baseName();

            bool isCyberpunk = qssBaseName.contains("Cyberpunk", Qt::CaseInsensitive);

            if (isCyberpunk) {
                m_window_themes.SetWindowTheme(Theme::QSS, ui->mw_searchbar, data.toString());
                Config::setMainWindowTheme(static_cast<int>(Theme::QSS));
                ui->setThemeQSS->setChecked(true);
            } else if (lastKnownTheme == Theme::QSS) {

                if (ui->setThemeDark->isChecked())
                    themeToReload = Theme::Dark;
                else if (ui->setThemeLight->isChecked())
                    themeToReload = Theme::Light;
                else if (ui->setThemeGreen->isChecked())
                    themeToReload = Theme::Green;
                else if (ui->setThemeBlue->isChecked())
                    themeToReload = Theme::Blue;
                else if (ui->setThemeViolet->isChecked())
                    themeToReload = Theme::Violet;
                else if (ui->setThemeGruvbox->isChecked())
                    themeToReload = Theme::Gruvbox;
                else if (ui->setThemeTokyoNight->isChecked())
                    themeToReload = Theme::TokyoNight;
                else if (ui->setThemeOled->isChecked())
                    themeToReload = Theme::Oled;
                else if (ui->setThemeNeon->isChecked())
                    themeToReload = Theme::Neon;
                else if (ui->setThemeShadlix->isChecked())
                    themeToReload = Theme::Shadlix;
                else if (ui->setThemeShadlixCave->isChecked())
                    themeToReload = Theme::ShadlixCave;
                else if (ui->setThemeDeepPurple->isChecked())
                    themeToReload = Theme::DeepPurple;

                if (themeToReload == Theme::QSS) {
                    themeToReload = Theme::Dark;
                }

                m_window_themes.SetWindowTheme(themeToReload, ui->mw_searchbar);
                Config::setMainWindowTheme(static_cast<int>(themeToReload));
                ui->setThemeQSS->setChecked(false);
            }
            QFile file(data.toString());
            if (file.open(QFile::ReadOnly)) {
                qApp->setStyleSheet(file.readAll());
                file.close();
            }
            Config::setGuiStyle(data.toString().toStdString());
        } else {

            QApplication::setStyle(QStyleFactory::create(styleName));

            Config::setGuiStyle(styleName.toStdString());
            qApp->setStyleSheet("");
        }
        if (Config::getEnableColorFilter()) {
            SetUiIcons(m_window_themes.iconBaseColor(), m_window_themes.iconHoverColor());
            if (m_game_list_frame) {
                m_game_list_frame->SetThemeColors(m_window_themes.textColor());
            }
        } else {
            QColor baseColor = (Config::getMainWindowTheme() == static_cast<int>(Theme::Light))
                                   ? Qt::black
                                   : Qt::white;
            SetUiIcons(baseColor, baseColor);
            if (m_game_list_frame) {
                m_game_list_frame->SetThemeColors(baseColor);
            }
        }
        m_game_list_frame->SetThemeColors(m_window_themes.textColor());

        const auto config_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        Config::saveMainWindow(config_dir / "config.toml");

        if (isTableList) {
            if (m_game_list_frame)
                m_game_list_frame->RefreshListBackgroundImage();
        } else {
            if (m_game_grid_frame)
                m_game_grid_frame->RefreshGridBackgroundImage();
        }
    });

    connect(ui->setCustomBackgroundAct, &QAction::triggered, this, [this]() {
        QString filePath =
            QFileDialog::getOpenFileName(this, tr("Select Background Image"), QDir::homePath(),
                                         tr("Images (*.png *.jpg *.jpeg *.bmp)"));

        if (!filePath.isEmpty()) {
            if (m_game_grid_frame) {
                m_game_grid_frame->SetCustomBackgroundImage(filePath);
            }
        }
        m_game_list_frame->ApplyCustomBackground();
        const auto config_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);

        Config::save(config_dir / "config.toml", false);
    });

    connect(ui->clearCustomBackgroundAct, &QAction::triggered, this, [this]() {
        if (m_game_grid_frame) {
            m_game_grid_frame->SetCustomBackgroundImage("");
        }
        const auto config_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);

        Config::save(config_dir / "config.toml", false);
    });

    connect(ui->installPkgButton, &QPushButton::clicked, this, [this]() {
        auto versionDialog = new VersionDialog(m_compat_info, this);
        versionDialog->InstallPkgWithV7();
    });

    connect(ui->launcherBox, &QCheckBox::clicked, this, [this](bool checked) {
        Config::setBootLauncher(checked);
        const auto config_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        Config::save(config_dir / "config.toml", false);
    });

    connect(ui->settingsButton, &QPushButton::clicked, this, [this]() {
        auto settingsDialog =
            new SettingsDialog(m_compat_info, m_ipc_client, this, Config::getGameRunning());

        connect(settingsDialog, &SettingsDialog::LanguageChanged, this,
                &MainWindow::OnLanguageChanged);

        connect(settingsDialog, &SettingsDialog::CompatibilityChanged, this,
                &MainWindow::RefreshGameTable);

        connect(settingsDialog, &SettingsDialog::accepted, this, &MainWindow::RefreshGameTable);
        connect(settingsDialog, &SettingsDialog::rejected, this, &MainWindow::RefreshGameTable);
        connect(settingsDialog, &SettingsDialog::close, this, &MainWindow::RefreshGameTable);

        connect(settingsDialog, &SettingsDialog::BackgroundOpacityChanged, this,
                [this](int opacity) {
                    Config::setBackgroundImageOpacity(opacity);
                    if (m_game_list_frame) {
                        QTableWidgetItem* current = m_game_list_frame->GetCurrentItem();
                        if (current) {
                            m_game_list_frame->SetListBackgroundImage(current);
                        }
                    }
                    if (m_game_grid_frame) {
                        if (m_game_grid_frame->IsValidCellSelected()) {
                            m_game_grid_frame->SetGridBackgroundImage(m_game_grid_frame->crtRow,
                                                                      m_game_grid_frame->crtColumn);
                        }
                    }
                });

        settingsDialog->exec();
    });

    connect(ui->controllerButton, &QPushButton::clicked, this, [this]() {
        ControlSettings* remapWindow = new ControlSettings(
            m_game_info, m_ipc_client, Config::getGameRunning(), runningGameSerial, this);
        remapWindow->exec();
    });

    connect(ui->keyboardButton, &QPushButton::clicked, this, [this]() {
        auto kbmWindow = new KBMSettings(m_game_info, m_ipc_client, Config::getGameRunning(),
                                         runningGameSerial, this);
        kbmWindow->exec();
    });

#ifdef ENABLE_UPDATER
    connect(ui->updaterAct, &QAction::triggered, this, [this]() {
        auto checkUpdate = new CheckUpdate(true);
        checkUpdate->exec();
    });

    connect(ui->updaterButton, &QPushButton::clicked, this, [this]() {
        auto checkUpdate = new CheckUpdate(true);
        checkUpdate->exec();
    });
#endif

    connect(ui->aboutAct, &QAction::triggered, this, [this]() {
        auto aboutDialog = new AboutDialog(this);
        aboutDialog->exec();
    });

    connect(ui->versionAct, &QAction::triggered, this, [this]() {
        auto versionDialog = new VersionDialog(m_compat_info, this);
        versionDialog->show();
    });
    connect(ui->welcomeAct, &QAction::triggered, this, [this](bool checked) {
        Config::setShowWelcomeDialog(checked);
        Config::save(Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "config.toml", false);
    });

    connect(ui->bigPictureAct, &QAction::triggered, this, [this](bool checked) {
        bool currentBigPicture = Config::GamesMenuUI();
        bool currentHubMenu = Config::HubMenuUI();

        // If already enabled, disable it
        if (currentBigPicture) {
            Config::setGamesMenuUI(false);
            ui->bigPictureAct->setChecked(false);
        } else {
            // If enabling, check if the other is already enabled
            if (currentHubMenu) {
                QMessageBox::StandardButton reply =
                    QMessageBox::question(this, tr("Boot Settings"),
                                          tr("GameHub is already enabled. Do you want to disable "
                                             "GameHub and enable BigPicture instead?"),
                                          QMessageBox::Yes | QMessageBox::No);

                if (reply == QMessageBox::Yes) {
                    Config::setHubMenuUI(false);
                    ui->hubMenuAct->setChecked(false);
                    Config::setGamesMenuUI(true);
                    ui->bigPictureAct->setChecked(true);
                } else {
                    ui->bigPictureAct->setChecked(false);
                    return;
                }
            } else {
                Config::setGamesMenuUI(true);
                ui->bigPictureAct->setChecked(true);
            }
        }
        Config::save(Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "config.toml", false);
    });

    connect(ui->hubMenuAct, &QAction::triggered, this, [this](bool checked) {
        bool currentBigPicture = Config::GamesMenuUI();
        bool currentHubMenu = Config::HubMenuUI();

        if (currentHubMenu) {
            Config::setHubMenuUI(false);
            ui->hubMenuAct->setChecked(false);
        } else {
            if (currentBigPicture) {
                QMessageBox::StandardButton reply =
                    QMessageBox::question(this, tr("Boot Settings"),
                                          tr("BigPicture is already enabled. Do you want to "
                                             "disable BigPicture and enable GameHub instead?"),
                                          QMessageBox::Yes | QMessageBox::No);

                if (reply == QMessageBox::Yes) {
                    Config::setGamesMenuUI(false);
                    ui->bigPictureAct->setChecked(false);
                    Config::setHubMenuUI(true);
                    ui->hubMenuAct->setChecked(true);
                } else {
                    ui->hubMenuAct->setChecked(false);
                    return;
                }
            } else {
                Config::setHubMenuUI(true);
                ui->hubMenuAct->setChecked(true);
            }
        }
        Config::save(Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "config.toml", false);
    });

    connect(ui->pauseOnUnfocusAct, &QAction::triggered, this, [this](bool checked) {
        Config::setPauseOnUnfocus(checked);
        const auto config_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        Config::saveMainWindow(config_dir / "config.toml");
    });

    connect(ui->versionButton, &QPushButton::clicked, this, [this]() {
        auto versionDialog = new VersionDialog(m_compat_info, this);
        versionDialog->show();
    });
    connect(ui->bigPictureButton, &QPushButton::clicked, this,
            [this]() { m_bigPicture->toggle(); });
    connect(ui->hubMenuButton, &QPushButton::clicked, this, [this]() { m_hubMenu->toggle(); });
    connect(ui->cinemaButton, &QPushButton::clicked, this,
            [this]() { m_game_cinematic_frame->toggle(); });
    connect(ui->bpBootButton, &QPushButton::clicked, this, [this]() {
        Config::setGameRunning(true);
        Config::setIsFullscreen(true);

        QApplication::quit();

        QString emulatorPath = QCoreApplication::applicationFilePath();
        QString emulatorDir = QFileInfo(emulatorPath).absolutePath();

#ifdef Q_OS_WIN
        QString scriptFileName = QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
                                 "/relaunch_bigpicture.ps1";

        QString scriptContent =
            QStringLiteral(
                "Start-Sleep -Seconds 2\n"
                "Start-Process -FilePath \"%1\" -WorkingDirectory \"%2\" -ArgumentList \"-b\"\n")
                .arg(emulatorPath, emulatorDir);

        QFile scriptFile(scriptFileName);
        if (scriptFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&scriptFile);
            scriptFile.write("\xEF\xBB\xBF");
            out << scriptContent;
            scriptFile.close();

            bool started = QProcess::startDetached("powershell.exe",
                                                   QStringList() << "-ExecutionPolicy"
                                                                 << "Bypass"
                                                                 << "-File" << scriptFileName);
            if (!started) {
                qWarning() << "Failed to start big picture relaunch PowerShell script";
            }
        } else {
            qWarning() << "Failed to write big picture relaunch PowerShell script";
        }
#elif defined(Q_OS_LINUX) || defined(Q_OS_MAC)
        QString scriptFileName = "/tmp/relaunch_bigpicture.sh";

        QString scriptContent = QStringLiteral("#!/bin/bash\n"
                                               "sleep 2\n"
                                               "exec \"%1\" -b \"$@\" &\n")
                                    .arg(emulatorPath);

        QFile scriptFile(scriptFileName);
        if (scriptFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&scriptFile);
            out << scriptContent;
            scriptFile.close();

            QFile::setPermissions(scriptFileName,
                                 QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

            bool started = QProcess::startDetached(scriptFileName);
            if (!started) {
                qWarning() << "Failed to start big picture relaunch shell script";
            }
        } else {
            qWarning() << "Failed to write big picture relaunch shell script";
        }
#endif
    });
    connect(ui->zarBootButton, &QPushButton::clicked, this, [this]() { BootZarGame(); });

    connect(ui->zarConvertButton, &QPushButton::clicked, this, [this]() { ConvertToZar(); });

    connect(ui->modManagerButton, &QPushButton::clicked, this, [this]() {
        if (m_game_info->m_games.empty()) {
            QMessageBox::warning(this, tr("Mod Manager"), tr("No game selected."));
            return;
        }
        int selectedIndex = -1;
        if (isTableList) {
            QTableWidgetItem* current = m_game_list_frame->GetCurrentItem();
            if (!current) {
                QMessageBox::warning(this, tr("Mod Manager"), tr("No game selected."));
                return;
            }
            selectedIndex = current->row();
        } else {
            if (!m_game_grid_frame->IsValidCellSelected()) {
                QMessageBox::warning(this, tr("Mod Manager"), tr("No game selected."));
                return;
            }
            selectedIndex = m_game_grid_frame->crtRow;
        }
        if (selectedIndex < 0 || selectedIndex >= m_game_info->m_games.size()) {
            QMessageBox::warning(this, tr("Mod Manager"), tr("Invalid game index."));
            return;
        }
        const GameInfo& game = m_game_info->m_games[selectedIndex];
        QString gamePathQString;
        Common::FS::PathToQString(gamePathQString, game.path);
        auto dlg = new ModManagerDialog(gamePathQString, QString::fromStdString(game.serial), this);
        dlg->show();
    });

    connect(ui->configureHotkeys, &QAction::triggered, this, [this]() {
        auto hotkeyDialog = new Hotkeys(m_ipc_client, Config::getGameRunning(), this);
        hotkeyDialog->exec();
    });

    connect(ui->configureHotkeysButton, &QPushButton::clicked, this, [this]() {
        auto hotkeyDialog = new Hotkeys(m_ipc_client, Config::getGameRunning(), this);
        hotkeyDialog->exec();
    });

    connect(ui->setIconSizeTinyAct, &QAction::triggered, this, [this]() {
        if (isTableList) {
            m_game_list_frame->icon_size = 36;
            ui->sizeSlider->setValue(0); // icone_size - 36
            Config::setIconSize(36);
            Config::setSliderPosition(0);
            m_game_list_frame->m_zar_list->setIconSize(QSize(36, 36));
        } else {
            m_game_grid_frame->icon_size = 69;
            ui->sizeSlider->setValue(0); // icone_size - 36
            Config::setIconSizeGrid(69);
            Config::setSliderPositionGrid(0);
            m_game_grid_frame->PopulateGameGrid(m_game_info->m_games, false);
            m_game_grid_frame->m_zar_list->setIconSize(QSize(69, 69));
        }
    });

    // handle resize like this for now, we deal with it when we add more docks
    connect(this, &MainWindow::WindowResized, this, [&]() {});

    connect(ui->setIconSizeSmallAct, &QAction::triggered, this, [this]() {
        if (isTableList) {
            m_game_list_frame->icon_size = 64;
            ui->sizeSlider->setValue(28);
            Config::setIconSize(64);
            Config::setSliderPosition(28);
            m_game_list_frame->m_zar_list->setIconSize(QSize(64, 64));
        } else {
            m_game_grid_frame->icon_size = 97;
            ui->sizeSlider->setValue(28);
            Config::setIconSizeGrid(97);
            Config::setSliderPositionGrid(28);
            m_game_grid_frame->PopulateGameGrid(m_game_info->m_games, false);
            m_game_grid_frame->m_zar_list->setIconSize(QSize(97, 97));
        }
    });

    connect(ui->setIconSizeMediumAct, &QAction::triggered, this, [this]() {
        if (isTableList) {
            m_game_list_frame->icon_size = 128;
            ui->sizeSlider->setValue(92);
            Config::setIconSize(128);
            Config::setSliderPosition(92);
            m_game_list_frame->m_zar_list->setIconSize(QSize(128, 128));
        } else {
            m_game_grid_frame->icon_size = 161;
            ui->sizeSlider->setValue(92);
            Config::setIconSizeGrid(161);
            Config::setSliderPositionGrid(92);
            m_game_grid_frame->PopulateGameGrid(m_game_info->m_games, false);
            m_game_grid_frame->m_zar_list->setIconSize(QSize(161, 161));
        }
    });

    connect(ui->setIconSizeLargeAct, &QAction::triggered, this, [this]() {
        if (isTableList) {
            m_game_list_frame->icon_size = 256;
            ui->sizeSlider->setValue(220);
            Config::setIconSize(256);
            Config::setSliderPosition(220);
            m_game_list_frame->m_zar_list->setIconSize(QSize(256, 256));
        } else {
            m_game_grid_frame->icon_size = 256;
            ui->sizeSlider->setValue(220);
            Config::setIconSizeGrid(256);
            Config::setSliderPositionGrid(220);
            m_game_grid_frame->PopulateGameGrid(m_game_info->m_games, false);
            m_game_grid_frame->m_zar_list->setIconSize(QSize(256, 256));
        }
    });
    connect(ui->setlistModeListAct, &QAction::triggered, this, [this]() {
        ui->sizeSlider->setEnabled(true);
        BackgroundMusicPlayer::getInstance().stopMusic();

        Config::setTableMode(0);
        CreateDockWindows(false);
        ui->mw_searchbar->setText("");
        SetLastIconSizeBullet();
        Config::save(Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "config.toml", false);
    });

    connect(ui->setlistModeGridAct, &QAction::triggered, this, [this]() {
        ui->sizeSlider->setEnabled(true);
        BackgroundMusicPlayer::getInstance().stopMusic();

        Config::setTableMode(1);
        CreateDockWindows(false);
        ui->mw_searchbar->setText("");
        SetLastIconSizeBullet();
        Config::save(Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "config.toml", false);
    });

    connect(ui->setlistElfAct, &QAction::triggered, this, [this]() {
        ui->sizeSlider->setEnabled(false);
        BackgroundMusicPlayer::getInstance().stopMusic();

        Config::setTableMode(2);
        CreateDockWindows(false);
        SetLastIconSizeBullet();
        Config::save(Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "config.toml", false);
    });
    connect(ui->setlistModeCinematicAct, &QAction::triggered, this,
            [this]() { m_game_cinematic_frame->toggle(); });

    connect(ui->downloadCheatsPatchesAct, &QAction::triggered, this, [this]() {
        QDialog* panelDialog = new QDialog(this);
        QVBoxLayout* layout = new QVBoxLayout(panelDialog);
        QPushButton* downloadAllCheatsButton =
            new QPushButton(tr("Download Cheats For All Installed Games"), panelDialog);
        QPushButton* downloadAllPatchesButton =
            new QPushButton(tr("Download Patches For All Games"), panelDialog);

        layout->addWidget(downloadAllCheatsButton);
        layout->addWidget(downloadAllPatchesButton);

        panelDialog->setLayout(layout);

        connect(downloadAllCheatsButton, &QPushButton::clicked, this, [this, panelDialog]() {
            QEventLoop eventLoop;
            int pendingDownloads = 0;

            auto onDownloadFinished = [&]() {
                if (--pendingDownloads <= 0) {
                    eventLoop.quit();
                }
            };

            for (const GameInfo& game : m_game_info->m_games) {
                QString gameName = QString::fromStdString(game.name);
                QString gameSerial = QString::fromStdString(game.serial);
                QString gameVersion = QString::fromStdString(game.version);
                QString gameSize = QString::fromStdString(game.size);
                QPixmap gameImage; // empty pixmap

                std::shared_ptr<CheatsPatches> cheatsPatches = std::make_shared<CheatsPatches>(
                    gameName, gameSerial, m_ipc_client, gameVersion, gameSize, gameImage);

                // connect the signal properly
                connect(cheatsPatches.get(), &CheatsPatches::downloadFinished, &eventLoop,
                        [onDownloadFinished]() { onDownloadFinished(); });

                pendingDownloads += 3;

                cheatsPatches->downloadCheats("wolf2022", gameSerial, gameVersion, false);
                cheatsPatches->downloadCheats("GoldHEN", gameSerial, gameVersion, false);
                cheatsPatches->downloadCheats("shadPS4", gameSerial, gameVersion, false);
            }

            if (pendingDownloads > 0)
                eventLoop.exec();

            QMessageBox::information(
                nullptr, tr("Download Complete"),
                tr("You have downloaded cheats for all the games you have installed."));
            panelDialog->accept();
        });

        connect(downloadAllPatchesButton, &QPushButton::clicked, [this, panelDialog]() {
            QEventLoop eventLoop;
            int pendingDownloads = 0;

            auto onDownloadFinished = [&]() {
                if (--pendingDownloads <= 0) {
                    eventLoop.quit();
                }
            };

            QString empty = "";
            QPixmap emptyImage;
            auto cheatsPatches = std::make_shared<CheatsPatches>(empty, empty, m_ipc_client, empty,
                                                                 empty, emptyImage);

            connect(cheatsPatches.get(), &CheatsPatches::downloadFinished, &eventLoop,
                    [onDownloadFinished]() { onDownloadFinished(); });

            pendingDownloads += 2;

            cheatsPatches->downloadPatches("GoldHEN", false);
            cheatsPatches->downloadPatches("shadPS4", false);

            if (pendingDownloads > 0)
                eventLoop.exec();

            QMessageBox::information(nullptr, tr("Download Complete"),
                                     tr("Patches Downloaded Successfully!\nAll patches available "
                                        "for all games have been downloaded."));

            cheatsPatches->createFilesJson("GoldHEN");
            cheatsPatches->createFilesJson("shadPS4");
            panelDialog->accept();
        });
        panelDialog->exec();
    });

    connect(ui->dumpGameListAct, &QAction::triggered, this, [&] {
        QString filePath = qApp->applicationDirPath().append("/GameList.txt");
        QFile file(filePath);
        QTextStream out(&file);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            qDebug() << "Failed to open file for writing:" << file.errorString();
            return;
        }
        out << QString("%1 %2 %3 %4 %5\n")
                   .arg("          NAME", -50)
                   .arg("    ID", -10)
                   .arg("FW", -4)
                   .arg(" APP VERSION", -11)
                   .arg("                Path");
        for (const GameInfo& game : m_game_info->m_games) {
            QString game_path;
            Common::FS::PathToQString(game_path, game.path);
            out << QString("%1 %2 %3     %4 %5\n")
                       .arg(QString::fromStdString(game.name), -50)
                       .arg(QString::fromStdString(game.serial), -10)
                       .arg(QString::fromStdString(game.fw), -4)
                       .arg(QString::fromStdString(game.version), -11)
                       .arg(game_path);
        }
    });

    connect(ui->bootGameAct, &QAction::triggered, this, &MainWindow::BootGame);
    connect(ui->gameInstallPathAct, &QAction::triggered, this, &MainWindow::Directories);

    connect(ui->addElfFolderAct, &QAction::triggered, m_elf_viewer.data(),
            &ElfViewer::OpenElfFolder);

    connect(ui->trophyViewerAct, &QAction::triggered, this, [this]() {
        if (m_game_info->m_games.empty()) {
            QMessageBox::information(
                this, tr("Trophy Viewer"),
                tr("No games found. Please add your games to your library first."));
            return;
        }

        const auto& firstGame = m_game_info->m_games[0];
        QString trophyPath, gameTrpPath;
        Common::FS::PathToQString(trophyPath, firstGame.serial);
        Common::FS::PathToQString(gameTrpPath, firstGame.path);

        auto game_update_path = Common::FS::PathFromQString(gameTrpPath);
        game_update_path += "-UPDATE";
        if (std::filesystem::exists(game_update_path)) {
            Common::FS::PathToQString(gameTrpPath, game_update_path);
        } else {
            game_update_path = Common::FS::PathFromQString(gameTrpPath);
            game_update_path += "-patch";
            if (std::filesystem::exists(game_update_path)) {
                Common::FS::PathToQString(gameTrpPath, game_update_path);
            }
        }

        QVector<TrophyGameInfo> allTrophyGames;
        for (const auto& game : m_game_info->m_games) {
            TrophyGameInfo gameInfo;
            gameInfo.name = QString::fromStdString(game.name);
            Common::FS::PathToQString(gameInfo.trophyPath, game.serial);
            Common::FS::PathToQString(gameInfo.gameTrpPath, game.path);

            auto update_path = Common::FS::PathFromQString(gameInfo.gameTrpPath);
            update_path += "-UPDATE";
            if (std::filesystem::exists(update_path)) {
                Common::FS::PathToQString(gameInfo.gameTrpPath, update_path);
            } else {
                update_path = Common::FS::PathFromQString(gameInfo.gameTrpPath);
                update_path += "-patch";
                if (std::filesystem::exists(update_path)) {
                    Common::FS::PathToQString(gameInfo.gameTrpPath, update_path);
                }
            }

            allTrophyGames.append(gameInfo);
        }

        QString gameName = QString::fromStdString(firstGame.name);
        TrophyViewer* trophyViewer =
            new TrophyViewer(trophyPath, gameTrpPath, gameName, allTrophyGames);
        trophyViewer->show();
    });

    connect(ui->setThemeDark, &QAction::triggered, this, [this, applyThemeAndReconstruct]() {
        m_window_themes.SetWindowTheme(Theme::Dark, ui->mw_searchbar);
        Config::setMainWindowTheme(static_cast<int>(Theme::Dark));
        applyThemeAndReconstruct();
    });

    connect(ui->setThemeLight, &QAction::triggered, this, [this, applyThemeAndReconstruct]() {
        m_window_themes.SetWindowTheme(Theme::Light, ui->mw_searchbar);
        Config::setMainWindowTheme(static_cast<int>(Theme::Light));
        applyThemeAndReconstruct();
    });

    connect(ui->setThemeGreen, &QAction::triggered, this, [this, applyThemeAndReconstruct]() {
        m_window_themes.SetWindowTheme(Theme::Green, ui->mw_searchbar);
        Config::setMainWindowTheme(static_cast<int>(Theme::Green));
        applyThemeAndReconstruct();
    });

    connect(ui->setThemeBlue, &QAction::triggered, this, [this, applyThemeAndReconstruct]() {
        m_window_themes.SetWindowTheme(Theme::Blue, ui->mw_searchbar);
        Config::setMainWindowTheme(static_cast<int>(Theme::Blue));
        applyThemeAndReconstruct();
    });

    connect(ui->setThemeViolet, &QAction::triggered, this, [this, applyThemeAndReconstruct]() {
        m_window_themes.SetWindowTheme(Theme::Violet, ui->mw_searchbar);
        Config::setMainWindowTheme(static_cast<int>(Theme::Violet));
        applyThemeAndReconstruct();
    });

    connect(ui->setThemeGruvbox, &QAction::triggered, this, [this, applyThemeAndReconstruct]() {
        m_window_themes.SetWindowTheme(Theme::Gruvbox, ui->mw_searchbar);
        Config::setMainWindowTheme(static_cast<int>(Theme::Gruvbox));
        applyThemeAndReconstruct();
    });

    connect(ui->setThemeTokyoNight, &QAction::triggered, this, [this, applyThemeAndReconstruct]() {
        m_window_themes.SetWindowTheme(Theme::TokyoNight, ui->mw_searchbar);
        Config::setMainWindowTheme(static_cast<int>(Theme::TokyoNight));
        applyThemeAndReconstruct();
    });

    connect(ui->setThemeOled, &QAction::triggered, this, [this, applyThemeAndReconstruct]() {
        m_window_themes.SetWindowTheme(Theme::Oled, ui->mw_searchbar);
        Config::setMainWindowTheme(static_cast<int>(Theme::Oled));
        applyThemeAndReconstruct();
    });

    connect(ui->setThemeNeon, &QAction::triggered, this, [this, applyThemeAndReconstruct]() {
        m_window_themes.SetWindowTheme(Theme::Neon, ui->mw_searchbar);
        Config::setMainWindowTheme(static_cast<int>(Theme::Neon));
        applyThemeAndReconstruct();
    });

    connect(ui->setThemeShadlix, &QAction::triggered, this, [this, applyThemeAndReconstruct]() {
        m_window_themes.SetWindowTheme(Theme::Shadlix, ui->mw_searchbar);
        Config::setMainWindowTheme(static_cast<int>(Theme::Shadlix));
        applyThemeAndReconstruct();
    });

    connect(ui->setThemeShadlixCave, &QAction::triggered, this, [this, applyThemeAndReconstruct]() {
        m_window_themes.SetWindowTheme(Theme::ShadlixCave, ui->mw_searchbar);
        Config::setMainWindowTheme(static_cast<int>(Theme::ShadlixCave));
        applyThemeAndReconstruct();
    });

    connect(ui->setThemeDeepPurple, &QAction::triggered, this, [this, applyThemeAndReconstruct]() {
        m_window_themes.SetWindowTheme(Theme::DeepPurple, ui->mw_searchbar);
        Config::setMainWindowTheme(static_cast<int>(Theme::DeepPurple));
        applyThemeAndReconstruct();
    });

    connect(ui->setThemeQSS, &QAction::triggered, this, [this, applyThemeAndReconstruct]() {
        m_window_themes.SetWindowTheme(Theme::QSS, ui->mw_searchbar);
        Config::setMainWindowTheme(static_cast<int>(Theme::QSS));
        applyThemeAndReconstruct();
    });

    QObject::connect(m_ipc_client.get(), &IpcClient::LogEntrySent, this, &MainWindow::PrintLog);
}
void MainWindow::PrintLog(QString entry, QColor textColor) {
    ui->logDisplay->setTextColor(textColor);
    ui->logDisplay->append(entry);
    QScrollBar* sb = ui->logDisplay->verticalScrollBar();
    sb->setValue(sb->maximum());
}

void MainWindow::ToggleMute() {
    bool newMute = !Config::isMuteEnabled();
    Config::setMuteEnabled(newMute);

    if (Config::getGameRunning()) {
        if (m_ipc_client) {
            m_ipc_client->adjustVol(Config::getVolumeSlider());
        } else if (auto ipc = IpcClient::GetInstance()) {
            ipc->adjustVol(Config::getVolumeSlider());
        } else {
            Libraries::AudioOut::AdjustVol();
        }
    } else {
        Libraries::AudioOut::AdjustVol();
    }
}
void MainWindow::autoCheckLauncherBox() {
    QString emulatorExePath = QString::fromStdString(Config::getVersionPath());
    bool isPathValid = QFile::exists(emulatorExePath);

    if (isPathValid && Config::getBootLauncher()) {
        ui->launcherBox->setChecked(true);
    } else if (isPathValid && !Config::getBootLauncher()) {
        ui->launcherBox->setChecked(false);
    } else {
        ui->launcherBox->setChecked(false);
        Config::setBootLauncher(false);
        const auto config_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        Config::save(config_dir / "config.toml", false);
    }
}

void MainWindow::StartGame() {
    StartGameWithArgs({});
}

void MainWindow::StartGameWithArgs(QStringList args, int forcedIndex) {
    QString gamePath;
    std::filesystem::path file;

    if (forcedIndex != -1) {
        file = m_game_info->m_games[forcedIndex].path;
        runningGameSerial = m_game_info->m_games[forcedIndex].serial;
    } else {
        int table_mode = Config::getTableMode();

        if (table_mode == 0 && m_game_list_frame->currentItem()) {
            int id = m_game_list_frame->currentItem()->row();
            file = m_game_info->m_games[id].path;
            runningGameSerial = m_game_info->m_games[id].serial;
        } else if (table_mode == 1 && m_game_grid_frame->cellClicked) {
            int id = m_game_grid_frame->crtRow * m_game_grid_frame->columnCnt +
                     m_game_grid_frame->crtColumn;
            file = m_game_info->m_games[id].path;
            runningGameSerial = m_game_info->m_games[id].serial;
        } else if (m_elf_viewer->currentItem()) {
            int id = m_elf_viewer->currentItem()->row();
            gamePath = m_elf_viewer->m_elf_list[id];
        }
    }

    if (file.empty() && gamePath.isEmpty())
        return;

    bool ignorePatches = false;

    if (!file.empty()) {
        auto game_folder_name = file.filename().string();
        auto base_folder = file;
        auto parent = base_folder.parent_path();

        auto findFolderVariant =
            [&](const std::vector<std::string>& variants) -> std::filesystem::path {
            for (const auto& name : variants) {
                auto path = parent / name;
                if (std::filesystem::exists(path)) {
                    return path;
                }
            }
            return {};
        };

        auto update_folder =
            findFolderVariant({game_folder_name + "-UPDATE", game_folder_name + "-update",
                               game_folder_name + "-patch", game_folder_name + "-PATCH"});

        auto mods_folder = findFolderVariant(
            {game_folder_name + "-MODS", game_folder_name + "-mods", game_folder_name + "-Mods"});

        bool hasUpdate = !update_folder.empty() && std::filesystem::is_directory(update_folder);
        bool hasMods = !mods_folder.empty() && std::filesystem::exists(mods_folder);

        if (!runningGameSerial.empty()) {
            const auto game_config_path =
                Common::FS::GetUserPath(Common::FS::PathType::CustomConfigs) /
                (runningGameSerial + ".toml");
            Config::load(game_config_path, true);
        }

        if (hasUpdate || hasMods) {
            bool enableMods = Config::getEnableMods();
            bool enableUpdates = Config::getEnableUpdates();

            if (hasUpdate && enableUpdates) {
                Config::setRestartWithBaseGame(false);
                auto update_eboot = update_folder / "eboot.bin";
                if (std::filesystem::exists(update_eboot)) {
                    file = update_eboot;
                } else {
                    file = base_folder / "eboot.bin";
                }
            } else if (hasUpdate && !enableUpdates) {
                Config::setRestartWithBaseGame(true);
                file = base_folder / "eboot.bin";
                ignorePatches = true;
            }

            if (hasMods) {
                Core::FileSys::MntPoints::enable_mods = enableMods;
            }
        }
    }

    if (gamePath.isEmpty())
        Common::FS::PathToQString(gamePath, file);

    if (gamePath.isEmpty())
        return;

    std::filesystem::path path = Common::FS::PathFromQString(gamePath);
    std::filesystem::path launchPath = path;

    if (path.filename() == "eboot.bin") {
        const auto parentDir = path.parent_path();
        for (auto& entry : std::filesystem::directory_iterator(parentDir)) {
            if (!entry.is_regular_file())
                continue;

            auto ext = entry.path().extension().string();
            if ((ext == ".elf" || ext == ".self" || ext == ".oelf") &&
                entry.path().filename() != "eboot.bin") {
                launchPath = entry.path();
                break;
            }
        }
    }

    if (gamePath.isEmpty())
        Common::FS::PathToQString(gamePath, file);

    if (gamePath.isEmpty())
        return;

    if (path.filename() == "eboot.bin") {
        const auto parentDir = path.parent_path();
        for (auto& entry : std::filesystem::directory_iterator(parentDir)) {
            if (!entry.is_regular_file())
                continue;

            auto ext = entry.path().extension().string();
            if ((ext == ".elf" || ext == ".self" || ext == ".oelf") &&
                entry.path().filename() != "eboot.bin") {
                launchPath = entry.path();
                break;
            }
        }
    }

    if (file.empty())
        return;

    Common::FS::PathToQString(gamePath, file);

    if (Config::getGameRunning()) {
        m_ipc_client->stopGame();
        QThread::sleep(1);
    }

    QStringList fullArgs = args;
    fullArgs << QString::fromStdString(launchPath.string());

    if (!std::filesystem::exists(launchPath)) {
        QMessageBox::critical(this, tr("shadPS4"), tr("Invalid launch path."));
        return;
    }

    if (ignorePatches) {
        Core::FileSys::MntPoints::ignore_game_patches = true;
    }
    QString workDir = QDir::currentPath();
    BackgroundMusicPlayer::getInstance().stopMusic();
    if (Config::getBootLauncher() && Config::getQTInstalled()) {
        QString emulatorExePath = QString::fromStdString(Config::getVersionPath());

        m_ipc_client->startGame(QFileInfo(emulatorExePath), fullArgs, workDir);
        setWindowState(Qt::WindowMinimized);
    } else if (Config::getBootLauncher() && Config::getSdlInstalled()) {
        QString emulatorExePath = QString::fromStdString(Config::getVersionPath());
        QStringList final_args = args;
        final_args.prepend(QString::fromStdString(launchPath.string()));
        final_args.prepend("--game");

        if (ignorePatches) {
            Core::FileSys::MntPoints::ignore_game_patches = true;
        }

        BackgroundMusicPlayer::getInstance().stopMusic();

        bool started = false;

#if defined(Q_OS_WIN)
        started = QProcess::startDetached(emulatorExePath, final_args, QString(), nullptr);
#elif defined(Q_OS_MAC)
        QStringList macArgs;
        macArgs << emulatorExePath << "--args";
        macArgs.append(final_args);
        started = QProcess::startDetached("open", macArgs, QString(), nullptr);
#else
        started = QProcess::startDetached(emulatorExePath, final_args, QString(), nullptr);
#endif

        if (!started) {
            QMessageBox::critical(this, tr("Error"), tr("Failed to launch game in detached mode."));
        } else {
            setWindowState(Qt::WindowMinimized);
        }
    } else {

        m_ipc_client->startGame(QFileInfo(QCoreApplication::applicationFilePath()), fullArgs,
                                workDir);
        setWindowState(Qt::WindowMinimized);
    }
    if (ignorePatches) {
        Core::FileSys::MntPoints::ignore_game_patches = false;
    }
    Config::setGameRunning(true);

    m_ipc_client->setActiveController(GamepadSelect::GetSelectedGamepad());

    m_ipc_client->gameClosedFunc = [this]() {
        QMetaObject::invokeMethod(this, [this]() {
            Config::setGameRunning(false);
            UpdateToolbarButtons();
            setWindowState(Qt::WindowNoState);
            show();
            raise();
            activateWindow();
        });
    };

    lastGamePath = launchPath;
    UpdateToolbarButtons();
}

bool isTable;
void MainWindow::SearchGameTable(const QString& text) {
    if (isTableList) {
        if (isTable != true) {
            m_game_info->m_games = m_game_info->m_games_backup;
            m_game_list_frame->PopulateGameList();
            isTable = true;
        }
        for (int row = 0; row < m_game_list_frame->rowCount(); row++) {
            QString game_name = QString::fromStdString(m_game_info->m_games[row].name);
            bool match = (game_name.contains(text, Qt::CaseInsensitive)); // Check only in column 1
            m_game_list_frame->setRowHidden(row, !match);
        }
    } else {
        isTable = false;
        m_game_info->m_games = m_game_info->m_games_backup;
        m_game_grid_frame->PopulateGameGrid(m_game_info->m_games, false);

        QVector<GameInfo> filteredGames;
        for (const auto& gameInfo : m_game_info->m_games) {
            QString game_name = QString::fromStdString(gameInfo.name);
            if (game_name.contains(text, Qt::CaseInsensitive)) {
                filteredGames.push_back(gameInfo);
            }
        }
        std::sort(filteredGames.begin(), filteredGames.end(), m_game_info->CompareStrings);
        m_game_info->m_games = filteredGames;
        m_game_grid_frame->PopulateGameGrid(filteredGames, true);
    }
}

void MainWindow::ShowGameList() {
    if (ui->showGameListAct->isChecked()) {
        RefreshGameTable();
    } else {
        m_game_grid_frame->clearContents();
        m_game_list_frame->clearContents();
    }
};

void MainWindow::RefreshGameTable() {
    // m_game_info->m_games.clear();
    m_game_info->GetGameInfo(this);
    m_game_list_frame->clearContents();
    m_game_list_frame->PopulateGameList();
    m_game_grid_frame->clearContents();
    m_game_grid_frame->PopulateGameGrid(m_game_info->m_games, false);
    statusBar->clearMessage();
    int numGames = m_game_info->m_games.size();
    QString statusMessage = tr("Games: ") + QString::number(numGames);
    statusBar->showMessage(statusMessage);
}

void MainWindow::ConfigureGuiFromSettings() {
    setGeometry(Config::getMainWindowGeometryX(), Config::getMainWindowGeometryY(),
                Config::getMainWindowGeometryW(), Config::getMainWindowGeometryH());

    ui->showGameListAct->setChecked(true);
    if (Config::getTableMode() == 0) {
        ui->setlistModeListAct->setChecked(true);
    } else if (Config::getTableMode() == 1) {
        ui->setlistModeGridAct->setChecked(true);
    } else if (Config::getTableMode() == 2) {
        ui->setlistElfAct->setChecked(true);
    } else if (Config::getTableMode() == 3) {
        // NEW
        if (ui->setlistModeCinematicAct)
            ui->setlistModeCinematicAct->setChecked(true);
    }
    BackgroundMusicPlayer::getInstance().setVolume(Config::getBGMvolume());
}

void MainWindow::SaveWindowState() const {
    Config::setMainWindowWidth(this->width());
    Config::setMainWindowHeight(this->height());
    Config::setMainWindowGeometry(this->geometry().x(), this->geometry().y(),
                                  this->geometry().width(), this->geometry().height());
}

void MainWindow::BootGame() {
    QFileDialog dialog;
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setNameFilter(tr("ELF files (*.bin *.elf *.oelf *.self)"));

    if (dialog.exec()) {
        QStringList fileNames = dialog.selectedFiles();

        if (fileNames.size() > 1) {
            QMessageBox::critical(nullptr, tr("Game Boot"), tr("Only one file can be selected!"));
            return;
        }

        QString gamePath = fileNames[0];
        std::filesystem::path path = Common::FS::PathFromQString(gamePath);

        if (!std::filesystem::exists(path)) {
            QMessageBox::critical(nullptr, tr("Run Game"), tr("Eboot.bin file not found"));
            return;
        }

        StartGameWithPath(gamePath);

        lastGamePath = gamePath.toStdString();
    }
}

void MainWindow::BootZarGame() {
    QFileDialog dialog;
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setNameFilter(tr("ZAR files (*.zar)"));

    if (dialog.exec()) {
        QStringList fileNames = dialog.selectedFiles();

        if (fileNames.size() > 1) {
            QMessageBox::critical(nullptr, tr("Game Boot"), tr("Only one file can be selected!"));
            return;
        }

        QString gamePath = fileNames[0];
        std::filesystem::path path = Common::FS::PathFromQString(gamePath);

        if (!std::filesystem::exists(path)) {
            QMessageBox::critical(nullptr, tr("Run Game"), tr("ZAR file not found"));
            return;
        }

        QString selectedVersion = QString::fromStdString(Config::getVersionPath());
        if (selectedVersion.isEmpty() || !QFile::exists(selectedVersion)) {
            selectedVersion = QCoreApplication::applicationFilePath();
        }
        QFileInfo fileInfo(selectedVersion);
        if (!fileInfo.exists()) {
            QMessageBox::critical(nullptr, "shadPS4",
                                  QString(tr("Could not find the emulator executable")));
            return;
        }

        QStringList final_args;
        final_args << gamePath;

        Config::setGameRunning(true);
        lastGamePath = path;

        QString workDir = QDir::currentPath();
        m_ipc_client->startGame(fileInfo, final_args, workDir, false);
        m_ipc_client->setActiveController(GamepadSelect::GetSelectedGamepad());

        setWindowState(Qt::WindowMinimized);
    }
}

struct PackContext {
    std::filesystem::path outputFilePath;
    std::ofstream currentOutputFile;
    bool hasError{false};
};

void _pack_NewOutputFile(const int32_t partIndex, void* ctx) {
    PackContext* packContext = (PackContext*)ctx;
    packContext->currentOutputFile = std::ofstream(packContext->outputFilePath, std::ios::binary);
    if (!packContext->currentOutputFile.is_open()) {
        printf("Failed to create output file: %s\n", packContext->outputFilePath.string().c_str());
        packContext->hasError = true;
    }
}

void _pack_WriteOutputData(const void* data, size_t length, void* ctx) {
    PackContext* packContext = (PackContext*)ctx;
    packContext->currentOutputFile.write((const char*)data, length);
}

void MainWindow::ConvertToZar() {
    QFileDialog dialog;
    dialog.setFileMode(QFileDialog::Directory);
    dialog.setOption(QFileDialog::ShowDirsOnly, true);
    dialog.setWindowTitle(tr("Select Game Folder to Convert"));

    if (dialog.exec()) {
        QString folderPath = dialog.selectedFiles()[0];
        std::filesystem::path inputDir = Common::FS::PathFromQString(folderPath);

        if (!std::filesystem::exists(inputDir) || !std::filesystem::is_directory(inputDir)) {
            QMessageBox::critical(nullptr, tr("Convert to ZAR"), tr("Invalid directory selected"));
            return;
        }

        QString outputFileName = QFileInfo(folderPath).fileName() + ".zar";
        QString outputPath = QFileInfo(folderPath).absolutePath() + "/" + outputFileName;
        std::filesystem::path outputFile = Common::FS::PathFromQString(outputPath);

        if (std::filesystem::exists(outputFile)) {
            QMessageBox::StandardButton reply = QMessageBox::question(
                nullptr, tr("Convert to ZAR"), tr("Output file already exists. Overwrite?"),
                QMessageBox::Yes | QMessageBox::No);
            if (reply == QMessageBox::No) {
                return;
            }
            std::filesystem::remove(outputFile);
        }

        QProgressDialog progressDialog(tr("Converting folder to ZAR..."), tr("Cancel"), 0, 0, this);
        progressDialog.setWindowTitle(QCoreApplication::applicationName());
        progressDialog.setWindowModality(Qt::WindowModal);
        progressDialog.show();

        std::vector<uint8_t> buffer;
        buffer.resize(64 * 1024);

        PackContext packContext;
        packContext.outputFilePath = outputFile;
        ZArchiveWriter zWriter(_pack_NewOutputFile, _pack_WriteOutputData, &packContext);

        if (packContext.hasError) {
            QMessageBox::critical(nullptr, tr("Convert to ZAR"),
                                  tr("Failed to create output file"));
            return;
        }

        int fileCount = 0;
        for (auto const& dirEntry : std::filesystem::recursive_directory_iterator(inputDir)) {
            std::error_code ec;
            std::filesystem::path pathEntry =
                std::filesystem::relative(dirEntry.path(), inputDir, ec);

            if (dirEntry.is_directory()) {
                if (!zWriter.MakeDir(pathEntry.generic_string().c_str(), false)) {
                    QMessageBox::critical(nullptr, tr("Convert to ZAR"),
                                          tr("Failed to create directory: %1")
                                              .arg(QString::fromStdString(pathEntry.string())));
                    return;
                }
            } else if (dirEntry.is_regular_file()) {
                if (dirEntry == outputFile) {
                    continue;
                }

                progressDialog.setLabelText(
                    tr("Adding: %1").arg(QString::fromStdString(pathEntry.string())));
                QApplication::processEvents();

                if (!zWriter.StartNewFile(pathEntry.generic_string().c_str())) {
                    QMessageBox::critical(nullptr, tr("Convert to ZAR"),
                                          tr("Failed to create archive file: %1")
                                              .arg(QString::fromStdString(pathEntry.string())));
                    return;
                }

                std::ifstream inputFile(inputDir / pathEntry, std::ios::binary);
                if (!inputFile.is_open()) {
                    QMessageBox::critical(nullptr, tr("Convert to ZAR"),
                                          tr("Failed to open input file: %1")
                                              .arg(QString::fromStdString(pathEntry.string())));
                    return;
                }

                while (true) {
                    inputFile.read((char*)buffer.data(), buffer.size());
                    int32_t readBytes = (int32_t)inputFile.gcount();
                    if (readBytes <= 0)
                        break;
                    zWriter.AppendData(buffer.data(), readBytes);
                }
                fileCount++;
            }

            if (progressDialog.wasCanceled()) {
                std::filesystem::remove(outputFile);
                QMessageBox::information(nullptr, tr("Convert to ZAR"), tr("Conversion cancelled"));
                return;
            }

            if (packContext.hasError) {
                std::filesystem::remove(outputFile);
                QMessageBox::critical(nullptr, tr("Convert to ZAR"), tr("Conversion failed"));
                return;
            }
        }

        zWriter.Finalize();
        progressDialog.hide();

        QMessageBox::information(
            nullptr, tr("Convert to ZAR"),
            tr("Successfully converted %1 files to ZAR format").arg(fileCount));

        QMessageBox::StandardButton reply = QMessageBox::question(
            nullptr, tr("Convert to ZAR"), tr("Delete original folder to save space?"),
            QMessageBox::Yes | QMessageBox::No);
        if (reply == QMessageBox::Yes) {
            std::error_code ec;
            std::filesystem::remove_all(inputDir, ec);
            if (ec) {
                QMessageBox::warning(nullptr, tr("Convert to ZAR"),
                                     tr("Failed to delete original folder: %1")
                                         .arg(QString::fromStdString(ec.message())));
            } else {
                QMessageBox::information(nullptr, tr("Convert to ZAR"),
                                         tr("Original folder deleted successfully"));
            }
        }

        LoadGameLists();
    }
}

void MainWindow::UpdateIconBackgroundOpacity() {
    int opacity = Config::getIconBgOpacity();
    QColor bgColor = m_window_themes.backgroundColor();

    if (ui->backgroundImageLabel) {
        ui->backgroundImageLabel->setStyleSheet(QString("QLabel#backgroundImageLabel {"
                                                        "  background-color: rgba(%1, %2, %3, %4);"
                                                        "  border: none;"
                                                        "}")
                                                    .arg(bgColor.red())
                                                    .arg(bgColor.green())
                                                    .arg(bgColor.blue())
                                                    .arg(opacity));
    }
    if (ui->bootIconsArea) {
        ui->bootIconsArea->setStyleSheet(QString("QWidget#bootIconsArea {"
                                                 "  background-color: rgba(%1, %2, %3, %4);"
                                                 "  border-radius: 12px;"
                                                 "  border: 1px solid rgba(90, 170, 255, 80);"
                                                 "}")
                                             .arg(bgColor.red())
                                             .arg(bgColor.green())
                                             .arg(bgColor.blue())
                                             .arg(opacity));
    }
    if (m_game_list_frame) {
        m_game_list_frame->RefreshListBackgroundImage();
        m_game_list_frame->RefreshZarBackgroundImage();
        m_game_list_frame->update();
    }
    if (m_game_grid_frame) {
        m_game_grid_frame->RefreshGridBackgroundImage();
        m_game_grid_frame->RefreshZarBackgroundImage();
        m_game_grid_frame->update();
    }
}

void MainWindow::UpdateThemeBackgrounds() {
    QColor bgColor = m_window_themes.backgroundColor();
    QColor textColor = m_window_themes.textColor();

    // Update background image label
    if (ui->backgroundImageLabel) {
        int opacity = Config::getIconBgOpacity();
        ui->backgroundImageLabel->setStyleSheet(QString("QLabel#backgroundImageLabel {"
                                                        "  background-color: rgba(%1, %2, %3, %4);"
                                                        "  border: none;"
                                                        "}")
                                                    .arg(bgColor.red())
                                                    .arg(bgColor.green())
                                                    .arg(bgColor.blue())
                                                    .arg(opacity));
    }

    // Update boot icons area
    if (ui->bootIconsArea) {
        int opacity = Config::getIconBgOpacity();
        ui->bootIconsArea->setStyleSheet(QString("QWidget#bootIconsArea {"
                                                 "  background-color: rgba(%1, %2, %3, %4);"
                                                 "  border-radius: 12px;"
                                                 "  border: 1px solid rgba(90, 170, 255, 80);"
                                                 "}")
                                             .arg(bgColor.red())
                                             .arg(bgColor.green())
                                             .arg(bgColor.blue())
                                             .arg(opacity));
    }

    // Update game rectangle container
    int bgOpacity = Config::getBgOpacity();
    int bgAlpha = bgOpacity * 255 / 100;
    ui->gameRectangleContainer->setStyleSheet(QString("QWidget#gameRectangleContainer {"
                                                      "  background-color: rgba(%1, %2, %3, %4);"
                                                      "  border-radius: 15px;"
                                                      "  border: 2px solid rgba(90, 170, 255, 150);"
                                                      "}")
                                                  .arg(bgColor.red())
                                                  .arg(bgColor.green())
                                                  .arg(bgColor.blue())
                                                  .arg(bgAlpha));

    // Update log display
    if (ui->logDisplay) {
        int logOpacity = Config::getLogOpacity();
        ui->logDisplay->setStyleSheet(
            QString("QTextEdit#logDisplay { background-color: rgba(%1, %2, %3, %4); color: %5; "
                    "border: 1px solid rgba(90, 170, 255, 150); }")
                .arg(bgColor.red())
                .arg(bgColor.green())
                .arg(bgColor.blue())
                .arg(logOpacity * 255 / 100)
                .arg(textColor.name()));
    }

    // Update style label and selector
    QColor styleBgColor = bgColor;
    QList<QLabel*> labels = findChildren<QLabel*>();
    for (QLabel* label : labels) {
        if (label->text() == tr("GUI Style:")) {
            label->setStyleSheet(QString("color: %1; font-weight: bold; background-color: rgba(%2, %3, %4, "
                                  "200); padding: 5px; border-radius: 5px;")
                                  .arg(textColor.name())
                                  .arg(styleBgColor.red())
                                  .arg(styleBgColor.green())
                                  .arg(styleBgColor.blue()));
            break;
        }
    }

    if (ui->styleSelector) {
        ui->styleSelector->setStyleSheet(QString(
            "QComboBox { background-color: rgba(%1, %2, %3, 200); color: %4; padding: 5px; "
            "border-radius: 5px; border: 1px solid rgba(90, 170, 255, 150); } QComboBox::drop-down { "
            "border: none; } QComboBox QAbstractItemView { background-color: rgba(%1, %2, %3, 200); "
            "color: %4; selection-background-color: rgba(90, 170, 255, 150); }")
            .arg(styleBgColor.red())
            .arg(styleBgColor.green())
            .arg(styleBgColor.blue())
            .arg(textColor.name()));
    }

    // Update launcher box
    if (ui->launcherBox) {
        QColor launcherBgColor = bgColor;
        ui->launcherBox->setStyleSheet(QString(
            "QCheckBox { background-color: rgba(%1, %2, %3, 220); color: %4; border: 1px solid rgba(90, 170, 255, "
            "150); border-radius: 5px; padding: 5px; }")
            .arg(launcherBgColor.red())
            .arg(launcherBgColor.green())
            .arg(launcherBgColor.blue())
            .arg(textColor.name()));
    }

    // Update extra toolbar buttons
    QColor extraBtnBgColor = bgColor;
    for (QPushButton* extraBtn : {ui->toggleLogButton, ui->installPkgButton, ui->bpBootButton,
                                  ui->zarBootButton, ui->zarConvertButton, ui->themeButton}) {
        if (extraBtn) {
            extraBtn->setStyleSheet(QString(
                "QPushButton { background-color: rgba(%1, %2, %3, 200); color: %4; border: 1px solid rgba(90, "
                "170, 255, 150); border-radius: 5px; padding: 5px; }")
                .arg(extraBtnBgColor.red())
                .arg(extraBtnBgColor.green())
                .arg(extraBtnBgColor.blue())
                .arg(textColor.name()));
        }
    }

    // Update slider labels
    QString labelStyle = QString("color: %1; font-weight: bold; font-size: 11px;").arg(textColor.name());
    if (m_bootIconsLabel) {
        m_bootIconsLabel->setStyleSheet(labelStyle);
    }
    if (m_gameHeightLabel) {
        m_gameHeightLabel->setStyleSheet(labelStyle);
    }
    if (m_gameSizeLabel) {
        m_gameSizeLabel->setStyleSheet(labelStyle);
    }
    if (m_logOpacityLabel) {
        m_logOpacityLabel->setStyleSheet(labelStyle);
    }
    if (m_bgOpacityLabel) {
        m_bgOpacityLabel->setStyleSheet(labelStyle);
    }
    if (m_iconBgOpacityLabel) {
        m_iconBgOpacityLabel->setStyleSheet(labelStyle);
    }

    // Update toolbar button labels
    QString buttonLabelStyle = QString("color: %1; font-size: 11px;").arg(textColor.name());
    for (QWidget* container : m_toolbarContainers) {
        if (QLabel* label = container->property("buttonLabel").value<QLabel*>()) {
            label->setStyleSheet(buttonLabelStyle);
        }
    }

    // Update play and pause labels separately (they're in stacked containers)
    if (ui->playContainer) {
        if (QLabel* playLabel = ui->playContainer->findChild<QLabel*>()) {
            playLabel->setStyleSheet(buttonLabelStyle);
        }
    }
    if (ui->pauseContainer) {
        if (QLabel* pauseLabel = ui->pauseContainer->findChild<QLabel*>()) {
            pauseLabel->setStyleSheet(buttonLabelStyle);
        }
    }

    // Update ZAR backgrounds in game frames
    if (m_game_list_frame) {
        m_game_list_frame->RefreshZarBackgroundImage();
        m_game_list_frame->RefreshHeaderColors();
    }
    if (m_game_grid_frame) {
        m_game_grid_frame->RefreshZarBackgroundImage();
    }
}

#ifdef ENABLE_QT_GUI

QString MainWindow::getLastEbootPath() {
    return QString();
}
#endif

void MainWindow::StartGameWithPath(const QString& gamePath) {
    if (gamePath.isEmpty()) {
        QMessageBox::warning(this, tr("Run Game"), tr("No game path provided."));
        return;
    }

    AddRecentFiles(gamePath);

    const auto path = Common::FS::PathFromQString(gamePath);
    if (!std::filesystem::exists(path)) {
        QMessageBox::critical(nullptr, tr("Run Game"), tr("Eboot.bin file not found"));
        return;
    }
    BackgroundMusicPlayer::getInstance().stopMusic();
    emulatorProcess = new QProcess(this);
    QString exePath = QCoreApplication::applicationFilePath();
    emulatorProcess->setProcessChannelMode(QProcess::ForwardedChannels);

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("SHADPS4_ENABLE_IPC", "true");
    emulatorProcess->setProcessEnvironment(env);
    QString emulatorExePath = QString::fromStdString(Config::getVersionPath());

    if (emulatorExePath.isEmpty() || !QFile::exists(emulatorExePath)) {
        emulatorExePath = exePath;
    }

    QStringList final_args;

    final_args << "--game";

    final_args << gamePath;

    StartEmulator(path, final_args);

    Config::setGameRunning(true);
    m_ipc_client->setActiveController(GamepadSelect::GetSelectedGamepad());
    UpdateToolbarButtons();

    if (!m_ipc_client || !Config::getGameRunning()) {
        QMessageBox::critical(this, tr("shadPS4"), tr("Failed to start game process."));
        return;
    }

    m_ipc_client->gameClosedFunc = [this]() {
        QMetaObject::invokeMethod(this, [this]() {
            Config::setGameRunning(false);
            UpdateToolbarButtons();
            setWindowState(Qt::WindowNoState);
            show();
            raise();
            activateWindow();
        });
    };
}

void MainWindow::Directories() {
    GameDirectoryDialog dlg;
    dlg.exec();
    RefreshGameTable();
}

void MainWindow::StartEmulator(std::filesystem::path path, QStringList args) {
    if (Config::getGameRunning()) {
        QMessageBox::critical(nullptr, tr("Run Game"), QString(tr("Game is already running!")));
        return;
    }

    QString selectedVersion = QString::fromStdString(Config::getVersionPath());
    if (selectedVersion.isEmpty() || !QFile::exists(selectedVersion)) {
        selectedVersion = QCoreApplication::applicationFilePath();
    }
    QFileInfo fileInfo(selectedVersion);
    if (!fileInfo.exists()) {
        QMessageBox::critical(nullptr, "shadPS4",
                              QString(tr("Could not find the emulator executable")));
        return;
    }

    QStringList final_args{"--game", QString::fromStdWString(path.wstring())};

    final_args.append(args);

    Config::setGameRunning(true);
    lastGamePath = path;

    QString workDir = QDir::currentPath();
    m_ipc_client->startGame(fileInfo, final_args, workDir, false);
    m_ipc_client->setActiveController(GamepadSelect::GetSelectedGamepad());

    setWindowState(Qt::WindowMinimized);
}

void MainWindow::ApplyLastUsedStyle() {
    QString savedStyle = QString::fromStdString(Config::getGuiStyle());
    if (!savedStyle.isEmpty() && QFile::exists(savedStyle)) {
        QFile file(savedStyle);
        if (file.open(QFile::ReadOnly | QFile::Text)) {
            qApp->setStyleSheet(file.readAll());
            file.close();
            return; // done
        }
    }

    if (!savedStyle.isEmpty() && QStyleFactory::keys().contains(savedStyle, Qt::CaseInsensitive)) {
        QApplication::setStyle(QStyleFactory::create(savedStyle));
    }
}

void MainWindow::SetLastUsedTheme() {
    Theme lastTheme = static_cast<Theme>(Config::getMainWindowTheme());
    QString savedStylePath = QString::fromStdString(Config::getGuiStyle());

    m_window_themes.SetWindowTheme(lastTheme, ui->mw_searchbar);

    auto applyTheme = [this, lastTheme]() {
        if (Config::getEnableColorFilter()) {
            SetUiIcons(m_window_themes.iconBaseColor(), m_window_themes.iconHoverColor());

            if (m_game_list_frame) {
                m_game_list_frame->SetThemeColors(m_window_themes.textColor());
            }
        } else {
            QColor baseColor = (lastTheme == Theme::Light) ? Qt::black : Qt::white;
            SetUiIcons(baseColor, baseColor);

            if (m_game_list_frame) {
                m_game_list_frame->SetThemeColors(baseColor);
            }
        }

        if (m_game_cinematic_frame) {
            m_game_cinematic_frame->update();
        }

        // Update theme backgrounds on startup
        UpdateThemeBackgrounds();
    };

    switch (lastTheme) {
    case Theme::Light:
        ui->setThemeLight->setChecked(true);
        applyTheme();
        break;
    case Theme::Dark:
        ui->setThemeDark->setChecked(true);
        applyTheme();
        break;
    case Theme::Green:
        ui->setThemeGreen->setChecked(true);
        applyTheme();
        break;
    case Theme::Blue:
        ui->setThemeBlue->setChecked(true);
        applyTheme();
        break;
    case Theme::Violet:
        ui->setThemeViolet->setChecked(true);
        applyTheme();
        break;
    case Theme::Gruvbox:
        ui->setThemeGruvbox->setChecked(true);
        applyTheme();
        break;
    case Theme::TokyoNight:
        ui->setThemeTokyoNight->setChecked(true);
        applyTheme();
        break;
    case Theme::Oled:
        ui->setThemeOled->setChecked(true);
        applyTheme();
        break;
    case Theme::Neon:
        ui->setThemeNeon->setChecked(true);
        applyTheme();
        break;
    case Theme::Shadlix:
        ui->setThemeShadlix->setChecked(true);
        applyTheme();
        break;
    case Theme::ShadlixCave:
        ui->setThemeShadlixCave->setChecked(true);
        applyTheme();
        break;
    case Theme::DeepPurple:
        ui->setThemeDeepPurple->setChecked(true);
        applyTheme();
        break;
    case Theme::QSS:
        ui->setThemeQSS->setChecked(true);
        applyTheme();
        break;
    }
}

void MainWindow::SetLastIconSizeBullet() {
    // set QAction bullet point if applicable
    int lastSize = Config::getIconSize();
    int lastSizeGrid = Config::getIconSizeGrid();
    if (isTableList) {
        switch (lastSize) {
        case 36:
            ui->setIconSizeTinyAct->setChecked(true);
            break;
        case 64:
            ui->setIconSizeSmallAct->setChecked(true);
            break;
        case 128:
            ui->setIconSizeMediumAct->setChecked(true);
            break;
        case 256:
            ui->setIconSizeLargeAct->setChecked(true);
            break;
        }
    } else {
        switch (lastSizeGrid) {
        case 69:
            ui->setIconSizeTinyAct->setChecked(true);
            break;
        case 97:
            ui->setIconSizeSmallAct->setChecked(true);
            break;
        case 161:
            ui->setIconSizeMediumAct->setChecked(true);
            break;
        case 256:
            ui->setIconSizeLargeAct->setChecked(true);
            break;
        }
    }
}

QPixmap MainWindow::RecolorPixmap(const QIcon& icon, const QSize& size, const QColor& color) {
    QPixmap pixmap = icon.pixmap(size);
    if (pixmap.isNull())
        return QPixmap(size);
    QImage img = pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < img.height(); ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            int alpha = qAlpha(line[x]);
            if (alpha > 5) {
                line[x] = qRgba(color.red(), color.green(), color.blue(), alpha);
            } else {
                line[x] = qRgba(0, 0, 0, 0);
            }
        }
    }

    return QPixmap::fromImage(img);
}

QIcon MainWindow::RecolorIcon(const QIcon& icon, const QColor& baseColor,
                              const QColor& hoverColor) {
    QSize size = icon.actualSize(QSize(120, 120));
    QIcon result;

    // Normal
    QPixmap normal = RecolorPixmap(icon, size, baseColor);
    result.addPixmap(normal, QIcon::Normal, QIcon::Off);

    // Hover (both Active and Selected states)
    QPixmap hover = RecolorPixmap(icon, size, hoverColor);
    result.addPixmap(hover, QIcon::Active, QIcon::Off);

    // Disabled
    QPixmap disabled = RecolorPixmap(icon, size, Qt::gray);
    result.addPixmap(disabled, QIcon::Disabled, QIcon::Off);

    return result;
}

void MainWindow::SetUiIcons(const QColor& baseColor, const QColor& hoverColor) {
    auto recolor = [&](QPushButton* btn, const QString& path) {
        if (!btn)
            return;
        QIcon original(path);
        m_originalIcons[btn] = original; // keep pristine original
        btn->setIcon(RecolorIcon(original, baseColor, hoverColor));
    };

    recolor(ui->playButton, ":/images/play_icon.png");
    recolor(ui->pauseButton, ":/images/pause_icon.png");
    recolor(ui->stopButton, ":/images/stop_icon.png");
    recolor(ui->refreshButton, ":/images/refreshlist_icon.png");
    recolor(ui->restartButton, ":/images/restart_game_icon.png");
    recolor(ui->settingsButton, ":/images/settings_icon.png");
    recolor(ui->fullscreenButton, ":/images/fullscreen_icon.png");
    recolor(ui->controllerButton, ":/images/controller_icon.png");
    recolor(ui->keyboardButton, ":/images/keyboard_icon.png");
    recolor(ui->updaterButton, ":/images/update_icon.png");
    recolor(ui->versionButton, ":/images/utils_icon.png");
    recolor(ui->bigPictureButton, ":/images/games_icon.png");
    recolor(ui->hubMenuButton, ":/images/hub_icon.png");
    recolor(ui->cinemaButton, ":/images/cinema_icon.png");
    recolor(ui->modManagerButton, ":images/folder_icon.png");
    recolor(ui->configureHotkeysButton, ":/images/hotkeybutton.png");

    if (ui->bootGameAct)
        ui->bootGameAct->setIcon(
            RecolorIcon(QIcon(":/images/play_icon.png"), baseColor, hoverColor));
    if (ui->shadFolderAct)
        ui->shadFolderAct->setIcon(
            RecolorIcon(QIcon(":/images/folder_icon.png"), baseColor, hoverColor));
    if (ui->exitAct)
        ui->exitAct->setIcon(RecolorIcon(QIcon(":/images/exit_icon.png"), baseColor, hoverColor));
#ifdef ENABLE_UPDATER
    if (ui->updaterAct)
        ui->updaterAct->setIcon(
            RecolorIcon(QIcon(":/images/update_icon.png"), baseColor, hoverColor));
#endif
    if (ui->downloadCheatsPatchesAct)
        ui->downloadCheatsPatchesAct->setIcon(
            RecolorIcon(QIcon(":/images/dump_icon.png"), baseColor, hoverColor));
    if (ui->dumpGameListAct)
        ui->dumpGameListAct->setIcon(
            RecolorIcon(QIcon(":/images/dump_icon.png"), baseColor, hoverColor));
    if (ui->aboutAct)
        ui->aboutAct->setIcon(RecolorIcon(QIcon(":/images/about_icon.png"), baseColor, hoverColor));
    if (ui->versionAct)
        ui->versionAct->setIcon(
            RecolorIcon(QIcon(":/images/play_icon.png"), baseColor, hoverColor));
    if (ui->setlistModeListAct)
        ui->setlistModeListAct->setIcon(
            RecolorIcon(QIcon(":/images/list_icon.png"), baseColor, hoverColor));
    if (ui->setlistModeGridAct)
        ui->setlistModeGridAct->setIcon(
            RecolorIcon(QIcon(":/images/grid_icon.png"), baseColor, hoverColor));
    if (ui->gameInstallPathAct)
        ui->gameInstallPathAct->setIcon(
            RecolorIcon(QIcon(":/images/folder_icon.png"), baseColor, hoverColor));
    if (ui->refreshGameListAct)
        ui->refreshGameListAct->setIcon(
            RecolorIcon(QIcon(":/images/refreshlist_icon.png"), baseColor, hoverColor));
    if (ui->trophyViewerAct)
        ui->trophyViewerAct->setIcon(
            RecolorIcon(QIcon(":/images/trophy_icon.png"), baseColor, hoverColor));
    if (ui->configureAct)
        ui->configureAct->setIcon(
            RecolorIcon(QIcon(":/images/settings_icon.png"), baseColor, hoverColor));
    if (ui->addElfFolderAct)
        ui->addElfFolderAct->setIcon(
            RecolorIcon(QIcon(":/images/folder_icon.png"), baseColor, hoverColor));

    if (ui->menuThemes)
        ui->menuThemes->setIcon(
            RecolorIcon(QIcon(":/images/themes_icon.png"), baseColor, hoverColor));
    if (ui->menuGame_List_Icons)
        ui->menuGame_List_Icons->setIcon(
            RecolorIcon(QIcon(":/images/iconsize_icon.png"), baseColor, hoverColor));
    if (ui->menuUtils)
        ui->menuUtils->setIcon(
            RecolorIcon(QIcon(":/images/utils_icon.png"), baseColor, hoverColor));
    if (ui->menuGame_List_Mode)
        ui->menuGame_List_Mode->setIcon(
            RecolorIcon(QIcon(":/images/list_mode_icon.png"), baseColor, hoverColor));
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    emit WindowResized(event);
    QMainWindow::resizeEvent(event);

    if (ui->backgroundImageLabel && !ui->backgroundImageLabel->pixmap().isNull()) {
        QPixmap currentPixmap = ui->backgroundImageLabel->pixmap();
        QPixmap scaledPixmap =
            currentPixmap.scaled(ui->backgroundImageLabel->size(), Qt::KeepAspectRatioByExpanding,
                                 Qt::SmoothTransformation);
        ui->backgroundImageLabel->setPixmap(scaledPixmap);
    }

    if (ui->backgroundImageLabel && ui->backgroundImageLabel->findChild<QWidget*>("uiOverlay")) {
        QWidget* uiOverlay = ui->backgroundImageLabel->findChild<QWidget*>("uiOverlay");
        uiOverlay->setGeometry(0, 0, ui->backgroundImageLabel->width(),
                               ui->backgroundImageLabel->height());
    }
}

void MainWindow::HandleResize(QResizeEvent* event) {
    if (isTableList) {
        if (m_game_list_frame)
            m_game_list_frame->RefreshListBackgroundImage();
    } else {
        if (m_game_grid_frame) {
            m_game_grid_frame->windowWidth = this->width();
            m_game_grid_frame->PopulateGameGrid(m_game_info->m_games, false);
            m_game_grid_frame->RefreshGridBackgroundImage();
        }
    }

    ui->toolBar->updateGeometry();
    ui->toolBar->layout()->invalidate();
}

void MainWindow::AddRecentFiles(QString filePath) {
    std::vector<std::string> vec = Config::getRecentFiles();
    if (!vec.empty()) {
        if (filePath.toStdString() == vec.at(0)) {
            return;
        }
        auto it = std::find(vec.begin(), vec.end(), filePath.toStdString());
        if (it != vec.end()) {
            vec.erase(it);
        }
    }
    vec.insert(vec.begin(), filePath.toStdString());
    if (vec.size() > 6) {
        vec.pop_back();
    }
    Config::setRecentFiles(vec);
    const auto config_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
    Config::saveMainWindow(config_dir / "config.toml");
    CreateRecentGameActions();
}

void MainWindow::CreateRecentGameActions() {
    m_recent_files_group = new QActionGroup(this);
    ui->menuRecent->clear();
    std::vector<std::string> vec = Config::getRecentFiles();
    for (int i = 0; i < vec.size(); i++) {
        QAction* recentFileAct = new QAction(this);
        recentFileAct->setText(QString::fromStdString(vec.at(i)));
        ui->menuRecent->addAction(recentFileAct);
        m_recent_files_group->addAction(recentFileAct);
    }

    connect(m_recent_files_group, &QActionGroup::triggered, this, [this](QAction* action) {
        auto gamePath = Common::FS::PathFromQString(action->text());
        AddRecentFiles(action->text());
        if (!std::filesystem::exists(gamePath)) {
            QMessageBox::critical(nullptr, tr("Run Game"), QString(tr("Eboot.bin file not found")));
            return;
        }
    });
}

void MainWindow::LoadTranslation() {
    auto language = QString::fromStdString(Config::getEmulatorLanguage());

    const QString base_dir = QStringLiteral(":/translations");
    QString base_path = QStringLiteral("%1/%2.qm").arg(base_dir).arg(language);

    if (QFile::exists(base_path)) {
        if (translator != nullptr) {
            qApp->removeTranslator(translator);
        }

        translator = new QTranslator(qApp);
        if (!translator->load(base_path)) {
            QMessageBox::warning(
                nullptr, QStringLiteral("Translation Error"),
                QStringLiteral("Failed to find load translation file for '%1':\n%2")
                    .arg(language)
                    .arg(base_path));
            delete translator;
        } else {
            qApp->installTranslator(translator);
            ui->retranslateUi(this);
        }
    }
}

void MainWindow::PlayBackgroundMusic() {}

void MainWindow::OnLanguageChanged(const std::string& locale) {
    Config::setEmulatorLanguage(locale);

    LoadTranslation();
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event) {
    if (!Config::getEnableColorFilter())
        return QObject::eventFilter(obj, event);
    if (QPushButton* btn = qobject_cast<QPushButton*>(obj)) {
        auto it = m_originalIcons.find(btn);
        if (it != m_originalIcons.end()) {
            if (event->type() == QEvent::Enter) {
                btn->setIcon(RecolorIcon(it.value(), m_window_themes.iconHoverColor(),
                                         m_window_themes.iconHoverColor()));
                return true;
            } else if (event->type() == QEvent::Leave) {
                btn->setIcon(RecolorIcon(it.value(), m_window_themes.iconBaseColor(),
                                         m_window_themes.iconBaseColor()));
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::StopGame() {
#ifdef Q_OS_WIN
    QProcess::startDetached("taskkill", QStringList() << "/IM" << "shadps4-sdl.exe" << "/F");
#elif defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
    QProcess::startDetached("pkill", QStringList() << "shadps4-sdl");
#endif
    if (!Config::getGameRunning())
        return;

    m_ipc_client->stopGame();
    Config::setGameRunning(false);
    is_paused = false;
    UpdateToolbarButtons();
}

void MainWindow::PauseGame() {
    if (is_paused) {
        m_ipc_client->resumeGame();
        is_paused = false;
    } else {
        m_ipc_client->pauseGame();
        is_paused = true;
    }
    UpdateToolbarButtons();
}

void MainWindow::keyPressEvent(QKeyEvent* event) {

    if (ui->mw_searchbar && ui->mw_searchbar->hasFocus()) {
        QMainWindow::keyPressEvent(event);
        return;
    }

    if (event->key() == Qt::Key_H || event->key() == Qt::Key_F1) {
        if (m_hubMenu) {
            m_hubMenu->toggle();
        }
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_B || event->key() == Qt::Key_F2) {
        if (m_bigPicture) {
            m_bigPicture->toggle();
        }
        event->accept();
        return;
    }

    QMainWindow::keyPressEvent(event);
}

void MainWindow::RestartGame() {
    if (!Config::getGameRunning()) {
        QMessageBox::information(this, tr("Restart Game"), tr("No game is currently running."));
        return;
    }

    if (!m_ipc_client) {
        QMessageBox::critical(this, tr("Restart Game"), tr("IPC client not initialized."));
        return;
    }

    if (!Config::getRestartWithBaseGame()) {
        m_ipc_client->restartGame();
        return;
    }

    if (lastGamePath.empty()) {
        QMessageBox::critical(this, tr("Restart Game"), tr("Cannot restart: no stored game path."));
        return;
    }

    LOG_INFO(IPC, "Restarting current game with base game dialog...");

    Config::setGameRunning(false);
    m_ipc_client->stopGame();

    QTimer::singleShot(500, [this]() {
        LOG_INFO(IPC, "Restart delay done, relaunching game...");

        QStringList args = lastGameArgs;
        StartGameWithArgs(args);
    });
}

void MainWindow::initializeGamepad() {

    if (SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK) < 0) {
        return;
    }

    SDL_JoystickID* gamepads = SDL_GetGamepads(NULL);
    int initialCount = 0;
    if (gamepads) {
        for (int i = 0; gamepads[i] != 0; ++i) {
            initialCount++;
        }
        SDL_free(gamepads);
    }

    SDL_JoystickID* joysticks = SDL_GetJoysticks(NULL);
    int joystickCount = 0;
    if (joysticks) {
        for (int i = 0; joysticks[i] != 0; ++i) {
            joystickCount++;
        }
        SDL_free(joysticks);
    }

    m_gamepadTimer = new QTimer(this);
    connect(m_gamepadTimer, &QTimer::timeout, this, &MainWindow::handleGamepadEvents);
    m_gamepadTimer->start(50);

    m_gamepadLaunchTimer = new QTimer(this);
    m_gamepadLaunchTimer->setSingleShot(true);
    connect(m_gamepadLaunchTimer, &QTimer::timeout, this, &MainWindow::StartGame);

    m_gamepadFocusDelayTimer = new QTimer(this);
    m_gamepadFocusDelayTimer->setSingleShot(true);
    connect(m_gamepadFocusDelayTimer, &QTimer::timeout, this, &MainWindow::unblockGamepadInput);

    m_gamepadInitialized = true;
}

void MainWindow::startGamepadLaunchDelay() {
    if (m_gamepadLaunchTimer && !m_gamepadLaunchTimer->isActive()) {
        m_gamepadLaunchTimer->start(1000);
    }
}

void MainWindow::startGamepadFocusDelay() {
    m_gamepadInputBlocked = true;
    if (m_gamepadFocusDelayTimer && !m_gamepadFocusDelayTimer->isActive()) {
        m_gamepadFocusDelayTimer->start(1000);
    }
}

void MainWindow::unblockGamepadInput() {
    m_gamepadInputBlocked = false;
}

void MainWindow::focusInEvent(QFocusEvent* event) {
    QMainWindow::focusInEvent(event);
    startGamepadFocusDelay();
}

void MainWindow::focusOutEvent(QFocusEvent* event) {
    QMainWindow::focusOutEvent(event);
}

void MainWindow::handleGamepadEvents() {
    if (!m_gamepadInitialized)
        return;

    if (Config::getGameRunning()) {
        return;
    }

    // Don't process gamepad GUI navigation if main window is not focused
    if (!isActiveWindow()) {
        return;
    }

    try {
        SDL_UpdateGamepads();

        SDL_JoystickID* gamepads = SDL_GetGamepads(NULL);
        int numGamepads = 0;
        if (gamepads) {
            for (int i = 0; gamepads[i] != 0; ++i) {
                numGamepads++;
            }
            SDL_free(gamepads);
        }

        if (m_usingJoystickFallback) {
            if (numGamepads == 0) {
                m_usingJoystickFallback = false;
                m_gamepadFailed = false;
                qDebug() << "Gamepad disconnected, resetting fallback state";
            } else {
                handleJoystickInput();
                return;
            }
        }

        if (!m_gamepad && !m_gamepadFailed && numGamepads > 0) {
            qDebug() << "Found" << numGamepads << "gamepad(s), attempting to open first one...";

            m_gamepad = SDL_OpenGamepad(0);
            if (m_gamepad) {
                qDebug() << "Gamepad connected:" << SDL_GetGamepadName(m_gamepad);
                m_gamepadFailed = false;
            } else {
                qDebug() << "Failed to open gamepad:" << SDL_GetError();
                qDebug() << "Switching to joystick fallback mode";
                m_gamepadFailed = true;
                m_usingJoystickFallback = true;
                return;
            }
        }

        if (m_gamepad && numGamepads == 0) {
            qDebug() << "Gamepad disconnected";
            SDL_CloseGamepad(m_gamepad);
            m_gamepad = nullptr;
            m_gamepadFailed = false;
            m_usingJoystickFallback = false;
            return;
        }

        if (!m_gamepad && !m_usingJoystickFallback) {
            return;
        }

        if (m_usingJoystickFallback) {
            handleJoystickInput();
            return;
        }

        static int lastNavigationTime = 0;
        int currentTime = SDL_GetTicks();
        const int navigationDelay = 200;

        if (currentTime - lastNavigationTime > navigationDelay) {
            if (SDL_GetGamepadButton(m_gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP)) {
                navigateGamesUp();
                lastNavigationTime = currentTime;
            } else if (SDL_GetGamepadButton(m_gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) {
                navigateGamesDown();
                lastNavigationTime = currentTime;
            } else if (SDL_GetGamepadButton(m_gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) {
                navigateGamesLeft();
                lastNavigationTime = currentTime;
            } else if (SDL_GetGamepadButton(m_gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) {
                navigateGamesRight();
                lastNavigationTime = currentTime;
            }
        }

        static bool lastAState = false;
        static bool lastBState = false;
        static bool lastStartState = false;

        bool currentAState = SDL_GetGamepadButton(m_gamepad, SDL_GAMEPAD_BUTTON_SOUTH);
        bool currentBState = SDL_GetGamepadButton(m_gamepad, SDL_GAMEPAD_BUTTON_EAST);
        bool currentStartState = SDL_GetGamepadButton(m_gamepad, SDL_GAMEPAD_BUTTON_START);

        if (currentAState && !lastAState) {
            if (m_game_list_frame && m_game_list_frame->GetCurrentItem() != nullptr &&
                ui->playButton->isEnabled() && !m_gamepadInputBlocked) {
                startGamepadLaunchDelay();
            }
        }

        if (currentBState && !lastBState) {
            if (ui->stopButton->isEnabled()) {
                StopGame();
            }
        }

        if (currentStartState && !lastStartState) {
            if (m_game_list_frame && m_game_list_frame->GetCurrentItem() != nullptr &&
                ui->playButton->isEnabled() && !m_gamepadInputBlocked) {
                startGamepadLaunchDelay();
            }
        }

        lastAState = currentAState;
        lastBState = currentBState;
        lastStartState = currentStartState;
    } catch (const std::exception& e) {
    } catch (...) {
    }
}

void MainWindow::handleJoystickInput() {
    if (Config::getGameRunning()) {
        return;
    }

    if (!isActiveWindow()) {
        return;
    }

    static SDL_Joystick* joystick = nullptr;
    static bool joystickInitialized = false;

    if (!joystickInitialized) {
        SDL_JoystickID* joysticks = SDL_GetJoysticks(NULL);
        int joystickCount = 0;
        if (joysticks) {
            for (int i = 0; joysticks[i] != 0; ++i) {
                joystickCount++;
            }
            SDL_free(joysticks);
        }

        if (joystickCount > 0) {
            joystick = SDL_OpenJoystick(0);
            if (joystick) {
                joystickInitialized = true;
            } else {
                SDL_JoystickID* joystickIDs = SDL_GetJoysticks(NULL);
                if (joystickIDs && joystickIDs[0] != 0) {
                    joystick = SDL_OpenJoystick(joystickIDs[0]);
                    if (joystick) {
                        joystickInitialized = true;
                    } else {
                    }
                }
                SDL_free(joystickIDs);

                if (!joystickInitialized) {
                    return;
                }
            }
        } else {
            return;
        }
    }

    if (!joystick)
        return;

    SDL_UpdateJoysticks();

    static int lastNavigationTime = 0;
    int currentTime = SDL_GetTicks();
    const int navigationDelay = 200;

    if (currentTime - lastNavigationTime > navigationDelay) {
        if (SDL_GetNumJoystickHats(joystick) > 0) {
            Uint8 hat = SDL_GetJoystickHat(joystick, 0);
            if (hat & SDL_HAT_UP) {
                qDebug() << "Joystick D-pad UP detected";
                navigateGamesUp();
                lastNavigationTime = currentTime;
            } else if (hat & SDL_HAT_DOWN) {
                qDebug() << "Joystick D-pad DOWN detected";
                navigateGamesDown();
                lastNavigationTime = currentTime;
            } else if (hat & SDL_HAT_LEFT) {
                qDebug() << "Joystick D-pad LEFT detected";
                navigateGamesLeft();
                lastNavigationTime = currentTime;
            } else if (hat & SDL_HAT_RIGHT) {
                qDebug() << "Joystick D-pad RIGHT detected";
                navigateGamesRight();
                lastNavigationTime = currentTime;
            }
        }

        if (SDL_GetNumJoystickAxes(joystick) >= 2) {
            Sint16 axisX = SDL_GetJoystickAxis(joystick, 0);
            Sint16 axisY = SDL_GetJoystickAxis(joystick, 1);

            const int deadZone = 16384;
            if (axisY < -deadZone) {
                qDebug() << "Joystick axis UP detected:" << axisY;
                navigateGamesUp();
                lastNavigationTime = currentTime;
            } else if (axisY > deadZone) {
                qDebug() << "Joystick axis DOWN detected:" << axisY;
                navigateGamesDown();
                lastNavigationTime = currentTime;
            } else if (axisX < -deadZone) {
                qDebug() << "Joystick axis LEFT detected:" << axisX;
                navigateGamesLeft();
                lastNavigationTime = currentTime;
            } else if (axisX > deadZone) {
                qDebug() << "Joystick axis RIGHT detected:" << axisX;
                navigateGamesRight();
                lastNavigationTime = currentTime;
            }
        }
    }

    static bool lastButton0State = false;
    static bool lastButton1State = false;
    static bool lastButton7State = false;

    if (SDL_GetNumJoystickButtons(joystick) > 0) {
        bool currentButton0State = SDL_GetJoystickButton(joystick, 0);
        bool currentButton1State = SDL_GetJoystickButton(joystick, 1);
        bool currentButton7State = SDL_GetJoystickButton(joystick, 7);

        if (currentButton0State && !lastButton0State) {
            qDebug() << "Joystick button 0 (A) pressed";
            if (m_game_list_frame && m_game_list_frame->GetCurrentItem() != nullptr &&
                ui->playButton->isEnabled() && !m_gamepadInputBlocked) {
                startGamepadLaunchDelay();
            }
        }

        if (currentButton1State && !lastButton1State) {
            qDebug() << "Joystick button 1 (B) pressed";
            if (ui->stopButton->isEnabled()) {
                StopGame();
            }
        }

        if (currentButton7State && !lastButton7State) {
            qDebug() << "Joystick button 7 (Start) pressed";
            if (m_game_list_frame && m_game_list_frame->GetCurrentItem() != nullptr &&
                ui->playButton->isEnabled() && !m_gamepadInputBlocked) {
                startGamepadLaunchDelay();
            }
        }

        lastButton0State = currentButton0State;
        lastButton1State = currentButton1State;
        lastButton7State = currentButton7State;
    }
}

void MainWindow::navigateGamesUp() {
    if (isTableList && m_game_list_frame) {
        int currentRow = m_game_list_frame->currentRow();
        if (currentRow > 0) {
            m_game_list_frame->selectRow(currentRow - 1);
        } else {
            int rowCount = m_game_list_frame->rowCount();
            if (rowCount > 0) {
                m_game_list_frame->selectRow(rowCount - 1);
            }
        }
    } else if (!isTableList && m_game_grid_frame) {
        int currentRow = m_game_grid_frame->currentRow();
        int currentColumn = m_game_grid_frame->currentColumn();
        int columnCount = m_game_grid_frame->columnCount();

        if (currentRow > 0) {
            m_game_grid_frame->setCurrentCell(currentRow - 1, currentColumn);
        } else {
            int rowCount = m_game_grid_frame->rowCount();
            if (rowCount > 0) {
                m_game_grid_frame->setCurrentCell(rowCount - 1, currentColumn);
            }
        }
    }
}

void MainWindow::navigateGamesDown() {
    if (isTableList && m_game_list_frame) {
        int currentRow = m_game_list_frame->currentRow();
        int rowCount = m_game_list_frame->rowCount();
        if (currentRow < rowCount - 1) {
            m_game_list_frame->selectRow(currentRow + 1);
        } else {
            if (rowCount > 0) {
                m_game_list_frame->selectRow(0);
            }
        }
    } else if (!isTableList && m_game_grid_frame) {
        int currentRow = m_game_grid_frame->currentRow();
        int currentColumn = m_game_grid_frame->currentColumn();
        int rowCount = m_game_grid_frame->rowCount();

        if (currentRow < rowCount - 1) {
            m_game_grid_frame->setCurrentCell(currentRow + 1, currentColumn);
        } else {
            if (rowCount > 0) {
                m_game_grid_frame->setCurrentCell(0, currentColumn);
            }
        }
    }
}

void MainWindow::navigateGamesLeft() {
    if (isTableList && m_game_list_frame) {
        navigateGamesUp();
    } else if (!isTableList && m_game_grid_frame) {
        int currentRow = m_game_grid_frame->currentRow();
        int currentColumn = m_game_grid_frame->currentColumn();
        int columnCount = m_game_grid_frame->columnCount();

        if (currentColumn > 0) {
            m_game_grid_frame->setCurrentCell(currentRow, currentColumn - 1);
        } else {
            if (currentRow > 0) {
                m_game_grid_frame->setCurrentCell(currentRow - 1, columnCount - 1);
            } else {
                int rowCount = m_game_grid_frame->rowCount();
                if (rowCount > 0) {
                    m_game_grid_frame->setCurrentCell(rowCount - 1, columnCount - 1);
                }
            }
        }
    }
}

void MainWindow::navigateGamesRight() {
    if (isTableList && m_game_list_frame) {
        navigateGamesDown();
    } else if (!isTableList && m_game_grid_frame) {
        int currentRow = m_game_grid_frame->currentRow();
        int currentColumn = m_game_grid_frame->currentColumn();
        int columnCount = m_game_grid_frame->columnCount();
        int rowCount = m_game_grid_frame->rowCount();

        if (currentColumn < columnCount - 1) {
            m_game_grid_frame->setCurrentCell(currentRow, currentColumn + 1);
        } else {
            if (currentRow < rowCount - 1) {
                m_game_grid_frame->setCurrentCell(currentRow + 1, 0);
            } else {
                m_game_grid_frame->setCurrentCell(0, 0);
            }
        }
    }
}
