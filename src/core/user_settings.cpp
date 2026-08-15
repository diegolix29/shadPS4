// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <map>
#include <common/path_util.h>
#include <common/scm_rev.h>
#include <common/config.h>
#include "common/logging/log.h"
#include "user_settings.h"

using json = nlohmann::json;

// Singleton storage
std::shared_ptr<UserSettingsImpl> UserSettingsImpl::s_instance = nullptr;
std::mutex UserSettingsImpl::s_mutex;

// Singleton
UserSettingsImpl::UserSettingsImpl() = default;

UserSettingsImpl::~UserSettingsImpl() {
    if (m_loaded)
        Save();
}

std::shared_ptr<UserSettingsImpl> UserSettingsImpl::GetInstance() {
    std::lock_guard lock(s_mutex);
    if (!s_instance)
        s_instance = std::make_shared<UserSettingsImpl>();
    return s_instance;
}

void UserSettingsImpl::SetInstance(std::shared_ptr<UserSettingsImpl> instance) {
    std::lock_guard lock(s_mutex);
    s_instance = std::move(instance);
}

bool UserSettingsImpl::Save() const {
    // User settings are now read from config.toml, so no need to save to users.json
    // Config handles saving to config.toml automatically
    return true;
}

bool UserSettingsImpl::Load() {
    try {
        // Read user configuration from config.toml via Config
        auto user_names = Config::getUserNames();
        auto shadnet_enabled_states = Config::getShadNetEnabledStates();
        auto shadnet_npids = Config::getShadNetNpids();
        auto shadnet_passwords = Config::getShadNetPasswords();

        // Clear existing users
        m_userManager.GetUsers().user.clear();

        // Create users from config
        for (int i = 0; i < 4; i++) {
            User user;
            user.user_id = i + 1;
            user.user_name = user_names[i];
            user.user_color = i + 1;
            user.player_index = i + 1;
            user.shadnet_npid = shadnet_npids[i];
            user.shadnet_password = shadnet_passwords[i];
            user.shadnet_enabled = shadnet_enabled_states[i];
            m_userManager.GetUsers().user.push_back(user);
        }

        LOG_DEBUG(Config, "User settings loaded from config.toml");

        // Automatically log in the first user (user_id=1) if no users are logged in
        auto* first_user = m_userManager.GetUserByID(1);
        if (first_user && !m_userManager.GetLoggedInUsers()[0]) {
            m_userManager.LoginUser(first_user, 1);
            LOG_INFO(Config, "Automatically logged in user_id=1");
        }

        m_loaded = true;
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR(Config, "Error loading user settings from config: {}", e.what());
        if (m_userManager.GetUsers().user.empty())
            m_userManager.GetUsers() = m_userManager.CreateDefaultUsers();
        return false;
    }
}