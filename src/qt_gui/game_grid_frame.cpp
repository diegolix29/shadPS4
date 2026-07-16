// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include "common/path_util.h"
#include "game_grid_frame.h"
#include "main_window.h"
#include "qt_gui/compatibility_info.h"

class SpinningTextLabel : public QLabel {
public:
    SpinningTextLabel(const QString& text, QWidget* parent = nullptr) : QLabel(parent) {
        m_originalText = text;
        m_scrollOffset = 0;
        m_scrollDirection = 1;
        m_scrollTimer = new QTimer(this);
        connect(m_scrollTimer, &QTimer::timeout, [this]() { scrollText(); });
        m_scrollTimer->start(50);
        setText(text);
    }

    void setText(const QString& text) {
        m_originalText = text;
        QLabel::setText(text);
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        QFontMetrics fm(font());
        int textWidth = fm.horizontalAdvance(m_originalText);
        int labelWidth = width();

        if (textWidth <= labelWidth) {
            QLabel::paintEvent(event);
            return;
        }

        painter.setPen(palette().color(QPalette::WindowText));
        painter.setFont(font());

        int xPos = -m_scrollOffset;
        painter.drawText(xPos, fm.ascent(), m_originalText);

        if (xPos + textWidth < labelWidth) {
            painter.drawText(xPos + textWidth + 20, fm.ascent(), m_originalText);
        }
    }

private:
    void scrollText() {
        QFontMetrics fm(font());
        int textWidth = fm.horizontalAdvance(m_originalText);
        int labelWidth = width();

        if (textWidth <= labelWidth) {
            return;
        }

        m_scrollOffset += 2;

        if (m_scrollOffset > textWidth + 20) {
            m_scrollOffset = 0;
        }

        update();
    }

    QString m_originalText;
    int m_scrollOffset;
    int m_scrollDirection;
    QTimer* m_scrollTimer;
};

static QPixmap RoundedCover(const QPixmap& source, int radius) {
    if (source.isNull()) {
        return source;
    }
    QPixmap rounded(source.size());
    rounded.fill(Qt::transparent);

    QPainter painter(&rounded);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath path;
    path.addRoundedRect(QRectF(rounded.rect()), radius, radius);
    painter.setClipPath(path);
    painter.drawPixmap(0, 0, source);
    return rounded;
}

GameGridFrame::GameGridFrame(std::shared_ptr<GameInfoClass> game_info_get,
                             std::shared_ptr<CompatibilityInfoClass> compat_info_get,
                             std::shared_ptr<IpcClient> ipc_client, QWidget* parent)
    : QTableWidget(parent), m_game_info(game_info_get), m_compat_info(compat_info_get),
      m_ipc_client(ipc_client) {

    icon_size = Config::getIconSizeGrid();
    windowWidth = parent->width();

    this->setObjectName("GameGridFrame");

    this->setShowGrid(false);
    this->setEditTriggers(QAbstractItemView::NoEditTriggers);
    this->setSelectionBehavior(QAbstractItemView::SelectItems);
    this->setSelectionMode(QAbstractItemView::SingleSelection);
    this->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    this->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    this->verticalScrollBar()->installEventFilter(this);
    this->verticalScrollBar()->setSingleStep(20);
    this->horizontalScrollBar()->setSingleStep(20);
    this->horizontalHeader()->setVisible(false);
    this->verticalHeader()->setVisible(false);
    this->setContextMenuPolicy(Qt::CustomContextMenu);
    PopulateGameGrid(m_game_info->m_games, false);

    connect(this, &QTableWidget::currentCellChanged, this, &GameGridFrame::onCurrentCellChanged);

    connect(this->verticalScrollBar(), &QScrollBar::valueChanged, this,
            &GameGridFrame::RefreshGridBackgroundImage);
    connect(this->horizontalScrollBar(), &QScrollBar::valueChanged, this,
            &GameGridFrame::RefreshGridBackgroundImage);
    connect(this, &QTableWidget::customContextMenuRequested, this, [=, this](const QPoint& pos) {
        int changedFavorite = m_gui_context_menus.RequestGameMenu(
            pos, m_game_info->m_games, m_compat_info, m_ipc_client, this, false,
            [mw = QPointer<MainWindow>(qobject_cast<MainWindow*>(this->window()))](
                const QStringList& args) {
                if (mw)
                    mw.data()->StartGameWithArgs(args);
            });
        PopulateGameGrid(m_game_info->m_games, false);
    });
}

