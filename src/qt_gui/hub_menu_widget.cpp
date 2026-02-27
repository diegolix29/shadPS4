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
    float scale = height() / 1080.0f;
    if (scale < 0.1f)
        scale = 0.7f;

    int tileDim = static_cast<int>(400 * scale);
    int iconDim = static_cast<int>(180 * scale);

    tile->setFixedSize(tileDim, tileDim);
    tile->setObjectName("VerticalSidebarTile");

    QHBoxLayout* layout = new QHBoxLayout(tile);

    layout->setContentsMargins(20, 12, 12, 12);
    layout->setSpacing(0);

    QLabel* iconLabel = new QLabel(tile);
    iconLabel->setFixedSize(iconDim, iconDim);
    iconLabel->setPixmap(item.icon.pixmap(iconDim, iconDim));
    iconLabel->setScaledContents(true);

    layout->addWidget(iconLabel, 0, Qt::AlignLeft | Qt::AlignVCenter);

    layout->addStretch();

    tile->setLayout(layout);
    return tile;
}

QSize HubMenuWidget::calculateTileSize() const {
    float scaleFactor = height() / 1080.0f;
    if (scaleFactor < 0.4f)
        scaleFactor = 0.4f;

    int dynamicWidth = static_cast<int>(width() * 0.65f);

    int finalWidth = qBound(500, dynamicWidth, 2500);

    int finalHeight = static_cast<int>(500 * scaleFactor);

    return QSize(finalWidth, finalHeight);
}

void HubMenuWidget::buildVerticalSidebar() {
    if (m_sidebarLayout) {
        QLayoutItem* item;
        while ((item = m_sidebarLayout->takeAt(0)) != nullptr) {
            if (item->widget()) {
                item->widget()->deleteLater();
            }
            delete item;
        }
    }

    m_verticalMenuItems.clear();

    QIcon emulatorIcon;
    emulatorIcon.addFile(QString::fromUtf8(":/images/shadps4.ico"), QSize(512, 512));

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
        root->addSpacing(static_cast<int>(40));
        root->addWidget(m_hotkeysOverlay);
    }
}

void HubMenuWidget::positionActionsMenu() {
    if (!m_actionsMenu || !m_scroll || !m_scroll->viewport())
        return;

    float scale = height() / 1080.0f;
    if (scale < 0.5f)
        scale = 0.5f;

    int menuWidth = static_cast<int>(420 * scale);
    int menuHeight = static_cast<int>(850 * scale);
    m_actionsMenu->setFixedSize(menuWidth, menuHeight);

    if (auto* root = m_actionsMenu->layout()) {
        int margin = static_cast<int>(20 * scale);
        int spacing = static_cast<int>(12 * scale);
        root->setContentsMargins(margin, margin, margin, margin);
        root->setSpacing(spacing);
    }

    for (auto* btn : m_actionsMenu->findChildren<QPushButton*>()) {
        btn->setMinimumHeight(static_cast<int>(50 * scale));
        int fontSize = static_cast<int>(18 * scale);
        btn->setStyleSheet(QString("QPushButton { padding: 4px; font-size: %1px; }").arg(fontSize));
    }

    if (m_hotkeysOverlay) {
        int overlayFontSize = static_cast<int>(16 * scale);
        int paddingV = static_cast<int>(6 * scale);
        int paddingH = static_cast<int>(10 * scale);

        m_hotkeysOverlay->setStyleSheet(QString("background: none; "
                                                "padding: %1px %2px; "
                                                "font-size: %3px; "
                                                "line-height: 1.2;")
                                            .arg(paddingV)
                                            .arg(paddingH)
                                            .arg(overlayFontSize));

        m_hotkeysOverlay->setMinimumHeight(0);
    }

    int viewportWidth = m_scroll->viewport()->width();
    int viewportHeight = m_scroll->viewport()->height();

    QSize tileSize = calculateTileSize();
    int sidebarWidth = m_sidebarContainer ? m_sidebarContainer->width() : 0;
    int leftMargin = static_cast<int>(60 * scale);

    int tileRightEdge = sidebarWidth + leftMargin + tileSize.width();

    int desiredGap = static_cast<int>(40 * scale);
    int finalX = tileRightEdge + desiredGap;

    int rightBoundLimit = static_cast<int>(viewportWidth * 0.85f) - menuWidth;
    if (finalX > rightBoundLimit) {
        finalX = rightBoundLimit;
    }

    int finalY = (viewportHeight - menuHeight) / 2;

    QPoint finalPos(std::max(0, finalX), std::max(0, finalY));
    QPoint startPos(finalPos.x() + 40, finalPos.y());

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
    animFade->setEndValue(1.0);

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
    int spacing = 150;

    for (auto& entry : m_games) {
        const auto& g = m_gameInfo->m_games[entry.index];

        QWidget* tile = buildGameTile(g);
        tile->setParent(m_gameContainer);
        tile->setProperty("game_index", entry.index);
        tile->installEventFilter(this);

        tile->resize(900, 300);
        tile->move(0, y);

        tile->setProperty("baseGeom", tile->geometry());

        tile->show();

        entry.tile_widget = tile;
        entry.icon_widget = tile->findChild<QWidget*>("game_icon_container");

        y += 300 + spacing;
    }

    m_gameContainer->setMinimumHeight(y + 1000);
}

