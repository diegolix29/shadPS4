// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QHeaderView>
#include <QtWidgets>
#include <common/path_util.h>
#include <core/user_settings.h>
#include "core/emulator_settings.h"
#include "user_manager_dialog.h"

UserManagerDialog::UserManagerDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("User Manager"));
    setMinimumSize(QSize(800, 400));
    setModal(true);

    // Table
    m_table = new QTableWidget(this);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->setColumnCount(5); // User ID, Name, Color, Port, ShadNet
    m_table->setCornerButtonEnabled(false);
    m_table->setAlternatingRowColors(true);
    m_table->setHorizontalHeaderLabels(
        {"User ID", "User Name", "Color", "Controller Port", "ShadNet"});
    m_table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setDefaultSectionSize(150);
    m_table->installEventFilter(this);

    // Buttons
    push_create_user = new QPushButton(tr("&Create User"), this);
    push_create_user->setAutoDefault(false);

    push_remove_user = new QPushButton(tr("&Delete User"), this);
    push_remove_user->setAutoDefault(false);

    push_rename_user = new QPushButton(tr("&Rename User"), this);
    push_rename_user->setAutoDefault(false);

    push_set_default = new QPushButton(tr("&Set Default User"), this);
    push_set_default->setAutoDefault(false);

    push_set_color = new QPushButton(tr("&Set Color"), this);
    push_set_color->setAutoDefault(false);

    push_set_controller = new QPushButton(tr("&Set Controller Port"), this);
    push_set_controller->setAutoDefault(false);

    push_shadnet = new QPushButton(tr("Shad&Net..."), this);
    push_shadnet->setAutoDefault(false);

    push_close = new QPushButton(tr("&Close"), this);
    push_close->setAutoDefault(false);

    // Button Layout
    QHBoxLayout* hbox_buttons = new QHBoxLayout();
    hbox_buttons->addWidget(push_create_user);
    hbox_buttons->addWidget(push_remove_user);
    hbox_buttons->addWidget(push_rename_user);
    hbox_buttons->addWidget(push_set_default);
    hbox_buttons->addWidget(push_set_color);
    hbox_buttons->addWidget(push_set_controller);
    hbox_buttons->addWidget(push_shadnet);
    hbox_buttons->addStretch();
    hbox_buttons->addWidget(push_close);

    // Main Layout
    QVBoxLayout* vbox_main = new QVBoxLayout();
    vbox_main->addWidget(m_table);
    vbox_main->addLayout(hbox_buttons);
    setLayout(vbox_main);

    m_active_user = UserManagement.GetDefaultUser().user_id;
    UpdateTable();

    // Button enabling lambda
    auto enable_buttons = [this]() {
        const u32 key = GetUserKey();
        bool valid = key != 0;
        push_remove_user->setEnabled(valid && key != m_active_user);
        push_rename_user->setEnabled(valid);
        push_set_default->setEnabled(valid && key != m_active_user);
        push_set_color->setEnabled(valid);
        push_set_controller->setEnabled(valid);
        push_shadnet->setEnabled(valid);
    };

    enable_buttons();

    connect(push_create_user, &QAbstractButton::clicked, this, &UserManagerDialog::OnUserCreate);
    connect(push_remove_user, &QAbstractButton::clicked, this, &UserManagerDialog::OnUserRemove);
    connect(push_rename_user, &QAbstractButton::clicked, this, &UserManagerDialog::OnUserRename);
    connect(push_set_default, &QAbstractButton::clicked, this,
            &UserManagerDialog::OnUserSetDefault);
    connect(push_set_color, &QAbstractButton::clicked, this, &UserManagerDialog::OnUserSetColor);
    connect(push_set_controller, &QAbstractButton::clicked, this,
            &UserManagerDialog::OnUserSetControllerPort);
    connect(push_shadnet, &QAbstractButton::clicked, this, &UserManagerDialog::OnUserEditShadNet);
    connect(push_close, &QAbstractButton::clicked, this, &QDialog::accept);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, enable_buttons);
    connect(m_table->horizontalHeader(), &QHeaderView::sectionClicked, this,
            &UserManagerDialog::OnSort);
    connect(m_table, &QTableWidget::customContextMenuRequested, this,
            &UserManagerDialog::ShowContextMenu);
}

