// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStyleFactory>
#include <signal.h>

#include <QStyle>
#include <QWidget>

#include <QDockWidget>
#include <QKeyEvent>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QSplitter>
#include <QStatusBar>
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
#include "core/ipc/ipc.h"

#include "core/libraries/audio/audioout.h"
#include "version_dialog.h"

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
        ui->modManagerButton->installEventFilter(this);
        ui->launcherBox->installEventFilter(this);
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
        ui->modManagerButton->removeEventFilter(this);
        ui->launcherBox->removeEventFilter(this);
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
        QLabel* label = container->property("buttonLabel").value<QLabel*>();
        QString name = label ? label->text() : container->objectName();
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

        QAction* action = menu.addAction(name);
        action->setCheckable(true);
        action->setChecked(container->isVisible());
        action->setData(QVariant::fromValue(container));
        connect(action, &QAction::toggled, this, &MainWindow::toggleToolbarWidgetVisibility);
    }

    menu.exec(pos);
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

void MainWindow::forwardGamepadButton(int button) {
    if (!m_bigPicture || !m_bigPicture->isVisible())
        return;

    using GPB = BigPictureWidget::GamepadButton;
    GPB b;

    switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        b = GPB::Left;
        break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        b = GPB::Right;
        break;
    case SDL_GAMEPAD_BUTTON_SOUTH:
        b = GPB::South;
        break;
    case SDL_GAMEPAD_BUTTON_EAST:
        b = GPB::East;
        break;
    case SDL_GAMEPAD_BUTTON_NORTH:
        b = GPB::North;
        break;
    case SDL_GAMEPAD_BUTTON_WEST:
        b = GPB::West;
        break;
    default:
        return;
    }

    m_bigPicture->handleGamepadButton(b);
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

    auto createSpacer_wrapped = [this]() -> QWidget* {
        QWidget* spacer = new QWidget(this);
        spacer->setFixedWidth(5);
        spacer->setFixedHeight(5);
        return spacer;
    };

    auto createVLine = [this]() -> QFrame* {
        QFrame* line = new QFrame(this);
        line->setFrameShape(QFrame::VLine);
        line->setFrameShadow(QFrame::Sunken);
        line->setMinimumWidth(2);
        return line;
    };

    bool showLabels = ui->toggleLabelsAct->isChecked();
    ui->toolBar->clear();

    QWidget* toolbarContainer = new QWidget(this);
    QFlowLayout* flowLayout = new QFlowLayout(toolbarContainer, 5, 0, 5);
    toolbarContainer->setLayout(flowLayout);

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
    auto addToolbarWidget = [this, flowLayout, createButtonWithLabel_wrapped,
                             showLabels](QPushButton* button, const QString& text) {
        QWidget* container = createButtonWithLabel_wrapped(button, text, showLabels);
        container->setObjectName(button->objectName() + "Container");
        flowLayout->addWidget(container);
        m_toolbarContainers.append(container);
    };

    addToolbarWidget(ui->playButton, tr("Play"));
    addToolbarWidget(ui->pauseButton, tr("Pause"));
    addToolbarWidget(ui->stopButton, tr("Stop"));
    addToolbarWidget(ui->restartButton, tr("Restart"));

    addToolbarWidget(ui->settingsButton, tr("Settings"));
    addToolbarWidget(ui->fullscreenButton, tr("Full Screen"));

    addToolbarWidget(ui->controllerButton, tr("Controllers"));
    addToolbarWidget(ui->keyboardButton, tr("Keyboard"));

    addToolbarWidget(ui->configureHotkeysButton, tr("Hotkeys"));
    addToolbarWidget(ui->updaterButton, tr("Update"));

    addToolbarWidget(ui->refreshButton, tr("Refresh List"));
    addToolbarWidget(ui->versionButton, tr("Version"));
    addToolbarWidget(ui->modManagerButton, tr("Mods Manager"));

    addToolbarWidget(ui->bigPictureButton, tr("BigPicture"));

    addToolbarWidget(ui->hubMenuButton, tr("GameHub"));

    if (showLabels) {
        QLabel* label = ui->pauseButton->parentWidget()->findChild<QLabel*>();
        if (label)
            label->setVisible(false);
    }

    QWidget* searchSliderContainer = new QWidget(this);
    QVBoxLayout* searchSliderLayout = new QVBoxLayout(searchSliderContainer);
    searchSliderLayout->setContentsMargins(0, 0, 6, 6);
    searchSliderLayout->setSpacing(2);

    searchSliderLayout->addWidget(ui->sizeSliderContainer);
    searchSliderLayout->addWidget(ui->mw_searchbar);

    searchSliderContainer->setLayout(searchSliderLayout);

    QWidget* styleContainer = new QWidget(this);
    QVBoxLayout* styleLayout = new QVBoxLayout(styleContainer);
    styleLayout->setContentsMargins(2, 2, 2, 2);

    QLabel* styleLabel = new QLabel(tr("GUI Style:"), this);
    styleLayout->addWidget(styleLabel);
    styleLayout->addWidget(ui->styleSelector);

    ui->launcherBox = new QCheckBox(tr("Use Selected Version"), this);
    ui->FlowBox = new QCheckBox(tr(""), this);
    ui->launcherBox->setToolTip(tr("Let you Boot Game with selected Version"));
    ui->launcherBox->setChecked(Config::getBootLauncher());

    ui->toggleLogButton->setObjectName("ToggleLogButton");
    ui->installPkgButton->setObjectName("InstallPkgButton");
    ui->launcherBox->setObjectName("launcherBox");
    searchSliderContainer->setObjectName("searchSliderContainer");
    styleContainer->setObjectName("styleContainer");

    m_toolbarContainers.append(ui->installPkgButton);
    m_toolbarContainers.append(ui->toggleLogButton);
    m_toolbarContainers.append(styleContainer);
    m_toolbarContainers.append(searchSliderContainer);

    flowLayout->addWidget(styleContainer);
    flowLayout->addWidget(searchSliderContainer);

    flowLayout->addWidget(ui->installPkgButton);
    flowLayout->addWidget(ui->toggleLogButton);
    flowLayout->addWidget(ui->launcherBox);
    flowLayout->addWidget(ui->FlowBox);
    ui->FlowBox->setVisible(true);
    ui->FlowBox->setChecked(true);
    ui->FlowBox->setEnabled(false);
    ui->FlowBox->setToolTip(tr("This Enables Flow of Icons So its always ON"));

    m_toolbarContainers.append(ui->launcherBox);
    ui->sizeSliderContainer->setFixedWidth(130);
    ui->mw_searchbar->setFixedWidth(125);
    ui->styleSelector->setFixedWidth(125);

    ui->styleSelector->setFocusPolicy(Qt::ClickFocus);
    ui->launcherBox->setFocusPolicy(Qt::ClickFocus);
    ui->sizeSlider->setFocusPolicy(Qt::ClickFocus);

    for (QWidget* container : m_toolbarContainers) {
        if (!container->objectName().isEmpty()) {
            bool visible =
                Config::getToolbarWidgetVisibility(container->objectName().toStdString(), true);
            container->setVisible(visible);
        }
    }
    ui->toolBar->addWidget(toolbarContainer);

    ui->playButton->setVisible(true);
    ui->pauseButton->setVisible(false);

    ui->styleSelector->clear();
    QStringList styles = QStyleFactory::keys();
    for (const QString& styleName : styles)
        if (styleName.compare("windowsvista", Qt::CaseInsensitive) != 0)
            ui->styleSelector->addItem(styleName);

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
    if (Config::getGameRunning()) {
        ui->playButton->setVisible(false);
        ui->pauseButton->setVisible(true);

        if (is_paused) {
            ui->pauseButton->setIcon(ui->playButton->icon());
            ui->pauseButton->setToolTip(tr("Resume"));
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
                RecolorIcon(QIcon(":images/pause_icon.png"), baseColor, hoverColor));
            ui->pauseButton->setToolTip(tr("Pause"));
        }

        if (showLabels) {
            QLabel* playLabel = ui->playButton->parentWidget()->findChild<QLabel*>();
            QLabel* pauseLabel = ui->pauseButton->parentWidget()->findChild<QLabel*>();
            if (playLabel)
                playLabel->setVisible(false);
            if (pauseLabel) {
                pauseLabel->setText(is_paused ? tr("Resume") : tr("Pause"));
                pauseLabel->setVisible(true);
            }
        }
        return;
    }

    ui->playButton->setVisible(true);
    ui->pauseButton->setVisible(false);

    if (showLabels) {
        QLabel* playLabel = ui->playButton->parentWidget()->findChild<QLabel*>();
        QLabel* pauseLabel = ui->pauseButton->parentWidget()->findChild<QLabel*>();
        if (playLabel) {
            playLabel->setText(tr("Play"));
            playLabel->setVisible(true);
        }
        if (pauseLabel)
            pauseLabel->setVisible(false);
    }
}

