// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <memory>
#include <vector>
#include <QKeyEvent>

#include <QAudioOutput>
#include <QGraphicsOpacityEffect>
#include <QMap>

#include <QHBoxLayout>
#include <QLabel>
#include <QMediaPlayer>
#include <QObject>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

#include "common/path_util.h"
#include "compatibility_info.h"
#include "core/ipc/ipc_client.h"
#include "game_info.h"
#include "games_menu.h"
#include "qt_gui/game_list_frame.h"
#include "qt_gui/main_window_themes.h"

class VerticalGameActionsMenu : public QWidget {
    Q_OBJECT

public:
    explicit VerticalGameActionsMenu(QWidget* parent = nullptr) : QWidget(parent) {
        for (auto* btn : findChildren<QAbstractButton*>()) {
            btn->setFocusPolicy(Qt::NoFocus);
        }

        auto* rootLayout = new QVBoxLayout(this);
        rootLayout->setContentsMargins(20, 20, 20, 20);
        rootLayout->setSpacing(16);

        auto* mainHLayout = new QHBoxLayout();
        mainHLayout->setSpacing(20);
        mainHLayout->setAlignment(Qt::AlignTop);
        mainHLayout->setContentsMargins(0, 0, 0, 0);
        mainHLayout->setSpacing(20);
        mainHLayout->setAlignment(Qt::AlignTop);

        auto* column1 = new QVBoxLayout();
        column1->setSpacing(10);
        column1->setAlignment(Qt::AlignTop);
        setFocusPolicy(Qt::StrongFocus);

        auto* title1 = new QLabel(tr("Launch & Folders"), this);
        title1->setStyleSheet("color: white; font-size: 24px; font-weight: bold;");
        column1->addWidget(title1);

        m_launchBtn = createButton(tr("Launch Game"));
        m_openFolderBtn = createButton(tr("Open Game Folder"));
        m_openModsFolderBtn = createButton(tr("Open Mods Folder"));
        m_openUpdateFolderBtn = createButton(tr("Open Update Folder"));

        column1->addWidget(m_launchBtn);
        addSeparator(column1);
        column1->addWidget(m_openFolderBtn);
        column1->addWidget(m_openModsFolderBtn);
        column1->addWidget(m_openUpdateFolderBtn);

        auto* column2 = new QVBoxLayout();
        column2->setSpacing(10);
        column2->setAlignment(Qt::AlignTop);

        auto* title2 = new QLabel(tr("Tools & Config"), this);
        title2->setStyleSheet("color: white; font-size: 24px; font-weight: bold;");
        column2->addWidget(title2);

        m_gameConfigBtn = createButton(tr("Game Settings"));
        m_globalConfigBtn = createButton(tr("Global Settings"));
        m_modsManagerBtn = createButton(tr("Mods Manager"));
        m_cheatsBtn = createButton(tr("Cheats / Patches"));
        m_deleteShadersBtn = createButton(tr("Delete Shader Cache"));

        column2->addWidget(m_gameConfigBtn);
        column2->addWidget(m_globalConfigBtn);
        addSeparator(column2);
        column2->addWidget(m_modsManagerBtn);
        column2->addWidget(m_cheatsBtn);
        column2->addWidget(m_deleteShadersBtn);

        mainHLayout->addLayout(column1, 1);
        mainHLayout->addLayout(column2, 1);
        mainHLayout->addStretch();
        rootLayout->addLayout(mainHLayout);

        setFocusPolicy(Qt::StrongFocus);
        setFocusProxy(m_launchBtn);
        auto* sep = new QFrame(this);
        sep->setFrameShape(QFrame::HLine);
        sep->setStyleSheet("color: rgba(255,255,255,40);");

        rootLayout->addSpacing(8);
        rootLayout->addWidget(sep);
        rootLayout->addSpacing(8);

        const QList<QPushButton*> buttons = {
            m_launchBtn,           m_openFolderBtn, m_openModsFolderBtn,
            m_openUpdateFolderBtn, m_gameConfigBtn, m_globalConfigBtn,
            m_modsManagerBtn,      m_cheatsBtn,     m_deleteShadersBtn,
        };

        for (auto* btn : buttons) {
            if (btn)
                btn->setFocusPolicy(Qt::StrongFocus);
            btn->setProperty("no_arrow_nav", true);
        }
        for (auto* btn : buttons) {
            btn->installEventFilter(this);
        }

        connect(m_launchBtn, &QPushButton::clicked, this,
                &VerticalGameActionsMenu::launchRequested);
        connect(m_openFolderBtn, &QPushButton::clicked, this,
                &VerticalGameActionsMenu::openFolderRequested);
        connect(m_deleteShadersBtn, &QPushButton::clicked, this,
                &VerticalGameActionsMenu::deleteShadersRequested);

        connect(m_openModsFolderBtn, &QPushButton::clicked, this,
                &VerticalGameActionsMenu::openModsFolderRequested);
        connect(m_openUpdateFolderBtn, &QPushButton::clicked, this,
                &VerticalGameActionsMenu::openUpdateFolderRequested);

        connect(m_gameConfigBtn, &QPushButton::clicked, this,
                &VerticalGameActionsMenu::gameConfigRequested);
        connect(m_globalConfigBtn, &QPushButton::clicked, this,
                &VerticalGameActionsMenu::globalConfigRequested);
        connect(m_modsManagerBtn, &QPushButton::clicked, this,
                &VerticalGameActionsMenu::openModsRequested);
        connect(m_cheatsBtn, &QPushButton::clicked, this,
                &VerticalGameActionsMenu::openCheatsRequested);
    }