void UserManagerDialog::UpdateTable(bool mark_only) {
    QFont bold_font;
    bold_font.setBold(true);

    const auto& users = UserManagement.GetAllUsers();

    if (mark_only) {
        for (int i = 0; i < m_table->rowCount(); ++i) {
            for (int col = 0; col < m_table->columnCount(); ++col) {
                QTableWidgetItem* item = m_table->item(i, col);
                if (item) {
                    bool is_active =
                        (m_active_user == m_table->item(i, 0)->data(Qt::UserRole).toUInt());
                    item->setFont(is_active ? bold_font : QFont());
                }
            }
        }
        return;
    }

    m_table->setRowCount(static_cast<int>(users.size()));

    for (int row = 0; row < users.size(); ++row) {
        const User& u = users[row];

        // User ID
        QTableWidgetItem* id_item = new QTableWidgetItem(QString::number(u.user_id));
        id_item->setData(Qt::UserRole, u.user_id);
        id_item->setFlags(id_item->flags() & ~Qt::ItemIsEditable);
        m_table->setItem(row, 0, id_item);

        // Username
        QTableWidgetItem* username_item = new QTableWidgetItem(QString::fromStdString(u.user_name));
        username_item->setData(Qt::UserRole, u.user_id);
        username_item->setFlags(username_item->flags() & ~Qt::ItemIsEditable);
        m_table->setItem(row, 1, username_item);

        // Color
        QTableWidgetItem* color_item = new QTableWidgetItem();
        color_item->setFlags(color_item->flags() & ~Qt::ItemIsEditable);
        color_item->setData(Qt::DecorationRole, GetQColorFromIndex(u.user_color - 1));
        m_table->setItem(row, 2, color_item);

        // Controller port
        QString controller_text =
            (u.player_index >= 1 && u.player_index <= 4) ? QString::number(u.player_index) : "-";
        QTableWidgetItem* controller_item = new QTableWidgetItem(controller_text);
        controller_item->setFlags(controller_item->flags() & ~Qt::ItemIsEditable);
        m_table->setItem(row, 3, controller_item);

        // ShadNet status
        QString shadnet_text;
        if (u.shadnet_enabled) {
            shadnet_text = u.shadnet_npid.empty()
                               ? tr("On")
                               : tr("On (%1)").arg(QString::fromStdString(u.shadnet_npid));
        } else {
            shadnet_text = tr("Off");
        }
        QTableWidgetItem* shadnet_item = new QTableWidgetItem(shadnet_text);
        shadnet_item->setData(Qt::UserRole, u.user_id);
        shadnet_item->setFlags(shadnet_item->flags() & ~Qt::ItemIsEditable);
        m_table->setItem(row, 4, shadnet_item);

        // Bold if active
        bool is_active = (m_active_user == u.user_id);
        if (is_active) {
            id_item->setFont(bold_font);
            username_item->setFont(bold_font);
            color_item->setFont(bold_font);
            controller_item->setFont(bold_font);
            shadnet_item->setFont(bold_font);
        }
    }

    // Resize headers
    m_table->horizontalHeader()->resizeSections(QHeaderView::ResizeToContents);
}

u32 UserManagerDialog::GetUserKey() const {
    int row = m_table->currentRow();
    if (row < 0)
        return 0;
    const QTableWidgetItem* item = m_table->item(row, 0);
    if (!item)
        return 0;

    bool ok = false;
    u32 id = item->data(Qt::UserRole).toUInt(&ok);
    if (!ok)
        return 0;

    const auto& users = UserManagement.GetAllUsers();
    auto it =
        std::find_if(users.begin(), users.end(), [id](const User& u) { return u.user_id == id; });
    return (it != users.end()) ? id : 0;
}