void HubMenuWidget::repositionGameTiles() {
    if (!m_gameContainer)
        return;

    QSize tileSize = calculateTileSize();
    float scale = height() / 1080.0f;
    if (scale < 0.5f)
        scale = 0.5f;

    int spacing = static_cast<int>(5 * scale);
    int leftMargin = static_cast<int>(40 * scale);
    int currentY = static_cast<int>(600 * scale);

    for (auto& entry : m_games) {
        QWidget* tile = entry.tile_widget;
        if (!tile)
            continue;

        tile->setFixedSize(tileSize);

        int x = leftMargin;

        QRect newBase(x, currentY, tileSize.width(), tileSize.height());
        tile->setProperty("baseGeom", newBase);
        tile->move(x, currentY);

        currentY += tileSize.height() + spacing;
    }
    m_gameContainer->setMinimumHeight(currentY + static_cast<int>(800 * scale));
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
    tile->setStyleSheet("background: transparent;");
    float scale = height() / 1080.0f;
    if (scale < 0.1f)
        scale = 1.0f;

    QSize tileSize = calculateTileSize();
    tile->setFixedSize(tileSize);

    int iconSize = static_cast<int>(320 * scale);

    QHBoxLayout* h = new QHBoxLayout(tile);
    h->setSpacing(static_cast<int>(tileSize.width() * 0.08f));
    h->setContentsMargins(static_cast<int>(tileSize.width() * 0.05f), 10, 40, 10);

    QWidget* iconContainer = new QWidget(tile);
    iconContainer->setObjectName("game_icon_container");
    iconContainer->setFixedSize(iconSize, iconSize);

    QLabel* cover = new QLabel(iconContainer);
    cover->setScaledContents(true);
    cover->setFixedSize(iconSize, iconSize);

    bool hasGameIcon = !g.icon.isNull() || !g.icon_path.empty();
    cover->setPixmap(hasGameIcon ? LoadGameIcon(g, iconSize) : LoadShadIcon(iconSize));

    h->addWidget(iconContainer);

    QVBoxLayout* textLayout = new QVBoxLayout();
    textLayout->setSpacing(5);

    ScrollingLabel* title = new ScrollingLabel(tile);
    title->setText(QString::fromStdString(g.name));

    int fontSize = static_cast<int>(tileSize.height() * 0.18f);
    title->setStyleSheet(
        QString("color: white; font-size: %1px; font-weight: bold;").arg(fontSize));
    title->setFixedHeight(fontSize + 20);

    QLabel* cusa = new QLabel(tile);
    cusa->setText(QString::fromStdString(g.serial));
    int cusaSize = static_cast<int>(fontSize * 0.5f);
    cusa->setStyleSheet(QString("color: lightgray; font-size: %1px;").arg(cusaSize));

    textLayout->addStretch();
    textLayout->addWidget(title);
    textLayout->addWidget(cusa);
    textLayout->addStretch();

    h->addLayout(textLayout, 1);

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

static QRect scaleFromCenter(const QRect& base, qreal scale) {
    QPoint center = base.center();

    int newW = int(base.width() * scale);
    int newH = int(base.height() * scale);

    QRect r;
    r.setSize(QSize(newW, newH));
    r.moveCenter(center);

    return r;
}

void HubMenuWidget::highlightSelectedGame() {
    if (m_games.empty())
        return;

    int windowWidth = this->width();
    int sidebarWidth = m_sidebarContainer ? m_sidebarContainer->width() : 0;

    for (int i = 0; i < (int)m_games.size(); ++i) {
        QWidget* tile = m_games[i].tile_widget;
        if (!tile)
            continue;

        bool isSelected = (i == m_selectedIndex);
        qreal scale = isSelected ? 1.2 : 1.6;

        tile->setMinimumSize(0, 0);
        tile->setMaximumSize(16777215, 16777215);

        QRect baseRect = tile->property("baseGeom").toRect();

        int targetW = baseRect.width() * scale;
        int targetH = baseRect.height() * scale;

        int targetX = (windowWidth / 2) - sidebarWidth - (targetW / 2) + 300;

        int heightDiff = targetH - baseRect.height();
        int targetY = baseRect.y() - (heightDiff / 2);

        QRect targetRect(targetX, targetY, targetW, targetH);

        QPropertyAnimation* anim = new QPropertyAnimation(tile, "geometry");
        anim->setDuration(250);
        anim->setStartValue(tile->geometry());
        anim->setEndValue(targetRect);
        anim->setEasingCurve(QEasingCurve::OutBack);
        anim->start(QAbstractAnimation::DeleteWhenStopped);
    }

    updateBackground(m_selectedIndex);
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

    float scale = height() / 1080.0f;
    if (scale < 0.1f)
        scale = 0.5f;

    int sidebarWidth = static_cast<int>(width() * 0.22f);
    m_sidebarContainer->setFixedWidth(qBound(200, sidebarWidth, 270));

    buildVerticalSidebar();
    repositionGameTiles();

    if (m_actionsMenu && !m_actionsMenu->isHidden()) {
        positionActionsMenu();
    }

    updateBackground(m_selectedIndex);
    requestCenterSelectedGame();
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