void GameGridFrame::onCurrentCellChanged(int currentRow, int currentColumn, int previousRow,
                                         int previousColumn) {
    // Early exit for invalid indices
    if (currentRow < 0 || currentColumn < 0) {
        cellClicked = false;
        validCellSelected = false;
        BackgroundMusicPlayer::getInstance().stopMusic();
        return;
    }

    crtRow = currentRow;
    crtColumn = currentColumn;
    columnCnt = this->columnCount();

    // Prevent integer overflow
    if (columnCnt <= 0 || crtRow > (std::numeric_limits<int>::max() / columnCnt)) {
        cellClicked = false;
        validCellSelected = false;
        BackgroundMusicPlayer::getInstance().stopMusic();
        return;
    }

    auto itemID = (crtRow * columnCnt) + currentColumn;
    if (itemID < 0 || itemID > m_game_info->m_games.count() - 1) {
        cellClicked = false;
        validCellSelected = false;
        BackgroundMusicPlayer::getInstance().stopMusic();
        return;
    }

    cellClicked = true;
    validCellSelected = true;
    SetGridBackgroundImage(crtRow, crtColumn);
    auto snd0Path = QString::fromStdString(m_game_info->m_games[itemID].snd0_path.string());
    PlayBackgroundMusic(snd0Path);
}

void GameGridFrame::PlayBackgroundMusic(QString path) {
    if (path.isEmpty() || !Config::getPlayBGM()) {
        BackgroundMusicPlayer::getInstance().stopMusic();
        return;
    }
    BackgroundMusicPlayer::getInstance().playMusic(path);
}

