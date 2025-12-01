// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QApplication>
#include <QGraphicsOpacityEffect>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include "common/path_util.h"
#include "game_info.h"
#include "games_menu.h"

BigPictureWidget::BigPictureWidget(std::shared_ptr<GameInfoClass> gameInfo,
                                   std::shared_ptr<CompatibilityInfoClass> compatInfo,
                                   std::shared_ptr<IpcClient> ipcClient, WindowThemes* themes,
                                   QWidget* parent)
    : QWidget(parent), m_gameInfo(gameInfo), m_compatInfo(compatInfo), m_ipcClient(ipcClient),
      m_themes(themes) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setAutoFillBackground(false);
    setAttribute(Qt::WA_TranslucentBackground, true);
    buildUi();
    buildAnimations();
    Theme th = static_cast<Theme>(Config::getMainWindowTheme());
    m_window_themes.SetWindowTheme(th, nullptr);
    m_window_themes.ApplyThemeToWidget(this);
    applyTheme();
}

BigPictureWidget::~BigPictureWidget() {}

void BigPictureWidget::buildUi() {
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(12);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setFrameShape(QFrame::HLine);
    m_scroll->setFrameShadow(QFrame::Sunken);

    m_scroll->setStyleSheet("background: transparent;");
    m_scroll->viewport()->setStyleSheet("background: transparent;");

    m_container = new QWidget(m_scroll);

    for (int i = 0; i < m_gameInfo->m_games.size(); i++) {
        QWidget* tile = buildTile(m_gameInfo->m_games[i]);
        tile->setParent(m_container);
        tile->setProperty("game_index", i);
        tile->installEventFilter(this);
        m_tiles.push_back(tile);
    }
    QTimer::singleShot(0, this, [this]() {
        layoutTiles();
        highlightSelectedTile();
        centerSelectedTileAnimated();
    });

    m_scroll->setWidget(m_container);

    m_background = new QLabel(this);
    m_background->setScaledContents(true);
    m_background->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_background->setGeometry(rect());
    m_background->lower();

    m_dim = new QLabel(this);
    m_dim->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_dim->setStyleSheet("background-color: rgba(0,0,0,140);");
    m_dim->setGeometry(rect());
    m_dim->lower();
    m_background->raise();
    m_background->lower();
    m_dim->raise();
    m_scroll->raise();

    layout->addWidget(m_scroll);

    m_bottomBar = new QWidget(this);
    QHBoxLayout* bottomLayout = new QHBoxLayout(m_bottomBar);
    bottomLayout->setContentsMargins(8, 8, 8, 8);
    bottomLayout->setSpacing(12);

    m_btnGlobalCfg = new QPushButton(tr("Settings"), m_bottomBar);
    m_btnGameCfg = new QPushButton(tr("Game Settings"), m_bottomBar);
    m_btnMods = new QPushButton(tr("Mods Manager"), m_bottomBar);
    m_btnHotkeys = new QPushButton(tr("Hotkeys"), m_bottomBar);
    m_btnPlay = new QPushButton(tr("Play"), m_bottomBar);
    m_btnQuit = new QPushButton(tr("Quit"), m_bottomBar);

    bottomLayout->addStretch();
    bottomLayout->addWidget(m_btnGlobalCfg);
    bottomLayout->addWidget(m_btnGameCfg);
    bottomLayout->addWidget(m_btnMods);
    bottomLayout->addWidget(m_btnHotkeys);
    bottomLayout->addWidget(m_btnPlay);
    bottomLayout->addWidget(m_btnQuit);
    bottomLayout->addStretch();

    layout->addWidget(m_bottomBar);

    connect(m_btnPlay, &QPushButton::clicked, this, &BigPictureWidget::onPlayClicked);
    connect(m_btnHotkeys, &QPushButton::clicked, this, &BigPictureWidget::onHotkeysClicked);
    connect(m_btnMods, &QPushButton::clicked, this, &BigPictureWidget::onModsClicked);
    connect(m_btnQuit, &QPushButton::clicked, this, &BigPictureWidget::onQuitClicked);
    connect(m_btnGameCfg, &QPushButton::clicked, this, &BigPictureWidget::onGameConfigClicked);
    connect(m_btnGlobalCfg, &QPushButton::clicked, this, &BigPictureWidget::onGlobalConfigClicked);

    if (!m_tiles.empty()) {
        m_selectedIndex = 0;
        highlightSelectedTile();
        QTimer::singleShot(0, this, [this]() {
            centerSelectedTileAnimated();
            updateBackground(m_selectedIndex);
        });
    }
}

