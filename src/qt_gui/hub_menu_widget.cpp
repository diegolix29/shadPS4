// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/config.h"
#include "common/logging/log.h"
#include "gui_context_menus.h"
#include "hub_menu_widget.h"

#include <QApplication>

#include <QAudioOutput>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QParallelAnimationGroup>
#include <QPixmap>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <cmrc/cmrc.hpp>
#include "games_menu.h"

CMRC_DECLARE(res);

static QPixmap LoadGameIcon(const GameInfo& game, int size) {
    QPixmap source;
    if (!game.icon.isNull()) {
        source = QPixmap::fromImage(game.icon);
    } else if (!game.icon_path.empty()) {
        QString iconPath;
        Common::FS::PathToQString(iconPath, game.icon_path);
        source.load(iconPath);
    } else {
        source.load(":/images/default_game_icon.png");
    }

    if (source.isNull())
        return QPixmap();

    return source.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

HubMenuWidget::HubMenuWidget(std::shared_ptr<GameInfoClass> gameInfo,
                             std::shared_ptr<CompatibilityInfoClass> compatInfo,
                             std::shared_ptr<IpcClient> ipcClient, WindowThemes* themes,
                             QWidget* parent)
    : QWidget(parent), m_gameInfo(gameInfo), m_compatInfo(compatInfo), m_ipcClient(ipcClient),
      m_themes(themes) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setAutoFillBackground(false);
    setFocusPolicy(Qt::StrongFocus);

    for (int i = 0; i < m_gameInfo->m_games.size(); ++i) {
        const auto& g = m_gameInfo->m_games[i];
        HubGameEntry entry = {i, QString::fromStdString(g.name), QString::fromStdString(g.serial),
                              g.icon_path, nullptr};
        m_games.push_back(entry);
    }

    buildUi();
    buildAnimations();

    Theme th = static_cast<Theme>(Config::getMainWindowTheme());
    m_themes->SetWindowTheme(th, nullptr);
    m_themes->ApplyThemeToWidget(this);
    applyTheme();

    if (!m_games.empty()) {
        m_selectedIndex = 0;
        highlightSelectedGame();
    }
}

HubMenuWidget::~HubMenuWidget() = default;

QWidget* HubMenuWidget::buildVerticalMenuItem(const VerticalMenuItem& item) {
    QWidget* tile = new QWidget(this);
    tile->setFixedWidth(450);
    tile->setMinimumHeight(450);
    tile->setObjectName("VerticalSidebarTile");

    QHBoxLayout* layout = new QHBoxLayout(tile);
    layout->setContentsMargins(212, 12, 12, 12);
    layout->setSpacing(14);
    layout->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    tile->setStyleSheet("background-color: transparent;");
    QLabel* iconLabel = new QLabel(tile);
    iconLabel->setFixedSize(180, 180);
    iconLabel->setAlignment(Qt::AlignCenter);
    iconLabel->setPixmap(item.icon.pixmap(196, 196));

    layout->addWidget(iconLabel);
    layout->addStretch();

    tile->setLayout(layout);

    return tile;
}

void HubMenuWidget::buildVerticalSidebar() {
    m_verticalMenuItems.clear();

    QIcon emulatorIcon;
    emulatorIcon.addFile(QString::fromUtf8(":/images/shadps4.ico"), QSize(450, 450), QIcon::Normal,
                         QIcon::Off);
    m_verticalMenuItems.push_back({
        "emulator_home",
        emulatorIcon,
        "Emulator Home",
    });

    for (const auto& item : m_verticalMenuItems) {
        QWidget* tile = buildVerticalMenuItem(item);
        m_sidebarLayout->addWidget(tile);
        m_sidebarLayout->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

        QLabel* iconLabel = tile->findChild<QLabel*>();
        if (iconLabel && item.id == "emulator_home") {
            iconLabel->setCursor(Qt::PointingHandCursor);
            iconLabel->installEventFilter(this);
            iconLabel->setProperty("isShadIcon", true);
        }
    }
}