void GameGridFrame::PopulateGameGrid(QVector<GameInfo> m_games_search, bool fromSearch) {
    this->crtRow = -1;
    this->crtColumn = -1;
    QVector<GameInfo> m_games_;
    this->clearContents();
    if (fromSearch) {
        SortByFavorite(&m_games_search);
        m_games_ = m_games_search;
    } else {
        SortByFavorite(&(m_game_info->m_games));
        m_games_ = m_game_info->m_games;
    }
    m_games_shared = std::make_shared<QVector<GameInfo>>(m_games_);
    icon_size = Config::getIconSizeGrid(); // update icon size for resize event.

    int gamesPerRow = windowWidth / (icon_size + 20); // 2 x cell widget border size.
    if (gamesPerRow < 1)
        gamesPerRow = 1;

    int row = 0;
    int gameCounter = 0;
    int rowCount = m_games_.size() / gamesPerRow;
    if (m_games_.size() % gamesPerRow != 0) {
        rowCount += 1;
    }

    int column = 0;
    this->setColumnCount(gamesPerRow);
    this->setRowCount(rowCount);
    for (int i = 0; i < m_games_.size(); i++) {
        QWidget* widget = new QWidget();

        widget->setAttribute(Qt::WA_TranslucentBackground);
        widget->setFixedWidth(icon_size + 10);

        QVBoxLayout* layout = new QVBoxLayout();
        layout->setContentsMargins(5, 5, 5, 5);
        layout->setSpacing(8);

        QWidget* image_container = new QWidget();
        image_container->setFixedSize(icon_size, icon_size);
        image_container->setAttribute(Qt::WA_TranslucentBackground);

        QLabel* image_label = new QLabel(image_container);
        QImage icon = m_games_[gameCounter].icon.scaled(
            QSize(icon_size, icon_size), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        image_label->setFixedSize(icon.width(), icon.height());
        image_label->setPixmap(RoundedCover(QPixmap::fromImage(icon), 12));
        image_label->move(0, 0);

        QGraphicsDropShadowEffect* coverShadow = new QGraphicsDropShadowEffect();
        coverShadow->setBlurRadius(25);
        coverShadow->setColor(QColor(0, 0, 0, 180));
        coverShadow->setOffset(0, 6);
        image_container->setGraphicsEffect(coverShadow);

        SetFavoriteIcon(image_container, m_games_, gameCounter);
        SetGameConfigIcon(image_container, m_games_, gameCounter);

        SpinningTextLabel* name_label =
            new SpinningTextLabel(QString::fromStdString(m_games_[gameCounter].name));
        name_label->setAlignment(Qt::AlignHCenter);
        name_label->setWordWrap(false);
        name_label->setFixedWidth(icon_size);

        QLabel* serial_label = new QLabel(QString::fromStdString(m_games_[gameCounter].serial));
        serial_label->setAlignment(Qt::AlignHCenter);
        serial_label->setFixedWidth(icon_size);

        layout->addWidget(image_container);
        layout->addWidget(name_label);
        layout->addWidget(serial_label);

        float fontSize = (Config::getIconSizeGrid() / 5.5f);
        QString nameStyleSheet =
            QString("color: white; font-weight: bold; font-size: %1px; background: transparent;")
                .arg(fontSize);
        name_label->setStyleSheet(nameStyleSheet);

        float serialFontSize = (Config::getIconSizeGrid() / 8.0f);
        QString serialStyleSheet =
            QString(
                "color: #cccccc; font-weight: normal; font-size: %1px; background: transparent;")
                .arg(serialFontSize);
        serial_label->setStyleSheet(serialStyleSheet);

        QGraphicsDropShadowEffect* shadowEffect = new QGraphicsDropShadowEffect();
        shadowEffect->setBlurRadius(8);
        shadowEffect->setColor(QColor(0, 0, 0, 200));
        shadowEffect->setOffset(0, 2);

        name_label->setGraphicsEffect(shadowEffect);
        widget->setLayout(layout);
        QString tooltipText = QString::fromStdString(m_games_[gameCounter].name + " (" +
                                                     m_games_[gameCounter].version + ", " +
                                                     m_games_[gameCounter].region + ")");
        widget->setToolTip(tooltipText);

        QString tooltipStyle = QString("QWidget { background-color: transparent; }"
                                       "QToolTip {"
                                       "background-color: #ffffff;"
                                       "color: #000000;"
                                       "border: 1px solid #000000;"
                                       "padding: 2px;"
                                       "font-size: 12px; }");
        widget->setStyleSheet(tooltipStyle);

        this->setCellWidget(row, column, widget);

        column++;
        if (column == gamesPerRow) {
            column = 0;
            row++;
        }

        gameCounter++;
        if (gameCounter >= m_games_.size()) {
            break;
        }
    }
    m_games_.clear();
    this->resizeRowsToContents();
    this->resizeColumnsToContents();
}

void GameGridFrame::SetGridBackgroundImage(int row, int column) {
    backgroundImage = QImage();
    m_last_opacity = -1;
    m_current_game_path.clear();
    RefreshGridBackgroundImage();
    return;
}

void GameGridFrame::SetCustomBackgroundImage(const QString& filePath) {
    if (filePath.isEmpty()) {
        backgroundImage = QImage();
        m_last_opacity = -1;
        m_current_game_path.clear();
        Config::setCustomBackgroundImage("");
        RefreshGridBackgroundImage();
        return;
    }

    QImage original_image(filePath);
    if (!original_image.isNull()) {
        const int opacity = Config::getBackgroundImageOpacity();
        backgroundImage = m_game_list_utils.ChangeImageOpacity(
            original_image, original_image.rect(), opacity / 100.0f);

        m_last_opacity = opacity;
        m_current_game_path = filePath.toStdString();

        Config::setCustomBackgroundImage(filePath.toStdString());
    }

    RefreshGridBackgroundImage();
}

void GameGridFrame::LoadBackgroundImage(const QString& filePath) {
    if (filePath.isEmpty()) {
        backgroundImage = QImage();
        m_last_opacity = -1;
        m_current_game_path.clear();
        RefreshGridBackgroundImage();
        return;
    }

    QImage original_image(filePath);
    if (!original_image.isNull()) {
        const int opacity = Config::getBackgroundImageOpacity();
        backgroundImage = m_game_list_utils.ChangeImageOpacity(
            original_image, original_image.rect(), opacity / 100.0f);

        m_last_opacity = opacity;
        m_current_game_path = filePath.toStdString();
    }

    RefreshGridBackgroundImage();
}

void GameGridFrame::RefreshGridBackgroundImage() {

    QPalette palette = this->palette();

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
    } else {
        palette.setBrush(QPalette::Base, QBrush(Qt::NoBrush));
    }

    QColor transparentColor = QColor(135, 206, 235, 40);
    palette.setColor(QPalette::Highlight, transparentColor);

    this->setPalette(palette);
}

void GameGridFrame::resizeEvent(QResizeEvent* event) {
    QTableWidget::resizeEvent(event);
    RefreshGridBackgroundImage();
}

bool GameGridFrame::IsValidCellSelected() {
    return validCellSelected;
}

