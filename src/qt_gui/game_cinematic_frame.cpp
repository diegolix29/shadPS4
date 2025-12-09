// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/config.h"
#include "common/path_util.h"
#include "game_cinematic_frame.h"

#include <filesystem>
#include <QFile>
#include <QGraphicsDropShadowEffect>
#include <QImage>
#include <QKeyEvent>
#include <QPainter>
#include <QScrollBar>

GameCard::GameCard(const GameInfo& game, int index, QWidget* parent)
    : QWidget(parent), m_game(game), m_index(index) {

    setFixedSize(140, 200);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    m_selected = false;

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(5, 5, 5, 5);
    layout->setSpacing(5);

    m_iconLabel = new QLabel(this);
    QPixmap icon = QPixmap::fromImage(game.icon).scaled(120, 120, Qt::KeepAspectRatio,
                                                        Qt::SmoothTransformation);
    m_iconLabel->setPixmap(icon);
    m_iconLabel->setAlignment(Qt::AlignCenter);

    QLabel* title = new QLabel(QString::fromStdString(game.name), this);
    title->setWordWrap(true);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet("color: white; font-size: 11px; font-weight: bold;");

    layout->addWidget(m_iconLabel);
    layout->addWidget(title);
    layout->addStretch();
}

void GameCard::setSelected(bool selected) {
    m_selected = selected;
    if (selected) {
        setStyleSheet("background-color: rgba(255,255,255,40);"
                      "border-radius: 10px;");
    } else {
        setStyleSheet("background-color: transparent;");
    }
}

void GameCard::mousePressEvent(QMouseEvent* event) {
    emit cardClicked(m_index);
    QWidget::mousePressEvent(event);
}

void GameCard::enterEvent(QEnterEvent* event) {
    if (!m_selected)
        setStyleSheet("background-color: rgba(255,255,255,30);"
                      "border-radius: 10px;");
    QWidget::enterEvent(event);
}

void GameCard::leaveEvent(QEvent* event) {
    if (!m_selected)
        setStyleSheet("background-color: transparent;");
    QWidget::leaveEvent(event);
}

GameCinematicFrame::GameCinematicFrame(std::shared_ptr<GameInfoClass> game_info_get,
                                       std::shared_ptr<CompatibilityInfoClass> compat_info_get,
                                       std::shared_ptr<IpcClient> ipc_client, QWidget* parent)
    : QWidget(parent), m_game_info(game_info_get), m_compat_info(compat_info_get),
      m_ipc_client(ipc_client) {

    setObjectName("CinematicFrame");
    setFocusPolicy(Qt::StrongFocus);

    SetupUI();
    PopulateGameList();
}

void GameCinematicFrame::SetupUI() {
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    QWidget* heroWidget = new QWidget(this);

    QHBoxLayout* heroLayout = new QHBoxLayout(heroWidget);
    heroLayout->setContentsMargins(40, 40, 40, 20);

    QVBoxLayout* infoLayout = new QVBoxLayout();
    infoLayout->setAlignment(Qt::AlignBottom | Qt::AlignLeft);

    m_titleLabel = new QLabel("Select a Game", this);
    m_titleLabel->setStyleSheet("font-size: 36px; font-weight: bold; color: white;");

    auto* shadow = new QGraphicsDropShadowEffect(this);
    shadow->setBlurRadius(12);
    shadow->setColor(QColor(0, 0, 0, 200));
    shadow->setOffset(2, 2);
    m_titleLabel->setGraphicsEffect(shadow);

    m_idLabel = new QLabel(this);
    m_idLabel->setStyleSheet("font-size: 13px; color: #e0e0e0;"
                             "padding: 2px 6px; background: rgba(0,0,0,120);"
                             "border-radius: 4px;");

    m_detailsLabel = new QLabel(this);
    m_detailsLabel->setStyleSheet("font-size: 14px; color: #cccccc;");

    m_compatLabel = new QLabel(this);
    m_compatLabel->setStyleSheet("font-size: 14px; font-weight: bold;"
                                 "padding: 4px 8px; border-radius: 4px;");

    m_playButton = new QPushButton(tr("Play Game"), this);
    m_playButton->setFixedSize(150, 45);
    m_playButton->setCursor(Qt::PointingHandCursor);
    m_playButton->setStyleSheet("QPushButton { background-color: #0078d4; color: white; "
                                "font-size: 16px; font-weight: bold; border-radius: 5px; }"
                                "QPushButton:hover { background-color: #0099ff; }"
                                "QPushButton:pressed { background-color: #005a9e; }");

    connect(m_playButton, &QPushButton::clicked, this, &GameCinematicFrame::onPlayButtonClicked);

    m_playButton->hide();

    infoLayout->addWidget(m_titleLabel);
    infoLayout->addWidget(m_idLabel);
    infoLayout->addWidget(m_compatLabel);
    infoLayout->addWidget(m_detailsLabel);
    infoLayout->addSpacing(15);
    infoLayout->addWidget(m_playButton);

    heroLayout->addLayout(infoLayout);
    heroLayout->addStretch();

    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setFixedHeight(240);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setStyleSheet("background: rgba(0,0,0,110);"
                                "border-top: 1px solid rgba(255,255,255,40);");

    m_scrollContent = new QWidget();
    m_scrollContent->setStyleSheet("background: transparent;");

    m_scrollLayout = new QHBoxLayout(m_scrollContent);
    m_scrollLayout->setContentsMargins(20, 10, 20, 10);
    m_scrollLayout->setSpacing(15);
    m_scrollLayout->setAlignment(Qt::AlignLeft);

    m_scrollArea->setWidget(m_scrollContent);

    mainLayout->addWidget(heroWidget, 1);
    mainLayout->addWidget(m_scrollArea, 0);
}