void HubMenuWidget::executeGameAction(GameAction action) {
    ensureSelectionValid();

    if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_gameInfo->m_games.size()))
        return;

    GuiContextMenus ctx;

    ctx.ExecuteGameAction(
        action, m_selectedIndex, m_gameInfo->m_games, m_compatInfo, m_ipcClient, nullptr,
        [this](QStringList args) { emit launchRequestedFromHub(m_selectedIndex); });

    highlightSelectedGame();
}

void HubMenuWidget::buildUi() {
    QHBoxLayout* mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    m_background = new QLabel(this);
    m_background->setScaledContents(true);
    m_background->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);

    m_background->setGeometry(rect());
    m_background->lower();

    m_dim = new QLabel(this);
    m_dim->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_dim->setStyleSheet("background-color: rgba(255,255,255,140);");
    m_dim->setGeometry(rect());
    m_dim->lower();

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    m_scroll->setStyleSheet("background: transparent;");
    m_scroll->viewport()->setStyleSheet("background: transparent;");

    m_gameContainer = new QWidget(m_scroll);
    m_scroll->setWidget(m_gameContainer);
    m_gameContainer->move(-500, 0);

    m_sidebarContainer = new QWidget(this);
    m_sidebarContainer->setFixedWidth(400);
    m_sidebarContainer->setFocusPolicy(Qt::StrongFocus);
    m_sidebarContainer->setAttribute(Qt::WA_TransparentForMouseEvents, false);

    for (auto* child : m_sidebarContainer->findChildren<QWidget*>()) {
        child->setFocusPolicy(Qt::StrongFocus);
        child->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    }
    m_sidebarLayout = new QVBoxLayout(m_sidebarContainer);
    m_sidebarLayout->setContentsMargins(0, 40, 0, 0);
    m_sidebarLayout->setSpacing(20);
    m_sidebarLayout->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    buildVerticalSidebar();

    m_actionsMenu = new VerticalGameActionsMenu(m_scroll->viewport());
    m_actionsMenu->setStyleSheet("background-color: rgba(0, 0, 0, 180);"
                                 "border-left: 1px solid #57a1ff;");
    m_actionsMenu->setFixedWidth(420);
    m_actionsMenu->setFixedHeight(600);
    m_actionsMenu->hide();
    m_actionsMenu->installEventFilter(this);

    mainLayout->addWidget(m_sidebarContainer);
    mainLayout->addWidget(m_scroll, 1);

    connect(m_actionsMenu, &VerticalGameActionsMenu::launchRequested, this,
            [this]() { executeGameAction(GameAction::LaunchDefault); });

    connect(m_actionsMenu, &VerticalGameActionsMenu::openFolderRequested, this,
            [this]() { executeGameAction(GameAction::OpenGameFolder); });

    connect(m_actionsMenu, &VerticalGameActionsMenu::deleteShadersRequested, this,
            [this]() { executeGameAction(GameAction::DeleteShaderCache); });

    connect(m_actionsMenu, &VerticalGameActionsMenu::globalConfigRequested, this,
            [this]() { emit globalConfigRequested(); });

    connect(m_actionsMenu, &VerticalGameActionsMenu::openModsFolderRequested, this,
            [this]() { executeGameAction(GameAction::OpenModsFolder); });

    connect(m_actionsMenu, &VerticalGameActionsMenu::openUpdateFolderRequested, this,
            [this]() { executeGameAction(GameAction::OpenUpdateFolder); });

    connect(m_actionsMenu, &VerticalGameActionsMenu::gameConfigRequested, this, [this]() {
        ensureSelectionValid();
        if (m_selectedIndex >= 0)
            emit gameConfigRequested(m_selectedIndex);
    });

    connect(m_actionsMenu, &VerticalGameActionsMenu::openModsRequested, this, [this]() {
        ensureSelectionValid();
        if (m_selectedIndex >= 0)
            emit openModsManagerRequested(m_selectedIndex);
    });

    connect(m_actionsMenu, &VerticalGameActionsMenu::openCheatsRequested, this, [this]() {
        ensureSelectionValid();
        if (m_selectedIndex >= 0)
            emit openCheatsRequested(m_selectedIndex);
    });

    connect(m_actionsMenu, &VerticalGameActionsMenu::exitToGamesRequested, this, [this]() {
        m_focusArea = FocusArea::Games;
        m_actionsMenu->clearFocus();
        setFocus(Qt::OtherFocusReason);
        highlightSelectedGame();
    });

    m_hotkeysOverlay = new HotkeysOverlay(Qt::Vertical, m_actionsMenu);
    m_hotkeysOverlay->setTitle("Hotkeys & Navigation Keys");
    m_hotkeysOverlay->setHotkeys({{"Arrow Up/Down", "Navigate Games/Buttons"},
                                  {"Arrow Right", "Focus on Buttons"},
                                  {"Arrow Left", "Focus on Games"},
                                  {"Enter/Space", "Select/Play"},
                                  {"Backspace", "Hide/Show Games and Buttons"},
                                  {"Press - P - ", "Play Highlighted Game"},
                                  {"Press - M - ", "Mods Manager"},
                                  {"Press - G - ", "Games Settings"},
                                  {"Press - S - ", "Global Settings"},
                                  {"Press - H - ", "Hotkeys Setup"},
                                  {"Esc/Click on Fork Icon", "Exit"}});
    m_hotkeysOverlay->setStyleSheet("background: none; padding: 6px 12px;");
    m_background->raise();
    m_background->lower();
    m_dim->raise();
    m_scroll->raise();
    buildGameList();
    if (auto* root = qobject_cast<QVBoxLayout*>(m_actionsMenu->layout())) {
        root->addWidget(m_hotkeysOverlay);
    }
}

