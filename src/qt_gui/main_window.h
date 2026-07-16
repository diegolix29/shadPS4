// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <QActionGroup>
#include <QDragEnterEvent>
#include <QEasingCurve>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsOpacityEffect>
#include <QLayout>
#include <QParallelAnimationGroup>
#include <QProcess>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRect>
#include <QTranslator>
#include <QVector>
#include <QWidgetItem>

#include "background_music_player.h"
#include "cheats_patches.h"
#include "common/config.h"
#include "common/path_util.h"
#include "compatibility_info.h"
#include "core/file_format/psf.h"
#include "core/file_sys/fs.h"
#include "core/ipc/ipc_client.h"
#include "elf_viewer.h"
#include "emulator.h"
#include "game_cinematic_frame.h"
#include "game_grid_frame.h"
#include "game_info.h"
#include "game_list_frame.h"
#include "game_list_utils.h"
#include "games_menu.h"
#include "hub_menu_widget.h"
#include "main_window_themes.h"
#include "main_window_ui.h"
#include "sdl_window.h"
#include "video_core/cache_storage.h"

class QFlowLayout : public QLayout {
public:
    explicit QFlowLayout(QWidget* parent, int margin = -1, int hSpacing = -1, int vSpacing = -1);
    explicit QFlowLayout(int margin = -1, int hSpacing = -1, int vSpacing = -1);
    ~QFlowLayout() override;

    void addItem(QLayoutItem* item) override;
    int horizontalSpacing() const;
    int verticalSpacing() const;
    Qt::Orientations expandingDirections() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    int count() const override;
    QLayoutItem* itemAt(int index) const override;
    QSize minimumSize() const override;
    void setGeometry(const QRect& rect) override;
    QSize sizeHint() const override;
    QLayoutItem* takeAt(int index) override;

private:
    int doLayout(const QRect& rect, bool testOnly) const;
    int smartSpacing(QStyle::PixelMetric pm) const;

    QVector<QLayoutItem*> m_itemList;
    int m_hSpace = -1;
    int m_vSpace = -1;
};

class FlowContainer : public QWidget {
public:
    using QWidget::QWidget;

    bool hasHeightForWidth() const override {
        return layout() && layout()->hasHeightForWidth();
    }

    int heightForWidth(int w) const override {
        return layout()->heightForWidth(w);
    }

    QSize sizeHint() const override {
        return layout()->sizeHint();
    }

    QSize minimumSizeHint() const override {
        return layout()->minimumSize();
    }
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        if (layout()) {
            layout()->invalidate(); // Triggers a re-layout.
        }
    }
};

class HoverAnimator : public QObject {
    Q_OBJECT
public:
    explicit HoverAnimator(QWidget* target, QColor glowColor = QColor(90, 170, 255))
        : QObject(target), m_target(target), m_glowColor(glowColor) {
        if (m_target) {
            m_target->installEventFilter(this);
            m_target->setAttribute(Qt::WA_Hover);

            if (auto* btn = qobject_cast<QPushButton*>(m_target)) {
                m_baseIconSize = btn->iconSize();
            }

            m_shadow = new QGraphicsDropShadowEffect(m_target);
            m_shadow->setColor(m_glowColor);
            m_shadow->setOffset(0, 0);
            m_shadow->setBlurRadius(0);
            m_target->setGraphicsEffect(m_shadow);
        }
    }

