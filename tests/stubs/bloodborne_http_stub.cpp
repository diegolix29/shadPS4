// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/bloodborne_re.h"
#include "core/debugger.h"

namespace Core::Bloodborne {
namespace {

std::optional<std::string> host_placement;

} // namespace

std::optional<std::string> GetSeamlessHostPlacementHeader() {
    return host_placement;
}

bool SetSeamlessHostPlacementHeader(std::string_view value) {
    host_placement = value;
    return true;
}

void ClearSeamlessHostPlacementHeader() {
    host_placement.reset();
}

} // namespace Core::Bloodborne

namespace Core::Debugger {

int GetCurrentPid() {
    return 0;
}

} // namespace Core::Debugger
