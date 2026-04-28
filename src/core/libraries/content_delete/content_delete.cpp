// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// v208: sceContentDelete HLE — now actually deletes instead of stubbing.
//
// Three NIDs are imported from libSceContentDelete:
//   pXJh3aVk8Ks  (FuncA)
//   5XLSih32qHA  (FuncB)
//   zoxb0wEChEM  (FuncC)
//
// We don't yet know which NID corresponds to which operation (init / delete /
// terminate). Rather than wait for a round-trip to identify, v208 takes the
// shotgun approach: ANY of the three functions, if called with a string arg
// that looks like a content_id (prefix "UP9000-"), routes to DeleteContentById.
//
// If multiple NIDs are called with the same content_id for a single action
// (e.g. FuncA=begin, FuncB=commit, FuncC=end), DeleteContentById handles it
// idempotently: first call removes the entry + file, subsequent calls find
// nothing to remove and return false harmlessly.
//
// Logging remains verbose so we can identify each NID's true role on the
// next run — this also serves as a fallback if the "UP9000-" heuristic
// misses something.

#include <cstring>
#include <string>

#include "common/logging/log.h"
#include "core/libraries/libs.h"
#include "core/libraries/content_delete/content_delete.h"
#include "core/libraries/content_search/content_search.h"

namespace Libraries::ContentDelete {

// Best-effort content_id extraction from an arg. Returns empty string if the
// pointer doesn't lead to a plausible content_id.
//
// NAMING(GR2FORK v1.0): content ids are now "YYYYMMDD_HHMMSS_NNN" (19 chars,
// digits + underscore only). Previously they were "UP9000-CUSA04943_00-
// SCREENSHOTnnnnn" (36 chars). We no longer gate on the "UP" prefix — we
// accept any printable ASCII string of plausible length and let
// Libraries::ContentSearch::DeleteContentById() decide whether it matches
// a tracked photo. DeleteContentById bails out cleanly for unknown ids,
// so speculative calls are safe.
static std::string TryExtractContentId(u64 arg) {
    if (arg <= 0x100000 || arg >= 0x800000000000ULL) return {};
    const char* s = reinterpret_cast<const char*>(arg);
    // Printable-first-char sanity check
    if (s[0] < 0x20 || s[0] >= 0x7f) return {};
    // Copy up to 64 chars, stopping at null or non-printable
    char buf[65] = {0};
    u32 len = 0;
    for (; len < 64; ++len) {
        char c = s[len];
        if (c == 0) break;
        if (c < 0x20 || c >= 0x7f) return {}; // non-printable midway → not a string
        buf[len] = c;
    }
    // "YYYYMMDD_HHMMSS_NNN" is 19 chars. Accept slightly shorter in case the
    // PS4 side ever passes a sub-form, and much longer for legacy UP-prefixed
    // ids that may still be in-memory after migration.
    if (len < 12) return {}; // too short to be any plausible id
    return std::string(buf, len);
}

// Shared handler: log the call and, if any arg is a content_id, delete it.
static int HandleDeleteCall(const char* func_name, u64 a1, u64 a2, u64 a3,
                             u64 a4, u64 a5, u64 a6) {
    LOG_INFO(Core, "[ContentDelete] {} called: "
             "a1={:#x} a2={:#x} a3={:#x} a4={:#x} a5={:#x} a6={:#x}",
             func_name, a1, a2, a3, a4, a5, a6);

    // Try to extract a content_id from each of the first two args (the rest
    // are very unlikely to hold strings). If found, route to DeleteContentById.
    for (u64 arg : {a1, a2}) {
        auto id = TryExtractContentId(arg);
        if (id.empty()) continue;

        LOG_INFO(Core, "[ContentDelete] {} — detected content_id '{}', "
                 "routing to DeleteContentById", func_name, id);
        bool deleted = Libraries::ContentSearch::DeleteContentById(id);
        LOG_INFO(Core, "[ContentDelete] {} — DeleteContentById('{}') returned {}",
                 func_name, id, deleted ? "true (removed)" : "false (not found)");
        // Only act on the first content_id we find. No two args should both
        // be content_ids for a single delete operation.
        break;
    }

    return 0; // ORBIS_OK — game expects success
}

// NID pXJh3aVk8Ks
int PS4_SYSV_ABI sceContentDeleteFuncA(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6) {
    return HandleDeleteCall("FuncA(pXJh3aVk8Ks)", a1, a2, a3, a4, a5, a6);
}

// NID 5XLSih32qHA
int PS4_SYSV_ABI sceContentDeleteFuncB(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6) {
    return HandleDeleteCall("FuncB(5XLSih32qHA)", a1, a2, a3, a4, a5, a6);
}

// NID zoxb0wEChEM
int PS4_SYSV_ABI sceContentDeleteFuncC(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6) {
    return HandleDeleteCall("FuncC(zoxb0wEChEM)", a1, a2, a3, a4, a5, a6);
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("pXJh3aVk8Ks", "libSceContentDelete", 1, "libSceContentDelete",
                 sceContentDeleteFuncA);
    LIB_FUNCTION("5XLSih32qHA", "libSceContentDelete", 1, "libSceContentDelete",
                 sceContentDeleteFuncB);
    LIB_FUNCTION("zoxb0wEChEM", "libSceContentDelete", 1, "libSceContentDelete",
                 sceContentDeleteFuncC);
    LOG_INFO(Core, "[ContentDelete] v208 HLE registered — content_id detection "
             "will route to DeleteContentById");
}

} // namespace Libraries::ContentDelete