    static void Attach(QPushButton* button, QColor glowColor = QColor(90, 170, 255)) {
        if (!button || button->property("hoverAnimAttached").toBool())
            return;
        button->setProperty("hoverAnimAttached", true);
        new HoverAnimator(button, glowColor);
    }

protected:
    bool eventFilter(QObject* obj, QEvent* event) override {
        if (obj == m_target) {
            if (event->type() == QEvent::Enter) {
                animate(true);
            } else if (event->type() == QEvent::Leave) {
                animate(false);
            }
        }
        return QObject::eventFilter(obj, event);
    }

private:
    void animate(bool hovered) {
        if (auto* btn = qobject_cast<QPushButton*>(m_target)) {
            if (m_baseIconSize.isEmpty())
                m_baseIconSize = btn->iconSize();

            const QSize targetSize = hovered
                                         ? QSize(static_cast<int>(m_baseIconSize.width() * 1.16),
                                                 static_cast<int>(m_baseIconSize.height() * 1.16))
                                         : m_baseIconSize;

            auto* sizeAnim = new QPropertyAnimation(btn, "iconSize", this);
            sizeAnim->setDuration(180);
            sizeAnim->setStartValue(btn->iconSize());
            sizeAnim->setEndValue(targetSize);
            sizeAnim->setEasingCurve(hovered ? QEasingCurve::OutBack : QEasingCurve::OutCubic);
            sizeAnim->start(QAbstractAnimation::DeleteWhenStopped);
        }

        if (m_shadow) {
            auto* glowAnim = new QPropertyAnimation(m_shadow, "blurRadius", this);
            glowAnim->setDuration(220);
            glowAnim->setStartValue(m_shadow->blurRadius());
            glowAnim->setEndValue(hovered ? 24.0 : 0.0);
            glowAnim->setEasingCurve(QEasingCurve::OutQuad);
            glowAnim->start(QAbstractAnimation::DeleteWhenStopped);
        }
    }

    QWidget* m_target;
    QGraphicsDropShadowEffect* m_shadow = nullptr;
    QSize m_baseIconSize;
    QColor m_glowColor;
};

class GameListFrame;

class MainWindow : public QMainWindow {
    Q_OBJECT
signals:
    void WindowResized(QResizeEvent* event);

public:
    void StartEmulator(std::filesystem::path path, QStringList args);
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();
    std::string GetRunningGameSerial() const;
    bool Init();
    void onHubMenuOpenFolderRequested(int index);
    void onDeleteShaderCacheRequested(int index);
    void onShaderCacheError(const QString& gameSerial);
    void openSettingsWindow();
    void createToolbarContextMenu(const QPoint& pos);
    void contextMenuEvent(QContextMenuEvent* event) override;
    void toggleToolbarWidgetVisibility(bool checked);
    void forwardGamepadButton(int sdlButton);
    void restoreBigPictureFocus();
    void restoreHubFocus();
    void openHotkeysWindow();
    void StartGameByIndex(int index, QStringList args);
    void toggleColorFilter();
    void onHubMenuLaunchGameRequested(int index);
    void onHubMenuGameConfigRequested(int index);
    void onHubMenuOpenModsManagerRequested(int index);
    void onOpenCheatsRequested(int index);
    void StartGameWithPath(const QString& gamePath);
    void Directories();
    void ApplyLastUsedStyle();
    void StartGame();
    void StartGameWithArgs(QStringList args, int forcedIndex = -1);
    void PauseGame();
    bool showLabels;
    void StopGame();
    void keyPressEvent(QKeyEvent* event) override;
    void RestartGame();
    std::unique_ptr<Core::Emulator> emulator;
    qint64 detachedGamePid = -1;
    bool isDetachedLaunch = false;
    void ToggleMute();
    void autoCheckLauncherBox();
    std::string runningGameSerial = "";
    bool m_showWelcomeOnLaunch = true;

