// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QApplication>
#include <QDialog>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>

class HelpDialog : public QDialog {
    Q_OBJECT
public:
    explicit HelpDialog(QWidget* parent = nullptr);

private:
    QString quickstart() {
        return "This is the quickstart guide...\n\n\n\ntodo";
    }
    QString faq() {
        return "This is the FAQ section...\n\n\n\ntodo";
    }
    QString syntax() {
        return "Here's all key and controller binding's syntax...\n\n\n\ntodo";
    }
};