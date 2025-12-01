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

class HotkeysOverlay : public QWidget {
    Q_OBJECT
public:
    explicit HotkeysOverlay(QWidget* parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TranslucentBackground);
        setFocusPolicy(Qt::NoFocus);

        m_layout = new QHBoxLayout(this);
        m_layout->setContentsMargins(8, 0, 8, 0);
        m_layout->setSpacing(12);
        m_layout->setAlignment(Qt::AlignCenter);
    }

    void setHotkeys(const std::vector<std::pair<QString, QString>>& hotkeys) {
        QLayoutItem* child;
        while ((child = m_layout->takeAt(0)) != nullptr) {
            delete child->widget();
            delete child;
        }

        for (const auto& hk : hotkeys) {
            QLabel* lbl = new QLabel(QString("%1: %2").arg(hk.first, hk.second), this);
            lbl->setStyleSheet("color: white; background-color: rgba(0,0,0,160); "
                               "padding: 4px 8px; border-radius: 6px; font-size: 14px;");
            m_layout->addWidget(lbl);
        }

        m_layout->setAlignment(Qt::AlignCenter);
        updateGeometry();
    }

private:
    QHBoxLayout* m_layout;
};

class AnimatedTile : public QWidget {
    Q_OBJECT
public:
    explicit AnimatedTile(QWidget* parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_TranslucentBackground);
        setContentsMargins(0, 0, 0, 0);

        m_content = new QWidget(this);
        m_content->setObjectName("tile_content");
        m_content->setAttribute(Qt::WA_TranslucentBackground);
        m_content->setContentsMargins(0, 0, 0, 0);

        QVBoxLayout* l = new QVBoxLayout(m_content);
        l->setSpacing(10);
        l->setContentsMargins(0, 0, 0, 0);
    }

    QWidget* content() const {
        return m_content;
    }

protected:
    void resizeEvent(QResizeEvent* ev) override {
        QWidget::resizeEvent(ev);
        if (m_content)
            m_content->setGeometry(rect());
    }

private:
    QWidget* m_content = nullptr;
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

    void updateDepthEffect();

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
    void showEvent(QShowEvent* ev) override;
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
    enum class FocusMode { Tiles, Buttons };

    FocusMode m_focusMode = FocusMode::Tiles;
    HotkeysOverlay* m_hotkeysOverlay = nullptr;

    std::vector<QWidget*> m_tiles;
    int m_selectedIndex = 0;
    bool m_visible = false;
    bool m_navigationLocked = false;

    QGraphicsOpacityEffect* m_opacity = nullptr;
    QPropertyAnimation* m_fadeIn = nullptr;
    QPropertyAnimation* m_fadeOut = nullptr;
    QPropertyAnimation* m_scrollAnim = nullptr;
};