void GameCinematicFrame::PopulateGameList() {
    while (QLayoutItem* item = m_scrollLayout->takeAt(0)) {
        if (item->widget()) {
            delete item->widget();
        }
        delete item;
    }

    const auto& games = m_game_info->m_games;
    for (size_t i = 0; i < games.size(); ++i) {
        auto* card = new GameCard(games[i], static_cast<int>(i), m_scrollContent);
        connect(card, &GameCard::cardClicked, this, &GameCinematicFrame::onCardClicked);
        m_scrollLayout->addWidget(card);
    }

    if (!games.empty())
        onCardClicked(0);
}

void GameCinematicFrame::RefreshBackground() {
    QPalette palette;
    if (!backgroundImage.isNull() && Config::getShowBackgroundImage()) {
        QSize widgetSize = size();
        QPixmap scaledPixmap =
            QPixmap::fromImage(backgroundImage)
                .scaled(widgetSize, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        int x = (widgetSize.width() - scaledPixmap.width()) / 2;
        int y = (widgetSize.height() - scaledPixmap.height()) / 2;
        QPixmap finalPixmap(widgetSize);
        finalPixmap.fill(Qt::transparent);
        QPainter painter(&finalPixmap);
        painter.drawPixmap(x, y, scaledPixmap);
        palette.setBrush(QPalette::Base, QBrush(finalPixmap));
    }
    QColor transparentColor = QColor(135, 206, 235, 40);
    palette.setColor(QPalette::Highlight, transparentColor);
    this->setPalette(palette);
}

void GameCinematicFrame::onCardClicked(int index) {
    if (index < 0 || index >= (int)m_game_info->m_games.size())
        return;

    for (int i = 0; i < m_scrollLayout->count(); ++i) {
        auto* card = qobject_cast<GameCard*>(m_scrollLayout->itemAt(i)->widget());
        if (card)
            card->setSelected(i == index);
    }

    m_currentIndex = index;
    UpdateHeroSection(m_game_info->m_games[index]);

    if (m_scrollLayout->count() > index && m_scrollLayout->itemAt(index) &&
        m_scrollLayout->itemAt(index)->widget()) {
        m_scrollArea->ensureWidgetVisible(m_scrollLayout->itemAt(index)->widget(), 50, 0);
    }

    setFocus();
}

static QString pathToQString(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(u8.data()), static_cast<int>(u8.size()));
}

void GameCinematicFrame::UpdateHeroSection(const GameInfo& game) {
    m_titleLabel->setText(QString::fromStdString(game.name));
    m_idLabel->setText(QString::fromStdString(game.serial));

    m_detailsLabel->setText(QString("Region: %1 | Version: %2")
                                .arg(QString::fromStdString(game.region))
                                .arg(QString::fromStdString(game.version)));

    auto compat = m_compat_info->GetCompatibilityInfo(game.serial);
    m_compatLabel->setText(m_compat_info->GetCompatStatusString(compat.status));

    QString color = "#828282";
    if (compat.status == CompatibilityStatus::Playable)
        color = "#47D35C";
    else if (compat.status == CompatibilityStatus::Ingame)
        color = "#F2D624";
    else if (compat.status == CompatibilityStatus::Menus)
        color = "#FF6A00";

    m_compatLabel->setStyleSheet(QString("background:%1; color:black;"
                                         "padding:4px 8px; border-radius:4px; font-weight:bold;")
                                     .arg(color));

    m_playButton->show();

    if (Config::getShowBackgroundImage() && !game.pic_path.empty()) {
        QString imagePath = pathToQString(game.pic_path);
        QImage original_image(imagePath);
        if (!original_image.isNull()) {

            m_background = QPixmap::fromImage(original_image);
            update();
        } else {
            m_background = QPixmap();
            update();
        }
    } else {
        m_background = QPixmap();
        update();
    }
}

void GameCinematicFrame::keyPressEvent(QKeyEvent* event) {
    if (m_game_info->m_games.empty()) {
        QWidget::keyPressEvent(event);
        return;
    }

    if (event->key() == Qt::Key_Right) {
        int next = qMin(m_currentIndex + 1, (int)m_game_info->m_games.size() - 1);
        onCardClicked(next);
        event->accept();
        return;
    } else if (event->key() == Qt::Key_Left) {
        int prev = qMax(m_currentIndex - 1, 0);
        onCardClicked(prev);
        event->accept();
        return;
    } else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        onPlayButtonClicked();
        event->accept();
        return;
    }

    QWidget::keyPressEvent(event);
}

void GameCinematicFrame::onPlayButtonClicked() {
    if (m_currentIndex >= 0 && m_currentIndex < (int)m_game_info->m_games.size()) {
    }
    emit launchGameRequested(m_currentIndex);
}
void GameCinematicFrame::mouseDoubleClickEvent(QMouseEvent* event) {
    if (m_currentIndex >= 0 && m_currentIndex < static_cast<int>(m_game_info->m_games.size())) {
        emit launchGameRequested(m_currentIndex);
    }

    QWidget::mouseDoubleClickEvent(event);
}

void GameCinematicFrame::paintEvent(QPaintEvent*) {
    QPainter p(this);
    RefreshBackground();
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    if (!m_background.isNull()) {
        p.drawPixmap(rect(), m_background.scaled(size(), Qt::KeepAspectRatioByExpanding,
                                                 Qt::SmoothTransformation));
    }

    QLinearGradient g(0, 0, 0, height());
    g.setColorAt(0.0, QColor(0, 0, 0, 100));
    g.setColorAt(1.0, QColor(0, 0, 0, 220));
    p.fillRect(rect(), g);
}