void UserManagerDialog::OnUserCreate() {
    int new_uid = 0;
    for (auto& u : UserManagement.GetAllUsers()) {
        if (u.user_id >= new_uid)
            new_uid = u.user_id + 1;
    }
    if (UserManagement.GetAllUsers().size() > 16) {
        QMessageBox::warning(this, tr("Error"), tr("Cannot add more users."));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Create New User"));
    dialog.setModal(true);
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(tr("New User ID: %1").arg(new_uid)));
    layout->addWidget(new QLabel(tr("Username (3–16 chars, letters, numbers, _, -)")));
    QLineEdit* edit = new QLineEdit(&dialog);
    edit->setValidator(
        new QRegularExpressionValidator(QRegularExpression("^[A-Za-z0-9_-]{3,16}$")));
    layout->addWidget(edit);

    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, [&]() {
        QString name = edit->text().trimmed();
        if (!edit->hasAcceptableInput()) {
            QMessageBox::warning(&dialog, tr("Invalid Username"),
                                 tr("Username must be 3–16 chars and valid."));
            return;
        }
        User u;
        u.user_id = new_uid;
        u.user_name = name.toStdString();
        u.user_color = 1;
        u.player_index = -1;
        UserManagement.AddUser(u);
        UpdateTable();
        dialog.accept();
    });
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}

void UserManagerDialog::OnUserRemove() {
    u32 id = GetUserKey();
    if (id == 0)
        return;
    if (QMessageBox::question(this, tr("Delete Confirmation"), tr("Delete user ID %1?").arg(id),
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) == QMessageBox::Yes) {
        UserManagement.RemoveUser(id);
        UpdateTable();
    }
}

void UserManagerDialog::OnUserRename() {
    u32 id = GetUserKey();
    if (id == 0)
        return;
    User* user = UserManagement.GetUserByID(id);
    if (!user)
        return;

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Rename User"));
    dialog.setMinimumWidth(300);
    QVBoxLayout layout(&dialog);
    layout.addWidget(
        new QLabel(tr("Old Username: %1").arg(QString::fromStdString(user->user_name))));
    QLineEdit edit(QString::fromStdString(user->user_name));
    layout.addWidget(&edit);
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout.addWidget(&buttons);

    auto ok_button = buttons.button(QDialogButtonBox::Ok);
    ok_button->setEnabled(false);
    QRegularExpression regex("^[A-Za-z0-9_-]{3,16}$");
    QObject::connect(&edit, &QLineEdit::textChanged, [&]() {
        ok_button->setEnabled(regex.match(edit.text().trimmed()).hasMatch());
    });
    QObject::connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() == QDialog::Accepted) {
        UserManagement.RenameUser(id, edit.text().trimmed().toStdString());
        UpdateTable();
    }
}

void UserManagerDialog::OnUserSetDefault() {
    u32 id = GetUserKey();
    if (id == 0)
        return;
    UserManagement.SetDefaultUser(id);
    m_active_user = id;
    UpdateTable();
}

void UserManagerDialog::OnUserSetColor() {
    u32 id = GetUserKey();
    if (id == 0)
        return;
    User* user = UserManagement.GetUserByID(id);
    if (!user)
        return;

    QStringList colors = {"Blue", "Red", "Green", "Pink"};
    bool ok = false;
    QString color = QInputDialog::getItem(this, tr("Set User Color"), tr("Select color:"), colors,
                                          user->user_color - 1, false, &ok);
    if (ok) {
        user->user_color = colors.indexOf(color) + 1;
        UserManagement.Save();
        UpdateTable();
    }
}

void UserManagerDialog::OnUserSetControllerPort() {
    const u32 user_id = GetUserKey();
    if (user_id == 0)
        return;

    // Current port of the selected user
    User* user = UserManagement.GetUserByID(user_id);
    if (!user)
        return;

    bool ok = false;
    int new_port =
        QInputDialog::getInt(this, tr("Set Controller Port"), tr("Assign port (1-4) to this user:"),
                             user->player_index > 0 ? user->player_index : 1, // default
                             1, 4, 1, &ok);

    if (ok) {
        UserManagement.SetControllerPort(user_id, new_port);
        UpdateTable();
    }
}

