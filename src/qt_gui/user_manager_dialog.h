// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QDialog>
#include <QTableWidget>
#include <QPushButton>
#include "core/user_manager.h"

class UserManagerDialog : public QDialog {
    Q_OBJECT

public:
    explicit UserManagerDialog(QWidget* parent = nullptr);
    ~UserManagerDialog() override = default;

private:
    void UpdateTable(bool mark_only = false);
    u32 GetUserKey() const;
    void OnUserCreate();
    void OnUserRemove();
    void OnUserRename();
    void OnUserSetDefault();
    void OnUserSetColor();
    void OnUserSetControllerPort();
    void OnUserEditShadNet();
    void OnSort(int logicalIndex);
    void ShowContextMenu(const QPoint& pos);
    void closeEvent(QCloseEvent* event) override;

    QColor GetQColorFromIndex(int index) const {
        const QColor colors[] = {Qt::blue, Qt::red, Qt::green, Qt::magenta};
        return colors[index % 4];
    }

    QTableWidget* m_table = nullptr;
    QPushButton* push_create_user = nullptr;
    QPushButton* push_remove_user = nullptr;
    QPushButton* push_rename_user = nullptr;
    QPushButton* push_set_default = nullptr;
    QPushButton* push_set_color = nullptr;
    QPushButton* push_set_controller = nullptr;
    QPushButton* push_shadnet = nullptr;
    QPushButton* push_close = nullptr;

    u32 m_active_user = 0;
    int m_sort_column = 0;
    bool m_sort_ascending = true;
};