void MainWindow::UpdateToolbarLabels() {
    bool showLabels = ui->toggleLabelsAct->isChecked();

    for (QPushButton* button :
         {ui->playButton, ui->stopButton, ui->restartButton, ui->settingsButton,
          ui->fullscreenButton, ui->controllerButton, ui->keyboardButton, ui->versionButton,
          ui->fullscreenButton, ui->controllerButton, ui->keyboardButton, ui->bigPictureButton,
          ui->hubMenuButton, ui->configureHotkeysButton, ui->updaterButton, ui->refreshButton,
          ui->modManagerButton}) {
        QLabel* label = button->parentWidget()->findChild<QLabel*>();
        if (label)
            label->setVisible(showLabels);
    }

    QLabel* pauseLabel = ui->pauseButton->parentWidget()->findChild<QLabel*>();
    if (pauseLabel)
        pauseLabel->setVisible(showLabels && Config::getGameRunning());
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
    QWidget* phCentralWidget = new QWidget(this);
    setCentralWidget(phCentralWidget);

    QWidget* dockContents = new QWidget(this);
    QVBoxLayout* dockLayout = new QVBoxLayout(dockContents);

    ui->splitter = new QSplitter(Qt::Vertical);
    ui->logDisplay = new QTextEdit(ui->splitter);
    ui->logDisplay->setText(tr("Game Log"));
    ui->logDisplay->setReadOnly(true);

    if (newDock) {
        m_dock_widget.reset(new QDockWidget(tr("Game List"), this));
        m_game_list_frame.reset(new GameListFrame(m_game_info, m_compat_info, m_ipc_client, this));
        m_game_list_frame->setObjectName("gamelist");

        m_game_grid_frame.reset(new GameGridFrame(m_game_info, m_compat_info, m_ipc_client, this));
        m_game_grid_frame->setObjectName("gamegridlist");

        m_elf_viewer.reset(new ElfViewer(this));
        m_elf_viewer->setObjectName("elflist");

        m_game_cinematic_frame.reset(
            new GameCinematicFrame(m_game_info, m_compat_info, m_ipc_client, this));
        m_game_cinematic_frame->setObjectName("cinematiclist");
    }

    int table_mode = Config::getTableMode();
    int slider_pos = 0;

    if (table_mode == 0) {
        m_game_grid_frame->hide();
        m_elf_viewer->hide();
        m_game_cinematic_frame->hide();

        m_game_list_frame->show();
        if (!newDock) {
            m_game_list_frame->clearContents();
            m_game_list_frame->PopulateGameList();
        }
        ui->splitter->addWidget(m_game_list_frame.data());

        ui->sizeSlider->setEnabled(true);
        ui->sizeSlider->setSliderPosition(slider_pos);
        isTableList = true;

    } else if (table_mode == 1) {
        m_game_list_frame->hide();
        m_elf_viewer->hide();
        m_game_cinematic_frame->hide();

        m_game_grid_frame->show();
        if (!newDock) {
            if (m_game_grid_frame->item(0, 0) == nullptr) {
                m_game_grid_frame->clearContents();
                m_game_grid_frame->PopulateGameGrid(m_game_info->m_games, false);
            }
        }
        ui->splitter->addWidget(m_game_grid_frame.data());

        ui->sizeSlider->setEnabled(true);
        ui->sizeSlider->setSliderPosition(slider_pos);
        isTableList = false;

    } else if (table_mode == 2) {
        m_game_list_frame->hide();
        m_game_grid_frame->hide();
        m_game_cinematic_frame->hide();

        m_elf_viewer->show();
        ui->splitter->addWidget(m_elf_viewer.data());

        ui->sizeSlider->setEnabled(false);
        isTableList = false;

    } else if (table_mode == 3) {
        m_game_list_frame->hide();
        m_game_grid_frame->hide();
        m_elf_viewer->hide();

        m_game_cinematic_frame->show();
        if (!newDock) {
            m_game_cinematic_frame->PopulateGameList();
        }
        ui->splitter->addWidget(m_game_cinematic_frame.get());

        ui->sizeSlider->setEnabled(false);
        isTableList = false;
    }

    QPalette logPalette = ui->logDisplay->palette();
    logPalette.setColor(QPalette::Base, Qt::black);
    ui->logDisplay->setPalette(logPalette);
    ui->splitter->addWidget(ui->logDisplay);

    QList<int> defaultSizes = {800, 200, 50};
    QList<int> sizes = m_compat_info->LoadDockWidgetSizes();
    if (sizes.isEmpty() || sizes.size() < 3 || sizes[1] == 0)
        sizes = defaultSizes;

    ui->splitter->setSizes(sizes);
    ui->splitter->setCollapsible(0, false);
    ui->splitter->setCollapsible(1, false);

    dockLayout->addWidget(ui->splitter);
    dockContents->setLayout(dockLayout);
    m_dock_widget->setWidget(dockContents);

    ui->welcomeAct->setCheckable(true);
    ui->welcomeAct->setChecked(Config::getShowWelcomeDialog());

    ui->bigPictureAct->setCheckable(true);
    ui->bigPictureAct->setChecked(Config::GamesMenuUI());

    ui->pauseOnUnfocusAct->setCheckable(true);
    ui->pauseOnUnfocusAct->setChecked(Config::getPauseOnUnfocus());

    m_dock_widget->setAllowedAreas(Qt::AllDockWidgetAreas);
    m_dock_widget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_dock_widget->resize(this->width(), this->height());

    addDockWidget(Qt::LeftDockWidgetArea, m_dock_widget.data());
    setDockNestingEnabled(true);

    bool showLog = m_compat_info->LoadShowLogSetting();
    ui->logDisplay->setVisible(showLog);
    ui->toggleLogButton->setText(showLog ? tr("Hide Log") : tr("Show Log"));
    ui->installPkgButton->setText(tr("Install PKG"));

    disconnect(ui->toggleLogButton, nullptr, nullptr, nullptr);

    connect(ui->toggleLogButton, &QPushButton::clicked, this, [this]() {
        bool visible = ui->logDisplay->isVisible();
        ui->logDisplay->setVisible(!visible);
        ui->toggleLogButton->setText(visible ? tr("Show Log") : tr("Hide Log"));
        m_compat_info->SaveShowLogSetting(!visible);
    });
}

