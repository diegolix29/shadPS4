// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QCheckBox>
#include <QCoreApplication>
#include <QDialog>
#include <QSettings>
#include "main_window_themes.h"
#include "qt_gui/compatibility_info.h"

class QPushButton;

class WelcomeDialog : public QDialog {
    Q_OBJECT
public:
    WelcomeDialog(std::shared_ptr<CompatibilityInfoClass> compat, WindowThemes* themes,
                  QWidget* parent = nullptr);

    bool skipOnNextLaunch() const {
        return m_skipNextLaunch;
    }

    static bool Early_ShouldSkipWelcome() {
        QSettings settings(QCoreApplication::applicationDirPath() + "/startup.ini",
                           QSettings::IniFormat);
        return settings.value("skip_welcome", false).toBool();
    }
    bool m_portableChosen = false;
    bool m_userMadeChoice = false;

private:
    QCheckBox* m_skipCheck = nullptr;
    bool m_skipNextLaunch = false;
    std::shared_ptr<CompatibilityInfoClass> m_compat_info;
    void SetupUI();
    WindowThemes* m_themes = nullptr;
    void ApplyTheme();
};