    void focusFirstButton() {
        if (m_launchBtn)
            m_launchBtn->setFocus(Qt::OtherFocusReason);
    }

signals:
    void launchRequested();
    void openFolderRequested();
    void deleteShadersRequested();

    void globalConfigRequested();
    void gameConfigRequested();
    void openModsRequested();
    void openCheatsRequested();

    void openModsFolderRequested();
    void openUpdateFolderRequested();
    void exitToGamesRequested();

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override {
        if (ev->type() == QEvent::KeyPress) {
            auto* e = static_cast<QKeyEvent*>(ev);

            switch (e->key()) {
            case Qt::Key_Left:
                emit exitToGamesRequested();
                return true;

            case Qt::Key_Return:
            case Qt::Key_Enter:
                if (auto* btn = qobject_cast<QPushButton*>(obj)) {
                    btn->click();
                    return true;
                }
                break;

            default:
                break;
            }
        }

        return QWidget::eventFilter(obj, ev);
    }

private:
    QPushButton* createButton(const QString& text) {
        auto* btn = new QPushButton(text, this);
        btn->setMinimumHeight(22);
        return btn;
    }

    void addSeparator(QVBoxLayout* layout) {
        auto* sep = new QFrame(this);
        sep->setFrameShape(QFrame::HLine);
        sep->setStyleSheet("color: rgba(255,255,255,40);");
        layout->addWidget(sep);
    }

private:
    QPushButton* m_launchBtn = nullptr;
    QPushButton* m_openFolderBtn = nullptr;
    QPushButton* m_openModsFolderBtn = nullptr;
    QPushButton* m_openUpdateFolderBtn = nullptr;

    QPushButton* m_gameConfigBtn = nullptr;
    QPushButton* m_globalConfigBtn = nullptr;
    QPushButton* m_modsManagerBtn = nullptr;
    QPushButton* m_cheatsBtn = nullptr;
    QPushButton* m_deleteShadersBtn = nullptr;
};

struct HubGameEntry {
    int index;
    QString name;
    QString serial;
    std::filesystem::path icon_path;
    QWidget* tile_widget = nullptr;
    QWidget* icon_widget = nullptr;
    QRect baseGeometry;
};

