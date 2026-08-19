// SPDX-FileCopyrightText: Copyright 2025-2026 shadLauncher4 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <memory>
#include <string>
#include <QDialog>
#include <QTableWidget>

class GUISettings;
class EmulatorSettingsImpl;

class UserManagerDialog : public QDialog {
    Q_OBJECT

public:
    explicit UserManagerDialog(QWidget* parent = nullptr);

private Q_SLOTS:
    void OnUserCreate();
    void OnUserRemove();
    void OnUserRename();
    void OnUserSetDefault();
    void OnUserSetColor();
    void OnUserSetControllerPort();
    void OnUserEditShadNet();
    void OnSort(int logicalIndex);

private:
    QColor GetQColorFromIndex(int index) {
        switch (index) {
        case 0:
            return Qt::blue;
        case 1:
            return Qt::red;
        case 2:
            return Qt::green;
        case 3:
            return Qt::magenta;
        default:
            return Qt::gray;
        }
    }
    QString GetColorName(int index) {
        switch (index) {
        case 0:
            return "Blue";
        case 1:
            return "Red";
        case 2:
            return "Green";
        case 3:
            return "Pink";
        default:
            return "Unknown";
        }
    }
    void UpdateTable(bool mark_only = false);
    u32 GetUserKey() const;
    void ShowContextMenu(const QPoint& pos);
    void closeEvent(QCloseEvent* event) override;

    QTableWidget* m_table = nullptr;
    int m_active_user;

    QPushButton* push_create_user;
    QPushButton* push_remove_user;
    QPushButton* push_rename_user;
    QPushButton* push_set_default;
    QPushButton* push_set_color;
    QPushButton* push_set_controller;
    QPushButton* push_shadnet;
    QPushButton* push_close;

    int m_sort_column = 1;
    bool m_sort_ascending = true;
};