void BigPictureWidget::onModsClicked() {
    ensureSelectionValid();
    emit openModsManagerRequested(m_selectedIndex);
}

void BigPictureWidget::onHotkeysClicked() {
    emit openHotkeysRequested();
}
void BigPictureWidget::layoutTiles() {
    const int baseW = 300;
    const int baseH = 420;
    const int spacing = 10;

    int viewportH = m_scroll->viewport()->height();
    if (viewportH <= 0)
        viewportH = height();

    int containerH = viewportH;

    int centerY = (containerH - baseH);
    if (centerY < 0)
        centerY = +400;

    int x = +500;

    for (int i = 0; i < m_tiles.size(); i++) {
        QWidget* tile = m_tiles[i];

        QRect geom(x, centerY, baseW, baseH);
        tile->setGeometry(geom);
        tile->setProperty("baseGeom", geom);

        x += baseW + spacing;
    }

    m_container->setFixedSize(x - spacing, containerH + 850);
}

void BigPictureWidget::applyTheme() {
    if (!m_themes)
        return;

    QString textColor = m_themes->textColor().name();
    QString baseColor = m_themes->iconBaseColor().name();
    QString hoverColor = m_themes->iconHoverColor().name();

    for (auto* label : findChildren<QLabel*>()) {
        label->setStyleSheet(QString("color: %1;").arg(textColor));
    }

    QString buttonStyle = QString(R"(
        QPushButton {
            background-color: %1;
            color: %2;
            border-radius: 6px;
            padding: 8px 16px;
            font-size: 16px;
        }
        QPushButton:hover {
            background-color: %3;
        }
    )")
                              .arg(baseColor, textColor, hoverColor);

    for (auto* btn : findChildren<QPushButton*>()) {
        btn->setStyleSheet(buttonStyle);
    }

    if (m_scroll)
        m_scroll->setStyleSheet("background: transparent;");

    setAutoFillBackground(true);
}

QWidget* BigPictureWidget::buildTile(const GameInfo& g) {
    AnimatedTile* tile = new AnimatedTile(m_container);

    tile->setFixedSize(300, 450);

    QVBoxLayout* v = new QVBoxLayout(tile);
    v->setSpacing(10);
    v->setContentsMargins(0, 0, 0, 0);

    QLabel* cover = new QLabel(tile);
    cover->setFixedSize(280, 280);
    cover->setAlignment(Qt::AlignCenter);
    cover->setScaledContents(true);

    if (!g.pic_path.empty()) {
        QString path;
        Common::FS::PathToQString(path, g.pic_path);
        cover->setPixmap(QPixmap(path));
    }

    ScrollingLabel* title = new ScrollingLabel(tile);
    title->setText(QString::fromStdString(g.name));
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet("color: white; font-size: 18px;"
                         "padding-left: 8px; padding-right: 8px;");

    title->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    title->setFixedHeight(40);
    title->setWordWrap(false);
    QTimer::singleShot(0, title, [title]() { title->startScrollIfNeeded(); });

    v->addWidget(cover);
    v->addWidget(title);

    return tile;
}

void BigPictureWidget::updateBackground(int index) {
    if (index < 0 || index >= m_gameInfo->m_games.size()) {
        m_background->clear();
        return;
    }

    const GameInfo& g = m_gameInfo->m_games[index];
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
            m_background->lower();
            m_dim->setGeometry(rect());
            m_dim->raise();
            m_scroll->raise();
        }
    } else {
        m_background->clear();
    }
}

