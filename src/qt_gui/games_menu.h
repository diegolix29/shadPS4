// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <vector>
#include <QAudioOutput>
#include <QMediaPlayer>
#include <QPainter>
#include <QPointer>
#include <QPropertyAnimation>
#include <QStyleOption>
#include <QWidget>
#include "main_window_themes.h"

class GameInfoClass;
class CompatibilityInfoClass;
class IpcClient;

class QLabel;
class QScrollArea;
class QHBoxLayout;
class QVBoxLayout;
class QGraphicsOpacityEffect;
class QPropertyAnimation;
class QPushButton;

class HotkeysOverlay : public QWidget {
    Q_OBJECT
public:
    explicit HotkeysOverlay(Qt::Orientation orientation, QWidget* parent = nullptr)
        : QWidget(parent), m_orientation(orientation) {

        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_StyledBackground, true);
        setFocusPolicy(Qt::NoFocus);
        m_mainLayout = new QVBoxLayout(this);
        m_mainLayout->setContentsMargins(0, 0, 0, 0);
        m_mainLayout->setSpacing(2);

        m_titleLabel = new QLabel(this);
        m_titleLabel->setStyleSheet(
            "color: white; font-size: 24px; font-weight: bold; padding: 6px 12px;");
        m_titleLabel->setText("Hotkeys & Navigation Keys");
        m_titleLabel->setAlignment(Qt::AlignCenter);
        m_mainLayout->addWidget(m_titleLabel);

        if (m_orientation == Qt::Vertical) {
            m_mainLayout->setAlignment(Qt::AlignCenter);
            m_topRow = nullptr;
            m_bottomRow = nullptr;
        } else {
            m_mainLayout->setAlignment(Qt::AlignCenter);

            m_topRow = new QHBoxLayout();
            m_bottomRow = new QHBoxLayout();

            m_topRow->setAlignment(Qt::AlignCenter);

            m_bottomRow->setAlignment(Qt::AlignCenter);

            m_mainLayout->addLayout(m_topRow);
            m_mainLayout->addLayout(m_bottomRow);
        }
    }

    void paintEvent(QPaintEvent*) override {
        QStyleOption opt;
        opt.initFrom(this);
        QPainter p(this);
        style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
    }

    void setTitle(const QString& title) {
        if (!m_titleLabel)
            return;

        m_titleLabel->setText(title);
    }
    void setHotkeys(const std::vector<std::pair<QString, QString>>& hotkeys) {
        auto clearLayout = [](QLayout* layout) {
            if (!layout)
                return;
            QLayoutItem* child;
            while ((child = layout->takeAt(0))) {
                if (child->widget())
                    delete child->widget();
                delete child;
            }
        };

        if (m_orientation == Qt::Vertical) {
            QLayoutItem* child;
            while ((child = m_mainLayout->takeAt(1))) {
                if (child->widget())
                    delete child->widget();
                delete child;
            }
        } else {
            clearLayout(m_topRow);
            clearLayout(m_bottomRow);
        }
        for (const auto& hk : hotkeys) {
            QString text;
            if (hk.first.isEmpty()) {
                text = "";
            } else {
                text = QString("%1: %2").arg(hk.first, hk.second);
            }

            QLabel* lbl = new QLabel(text, this);

            if (m_orientation == Qt::Vertical) {
                lbl->setAlignment(Qt::AlignCenter);
                m_mainLayout->addWidget(lbl);
            } else {
                lbl->setAlignment(Qt::AlignCenter);
                if (hk.first.startsWith("Press")) {
                    if (m_topRow)
                        m_topRow->addWidget(lbl);
                } else {
                    if (m_bottomRow)
                        m_bottomRow->addWidget(lbl);
                }
            }
        }

        m_mainLayout->activate();
        adjustSize();
        updateGeometry();
    }

private:
    Qt::Orientation m_orientation;
    QVBoxLayout* m_mainLayout;
    QLabel* m_titleLabel = nullptr;
    QHBoxLayout* m_topRow = nullptr;
    QHBoxLayout* m_bottomRow = nullptr;
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
    enum class FocusMode { Tiles, Buttons };

    void toggle();
    void playNavSound();
    void showFull();
    void hideFull();

    void updateDepthEffect();

    enum class GamepadButton { Left, Right, South, East, West, North };
    std::shared_ptr<GameInfoClass> m_gameInfo;
    std::shared_ptr<CompatibilityInfoClass> m_compatInfo;
    std::shared_ptr<IpcClient> m_ipcClient;
signals:
    void launchRequestedFromGameMenu(int index);
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
    void focusInEvent(QFocusEvent* event) override;

public:
    void handleGamepadButton(GamepadButton btn);

    void toggleBackgroundMusic();
    void setMinimalUi(bool hide);

private:
    void buildUi();
    void layoutTiles();
    void UpdateCurrentGameAudio();
    void showEvent(QShowEvent* ev) override;
    void onModsClicked();
    void onHotkeysClicked();
    void setDimVisible(bool visible);
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
    QWidget* m_dim = nullptr;
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
    QMediaPlayer* m_player = nullptr;
    QAudioOutput* m_audioOutput = nullptr;
    QMediaPlayer* m_uiSound = nullptr;
    QAudioOutput* m_uiOutput = nullptr;
    QMediaPlayer* m_playSound = nullptr;
    QAudioOutput* m_playOutput = nullptr;
    bool m_hideUi = false;

    FocusMode m_focusMode = FocusMode::Tiles;
    HotkeysOverlay* m_hotkeysOverlay = nullptr;

    std::vector<QWidget*> m_tiles;
    int m_selectedIndex = 0;
    bool m_visible = false;
    bool m_navigationLocked = false;
    bool m_scrollBarHidden = false;
    bool m_bottomBarHidden = false;
    bool m_overlayVisible = false;
    QGraphicsOpacityEffect* m_dimOpacity = nullptr;
    QPropertyAnimation* m_dimFade = nullptr;
    QGraphicsOpacityEffect* m_opacity = nullptr;
    QPropertyAnimation* m_fadeIn = nullptr;
    QPropertyAnimation* m_fadeOut = nullptr;
    QPropertyAnimation* m_scrollAnim = nullptr;
};