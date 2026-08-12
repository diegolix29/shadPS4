// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <random>

#include "common/types.h"
#include "core/libraries/np/np_handler.h"

namespace Libraries::Np {

NpHandler& NpHandler::GetInstance() {
    static NpHandler instance;
    return instance;
}

std::string NpHandler::GenerateUuidV4() {
    // Standard RFC 4122 version-4 UUID: 122 random bits, with the version
    // (0100) and variant (10xx) bits patched in. thread_local so concurrent
    // callers (WebAPI requests can come from multiple user contexts) don't
    // share generator state.
    thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<u64> dist;

    const u64 hi = dist(rng);
    const u64 lo = dist(rng);

    std::array<u8, 16> bytes{};
    for (int i = 0; i < 8; ++i) {
        bytes[i] = static_cast<u8>(hi >> (8 * (7 - i)));
        bytes[8 + i] = static_cast<u8>(lo >> (8 * (7 - i)));
    }

    bytes[6] = (bytes[6] & 0x0F) | 0x40; // version 4
    bytes[8] = (bytes[8] & 0x3F) | 0x80; // variant 10xx

    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (size_t i = 0; i < bytes.size(); ++i) {
        out.push_back(hex[bytes[i] >> 4]);
        out.push_back(hex[bytes[i] & 0x0F]);
        if (i == 3 || i == 5 || i == 7 || i == 9) {
            out.push_back('-');
        }
    }
    return out;
}

std::string NpHandler::GetBearerToken(Libraries::UserService::OrbisUserServiceUserId userId) {
    std::scoped_lock lk{m_mutex};
    auto it = m_tokens.find(userId);
    if (it != m_tokens.end()) {
        return it->second;
    }
    // No real shadNet session token has been installed for this user yet
    // (either shadNet login hasn't happened, or the configured WebAPI host
    // is a third-party server that never validates the token anyway).
    // Fabricate a stable one so requests aren't sent unauthenticated.
    std::string fallback = GenerateUuidV4();
    m_tokens.emplace(userId, fallback);
    return fallback;
}

void NpHandler::SetBearerToken(Libraries::UserService::OrbisUserServiceUserId userId,
                               std::string token) {
    std::scoped_lock lk{m_mutex};
    m_tokens[userId] = std::move(token);
}

void NpHandler::ClearBearerToken(Libraries::UserService::OrbisUserServiceUserId userId) {
    std::scoped_lock lk{m_mutex};
    m_tokens.erase(userId);
}

} // namespace Libraries::Np