void HubMenuWidget::positionActionsMenu() {
    if (!m_actionsMenu || !m_scroll || !m_scroll->viewport())
        return;

    if (m_scroll->viewport()->width() < 100 || m_scroll->viewport()->height() < 100) {
        m_actionsMenu->hide();
        return;
    }

    QList<QAbstractAnimation*> oldAnims = m_actionsMenu->findChildren<QAbstractAnimation*>();
    for (auto* anim : oldAnims) {
        anim->stop();
        anim->deleteLater();
    }

    int viewportWidth = m_scroll->viewport()->width();
    int viewportHeight = m_scroll->viewport()->height();

    int fixed_x = 580;
    int fixed_y = (viewportHeight - m_actionsMenu->height()) / 2;

    int finalX = std::clamp(fixed_x, 0, viewportWidth - m_actionsMenu->width());
    int finalY = std::clamp(fixed_y, 0, viewportHeight - m_actionsMenu->height());
    QPoint finalPos(finalX, finalY);
    QPoint startPos(finalX - 60, finalY);

    auto* eff = qobject_cast<QGraphicsOpacityEffect*>(m_actionsMenu->graphicsEffect());
    if (!eff) {
        eff = new QGraphicsOpacityEffect(m_actionsMenu);
        m_actionsMenu->setGraphicsEffect(eff);
    }

    m_actionsMenu->move(startPos);
    eff->setOpacity(0.0);
    m_actionsMenu->show();

    QParallelAnimationGroup* group = new QParallelAnimationGroup(m_actionsMenu);

    QPropertyAnimation* animFade = new QPropertyAnimation(eff, "opacity");
    animFade->setDuration(250);
    animFade->setStartValue(0.0);
    animFade->setEndValue(1.0);
    animFade->setEasingCurve(QEasingCurve::OutQuad);

    QPropertyAnimation* animMove = new QPropertyAnimation(m_actionsMenu, "pos");
    animMove->setDuration(300);
    animMove->setStartValue(startPos);
    animMove->setEndValue(finalPos);
    animMove->setEasingCurve(QEasingCurve::OutCubic);

    group->addAnimation(animFade);
    group->addAnimation(animMove);

    connect(group, &QAbstractAnimation::finished, group, &QObject::deleteLater);
    group->start();
}
void HubMenuWidget::setMinimalUi(bool hide) {
    m_hideUi = hide;

    if (m_sidebarContainer)
        m_sidebarContainer->setVisible(!hide);

    if (m_actionsMenu)
        m_actionsMenu->setVisible(!hide && m_menuVisible);

    if (m_dim)
        m_dim->setVisible(!hide);

    if (m_scroll)
        m_scroll->setVisible(!hide);

    highlightSelectedGame();
    requestCenterSelectedGame();
}

