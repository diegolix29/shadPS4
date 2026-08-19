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
    const auto path = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "users.json";
    try {
        json j;
        j["Users"] = m_userManager.GetUsers();
        j["Users"]["commit_hash"] = std::string(Common::g_scm_rev);

        json existing = json::object();
        if (std::ifstream existingIn{path}; existingIn.good()) {
            try {
                existingIn >> existing;
            } catch (...) {
                existing = json::object();
            }
        }

        if (existing.contains("Users") && existing["Users"].is_object())
            existing["Users"].update(j["Users"]);
        else
            existing["Users"] = j["Users"];

        std::ofstream out(path);
        if (!out) {
            LOG_ERROR(Config, "Failed to open user settings for writing: {}", path.string());
            return false;
        }
        out << std::setw(2) << existing;
        return !out.fail();
    } catch (const std::exception& e) {
        LOG_ERROR(Config, "Error saving user settings: {}", e.what());
        return false;
    }
}

bool UserSettingsImpl::Load() {
    const auto json_path = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "users.json";
    bool loaded_from_json = false;

    LOG_INFO(Input, "UserSettings::Load: Attempting to load from: {}", json_path.string());

    // Try loading from JSON first
    try {
        if (std::filesystem::exists(json_path)) {
            std::ifstream in(json_path);
            if (in) {
                json j;
                in >> j;

                auto default_users = m_userManager.CreateDefaultUsers();
                json default_json;
                default_json["Users"] = default_users;

                if (j.contains("Users")) {
                    json current = default_json["Users"];
                    current.update(j["Users"]);
                    m_userManager.GetUsers() = current.get<Users>();
                    LOG_INFO(Input, "UserSettings::Load: Loaded {} users from JSON", m_userManager.GetUsers().user.size());
                    loaded_from_json = true;
                }
            } else {
                LOG_WARNING(Input, "UserSettings::Load: Could not open JSON file for reading");
            }
        } else {
            LOG_INFO(Input, "UserSettings::Load: JSON file does not exist, will try config.toml");
        }
    } catch (const std::exception& e) {
        LOG_WARNING(Input, "UserSettings::Load: Failed to load from JSON: {}", e.what());
    }

    // If JSON loading failed or returned no users, try config.toml
    if (!loaded_from_json || m_userManager.GetUsers().user.empty()) {
        LOG_INFO(Input, "UserSettings::Load: Attempting to load from config.toml");
        try {
            auto user_names = Config::getUserNames();
            auto player_user_ids = Config::getPlayerUserIds();
            auto shadnet_enabled_states = Config::getShadNetEnabledStates();
            auto shadnet_npids = Config::getShadNetNpids();
            auto shadnet_passwords = Config::getShadNetPasswords();

            LOG_INFO(Input, "UserSettings::Load: Config values: user_ids=[{},{},{},{}], names=['{}','{}','{}','{}']",
                     player_user_ids[0], player_user_ids[1], player_user_ids[2], player_user_ids[3],
                     user_names[0], user_names[1], user_names[2], user_names[3]);

            m_userManager.GetUsers().user.clear();

            for (int i = 0; i < 4; i++) {
                User user;
                user.user_id = player_user_ids[i];
                user.user_name = user_names[i];
                user.user_color = i + 1;
                user.player_index = i + 1;
                user.shadnet_npid = shadnet_npids[i];
                user.shadnet_password = shadnet_passwords[i];
                user.shadnet_enabled = shadnet_enabled_states[i];
                m_userManager.GetUsers().user.push_back(user);
            }
            LOG_INFO(Input, "UserSettings::Load: Loaded {} users from config.toml", m_userManager.GetUsers().user.size());
        } catch (const std::exception& e) {
            LOG_ERROR(Input, "UserSettings::Load: Failed to load from config.toml: {}", e.what());
        }
    }

    // If still no users, create defaults
    if (m_userManager.GetUsers().user.empty()) {
        LOG_WARNING(Input, "UserSettings::Load: No users loaded from JSON or config, creating default users");
        m_userManager.GetUsers() = m_userManager.CreateDefaultUsers();
        Save();
    }

    m_loaded = true;

    // Validate and fix any invalid player_index values
    m_userManager.ValidateAndFixPlayerIndices();

    // Automatically log in the first user if no users are logged in
    const auto& users = m_userManager.GetUsers().user;
    if (!users.empty()) {
        bool any_logged_in = false;
        for (const auto& u : users) {
            if (u.logged_in) {
                any_logged_in = true;
                break;
            }
        }
        if (!any_logged_in) {
            // Log in the first user
            auto* first_user = m_userManager.GetUserByID(users[0].user_id);
            if (first_user) {
                m_userManager.LoginUser(first_user, users[0].player_index);
                LOG_INFO(Input, "Automatically logged in user_id={}, player_index={}", users[0].user_id, users[0].player_index);
            }
        }
    }

    LOG_INFO(Input, "Final user count: {}", m_userManager.GetUsers().user.size());
    for (const auto& u : m_userManager.GetUsers().user) {
        LOG_INFO(Input, "UserSettings::Load: User: id={}, name='{}', player_index={}", u.user_id, u.user_name, u.player_index);
    }

    return true;
}