    QString getLastEbootPath();
    std::filesystem::path lastGamePath;
    QStringList lastGameArgs;
    static QProcess* emulatorProcess;
    std::shared_ptr<IpcClient> m_ipc_client;
    std::unique_ptr<BigPictureWidget> m_bigPicture;
    std::unique_ptr<HubMenuWidget> m_hubMenu;
    std::shared_ptr<GameInfoClass> m_game_info = std::make_shared<GameInfoClass>();

private Q_SLOTS:
    void ConfigureGuiFromSettings();
    void SaveWindowState() const;
    void SearchGameTable(const QString& text);
    void ShowGameList();
    void RefreshGameTable();
    void HandleResize(QResizeEvent* event);
    void OnLanguageChanged(const std::string& locale);
    void toggleLabelsUnderIcons();

private:
    Ui_MainWindow* ui;
    void AddUiWidgets();
    void UpdateToolbarLabels();
    void UpdateToolbarButtons();
    QWidget* createButtonWithLabel(QPushButton* button, const QString& labelText, bool showLabel);
    void CreateActions();
    void toggleFullscreen();
    void CreateRecentGameActions();
    void CreateDockWindows(bool newDock);
    void LoadGameLists();
    void PrintLog(QString entry, QColor textColor);
    QList<QWidget*> m_toolbarContainers;

#ifdef ENABLE_UPDATER
    void CheckUpdateMain(bool checkSave);
#endif
    void CreateConnects();
    void SetLastUsedTheme();
    void SetLastIconSizeBullet();
    QPixmap RecolorPixmap(const QIcon& icon, const QSize& size, const QColor& color);
    void SetUiIcons(const QColor& baseColor, const QColor& hoverColor);
    void BootGame();
    void toggleWelcomeScreenOnLaunch(bool enabled);
    void onSetCustomBackground();
    void onClearCustomBackground();

    void AddRecentFiles(QString filePath);
    void LoadTranslation();
    void PlayBackgroundMusic();
    QIcon RecolorIcon(const QIcon& icon, const QColor& baseColor, const QColor& hoverColor);
    QMap<QPushButton*, QIcon> m_originalIcons;

    bool isIconBlack = false;
    bool isTableList = true;
    bool isGameRunning = false;
    bool isWhite = false;
    bool is_paused = false;
    bool m_isRepopulatingStyleSelector = false;

    QActionGroup* m_icon_size_act_group = nullptr;
    QActionGroup* m_list_mode_act_group = nullptr;
    QActionGroup* m_theme_act_group = nullptr;
    QActionGroup* m_recent_files_group = nullptr;

    WindowThemes m_window_themes;
    GameListUtils m_game_list_utils;

    QScopedPointer<GameListFrame> m_game_list_frame;
    QScopedPointer<GameGridFrame> m_game_grid_frame;
    QScopedPointer<ElfViewer> m_elf_viewer;
    std::unique_ptr<GameCinematicFrame> m_game_cinematic_frame;
    std::unique_ptr<Frontend::WindowSDL> m_sdlWindow;

    QScopedPointer<QStatusBar> statusBar;

    PSF psf;
    std::filesystem::path currentlyRunningElf;

    std::shared_ptr<CompatibilityInfoClass> m_compat_info =
        std::make_shared<CompatibilityInfoClass>();

    QTranslator* translator;
    QString currentGameFilePath;

    // Gamepad support
    SDL_Gamepad* m_gamepad = nullptr;
    QTimer* m_gamepadTimer = nullptr;
    QTimer* m_gamepadLaunchTimer = nullptr;
    QTimer* m_gamepadFocusDelayTimer = nullptr;
    bool m_gamepadInitialized = false;
    bool m_gamepadFailed = false;
    bool m_usingJoystickFallback = false;
    bool m_gamepadInputBlocked = false;

    // Gamepad methods
    void initializeGamepad();
    void cleanupGamepad();
    void handleGamepadEvents();
    void handleJoystickInput();
    void navigateGamesUp();
    void navigateGamesDown();
    void navigateGamesLeft();
    void navigateGamesRight();
    void startGamepadLaunchDelay();
    void startGamepadFocusDelay();
    void unblockGamepadInput();
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event1) override {
        if (event1->mimeData()->hasUrls()) {
            event1->acceptProposedAction();
        }
    }
    void resizeEvent(QResizeEvent* event) override;
    bool use_for_all_queued = false;
};

extern MainWindow* g_MainWindow;