void HubMenuWidget::buildGameList() {
    int y = 650;
    int spacing = 200;

    for (auto& entry : m_games) {
        const auto& g = m_gameInfo->m_games[entry.index];
        QWidget* tile = buildGameTile(g);
        tile->setParent(m_gameContainer);
        tile->setProperty("game_index", entry.index);
        tile->installEventFilter(this);

        int startX = (m_gameContainer->width() - tile->width()) / 2 + 100;
        tile->move(startX, y);
        tile->show();

        entry.tile_widget = tile;
        entry.icon_widget = tile->findChild<QWidget*>("game_icon_container");
        tile->setProperty("baseGeom", tile->geometry());

        y += tile->height() + spacing;
    }

    if (!m_games.empty()) {
        QWidget* lastTile = m_games.back().tile_widget;
        int totalHeight = lastTile->y() + lastTile->height();
        m_gameContainer->setMinimumHeight(totalHeight + 50);
    }
}

static QPixmap LoadShadIcon(int size) {
    QPixmap pm(":/images/shadps4.ico");
    if (pm.isNull())
        return QPixmap();

    return pm.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

QWidget* HubMenuWidget::buildGameTile(const GameInfo& g) {
    AnimatedTile* tile = new AnimatedTile(m_gameContainer);
    tile->setAttribute(Qt::WA_TranslucentBackground);
    tile->setContentsMargins(0, 0, 0, 0);
    tile->setStyleSheet("background: transparent;");

    tile->setMinimumHeight(300);
    tile->setMaximumHeight(450);

    QHBoxLayout* h = new QHBoxLayout(tile);
    h->setSpacing(35);
    h->setContentsMargins(30, 20, 30, 20);

    QWidget* iconContainer = new QWidget(tile);
    iconContainer->setObjectName("game_icon_container");

    iconContainer->setFixedSize(250, 250);
    iconContainer->setAttribute(Qt::WA_TranslucentBackground);

    QLabel* cover = new QLabel(iconContainer);
    cover->setScaledContents(true);

    cover->setFixedSize(250, 250);

    bool hasGameIcon = !g.icon.isNull() || !g.icon_path.empty();

    if (hasGameIcon) {
        cover->setPixmap(LoadGameIcon(g, 250));
    } else {
        cover->setPixmap(LoadShadIcon(540));
        cover->setAlignment(Qt::AlignCenter);
    }

    cover->move(0, 0);

    h->addWidget(iconContainer);

    QVBoxLayout* textLayout = new QVBoxLayout();
    textLayout->setAlignment(Qt::AlignVCenter);

    ScrollingLabel* title = new ScrollingLabel(tile);
    title->setText(QString::fromStdString(g.name));

    title->setStyleSheet("color: white; font-size: 52px; font-weight: bold;");
    title->setFixedHeight(65);

    QLabel* cusa = new QLabel(tile);
    cusa->setText(QString::fromStdString(g.serial));

    cusa->setStyleSheet("color: lightgray; font-size: 26px;");
    cusa->setFixedHeight(35);

    textLayout->addWidget(title);
    textLayout->addWidget(cusa);
    textLayout->addStretch();

    h->addLayout(textLayout);
    h->addStretch();
    return tile;
}

void HubMenuWidget::applyTheme() {
    if (!m_themes)
        return;

    QString textColor = m_themes->textColor().name();

    for (auto* label : findChildren<QLabel*>()) {
        label->setStyleSheet(QString("color: %1;").arg(textColor));
    }

    if (m_scroll)
        m_scroll->setStyleSheet("background: transparent;");

    setAutoFillBackground(true);

    if (m_dim) {
        m_dim->setGeometry(rect());
        m_dim->raise();
    }
}

void HubMenuWidget::buildAnimations() {
    m_opacity = new QGraphicsOpacityEffect(this);
    setGraphicsEffect(m_opacity);
    m_opacity->setOpacity(0.0);

    m_fadeIn = new QPropertyAnimation(m_opacity, "opacity", this);
    m_fadeIn->setDuration(250);
    m_fadeIn->setStartValue(0.0);
    m_fadeIn->setEndValue(1.0);

    m_fadeOut = new QPropertyAnimation(m_opacity, "opacity", this);
    m_fadeOut->setDuration(200);
    m_fadeOut->setStartValue(1.0);
    m_fadeOut->setEndValue(0.0);

    connect(m_fadeOut, &QPropertyAnimation::finished, this, [this]() {
        QWidget::hide();
        m_visible = false;
    });

    m_scrollAnim = new QPropertyAnimation(m_scroll->verticalScrollBar(), "value", this);
    m_scrollAnim->setDuration(300);
    m_scrollAnim->setEasingCurve(QEasingCurve::OutCubic);
    m_scroll->verticalScrollBar()->setValue((m_scroll->verticalScrollBar()->maximum()) / 2);
    connect(m_scrollAnim, &QPropertyAnimation::finished, this, [this]() {
        m_navigationLocked = false;
        positionActionsMenu();
    });
}

void HubMenuWidget::centerSelectedGameAnimated() {
    if (!m_scroll || m_selectedIndex < 0 || m_selectedIndex >= m_games.size())
        return;

    if (m_scrollAnim->state() == QAbstractAnimation::Running)
        return;

    QWidget* tile = m_games[m_selectedIndex].tile_widget;
    QWidget* icon = m_games[m_selectedIndex].icon_widget;
    if (!tile || !icon)
        return;

    if (m_actionsMenu)
        m_actionsMenu->hide();

    int viewportH = m_scroll->viewport()->height();
    int iconCenterY_local = icon->geometry().center().y();
    int iconCenterY_content = tile->geometry().top() + iconCenterY_local;

    int target = iconCenterY_content - viewportH / 2;
    target = std::clamp(target, 0, m_scroll->verticalScrollBar()->maximum());

    int current = m_scroll->verticalScrollBar()->value();

    if (current != target) {
        m_navigationLocked = true;
        m_scrollAnim->stop();
        m_scrollAnim->setStartValue(current);
        m_scrollAnim->setEndValue(target);
        m_scrollAnim->start();
    } else {
        positionActionsMenu();
    }
}

void HubMenuWidget::highlightSelectedGame() {
    if (m_games.empty())
        return;

    setDimVisible(true);

    raise();
    setFocus();

    ensureSelectionValid();

    requestCenterSelectedGame();
    m_fadeIn->start();
    m_menuVisible = true;
    updateBackground(m_selectedIndex);

    for (int i = 0; i < (int)m_games.size(); ++i) {
        QWidget* tile = m_games[i].tile_widget;
        if (!tile)
            continue;

        QRect base = tile->property("baseGeom").toRect();
        if (!base.isValid())
            continue;
    }
}

void HubMenuWidget::keyPressEvent(QKeyEvent* e) {
    if (m_navigationLocked) {
        e->accept();
        return;
    }

    if (e->key() == Qt::Key_Down) {
        m_actionsMenu->hide();

        if (m_selectedIndex + 1 < (int)m_games.size()) {
            m_navigationLocked = true;
            m_selectedIndex++;
            highlightSelectedGame();
            requestCenterSelectedGame();
        }
        e->accept();
        return;
    }

    if (e->key() == Qt::Key_Up) {
        m_actionsMenu->hide();

        if (m_selectedIndex > 0) {
            m_navigationLocked = true;
            m_selectedIndex--;
            highlightSelectedGame();
            requestCenterSelectedGame();
        }
        e->accept();
        return;
    }

    if (e->key() == Qt::Key_Right) {
        if (m_actionsMenu && m_menuVisible) {
            m_focusArea = FocusArea::ActionsMenu;
            m_actionsMenu->setFocus(Qt::OtherFocusReason);
            m_actionsMenu->focusFirstButton();
        }
        e->accept();
        return;
    }

    if (e->key() == Qt::Key_Escape) {
        hideFull();
        e->accept();
        return;
    }
    if (e->key() == Qt::Key_C) {
        ensureSelectionValid();
        if (m_selectedIndex >= 0)
            emit openCheatsRequested(m_selectedIndex);

        e->accept();
        return;
    }

    if (e->key() == Qt::Key_P) {
        onLaunchClicked();
        e->accept();
        return;
    }

    if (e->key() == Qt::Key_M) {
        ensureSelectionValid();
        if (m_selectedIndex >= 0)
            emit openModsManagerRequested(m_selectedIndex);
        e->accept();
        return;
    }

    if (e->key() == Qt::Key_S) {
        emit globalConfigRequested();
        e->accept();
        return;
    }

    if (e->key() == Qt::Key_G) {
        ensureSelectionValid();
        if (m_selectedIndex >= 0)
            emit gameConfigRequested(m_selectedIndex);
        e->accept();
        return;
    }

    if (e->key() == Qt::Key_Left) {
        m_focusArea = FocusArea::Games;
        setFocus(Qt::OtherFocusReason);
        highlightSelectedGame();
        e->accept();
        return;
    }

    if (e->key() == Qt::Key_Backspace) {
        setMinimalUi(!m_hideUi);
        e->accept();
        return;
    }

    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter || e->key() == Qt::Key_Space) {
        if (m_focusArea == FocusArea::Games) {
            onLaunchClicked();
        }
        e->accept();
        return;
    }

    QWidget::keyPressEvent(e);
}