struct VerticalMenuItem {
    QString id;
    QIcon icon;
    QString tooltip;
};

class HubMenuWidget : public QWidget {
    Q_OBJECT

    enum class FocusArea { Games, ActionsMenu };

    FocusArea m_focusArea = FocusArea::Games;
    QRect enlargedFromBase(const QRect& base, qreal scale) const {
        if (!base.isValid())
            return base;

        const QPoint center = base.center();
        const QSize newSize(int(base.width() * scale), int(base.height() * scale));

        QRect r(QPoint(0, 0), newSize);
        r.moveCenter(center);
        return r;
    }

public:
    explicit HubMenuWidget(std::shared_ptr<GameInfoClass> gameInfo,
                           std::shared_ptr<CompatibilityInfoClass> compatInfo,
                           std::shared_ptr<IpcClient> ipcClient, WindowThemes* themes,
                           QWidget* parent = nullptr);
    ~HubMenuWidget() override;

signals:
    void launchRequestedFromHub(int gameIndex);
    void openFolderRequested(int gameIndex);
    void deleteShadersRequested(int gameIndex);

    void globalConfigRequested();
    void gameConfigRequested(int gameIndex);
    void openModsManagerRequested(int gameIndex);
    void openCheatsRequested(int gameIndex);

public slots:
    void showFull();
    void hideFull();
    void toggle();
    void setMinimalUi(bool hide);

protected:
    void buildUi();
    void positionActionsMenu();
    void focusInEvent(QFocusEvent* event) override;
    void buildAnimations();
    void applyTheme();
    void highlightSelectedGame();
    void centerSelectedGameAnimated();
    void ensureSelectionValid();
    void updateBackground(int gameIndex);
    void setDimVisible(bool visible);

    void resizeEvent(QResizeEvent* e) override;
    void requestCenterSelectedGame();
    void keyPressEvent(QKeyEvent* e) override;
    void showEvent(QShowEvent* ev) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;

    QWidget* buildGameTile(const GameInfo& g);
    void buildGameList();

    void buildVerticalSidebar();
    QWidget* buildVerticalMenuItem(const VerticalMenuItem& item);

    void executeGameAction(GameAction action);

    void onLaunchClicked();
    void onGlobalConfigClicked();
    void onGameConfigClicked();
    void onModsClicked();

private:
    std::shared_ptr<GameInfoClass> m_gameInfo;
    std::shared_ptr<CompatibilityInfoClass> m_compatInfo;
    std::shared_ptr<IpcClient> m_ipcClient;
    WindowThemes* m_themes = nullptr;

    QLabel* m_background = nullptr;
    QWidget* m_dim = nullptr;
    QScrollArea* m_scroll = nullptr;
    QWidget* m_gameContainer = nullptr;
    QWidget* m_sidebarContainer = nullptr;
    QVBoxLayout* m_sidebarLayout = nullptr;
    VerticalGameActionsMenu* m_actionsMenu = nullptr;
    HotkeysOverlay* m_hotkeysOverlay = nullptr;

    std::vector<HubGameEntry> m_games;
    std::vector<VerticalMenuItem> m_verticalMenuItems;

    int m_selectedIndex = -1;
    bool m_hideUi = false;
    bool m_centerPending = false;
    bool m_visible = false;
    bool m_navigationLocked = false;
    bool m_menuVisible = false;
    QMap<QWidget*, QPropertyAnimation*> m_tileAnimations;

    QGraphicsOpacityEffect* m_opacity = nullptr;
    QPropertyAnimation* m_fadeIn = nullptr;
    QPropertyAnimation* m_fadeOut = nullptr;
    QPropertyAnimation* m_scrollAnim = nullptr;

    QMediaPlayer* m_player = nullptr;
    QAudioOutput* m_audioOutput = nullptr;
};
