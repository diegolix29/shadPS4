// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

// DLL mods: native mods that run inside the emulator process (e.g. BBSC - Bloodborne Seamless
// Co-op). Same folder layout and mod.json format as the BBSC emulator fork, so its release can be
// dropped in as-is:
//
//   <user>/mods/<mod folder>/mod.json            (any game, filtered by "games")
//   <user>/mods/<serial>/<mod folder>/mod.json   (BBSC Play package layout)
//   <mod folder>/<dll named in mod.json>
//
// mod.json fields used here:
//   "id", "name", "games": ["CUSA03173", ...] (empty/missing = every game),
//   "dll": "x.dll" or "dlls": ["a.dll", ...], "enabled_by_default": true,
//   "settings": [{"id", "type": "bool"|"int"|"choice"|"text", "default", "env": "VAR" | [...]}]
//
// Per-user choices (optional): <mod folder>/settings.json
//   {"enabled": false, "<setting id>": <value>, ...}
// A file named "disabled" in the mod folder also turns the mod off.
// Environment variables already set before start (e.g. by a launcher) always win.

namespace Core::DllMods {

// Puts every enabled mod's settings into the environment. Call before LoadEnabledDlls.
void ApplyEnvironment(std::string_view serial);

// Loads the enabled mods' DLLs. Call once the eboot is in memory, before the game runs.
void LoadEnabledDlls(std::string_view serial);

// For the settings GUI: whether the mod with this "id" is installed for `serial`, whether it is
// on, and switching it (creates/removes the "disabled" file and sets "enabled" in settings.json).
bool IsModInstalled(std::string_view serial, std::string_view id);
bool IsModEnabled(std::string_view serial, std::string_view id);
void SetModEnabled(std::string_view serial, std::string_view id, bool enabled);

} // namespace Core::DllMods