void HubMenuWidget::setDimVisible(bool visible) {
    int targetAlpha = visible ? 180 : 0;

    QColor themeColor = this->palette().color(QPalette::Window);
    int r = themeColor.red();
    int g = themeColor.green();
    int b = themeColor.blue();

    QObject::disconnect(nullptr, nullptr, m_dim, nullptr);

    QPropertyAnimation* alphaAnimation = new QPropertyAnimation(m_dim, "alphaChannel", this);
    alphaAnimation->setDuration(220);
    alphaAnimation->setEasingCurve(QEasingCurve::OutQuad);

    int startAlpha = 0;
    QString currentStyle = m_dim->styleSheet();
    if (currentStyle.contains(QString("rgba(%1, %2, %3, 0)").arg(r).arg(g).arg(b))) {
        startAlpha = 0;
    } else if (visible) {
        startAlpha = 0;
    } else {
        startAlpha = 180;
    }

    alphaAnimation->setStartValue(startAlpha);
    alphaAnimation->setEndValue(targetAlpha);

    QObject::connect(
        alphaAnimation, &QPropertyAnimation::valueChanged, m_dim,
        [this, r, g, b](const QVariant& value) {
            int alpha = value.toInt();
            m_dim->setStyleSheet(
                QString("background-color: rgba(%1, %2, %3, %4);").arg(r).arg(g).arg(b).arg(alpha));
        });

    QObject::connect(alphaAnimation, &QPropertyAnimation::finished, alphaAnimation,
                     &QObject::deleteLater);

    QObject::connect(alphaAnimation, &QPropertyAnimation::finished, m_dim,
                     [this, r, g, b, targetAlpha] {
                         m_dim->setStyleSheet(QString("background-color: rgba(%1, %2, %3, %4);")
                                                  .arg(r)
                                                  .arg(g)
                                                  .arg(b)
                                                  .arg(targetAlpha));
                     });
    alphaAnimation->start();
}