void BigPictureWidget::buildAnimations() {
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

    m_scrollAnim = new QPropertyAnimation(m_scroll->horizontalScrollBar(), "value", this);
    m_scrollAnim->setDuration(300);
    m_scrollAnim->setEasingCurve(QEasingCurve::OutCubic);
}

void BigPictureWidget::toggle() {
    if (m_visible)
        hideFull();
    else
        showFull();
}

void BigPictureWidget::showFull() {
    if (m_visible)
        return;

    QWidget* top = parentWidget();
    if (top)
        setGeometry(top->geometry());
    else
        setGeometry(QApplication::primaryScreen()->geometry());

    show();
    raise();
    setFocus();

    ensureSelectionValid();
    updateBackground(m_selectedIndex);
    highlightSelectedTile();
    centerSelectedTileAnimated();

    m_fadeIn->start();
    m_visible = true;
}

void BigPictureWidget::hideFull() {
    if (!m_visible)
        return;
    m_fadeOut->start();
}

void BigPictureWidget::highlightSelectedTile() {
    const qreal zoomFactor = 1.25;

    if (m_tiles.empty())
        return;

    int selected = m_selectedIndex;
    QRect selectedBase = m_tiles[selected]->property("baseGeom").toRect();
    QSize zoomedSize(selectedBase.width() * zoomFactor, selectedBase.height() * zoomFactor);
    int extraWidth = (zoomedSize.width() - selectedBase.width()) / 2;

    for (int i = 0; i < m_tiles.size(); i++) {
        QWidget* tile = m_tiles[i];
        QRect baseGeom = tile->property("baseGeom").toRect();

        if (i == selected) {
            tile->raise();
            tile->setStyleSheet("background-color: rgba(34,34,34,180);"
                                "border: 3px solid #57a1ff; border-radius: 14px;");

            zoomSelectedTile(tile, baseGeom, zoomFactor);
        } else {
            tile->setStyleSheet("background-color: rgba(34,34,34,180);"
                                "border: none; border-radius: 12px;");

            int shiftX = 0;
            if (i < selected)
                shiftX = -extraWidth;
            if (i > selected)
                shiftX = extraWidth;

            tile->setGeometry(baseGeom.translated(shiftX, 0));
        }
    }
}

void BigPictureWidget::zoomSelectedTile(QWidget* tile, const QRect& baseGeom, qreal factor) {
    if (!tile)
        return;

    QSize targetSize(baseGeom.width() * factor, baseGeom.height() * factor);
    QPoint center = tile->geometry().center();

    QRect targetGeom(center.x() - targetSize.width() / 2, center.y() - targetSize.height() / 2,
                     targetSize.width(), targetSize.height());

    auto* anim = new QPropertyAnimation(tile, "geometry");
    anim->setDuration(200);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    anim->setStartValue(tile->geometry());
    anim->setEndValue(targetGeom);
    anim->start(QPropertyAnimation::DeleteWhenStopped);
}

bool BigPictureWidget::eventFilter(QObject* obj, QEvent* ev) {

    QWidget* tile = qobject_cast<QWidget*>(obj);
    if (!tile)
        return QWidget::eventFilter(obj, ev);

    int index = tile->property("game_index").toInt();

    if (ev->type() == QEvent::MouseButtonRelease) {

        if (m_selectedIndex != index) {
            m_selectedIndex = index;
            updateBackground(index);
            highlightSelectedTile();
            centerSelectedTileAnimated();
        }

        setFocus();
        return true;
    }

    if (ev->type() == QEvent::MouseButtonDblClick) {
        m_selectedIndex = index;
        onPlayClicked();
        return true;
    }

    return QWidget::eventFilter(obj, ev);
}