void UserManagerDialog::OnUserEditShadNet() {
    const u32 user_id = GetUserKey();
    if (user_id == 0)
        return;
    User* user = UserManagement.GetUserByID(user_id);
    if (!user)
        return;

    QDialog dialog(this);
    dialog.setWindowTitle(tr("ShadNet Settings - %1").arg(QString::fromStdString(user->user_name)));
    dialog.setMinimumWidth(360);

    auto* enabled = new QCheckBox(tr("Enable ShadNet for this user"), &dialog);
    enabled->setChecked(user->shadnet_enabled);

    auto* npid = new QLineEdit(QString::fromStdString(user->shadnet_npid), &dialog);
    npid->setPlaceholderText(tr("Account ID (NPID)"));

    auto* password = new QLineEdit(QString::fromStdString(user->shadnet_password), &dialog);
    password->setPlaceholderText(tr("Password"));
    password->setEchoMode(QLineEdit::Password);

    auto* show_pw = new QCheckBox(tr("Show password"), &dialog);
    connect(show_pw, &QCheckBox::toggled, password, [password](bool on) {
        password->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
    });

    // NPID/password only make sense when ShadNet is enabled.
    auto* form_host = new QWidget(&dialog);
    auto* form = new QFormLayout(form_host);
    form->setContentsMargins(0, 0, 0, 0);
    form->addRow(tr("Account ID (NPID):"), npid);
    form->addRow(tr("Password:"), password);
    form->addRow(QString(), show_pw);

    auto sync_enabled = [form_host](bool on) { form_host->setEnabled(on); };
    sync_enabled(enabled->isChecked());
    connect(enabled, &QCheckBox::toggled, form_host, sync_enabled);

    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(enabled);
    layout->addWidget(form_host);
    layout->addStretch();
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    // Re-fetch in case the list changed while the modal was open.
    user = UserManagement.GetUserByID(user_id);
    if (!user) {
        return;
    }
    user->shadnet_enabled = enabled->isChecked();
    user->shadnet_npid = npid->text().trimmed().toStdString();
    user->shadnet_password = password->text().toStdString();
    UserManagement.Save();
    UpdateTable();
}

void UserManagerDialog::OnSort(int logicalIndex) {
    if (logicalIndex < 0) {
        return;
    } else if (logicalIndex == m_sort_column) {
        m_sort_ascending ^= true;
    } else {
        m_sort_ascending = true;
    }
    m_sort_column = logicalIndex;
    m_table->sortByColumn(m_sort_column,
                          m_sort_ascending ? Qt::AscendingOrder : Qt::DescendingOrder);
}

void UserManagerDialog::closeEvent(QCloseEvent* event) {
    QDialog::closeEvent(event);
}

void UserManagerDialog::ShowContextMenu(const QPoint& pos) {
    const u32 key = GetUserKey();
    if (key == 0) {
        return;
    }

    QMenu* context_menu = new QMenu();

    QAction* remove_act = context_menu->addAction(tr("&Delete User"));
    QAction* rename_act = context_menu->addAction(tr("&Rename User"));
    QAction* default_user_act = context_menu->addAction(tr("&Set Default User"));
    QAction* color_act = context_menu->addAction(tr("&Set Color"));
    QAction* port_act = context_menu->addAction(tr("&Set Controller Port"));
    QAction* shadnet_act = context_menu->addAction(tr("Shad&Net Settings..."));
    QAction* show_dir_act = context_menu->addAction(tr("&Open User Directory"));

    bool enabled = key != m_active_user; // don't allow removing or setting default on active user

    remove_act->setEnabled(enabled);
    rename_act->setEnabled(enabled);

    // Connects and Events
    connect(remove_act, &QAction::triggered, this, &UserManagerDialog::OnUserRemove);
    connect(rename_act, &QAction::triggered, this, &UserManagerDialog::OnUserRename);
    connect(default_user_act, &QAction::triggered, this, &UserManagerDialog::OnUserSetDefault);
    connect(color_act, &QAction::triggered, this, &UserManagerDialog::OnUserSetColor);
    connect(port_act, &QAction::triggered, this, &UserManagerDialog::OnUserSetControllerPort);
    connect(shadnet_act, &QAction::triggered, this, &UserManagerDialog::OnUserEditShadNet);

    connect(show_dir_act, &QAction::triggered, this, [this, key]() {
        QString userDirPath;
        Common::FS::PathToQString(userDirPath, Common::FS::GetUserPath(Common::FS::PathType::HomeDir) / std::to_string(key));
        QDesktopServices::openUrl(QUrl::fromLocalFile(userDirPath));
    });

    context_menu->exec(m_table->viewport()->mapToGlobal(pos));
}