bool HubMenuWidget::eventFilter(QObject* obj, QEvent* ev) {
    QLabel* lbl = qobject_cast<QLabel*>(obj);
    if (lbl && ev->type() == QEvent::MouseButtonRelease) {
        if (lbl->property("isShadIcon").toBool()) {
            hideFull();
            return true;
        }
    }

    QWidget* tile = qobject_cast<QWidget*>(obj);
    if (tile) {
        int index = tile->property("game_index").toInt();

        if (obj == m_actionsMenu || m_actionsMenu->isAncestorOf(tile)) {
            if (ev->type() == QEvent::KeyPress) {
                auto* keyEv = static_cast<QKeyEvent*>(ev);
                if (keyEv->key() == Qt::Key_Left) {
                    m_focusArea = FocusArea::Games;
                    m_actionsMenu->clearFocus();
                    setFocus(Qt::OtherFocusReason);
                    highlightSelectedGame();
                    return true;
                }
            }
        }

        if (ev->type() == QEvent::MouseButtonRelease) {
            if (m_selectedIndex != index) {
                m_selectedIndex = index;
                highlightSelectedGame();
                requestCenterSelectedGame();
            }
            setFocus();
            return true;
        }

        if (ev->type() == QEvent::MouseButtonDblClick) {
            m_selectedIndex = index;
            onLaunchClicked();
            return true;
        }
    }

    return QWidget::eventFilter(obj, ev);
}