void BigPictureWidget::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) {
        hideFull();
        return;
    }

    if (e->key() == Qt::Key_Right) {
        if (m_selectedIndex + 1 < (int)m_tiles.size()) {
            m_selectedIndex++;
            updateBackground(m_selectedIndex);
            highlightSelectedTile();
            centerSelectedTileAnimated();
        }
        e->accept();
        return;
    }

    if (e->key() == Qt::Key_Left) {
        if (m_selectedIndex > 0) {
            m_selectedIndex--;
            updateBackground(m_selectedIndex);
            highlightSelectedTile();
            centerSelectedTileAnimated();
        }
        e->accept();
        return;
    }

    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter || e->key() == Qt::Key_Space) {
        QWidget* fw = focusWidget();
        if (fw) {
            if (QPushButton* btn = qobject_cast<QPushButton*>(fw)) {
                btn->click();
            } else if (std::find(m_tiles.begin(), m_tiles.end(), fw) != m_tiles.end()) {
                onPlayClicked();
            }
        } else {
            onPlayClicked();
        }
        e->accept();
        return;
    }

    if (e->key() == Qt::Key_Down) {
        m_btnPlay->setFocus();
        e->accept();
        return;
    }

    if (e->key() == Qt::Key_Up) {
        this->setFocus();
        ensureSelectionValid();
        highlightSelectedTile();
        centerSelectedTileAnimated();
        e->accept();
        return;
    }

    QWidget::keyPressEvent(e);
}

void BigPictureWidget::centerSelectedTileAnimated() {
    if (!m_scroll || m_selectedIndex < 0 || m_selectedIndex >= (int)m_tiles.size())
        return;

    QWidget* tile = m_tiles[m_selectedIndex];
    QRect baseGeom = tile->property("baseGeom").toRect();

    int viewportW = m_scroll->viewport()->width();

    int centerX = baseGeom.center().x();

    int target = centerX - viewportW / 2;
    target = std::clamp(target, 0, m_scroll->horizontalScrollBar()->maximum());

    m_scrollAnim->stop();
    m_scrollAnim->setStartValue(m_scroll->horizontalScrollBar()->value());
    m_scrollAnim->setEndValue(target);
    m_scrollAnim->start();
}

void BigPictureWidget::ensureSelectionValid() {
    if (m_selectedIndex < 0 && !m_tiles.empty())
        m_selectedIndex = 0;
    if (m_selectedIndex >= (int)m_tiles.size())
        m_selectedIndex = (int)m_tiles.size() - 1;
}

void BigPictureWidget::onPlayClicked() {
    ensureSelectionValid();
    if (m_selectedIndex >= 0 && m_selectedIndex < (int)m_tiles.size()) {
        emit launchGameRequested(m_selectedIndex);
    }
}

void BigPictureWidget::onGlobalConfigClicked() {
    emit globalConfigRequested();
}

void BigPictureWidget::onGameConfigClicked() {
    ensureSelectionValid();
    emit gameConfigRequested(m_selectedIndex);
}

void BigPictureWidget::handleGamepadButton(GamepadButton btn) {
    switch (btn) {
    case GamepadButton::Left:
        keyPressEvent(new QKeyEvent(QEvent::KeyPress, Qt::Key_Left, Qt::NoModifier));
        break;
    case GamepadButton::Right:
        keyPressEvent(new QKeyEvent(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier));
        break;
    case GamepadButton::South:
        onPlayClicked();
        break;
    case GamepadButton::East:
        hideFull();
        break;
    case GamepadButton::West:
        onHotkeysClicked();
        break;
    case GamepadButton::North:
        onGameConfigClicked();
        break;
    default:
        break;
    }
}

void BigPictureWidget::onQuitClicked() {
    hideFull();
}

void BigPictureWidget::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);

    m_background->setGeometry(rect());
    m_dim->setGeometry(rect());

    m_background->lower();
    m_dim->raise();
    m_scroll->raise();
    m_bottomBar->raise();

    layoutTiles();

    updateBackground(m_selectedIndex);
    for (auto* tile : m_tiles)
        tile->setProperty("baseGeom", tile->geometry());
    highlightSelectedTile();
    centerSelectedTileAnimated();
}
