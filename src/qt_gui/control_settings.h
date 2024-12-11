// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QDialog>
#include <QPushButton>

namespace Ui {
class ControlSettings;
}

class ControlSettings : public QDialog {
    Q_OBJECT
public:
    explicit ControlSettings(QWidget* parent = nullptr);
    ~ControlSettings();

private Q_SLOTS:
    void SaveControllerConfig();
    void SetDefault();
    void AddBoxItems();
    void SetUIValuestoMappings();

private:
    std::unique_ptr<Ui::ControlSettings> ui;
};