void HubMenuWidget::onLaunchClicked() {
    ensureSelectionValid();
    setMinimalUi(!m_hideUi);

    if (m_selectedIndex >= 0 && m_selectedIndex < (int)m_games.size()) {
        if (m_player)
            m_player->stop();

        emit launchRequestedFromHub(m_selectedIndex);
    }
    if (!Config::getGameRunning()) {
        setMinimalUi(!m_hideUi);
    }
}

void HubMenuWidget::focusInEvent(QFocusEvent* event) {
    QWidget::focusInEvent(event);

    if (!Config::getGameRunning()) {
        return;
    }
    setMinimalUi(!m_hideUi);
}

void HubMenuWidget::onGlobalConfigClicked() {
    emit globalConfigRequested();
}

void HubMenuWidget::onGameConfigClicked() {
    ensureSelectionValid();
    emit gameConfigRequested(m_selectedIndex);
}

void HubMenuWidget::onModsClicked() {
    ensureSelectionValid();
    emit openModsManagerRequested(m_selectedIndex);
}

void HubMenuWidget::showFull() {
    if (m_visible)
        return;

    setWindowFlag(Qt::FramelessWindowHint);
    setWindowState(Qt::WindowFullScreen);
    setDimVisible(true);

    show();
    raise();
    setFocus();

    ensureSelectionValid();
    updateBackground(m_selectedIndex);
    highlightSelectedGame();

    requestCenterSelectedGame();
    m_fadeIn->start();
    m_visible = true;
    m_menuVisible = true;
}

void HubMenuWidget::hideFull() {
    setDimVisible(false);
    m_fadeOut->start();
    m_actionsMenu->hide();
}

void HubMenuWidget::toggle() {
    if (m_visible)
        hideFull();
    else
        showFull();
}

void HubMenuWidget::ensureSelectionValid() {
    if (m_selectedIndex < 0 && !m_games.empty())
        m_selectedIndex = 0;
    if (m_selectedIndex >= (int)m_games.size())
        m_selectedIndex = (int)m_games.size() - 1;
}

void HubMenuWidget::updateBackground(int gameIndex) {

    if (!m_background || gameIndex < 0 || gameIndex >= m_gameInfo->m_games.size()) {
        m_background->clear();
        return;
    }

    const auto& g = m_gameInfo->m_games[gameIndex];

    if (!g.pic_path.empty()) {
        QString path;
        Common::FS::PathToQString(path, g.pic_path);
        QImage img(path);

        if (!img.isNull()) {
            QSize widgetSize = size();

            QPixmap scaled = QPixmap::fromImage(img).scaled(
                widgetSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);

            m_background->setPixmap(scaled);

            m_background->setGeometry(rect());
            m_dim->setGeometry(rect());
            m_background->lower();
            m_dim->raise();
            m_scroll->raise();
        } else {
            m_background->clear();
        }
    } else {
        m_background->clear();
    }
}

void HubMenuWidget::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);

    m_background->setGeometry(rect());
    m_dim->setGeometry(rect());
    m_dim->raise();
    m_scroll->raise();

    highlightSelectedGame();
    updateBackground(m_selectedIndex);
    requestCenterSelectedGame();
}
static QRect enlargedFromBase(const QRect& base, qreal scale) {
    if (!base.isValid())
        return base;

    int newWidth = int(base.width() * scale);
    int newHeight = int(base.height() * scale);

    int dx = (newWidth - base.width()) / 2;
    int dy = (newHeight - base.height()) / 2;

    return QRect(base.topLeft() - QPoint(dx, dy), QSize(newWidth, newHeight));
}

void HubMenuWidget::requestCenterSelectedGame() {
    if (m_centerPending)
        return;

    m_centerPending = true;

    QTimer::singleShot(0, this, [this]() {
        m_centerPending = false;
        centerSelectedGameAnimated();
    });
}

void HubMenuWidget::showEvent(QShowEvent* ev) {
    QWidget::showEvent(ev);

    QTimer::singleShot(0, this, [this]() {
        ensureSelectionValid();
        highlightSelectedGame();
        requestCenterSelectedGame();
    });
}
