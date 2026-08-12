// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <mutex>
#include <string>
#include <unordered_map>

#include "core/libraries/system/userservice.h"

namespace Libraries::Np {

// Holds the per-user "session" identifier that gets sent as the WebAPI
// `Authorization: Bearer <token>` header (see np_web_api_internal.cpp).
//
// Two sources feed this today:
//  - A *real* shadNet account token, obtained over the authenticated TCP
//    session (Login + GetToken, see NpMatchingSession) once that user has
//    real shadNet credentials configured and logged in successfully.
//    Real shadNet validates this token against the `account` table.
//  - A fabricated per-user identifier, used as a fallback so requests to a
//    non-shadNet WebAPI host (e.g. an httpHostOverride pointing at a
//    community matchmaking server) still carry *some* bearer value instead
//    of going out unauthenticated. Community servers such as this commonly
//    accept any syntactically-plausible token and perform no real
//    validation, so a random UUIDv4 is sufficient here; NpHandler never
//    invents credentials for the real shadNet server itself.
//
// SetBearerToken() lets a real login flow override the fallback once it
// completes; ClearBearerToken() drops everything for that user (logout).
class NpHandler {
public:
    static NpHandler& GetInstance();

    // Returns the bearer token to use for `userId`, generating and caching a
    // fallback UUIDv4 on first use if no real token has been set yet. Never
    // returns an empty string.
    std::string GetBearerToken(Libraries::UserService::OrbisUserServiceUserId userId);

    // Installs a real, server-issued token for `userId` (e.g. from a
    // successful shadNet Login + GetToken exchange), replacing any fallback
    // value that had been generated for them.
    void SetBearerToken(Libraries::UserService::OrbisUserServiceUserId userId,
                         std::string token);

    // Drops any cached token for `userId` (call on logout / user switch).
    void ClearBearerToken(Libraries::UserService::OrbisUserServiceUserId userId);

    NpHandler(const NpHandler&) = delete;
    NpHandler& operator=(const NpHandler&) = delete;

private:
    NpHandler() = default;

    static std::string GenerateUuidV4();

    std::mutex m_mutex;
    std::unordered_map<Libraries::UserService::OrbisUserServiceUserId, std::string> m_tokens;
};

} // namespace Libraries::Np