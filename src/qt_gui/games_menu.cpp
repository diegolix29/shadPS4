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
#include <cmrc/cmrc.hpp>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "game_info.h"
#include "game_list_frame.h"
#include "games_menu.h"

CMRC_DECLARE(res);

BigPictureWidget::BigPictureWidget(std::shared_ptr<GameInfoClass> gameInfo,
                                   std::shared_ptr<CompatibilityInfoClass> compatInfo,
                                   std::shared_ptr<IpcClient> ipcClient, WindowThemes* themes,
                                   QWidget* parent)
    : QWidget(parent), m_gameInfo(gameInfo), m_compatInfo(compatInfo), m_ipcClient(ipcClient),
      m_themes(themes) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setAutoFillBackground(false);
    buildUi();
    auto resource = cmrc::res::get_filesystem();

    m_player = new QMediaPlayer(this);
    m_audioOutput = new QAudioOutput(this);
    m_player->setAudioOutput(m_audioOutput);

    float vol = static_cast<float>(Config::getVolumeSlider() / 100.f);
    m_audioOutput->setVolume(vol / 2);

    auto basePath = Common::FS::GetUserPath(Common::FS::PathType::CustomAudios);
    std::filesystem::path bgmMp3 = basePath / "bgm.mp3";
    std::filesystem::path bgmWav = basePath / "bgm.wav";

    QString file;
    bool customFound = false;

    if (std::filesystem::exists(bgmWav)) {
        file = QString::fromStdString(bgmWav.string());
        customFound = true;
    } else if (std::filesystem::exists(bgmMp3)) {
        file = QString::fromStdString(bgmMp3.string());
        customFound = true;
    }

    if (customFound) {
        m_player->setSource(QUrl::fromLocalFile(file));
        m_player->setLoops(QMediaPlayer::Infinite);
    } else {
        try {
            if (resource.exists("src/images/bgm.wav")) {
                auto resFile = resource.open("src/images/bgm.wav");

                QString tempPath =
                    QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/bgm.wav";

                QFile temp(tempPath);
                if (temp.open(QIODevice::WriteOnly)) {
                    temp.write(reinterpret_cast<const char*>(resFile.begin()), resFile.size());
                    temp.close();

                    m_player->setSource(QUrl::fromLocalFile(tempPath));
                    m_player->setLoops(QMediaPlayer::Infinite);
                }
            }
        } catch (...) {
        }
    }
    m_uiSound = new QMediaPlayer(this);
    m_uiOutput = new QAudioOutput(this);

    m_uiSound->setAudioOutput(m_uiOutput);
    m_uiOutput->setVolume(0.25f);
    std::filesystem::path tickWav = basePath / "tick.wav";
    std::filesystem::path tickMp3 = basePath / "tick.mp3";

    QString tickFile;
    bool customTickFound = false;

    if (std::filesystem::exists(tickWav)) {
        tickFile = QString::fromStdString(tickWav.string());
        customTickFound = true;
    } else if (std::filesystem::exists(tickMp3)) {
        tickFile = QString::fromStdString(tickMp3.string());
        customTickFound = true;
    }

    if (customTickFound) {
        m_uiSound->setSource(QUrl::fromLocalFile(tickFile));
    } else {
        try {
            if (resource.exists("src/images/tick.mp3")) {
                auto resFile = resource.open("src/images/tick.mp3");

                QString tempTick =
                    QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/ui_tick.mp3";

                QFile temp(tempTick);
                if (temp.open(QIODevice::WriteOnly)) {
                    temp.write(reinterpret_cast<const char*>(resFile.begin()), resFile.size());
                    temp.close();
                    m_uiSound->setSource(QUrl::fromLocalFile(tempTick));
                }
            }
        } catch (...) {
        }
    }
    m_playSound = new QMediaPlayer(this);
    m_playOutput = new QAudioOutput(this);
    m_playSound->setAudioOutput(m_playOutput);
    m_playOutput->setVolume(0.45f);

    std::filesystem::path playWav = basePath / "play.wav";
    std::filesystem::path playMp3 = basePath / "play.mp3";

    QString playFile;
    bool customPlayFound = false;

    if (std::filesystem::exists(playWav)) {
        playFile = QString::fromStdString(playWav.string());
        customPlayFound = true;
    } else if (std::filesystem::exists(playMp3)) {
        playFile = QString::fromStdString(playMp3.string());
        customPlayFound = true;
    }

    if (customPlayFound) {
        m_playSound->setSource(QUrl::fromLocalFile(playFile));
    } else {
        try {
            if (resource.exists("src/images/play.mp3")) {
                auto resFile = resource.open("src/images/play.mp3");

                QString tempPlay =
                    QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/ui_play.mp3";

                QFile f(tempPlay);
                if (f.open(QIODevice::WriteOnly)) {
                    f.write(reinterpret_cast<const char*>(resFile.begin()), resFile.size());
                    f.close();
                    m_playSound->setSource(QUrl::fromLocalFile(tempPlay));
                }
            }
        } catch (...) {
        }
    }

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
    m_scrollBarHidden = false;

    setFocusPolicy(Qt::StrongFocus);

    m_container = new QWidget(m_scroll);

    for (int i = 0; i < m_gameInfo->m_games.size(); i++) {
        QWidget* tile = buildTile(m_gameInfo->m_games[i]);
        tile->setParent(m_container);
        tile->setProperty("game_index", i);
        tile->installEventFilter(this);
        m_tiles.push_back(tile);
    }

    m_scroll->setWidget(m_container);
    m_scroll->viewport()->setFocusPolicy(Qt::NoFocus);

    m_background = new QLabel(this);
    m_background->setScaledContents(true);
    m_background->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);

    m_dim = new QWidget(this);
    m_dim->setStyleSheet("background-color: rgba(0, 0, 0, 0);");

    m_dimFade = new QPropertyAnimation(m_dimOpacity, "opacity", this);
    m_dimFade->setDuration(220);

    layout->addWidget(m_scroll);

    m_bottomBar = new QWidget(this);
    QHBoxLayout* bottomLayout = new QHBoxLayout(m_bottomBar);
    bottomLayout->setContentsMargins(8, 8, 8, 8);
    bottomLayout->setSpacing(12);
    m_bottomBarHidden = false;

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

    m_hotkeysOverlay = new HotkeysOverlay(Qt::Horizontal, this);
    m_hotkeysOverlay->setHotkeys({{"Arrow Left/Right", "Navigate Games/Buttons"},
                                  {"Arrow Down", "Focus on Buttons"},
                                  {"Arrow Up", "Focus on Games"},
                                  {"Enter/Space", "Select/Play"},
                                  {"Shift + Arrow Up", "Hide/Show Games and Buttons"},
                                  {"Shift + N", "Mute Background Music"},
                                  {"Press - R - ", "Stop/Play Background Music"},
                                  {"Press - P - ", "Play Highlighted Game"},
                                  {"Press - M - ", "Mods Manager"},
                                  {"Press - G - ", "Games Settings"},
                                  {"Press - S - ", "Global Settings"},
                                  {"Press - H - ", "Hotkeys Setup"},
                                  {"Esc", "Exit"}});
    m_hotkeysOverlay->setMinimumHeight(50);
    m_hotkeysOverlay->setStyleSheet("background: rgba(0,0,0,120); padding: 6px 12px;");
    m_hotkeysOverlay->show();

    m_background->lower();
    m_dim->raise();
    m_scroll->raise();
    m_bottomBar->raise();
    m_hotkeysOverlay->raise();

    connect(m_btnPlay, &QPushButton::clicked, this, &BigPictureWidget::onPlayClicked);
    connect(m_btnHotkeys, &QPushButton::clicked, this, &BigPictureWidget::onHotkeysClicked);
    connect(m_btnMods, &QPushButton::clicked, this, &BigPictureWidget::onModsClicked);
    connect(m_btnQuit, &QPushButton::clicked, this, &BigPictureWidget::onQuitClicked);
    connect(m_btnGameCfg, &QPushButton::clicked, this, &BigPictureWidget::onGameConfigClicked);
    connect(m_btnGlobalCfg, &QPushButton::clicked, this, &BigPictureWidget::onGlobalConfigClicked);

    const QList<QPushButton*> buttons = {
        m_btnGlobalCfg, m_btnGameCfg, m_btnMods, m_btnHotkeys, m_btnPlay, m_btnQuit,
    };

    for (auto* btn : buttons) {
        if (btn)
            btn->installEventFilter(this);
    }

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