void GameGridFrame::SetGameConfigIcon(QWidget* parentWidget, QVector<GameInfo> m_games_,
                                      int gameCounter) {
    std::string serialStr = m_games_[gameCounter].serial;

    bool hasGameConfig = std::filesystem::exists(
        Common::FS::GetUserPath(Common::FS::PathType::CustomConfigs) / (serialStr + ".toml"));

    QLabel* label = new QLabel(parentWidget);
    QPixmap iconPixmap = QPixmap(":images/game_settings.png")
                             .scaled(icon_size / 3.8, icon_size / 3.8, Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation);

    // Apply theme color tint to the icon
    QPainter painter(&iconPixmap);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);

    // Get theme color from config
    int theme = Config::getMainWindowTheme();
    QColor themeColor;

    switch (theme) {
    case 0: // Dark
        themeColor = QColor(90, 170, 255);
        break;
    case 1: // Light
        themeColor = QColor(0, 120, 215);
        break;
    case 2: // Green
        themeColor = QColor(76, 175, 80);
        break;
    case 3: // Blue
        themeColor = QColor(33, 150, 243);
        break;
    case 4: // Violet
        themeColor = QColor(156, 39, 176);
        break;
    case 5: // Gruvbox
        themeColor = QColor(251, 73, 52);
        break;
    case 6: // Tokyo Night
        themeColor = QColor(139, 92, 246);
        break;
    case 7: // OLED
        themeColor = QColor(255, 255, 255);
        break;
    case 8: // Neon
        themeColor = QColor(0, 255, 136);
        break;
    case 9: // Shadlix
        themeColor = QColor(255, 107, 107);
        break;
    case 10: // ShadlixCave
        themeColor = QColor(255, 159, 67);
        break;
    case 11: // DeepPurple
        themeColor = QColor(103, 58, 183);
        break;
    default:
        themeColor = QColor(90, 170, 255);
    }

    painter.fillRect(iconPixmap.rect(), themeColor);
    painter.end();

    label->setPixmap(iconPixmap);
    label->move(2, 2);
    label->raise();
    label->setVisible(hasGameConfig);
    label->setObjectName("gameConfigIcon");
}

void GameGridFrame::SetFavoriteIcon(QWidget* parentWidget, QVector<GameInfo> m_games_,
                                    int gameCounter) {
    QString serialStr = QString::fromStdString(m_games_[gameCounter].serial);

    QList<QString> list = m_compat_info->LoadFavorites();
    bool isFavorite = list.contains(serialStr);

    QLabel* label = new QLabel(parentWidget);
    QPixmap iconPixmap = QPixmap(":images/favorite_icon.png")
                             .scaled(icon_size / 1.2, icon_size / 1.2, Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation);

    // Apply theme color tint to the icon
    QPainter painter(&iconPixmap);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);

    // Get theme color from config
    int theme = Config::getMainWindowTheme();
    QColor themeColor;

    switch (theme) {
    case 0: // Dark
        themeColor = QColor(90, 170, 255);
        break;
    case 1: // Light
        themeColor = QColor(0, 120, 215);
        break;
    case 2: // Green
        themeColor = QColor(76, 175, 80);
        break;
    case 3: // Blue
        themeColor = QColor(33, 150, 243);
        break;
    case 4: // Violet
        themeColor = QColor(156, 39, 176);
        break;
    case 5: // Gruvbox
        themeColor = QColor(251, 73, 52);
        break;
    case 6: // Tokyo Night
        themeColor = QColor(139, 92, 246);
        break;
    case 7: // OLED
        themeColor = QColor(255, 255, 255);
        break;
    case 8: // Neon
        themeColor = QColor(0, 255, 136);
        break;
    case 9: // Shadlix
        themeColor = QColor(255, 107, 107);
        break;
    case 10: // ShadlixCave
        themeColor = QColor(255, 159, 67);
        break;
    case 11: // DeepPurple
        themeColor = QColor(103, 58, 183);
        break;
    default:
        themeColor = QColor(90, 170, 255);
    }

    painter.fillRect(iconPixmap.rect(), themeColor);
    painter.end();

    label->setPixmap(iconPixmap);
    label->move(icon_size - icon_size / 2, 1);
    label->raise();
    label->setVisible(isFavorite);
    label->setObjectName("favoriteIcon");
}

void GameGridFrame::SortByFavorite(QVector<GameInfo>* game_list) {
    std::sort(game_list->begin(), game_list->end(), [this](const GameInfo& a, const GameInfo& b) {
        return this->CompareWithFavorite(a, b);
    });
}

void GameGridFrame::keyPressEvent(QKeyEvent* event) {
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && validCellSelected) {
        emit cellDoubleClicked(crtRow, crtColumn);
        event->accept();
        return;
    }
    QTableWidget::keyPressEvent(event);
}

bool GameGridFrame::CompareWithFavorite(const GameInfo& a, const GameInfo& b) {
    QString serialStr_a = QString::fromStdString(a.serial);
    QString serialStr_b = QString::fromStdString(b.serial);

    QList<QString> list = m_compat_info->LoadFavorites();
    bool isFavorite_a = list.contains(serialStr_a);
    bool isFavorite_b = list.contains(serialStr_b);

    if (isFavorite_a != isFavorite_b) {
        return isFavorite_a; // favorites first
    } else {
        std::string name_a = a.name, name_b = b.name;
        std::transform(name_a.begin(), name_a.end(), name_a.begin(), ::tolower);
        std::transform(name_b.begin(), name_b.end(), name_b.begin(), ::tolower);
        return name_a < name_b;
    }
}