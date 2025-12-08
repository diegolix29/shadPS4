// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent> // Added for mouse events
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

#include <memory>
#include <vector>

#include "game_info.h"
#include "qt_gui/compatibility_info.h"

class GameInfoClass;
class IpcClient;

class GameCard : public QWidget {
    Q_OBJECT
public:
    explicit GameCard(const GameInfo& game, int index, QWidget* parent = nullptr);

    int getIndex() const {
        return m_index;
    }
    std::string getSerial() const {
        return m_game.serial;
    }

    void setSelected(bool selected);

signals:
    void cardClicked(int index);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    GameInfo m_game;
    int m_index = -1;
    bool m_selected = false;

    QLabel* m_iconLabel = nullptr;
};

class GameCinematicFrame : public QWidget {
    Q_OBJECT

public:
    explicit GameCinematicFrame(std::shared_ptr<GameInfoClass> game_info_get,
                                std::shared_ptr<CompatibilityInfoClass> compat_info_get,
                                std::shared_ptr<IpcClient> ipc_client, QWidget* parent = nullptr);

    void PopulateGameList();
    void RefreshBackground();

signals:
    void launchGameRequested(int index);

private slots:
    void onCardClicked(int index);
    void onPlayButtonClicked();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    QImage backgroundImage;

    void SetupUI();
    void UpdateHeroSection(const GameInfo& game);

    std::shared_ptr<GameInfoClass> m_game_info;
    std::shared_ptr<CompatibilityInfoClass> m_compat_info;
    std::shared_ptr<IpcClient> m_ipc_client;

    int m_currentIndex = -1;

    QPixmap m_background;

    QLabel* m_titleLabel = nullptr;
    QLabel* m_idLabel = nullptr;
    QLabel* m_detailsLabel = nullptr;
    QLabel* m_compatLabel = nullptr;
    QPushButton* m_playButton = nullptr;

    QScrollArea* m_scrollArea = nullptr;
    QWidget* m_scrollContent = nullptr;
    QHBoxLayout* m_scrollLayout = nullptr;
};