void BigPictureWidget::setDimVisible(bool visible) {
    if (m_dimFade) {
        m_dimFade->stop();
    }

    int targetAlpha = visible ? 140 : 0;

    QColor themeColor = this->palette().color(QPalette::Window);
    int r = themeColor.red();
    int g = themeColor.green();
    int b = themeColor.blue();

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
        startAlpha = 140;
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

void BigPictureWidget::layoutTiles() {
    int viewportW = m_scroll->viewport()->width();
    int viewportH = m_scroll->viewport()->height();
    if (viewportW <= 0)
        viewportW = width();
    if (viewportH <= 0)
        viewportH = height();

    const int baseH = viewportH * 0.4;
    const int baseW = baseH;

    const int spacing = baseW / 6;

    int containerH = viewportH;
    int centerY = (containerH - baseH) / 2;

    int leftPadding = viewportW / 2 - baseW / 2;
    int rightPadding = leftPadding;

    int x = leftPadding;
    for (int i = 0; i < m_tiles.size(); i++) {
        QWidget* tile = m_tiles[i];

        QRect geom(x, centerY, baseW, baseH);
        tile->setGeometry(geom);
        tile->setProperty("baseGeom", geom);

        x += baseW + spacing;
    }

    m_container->setMinimumSize(x - spacing + rightPadding, containerH);
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

QPixmap LoadGameIcon(const GameInfo& game, int size) {
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

QWidget* BigPictureWidget::buildTile(const GameInfo& g) {
    AnimatedTile* tile = new AnimatedTile(m_container);

    tile->setMinimumSize(300, 300);
    tile->setAttribute(Qt::WA_TranslucentBackground);
    tile->setContentsMargins(0, 0, 0, 0);
    tile->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    tile->setGraphicsEffect(nullptr);
    tile->setStyleSheet("background: transparent;");
    tile->setProperty("scale", 1.0);
    tile->setFocusPolicy(Qt::StrongFocus);

    QVBoxLayout* v = new QVBoxLayout(tile);
    v->setSpacing(10);
    v->setContentsMargins(0, 0, 0, 0);
    v->setAlignment(Qt::AlignHCenter);

    QLabel* cover = new QLabel(tile);
    cover->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    cover->setAlignment(Qt::AlignCenter);
    cover->setScaledContents(true);
    cover->setPixmap(LoadGameIcon(g, 512));

    ScrollingLabel* title = new ScrollingLabel(tile);
    title->setText(QString::fromStdString(g.name));
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet("color: white; font-size: 18px; padding-left: 8px; padding-right: 8px;");
    title->setFixedHeight(40);
    title->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    title->setWordWrap(false);

    ScrollingLabel* cusa = new ScrollingLabel(tile);
    cusa->setText(QString::fromStdString(g.serial));
    cusa->setAlignment(Qt::AlignCenter);
    cusa->setStyleSheet("color: white; font-size: 18px; padding-left: 8px; padding-right: 8px;");
    cusa->setFixedHeight(40);
    cusa->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    cusa->setWordWrap(false);

    v->addWidget(cover);
    v->addWidget(title);
    v->addWidget(cusa);

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
            m_bottomBar->raise();
            m_hotkeysOverlay->raise();
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

    connect(m_scrollAnim, &QPropertyAnimation::finished, this, [this]() {
        m_navigationLocked = false;
        emit centered();
    });
}

void BigPictureWidget::toggle() {
    if (m_visible)
        hideFull();
    else
        showFull();
}

void BigPictureWidget::playNavSound() {
    if (!m_uiSound)
        return;
    if (!m_visible)
        return;
    m_uiSound->stop();
    m_uiSound->play();
}

void BigPictureWidget::showFull() {
    if (m_visible)
        return;

    setWindowFlag(Qt::FramelessWindowHint);
    setWindowState(Qt::WindowFullScreen);

    show();
    raise();
    setFocus();

    ensureSelectionValid();
    updateBackground(m_selectedIndex);
    highlightSelectedTile();
    centerSelectedTileAnimated();
    setDimVisible(true);
    m_fadeIn->start();
    m_visible = true;

    if (m_player && m_player->source().isValid()) {
        m_player->play();
    }
}

void BigPictureWidget::hideFull() {
    if (m_player)
        m_player->stop();
    setDimVisible(false);
    m_fadeOut->start();
}

static void animateWidgetGeometry(QWidget* w, const QRect& endGeom, int duration = 220) {
    if (!w)
        return;
    auto* anim = new QPropertyAnimation(w, "geometry");
    anim->setDuration(duration);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    anim->setStartValue(w->geometry());
    anim->setEndValue(endGeom);
    anim->start(QPropertyAnimation::DeleteWhenStopped);
}

static void animateLabelSize(QLabel* lbl, const QSize& endSize, int duration = 220) {
    if (!lbl)
        return;
    auto* animW = new QPropertyAnimation(lbl, "maximumWidth");
    animW->setDuration(duration);
    animW->setEasingCurve(QEasingCurve::OutCubic);
    animW->setStartValue(lbl->maximumWidth() > 0 ? lbl->maximumWidth() : lbl->width());
    animW->setEndValue(endSize.width());
    animW->start(QPropertyAnimation::DeleteWhenStopped);

    auto* animH = new QPropertyAnimation(lbl, "maximumHeight");
    animH->setDuration(duration);
    animH->setEasingCurve(QEasingCurve::OutCubic);
    animH->setStartValue(lbl->maximumHeight() > 0 ? lbl->maximumHeight() : lbl->height());
    animH->setEndValue(endSize.height());
    animH->start(QPropertyAnimation::DeleteWhenStopped);
}

void BigPictureWidget::updateDepthEffect() {
    if (!m_visible)
        return;

    int viewportCenter =
        m_scroll->horizontalScrollBar()->value() + m_scroll->viewport()->width() / 2;

    const float maxScale = 1.2 + (height() / 1200.0f);
    const float minScale = 0.8;
    const float influence = width() / 3.0f;
    const int liftPx = height() / 20;

    const int duration = 220;

    for (int i = 0; i < m_tiles.size(); i++) {
        QWidget* tile = m_tiles[i];

        QRect base = tile->property("baseGeom").toRect();
        int tileCenter = base.center().x();

        int dist = std::abs(tileCenter - viewportCenter);

        float t = 1.0f - std::min(1.0f, float(dist) / influence);
        float scale = minScale + (maxScale - minScale) * t;

        int newW = int(base.width() * scale);
        int newH = int(base.height() * scale);

        int dx = (newW - base.width()) / 2;
        int dy = (newH - base.height()) / 2;

        int lift = int(liftPx * t);

        QRect newRect(base.x() - dx, base.y() - dy - lift, newW, newH);

        const int threshold = 2;
        QRect cur = tile->geometry();
        if (std::abs(cur.x() - newRect.x()) > threshold ||
            std::abs(cur.y() - newRect.y()) > threshold ||
            std::abs(cur.width() - newRect.width()) > threshold ||
            std::abs(cur.height() - newRect.height()) > threshold) {
            animateWidgetGeometry(tile, newRect, duration);
        } else {
            tile->setGeometry(newRect);
        }
    }
}

void BigPictureWidget::highlightSelectedTile() {
    if (m_tiles.empty())
        return;
    const int duration = 260;
    setDimVisible(true);

    int viewportH = m_scroll->viewport()->height();
    if (viewportH <= 0)
        viewportH = height();

    const qreal zoomFactor = std::min(1.8, viewportH / 500.0);
    const qreal sideScale = 0.6;
    const int baseDelta = viewportH / 40;

    int selected = m_selectedIndex;

    for (int i = 0; i < m_tiles.size(); i++) {
        QWidget* tile = m_tiles[i];
        QRect baseGeom = tile->property("baseGeom").toRect();

        QLabel* cover = tile->findChild<QLabel*>();

        if (i == selected) {
            tile->raise();
            tile->setStyleSheet("background-color: rgba(34,34,34,180);"
                                "border: none; border-radius: 14px;");

            int newW = int(baseGeom.width() * zoomFactor);
            int newH = int(baseGeom.height() * zoomFactor);
            int dx = (newW - baseGeom.width()) / 2;
            int dy = (newH - baseGeom.height()) / 2;
            QRect zoomed(baseGeom.x() - dx, baseGeom.y() - dy, newW, newH);

            animateWidgetGeometry(tile, zoomed, duration);

            if (cover) {
                const int margin = 12;

                int targetSize =
                    std::max(10, std::min(zoomed.width(), zoomed.height()) - 2 * margin - 40);
                QSize targetCoverSize(targetSize, targetSize);

                animateLabelSize(cover, targetCoverSize, duration);
                cover->setScaledContents(true);
            }

        } else {
            tile->setStyleSheet("background-color: rgba(34,34,34,180);"
                                "border: none; border-radius: 12px;");

            int indexDist = std::abs(i - selected);
            int baseDeltaReduced = baseDelta / 2;
            int delta = int(baseGeom.width() * ((zoomFactor) / 5.0));
            int push = baseDeltaReduced * (indexDist > 0 ? indexDist : 0);
            int shiftX = (i < selected) ? -(delta + push) : (delta + push);

            int scaledW = int(baseGeom.width() * sideScale);
            int scaledH = int(baseGeom.height() * sideScale);
            int dx = (scaledW - baseGeom.width()) / 2;
            int dy = (scaledH - baseGeom.height()) / 2;

            int downShift = 8 * indexDist;

            QRect target = baseGeom.translated(shiftX, downShift);
            target.setX(target.x() - dx);
            target.setY(target.y() - dy);
            target.setSize(QSize(scaledW, scaledH));

            animateWidgetGeometry(tile, target, duration);

            if (cover) {
                const int margin = 12;

                int targetSize =
                    std::max(10, std::min(target.width(), target.height()) - 2 * margin - 40);
                QSize desiredCover(targetSize, targetSize);

                animateLabelSize(cover, desiredCover, duration);
                cover->setScaledContents(true);
            }
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

    if (tile && tile->property("game_index").isValid()) {
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
    }

    if (QPushButton* btn = qobject_cast<QPushButton*>(obj)) {
        if (ev->type() == QEvent::KeyPress) {
            auto* e = static_cast<QKeyEvent*>(ev);

            switch (e->key()) {
            case Qt::Key_Up:
                m_focusMode = FocusMode::Tiles;
                setFocus(Qt::OtherFocusReason);
                highlightSelectedTile();
                return true;

            case Qt::Key_Return:
            case Qt::Key_Enter:
            case Qt::Key_Space:
                btn->click();
                return true;

            default:
                break;
            }
        }
    }

    return QWidget::eventFilter(obj, ev);
}

void BigPictureWidget::setMinimalUi(bool hide) {
    m_hideUi = hide;

    m_scrollBarHidden = hide;
    m_bottomBarHidden = hide;

    if (m_scroll)
        m_scroll->setVisible(!hide);

    if (m_bottomBar)
        m_bottomBar->setVisible(!hide);

    if (m_hotkeysOverlay)
        m_hotkeysOverlay->setVisible(!hide);

    if (hide) {
        m_dim->lower();
    } else {
        m_dim->raise();

        if (m_scroll)
            m_scroll->raise();
        if (m_bottomBar)
            m_bottomBar->raise();
        if (m_hotkeysOverlay)
            m_hotkeysOverlay->raise();
    }
}

void BigPictureWidget::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Up && e->modifiers().testFlag(Qt::ShiftModifier)) {
        setMinimalUi(!m_hideUi);

        layoutTiles();
        highlightSelectedTile();
        centerSelectedTileAnimated();
        updateDepthEffect();

        e->accept();
        return;
    }

    if (m_navigationLocked) {
        e->accept();
        return;
    }

    if (e->key() == Qt::Key_Escape) {
        hideFull();
        return;
    }
    if (e->key() == Qt::Key_S) {
        onGlobalConfigClicked();
        return;
    }
    if (e->key() == Qt::Key_P) {
        onPlayClicked();
        return;
    }
    if (e->key() == Qt::Key_G) {
        onGameConfigClicked();
        return;
    }
    if (e->key() == Qt::Key_M) {
        onModsClicked();
        return;
    }
    if (e->key() == Qt::Key_H) {
        onHotkeysClicked();
        return;
    }
    if (e->key() == Qt::Key_R) {
        toggleBackgroundMusic();
        return;
    }

    if (e->key() == Qt::Key_Right) {
        if (m_selectedIndex + 1 < (int)m_tiles.size()) {
            m_navigationLocked = true;
            m_selectedIndex++;
            updateBackground(m_selectedIndex);
            highlightSelectedTile();
            UpdateCurrentGameAudio();

            centerSelectedTileAnimated();
        }
        e->accept();
        return;
    }

    if (e->key() == Qt::Key_Left) {
        if (m_selectedIndex > 0) {
            m_navigationLocked = true;
            m_selectedIndex--;
            updateBackground(m_selectedIndex);
            highlightSelectedTile();
            UpdateCurrentGameAudio();

            centerSelectedTileAnimated();
        }
        e->accept();
        return;
    }

    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter || e->key() == Qt::Key_Space) {
        if (m_focusMode == FocusMode::Buttons) {
            QWidget* fw = focusWidget();
            if (QPushButton* btn = qobject_cast<QPushButton*>(fw)) {
                btn->click();
            }
        } else {
            onPlayClicked();
        }
        e->accept();
        return;
    }

    if (e->key() == Qt::Key_N && e->modifiers().testFlag(Qt::ShiftModifier)) {
        if (m_audioOutput) {
            bool nowMuted = !m_audioOutput->isMuted();
            m_audioOutput->setMuted(nowMuted);
        }

        e->accept();
        return;
    }

    if (e->key() == Qt::Key_Down) {

        m_focusMode = FocusMode::Buttons;
        m_btnPlay->setFocus(Qt::OtherFocusReason);
        e->accept();
        return;
    }

    if (e->key() == Qt::Key_Up) {
        if (m_focusMode == FocusMode::Buttons) {
            m_focusMode = FocusMode::Tiles;
            setFocus(Qt::OtherFocusReason);
            highlightSelectedTile();
            e->accept();
            return;
        }
    }

    QWidget::keyPressEvent(e);
}

void BigPictureWidget::centerSelectedTileAnimated() {
    if (!m_scroll || m_selectedIndex < 0 || m_selectedIndex >= (int)m_tiles.size())
        return;
    playNavSound();

    QRect baseGeom = m_tiles[m_selectedIndex]->property("baseGeom").toRect();

    int viewportW = m_scroll->viewport()->width();
    int centerX = baseGeom.center().x();

    int target = centerX - viewportW / 2;
    target = std::clamp(target, 0, m_scroll->horizontalScrollBar()->maximum());

    m_navigationLocked = true;

    int currentValue = m_scroll->horizontalScrollBar()->value();

    if (m_scrollAnim->state() == QAbstractAnimation::Running) {
        m_scrollAnim->setEndValue(target);
    } else {
        m_scrollAnim->stop();
        m_scrollAnim->setStartValue(currentValue);
        m_scrollAnim->setEndValue(target);
        m_scrollAnim->start();
    }
}

void BigPictureWidget::ensureSelectionValid() {
    if (m_selectedIndex < 0 && !m_tiles.empty())
        m_selectedIndex = 0;
    if (m_selectedIndex >= (int)m_tiles.size())
        m_selectedIndex = (int)m_tiles.size() - 1;
}

void BigPictureWidget::onPlayClicked() {
    ensureSelectionValid();
    setMinimalUi(!m_hideUi);

    if (m_player)
        m_player->stop();

    if (m_playSound) {
        m_playSound->stop();
        m_playSound->play();
    }

    if (m_selectedIndex >= 0 && m_selectedIndex < (int)m_tiles.size()) {
        emit launchRequestedFromGameMenu(m_selectedIndex);
    }
    if (!Config::getGameRunning()) {
        setMinimalUi(!m_hideUi);
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

void BigPictureWidget::toggleBackgroundMusic() {
    if (!m_player)
        return;

    if (m_player->playbackState() == QMediaPlayer::PlayingState) {
        m_player->stop();
    } else {
        m_player->play();
    }
}

void BigPictureWidget::UpdateCurrentGameAudio() {
    if (m_selectedIndex < 0 || m_selectedIndex >= m_gameInfo->m_games.size() ||
        !Config::getPlayBGM()) {
        BackgroundMusicPlayer::getInstance().stopMusic();
        if (m_audioOutput)
            m_audioOutput->setMuted(false);
        return;
    }

    const auto& game = m_gameInfo->m_games[m_selectedIndex];

    bool fileExists = !game.snd0_path.empty() && std::filesystem::exists(game.snd0_path);

    if (fileExists) {
        QString snd0path;
        Common::FS::PathToQString(snd0path, game.snd0_path);

        BackgroundMusicPlayer::getInstance().playMusic(snd0path);

        if (m_audioOutput) {
            m_audioOutput->setMuted(true);
        }
    } else {
        BackgroundMusicPlayer::getInstance().stopMusic();

        if (m_audioOutput) {
            m_audioOutput->setMuted(false);
        }
    }
}

void BigPictureWidget::showEvent(QShowEvent* ev) {
    QWidget::showEvent(ev);
    UpdateCurrentGameAudio();
    QTimer::singleShot(0, this, [this]() {
        layoutTiles();
        highlightSelectedTile();
        centerSelectedTileAnimated();
        updateDepthEffect();
        setDimVisible(true);
    });
}

void BigPictureWidget::onQuitClicked() {
    hideFull();
}

void BigPictureWidget::focusInEvent(QFocusEvent* event) {
    QWidget::focusInEvent(event);

    if (!Config::getGameRunning()) {
        return;
    }

    if (m_player) {
        m_player->play();
    }
    UpdateCurrentGameAudio();
    setMinimalUi(!m_hideUi);
}

void BigPictureWidget::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    if (m_dim)
        m_dim->setGeometry(rect());

    if (m_background)
        m_background->setGeometry(rect());
    m_dim->raise();
    m_scroll->raise();
    m_bottomBar->raise();

    if (m_hotkeysOverlay) {
        int margin = 8;
        QRect bottomGeom = m_bottomBar->geometry();

        int overlayWidth = bottomGeom.width();
        int overlayHeight = m_hotkeysOverlay->height();
        int overlayX = bottomGeom.x();
        int overlayY = bottomGeom.y() - overlayHeight - margin;

        m_hotkeysOverlay->setGeometry(overlayX, overlayY, overlayWidth, overlayHeight);
        m_hotkeysOverlay->raise();
    }
    m_hotkeysOverlay->raise();

    layoutTiles();
    updateBackground(m_selectedIndex);
    highlightSelectedTile();
    centerSelectedTileAnimated();
    updateDepthEffect();
}