void MainWindow::LoadGameLists() {
    if (Config::getCompatibilityEnabled())
        m_compat_info->LoadCompatibilityFile();

    if (Config::getCheckCompatibilityOnStartup())
        m_compat_info->UpdateCompatibilityDatabase(this);

    m_game_info->GetGameInfo(this);
    if (isTableList) {
        m_game_list_frame->PopulateGameList();
    } else if (Config::getTableMode() == 3) {
        m_game_cinematic_frame->PopulateGameList();
    } else {
        m_game_grid_frame->PopulateGameGrid(m_game_info->m_games, false);
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
    Config::save(Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "config.toml");
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
        } else {
            m_game_grid_frame->icon_size = 69 + value;
            m_game_grid_frame->PopulateGameGrid(m_game_info->m_games, false);
            Config::setIconSizeGrid(69 + value);
            Config::setSliderPositionGrid(value);
        }
    });

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
        } else if (Config::getTableMode() == 3) {
            if (m_game_cinematic_frame)
                m_game_cinematic_frame->RefreshBackground();
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

        Config::save(config_dir / "config.toml");
    });

    connect(ui->clearCustomBackgroundAct, &QAction::triggered, this, [this]() {
        if (m_game_grid_frame) {
            m_game_grid_frame->SetCustomBackgroundImage("");
        }
        const auto config_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);

        Config::save(config_dir / "config.toml");
    });

    connect(ui->installPkgButton, &QPushButton::clicked, this, [this]() {
        auto versionDialog = new VersionDialog(m_compat_info, this);
        versionDialog->InstallPkgWithV7();
    });

    connect(ui->launcherBox, &QCheckBox::clicked, this, [this](bool checked) {
        Config::setBootLauncher(checked);
        const auto config_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
        Config::save(config_dir / "config.toml");
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
        Config::save(Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "config.toml");
    });

    connect(ui->bigPictureAct, &QAction::triggered, this, [this](bool checked) {
        Config::setGamesMenuUI(checked);
        Config::save(Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "config.toml");
    });

    connect(ui->hubMenuAct, &QAction::triggered, this, [this](bool checked) {
        Config::setHubMenuUI(checked);
        Config::save(Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "config.toml");
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
        } else {
            m_game_grid_frame->icon_size = 69;
            ui->sizeSlider->setValue(0); // icone_size - 36
            Config::setIconSizeGrid(69);
            Config::setSliderPositionGrid(0);
            m_game_grid_frame->PopulateGameGrid(m_game_info->m_games, false);
        }
    });

    // handle resize like this for now, we deal with it when we add more docks
    connect(this, &MainWindow::WindowResized, this, [&]() {
        this->resizeDocks({m_dock_widget.data()}, {this->width()}, Qt::Orientation::Horizontal);
    });

    connect(ui->setIconSizeSmallAct, &QAction::triggered, this, [this]() {
        if (isTableList) {
            m_game_list_frame->icon_size = 64;
            ui->sizeSlider->setValue(28);
            Config::setIconSize(64);
            Config::setSliderPosition(28);
        } else {
            m_game_grid_frame->icon_size = 97;
            ui->sizeSlider->setValue(28);
            Config::setIconSizeGrid(97);
            Config::setSliderPositionGrid(28);
            m_game_grid_frame->PopulateGameGrid(m_game_info->m_games, false);
        }
    });

    connect(ui->setIconSizeMediumAct, &QAction::triggered, this, [this]() {
        if (isTableList) {
            m_game_list_frame->icon_size = 128;
            ui->sizeSlider->setValue(92);
            Config::setIconSize(128);
            Config::setSliderPosition(92);
        } else {
            m_game_grid_frame->icon_size = 161;
            ui->sizeSlider->setValue(92);
            Config::setIconSizeGrid(161);
            Config::setSliderPositionGrid(92);
            m_game_grid_frame->PopulateGameGrid(m_game_info->m_games, false);
        }
    });

    connect(ui->setIconSizeLargeAct, &QAction::triggered, this, [this]() {
        if (isTableList) {
            m_game_list_frame->icon_size = 256;
            ui->sizeSlider->setValue(220);
            Config::setIconSize(256);
            Config::setSliderPosition(220);
        } else {
            m_game_grid_frame->icon_size = 256;
            ui->sizeSlider->setValue(220);
            Config::setIconSizeGrid(256);
            Config::setSliderPositionGrid(220);
            m_game_grid_frame->PopulateGameGrid(m_game_info->m_games, false);
        }
    });
    connect(ui->setlistModeListAct, &QAction::triggered, m_dock_widget.data(), [this]() {
        ui->sizeSlider->setEnabled(true);
        BackgroundMusicPlayer::getInstance().stopMusic();

        const QList<int> sizes = ui->splitter->sizes();
        m_compat_info->SaveDockWidgetSizes(sizes);

        Config::setTableMode(0);
        CreateDockWindows(false);
        ui->mw_searchbar->setText("");
        SetLastIconSizeBullet();
    });

    connect(ui->setlistModeGridAct, &QAction::triggered, m_dock_widget.data(), [this]() {
        ui->sizeSlider->setEnabled(true);
        BackgroundMusicPlayer::getInstance().stopMusic();

        const QList<int> sizes = ui->splitter->sizes();
        m_compat_info->SaveDockWidgetSizes(sizes);

        Config::setTableMode(1);
        CreateDockWindows(false);
        ui->mw_searchbar->setText("");
        SetLastIconSizeBullet();
    });

    connect(ui->setlistElfAct, &QAction::triggered, m_dock_widget.data(), [this]() {
        ui->sizeSlider->setEnabled(false);
        BackgroundMusicPlayer::getInstance().stopMusic();

        const QList<int> sizes = ui->splitter->sizes();
        m_compat_info->SaveDockWidgetSizes(sizes);

        Config::setTableMode(2);
        CreateDockWindows(false);
        SetLastIconSizeBullet();
    });
    connect(ui->setlistModeCinematicAct, &QAction::triggered, m_dock_widget.data(), [this]() {
        ui->sizeSlider->setEnabled(true);
        BackgroundMusicPlayer::getInstance().stopMusic();

        const QList<int> sizes = ui->splitter->sizes();
        m_compat_info->SaveDockWidgetSizes(sizes);

        Config::setTableMode(3);
        CreateDockWindows(false);
        ui->mw_searchbar->setText("");
        SetLastIconSizeBullet();
    });

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
        Config::save(config_dir / "config.toml");
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
        auto update_folder = base_folder.parent_path() / (game_folder_name + "-UPDATE");
        auto mods_folder = base_folder.parent_path() / (game_folder_name + "-MODS");

        bool hasUpdate = std::filesystem::is_directory(update_folder);
        bool hasMods = std::filesystem::exists(mods_folder);

        if (hasUpdate || hasMods) {
            QMessageBox msgBox;

            msgBox.setWindowFlag(Qt::WindowCloseButtonHint, false);
            QPushButton* baseBtn = nullptr;
            QPushButton* updateBtn = nullptr;
            QPushButton* yesBtn = nullptr;
            QPushButton* noBtn = nullptr;
            QPushButton* detachedBtn = nullptr;

            if (hasUpdate && !hasMods) {
                msgBox.setWindowTitle(tr("Game Update Detected"));
                msgBox.setText(tr(R"(Game update detected, 
select to boot Base game or Update)"));
                baseBtn = msgBox.addButton(tr("Base Game"), QMessageBox::AcceptRole);
                updateBtn = msgBox.addButton(tr("Updated Game"), QMessageBox::YesRole);
                msgBox.setDefaultButton(updateBtn);
                msgBox.setStandardButtons(QMessageBox::Cancel);
            } else if (!hasUpdate && hasMods) {
                msgBox.setWindowTitle(tr("Mods Detected"));
                msgBox.setText(tr(R"("Mods detected, 
do you want to enable them?)"));
                yesBtn = msgBox.addButton(tr("Yes"), QMessageBox::AcceptRole);
                noBtn = msgBox.addButton(tr("No"), QMessageBox::RejectRole);
                msgBox.setDefaultButton(yesBtn);
                msgBox.setStandardButtons(QMessageBox::Cancel);
            } else if (hasUpdate && hasMods) {
                msgBox.setWindowTitle(tr("Game Update Detected"));
                msgBox.setText(tr(R"(Game update detected, 
select to boot Base game or Update)"));
                baseBtn = msgBox.addButton(tr("Base Game"), QMessageBox::AcceptRole);
                updateBtn = msgBox.addButton(tr("Updated Game"), QMessageBox::YesRole);
                msgBox.setDefaultButton(updateBtn);

                QCheckBox* modsCheck = new QCheckBox(tr("Enable MODS"), &msgBox);
                modsCheck->setChecked(true);
                msgBox.setCheckBox(modsCheck);
                msgBox.setStandardButtons(QMessageBox::Cancel);
            }

            msgBox.exec();
            const auto config_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);

            QAbstractButton* clicked = msgBox.clickedButton();
            if (hasUpdate && !hasMods) {
                if (clicked == baseBtn) {
                    Config::setRestartWithBaseGame(true);
                    Config::save(config_dir / "config.toml");
                    file = base_folder / "eboot.bin";
                    ignorePatches = true;
                } else if (clicked == updateBtn) {
                    Config::setRestartWithBaseGame(false);
                    Config::save(config_dir / "config.toml");

                    auto update_eboot = update_folder / "eboot.bin";
                    if (std::filesystem::exists(update_eboot)) {
                        file = update_eboot;
                    } else {
                        file = base_folder / "eboot.bin";
                    }
                } else {
                    return;
                }
            } else if (!hasUpdate && hasMods) {
                if (clicked == yesBtn)
                    Core::FileSys::MntPoints::enable_mods = true;
                else if (clicked == noBtn)
                    Core::FileSys::MntPoints::enable_mods = false;
                else
                    return;
            } else if (hasUpdate && hasMods) {
                if (clicked == baseBtn) {
                    Config::setRestartWithBaseGame(true);
                    Config::save(config_dir / "config.toml");
                    file = base_folder / "eboot.bin";
                    ignorePatches = true;
                } else if (clicked == updateBtn) {
                    Config::setRestartWithBaseGame(false);
                    Config::save(config_dir / "config.toml");

                    auto update_eboot = update_folder / "eboot.bin";
                    if (std::filesystem::exists(update_eboot)) {
                        file = update_eboot;
                    } else {
                        file = base_folder / "eboot.bin";
                    }
                } else {
                    return;
                }
                Core::FileSys::MntPoints::enable_mods = msgBox.checkBox()->isChecked();
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
        }
    } else {

        m_ipc_client->startGame(QFileInfo(QCoreApplication::applicationFilePath()), fullArgs,
                                workDir);
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
    QList<int> sizes = {ui->splitter->sizes()};
    m_compat_info->SaveDockWidgetSizes(sizes);
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
        Config::setGameRunning(true);
        UpdateToolbarButtons();
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
}

void MainWindow::HandleResize(QResizeEvent* event) {
    // Existing logic for game list updates...
    if (isTableList) {
        if (m_game_list_frame)
            m_game_list_frame->RefreshListBackgroundImage();
    } else if (Config::getTableMode() == 3) {
        if (m_game_cinematic_frame)
            m_game_cinematic_frame->RefreshBackground();
    } else {
        if (m_game_grid_frame) {
            m_game_grid_frame->windowWidth = this->width();
            m_game_grid_frame->PopulateGameGrid(m_game_info->m_games, false);
            m_game_grid_frame->RefreshGridBackgroundImage();
        }
    }

    // --- ADD THIS TO FORCE TOOLBAR LAYOUT UPDATE ---
    // This tells the toolbar to check its content's size policies and re-layout.
    ui->toolBar->updateGeometry();
    ui->toolBar->layout()->invalidate(); // Force immediate layout recalculation
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
