// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <vector>
#include <QPainter>
#include <QPointer>
#include <QPropertyAnimation>
#include <QWidget>
#include "main_window_themes.h"

class GameInfoClass;
class CompatibilityInfoClass;
class IpcClient;

class QLabel;
class QScrollArea;
class QHBoxLayout;
class QVBoxLayout;
class QWidget;
class QGraphicsOpacityEffect;
class QPropertyAnimation;
class QPushButton;

class AnimatedTile : public QWidget {
    Q_OBJECT
    Q_PROPERTY(qreal scale READ scale WRITE setScale)

public:
    explicit AnimatedTile(QWidget* parent = nullptr) : QWidget(parent), m_scale(1.0) {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setContentsMargins(0, 0, 0, 0);
    }

    qreal scale() const {
        return m_scale;
    }
    void setScale(qreal s) {
        if (!qFuzzyCompare(s, m_scale)) {
            m_scale = s;
            update();
        }
    }

protected:
    void paintEvent(QPaintEvent* e) override {
        Q_UNUSED(e);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);

        p.translate(width() / 2, height() / 2);
        p.scale(m_scale, m_scale);
        p.translate(-width() / 2, -height() / 2);

        QWidget::render(&p);
    }

private:
    qreal m_scale;
};

class ScrollingLabel : public QLabel {
    Q_OBJECT

public:
    explicit ScrollingLabel(QWidget* parent = nullptr) : QLabel(parent) {
        m_timer = new QTimer(this);
        m_timer->setInterval(25);
        connect(m_timer, &QTimer::timeout, this, &ScrollingLabel::onTick);
    }

    void startScrollIfNeeded() {
        QFontMetrics fm(font());
        int textW = fm.horizontalAdvance(text());
        int labelW = width();

        if (textW > labelW) {
            m_needScroll = true;
            m_offset = 0;
            m_timer->start();
        } else {
            m_needScroll = false;
            m_timer->stop();
            update();
        }
    }

protected:
    void resizeEvent(QResizeEvent* e) override {
        QLabel::resizeEvent(e);
        startScrollIfNeeded();
    }

    void paintEvent(QPaintEvent* ev) override {
        Q_UNUSED(ev);
        QPainter p(this);
        p.setPen(palette().windowText().color());

        if (!m_needScroll) {
            p.drawText(rect(), alignment(), text());
            return;
        }

        QFontMetrics fm(font());
        int textW = fm.horizontalAdvance(text());
        int y = (height() + fm.ascent() - fm.descent()) / 2;

        p.drawText(-m_offset, y, text());
        p.drawText(textW - m_offset + 40, y, text());
    }

private slots:
    void onTick() {
        QFontMetrics fm(font());
        int textW = fm.horizontalAdvance(text());

        m_offset += 2;
        if (m_offset > textW + 40)
            m_offset = 0;

        update();
    }

private:
    QTimer* m_timer;
    int m_offset = 0;
    bool m_needScroll = false;
};

class BigPictureWidget : public QWidget {
    Q_OBJECT
public:
    BigPictureWidget(std::shared_ptr<GameInfoClass> gameInfo,
                     std::shared_ptr<CompatibilityInfoClass> compatInfo,
                     std::shared_ptr<IpcClient> ipcClient, WindowThemes* themes = nullptr,
                     QWidget* parent = nullptr);
    ~BigPictureWidget();

    void toggle();
    void showFull();
    void hideFull();

    enum class GamepadButton { Left, Right, South, East, West, North };
    std::shared_ptr<GameInfoClass> m_gameInfo;
    std::shared_ptr<CompatibilityInfoClass> m_compatInfo;
    std::shared_ptr<IpcClient> m_ipcClient;
signals:
    void launchGameRequested(int index);
    void openModsManagerRequested(int index);
    void openHotkeysRequested();
    void globalConfigRequested();
    void gameConfigRequested(int index);
    void centered();

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;
    void keyPressEvent(QKeyEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private slots:
    void onPlayClicked();
    void onGlobalConfigClicked();
    void onGameConfigClicked();
    void onQuitClicked();

public:
    void handleGamepadButton(GamepadButton btn);

private:
    void buildUi();
    void layoutTiles();
    void onModsClicked();
    void onHotkeysClicked();
    QWidget* buildTile(const GameInfo& g);
    void buildAnimations();
    void updateBackground(int index);
    void highlightSelectedTile();
    void zoomSelectedTile(QWidget* tile, const QRect& baseGeom, qreal factor);
    void centerSelectedTileAnimated();
    void ensureSelectionValid();
    WindowThemes* m_themes = nullptr;
    void applyTheme();
    QLabel* m_background = nullptr;
    QLabel* m_dim = nullptr;
    QScrollArea* m_scroll = nullptr;
    QWidget* m_container = nullptr;
    QHBoxLayout* m_hbox = nullptr;
    QWidget* m_bottomBar = nullptr;
    QPushButton* m_btnGameCfg = nullptr;
    QPushButton* m_btnGlobalCfg = nullptr;
    QPushButton* m_btnPlay = nullptr;
    QPushButton* m_btnHotkeys = nullptr;
    QPushButton* m_btnMods = nullptr;
    QPushButton* m_btnQuit = nullptr;
    WindowThemes m_window_themes;

    std::vector<QWidget*> m_tiles;
    int m_selectedIndex = 0;
    bool m_visible = false;

    QGraphicsOpacityEffect* m_opacity = nullptr;
    QPropertyAnimation* m_fadeIn = nullptr;
    QPropertyAnimation* m_fadeOut = nullptr;
    QPropertyAnimation* m_scrollAnim = nullptr;
};
