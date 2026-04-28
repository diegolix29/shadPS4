// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstddef>
#include <cstring>

#include "common/logging/log.h"
#include "core/libraries/screenshot_service/screenshot_service.h"

namespace Libraries::ScreenshotService {

namespace {

struct PatchSite {
    u32         offset;        // VA offset from eboot base
    u8          size;          // 2 or 6
    const char* label;         // for logging
    u8          expected[6];   // bytes to match
    u8          replacement[6];// bytes to write
};

// ─── VIEW (v206) — 2 sites ──────────────────────────────────────────────
//   Dispatcher's two je-rel32 gates to sys_ng when the selected cell lacks
//   a valid inner state. NOPing enables Cross (view) on HLE photos.
//
// ─── MARK (v211/v212/v213/v214) — 12 sites ──────────────────────────────
//   Empirically verified: Mark (Square button) works via an INDIRECT CALL
//   path that static analysis can't see:
//
//     Square → 0xe33840 → 0xe3389d: `call r8`  (vtable dispatch)
//                       → resolves at runtime to 0xe33c70
//                       → body runs, hits gate at 0xe33cb9
//
//   The vtable holding this function pointer is constructed dynamically
//   (shadPS4 HLE runtime or game-side C++ init) — no static qword in the
//   27MB eboot contains 0xe33c70 as a value, which is why the call graph
//   looked like 0xe33c70 was only called from the Options button path.
//
//   With gate at 0xe33cb9 active: [sub_obj1+0xd8]==0 for HLE cells, so the
//   gate fires and 0xe33c70 tail-calls sys_ng before completing mark logic.
//   With gate NOPed: 0xe33c70 runs past the gate, does the real mark action
//   (checkmark appears on photo), returns cleanly; 0xe33840 continues to
//   its own sys_cancel tail-call.
//
//   Diagnostic sequence that confirmed this:
//     1. 0xe33cb9 = 0x90 0x90 (NOP)     → Mark works
//     2. 0xe33cb9 = 0xEB 0x00 (jmp +0)  → Mark works (byte pattern irrelevant;
//                                         behavior-equivalent NOPs both work)
//     3. 0xe33c70 = 0xC3 (ret)          → Mark BREAKS, sys_cancel also silent
//                                         (proves 0xe33c70 IS on Square's path
//                                         and 0xe33840 depends on its body)
//
//   The ONLY gate in kSites that functionally enables Mark is 0xe33cb9.
//   The other 11 patches (0xe31a5d, 0xe31aaf, 0xe31ab8, 0xe31ace, 0xe31af8,
//   0xe31b04, 0xe31b13, 0xe31a7a, 0xe31a87, 0xe31a93, 0xe31aa2) are in
//   0xe319f0 — that wrapper is Triangle/Delete-related (not yet fully
//   traced). Per Jun's observation, those patches control the SOUND Delete
//   plays (sys_cancel vs sys_ng) but don't affect whether Mark functions.
//   Leaving them in for Delete cosmetic reasons; they could be removed to
//   minimize the patch set if Delete sound correctness isn't important.
constexpr PatchSite kSites[] = {
    // VIEW
    {0x13465be, 6, "v206 VIEW gate A",
     {0x0F, 0x84, 0xCB, 0x14, 0x00, 0x00},
     {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}},
    {0x1347932, 6, "v206 VIEW gate B",
     {0x0F, 0x84, 0x57, 0x01, 0x00, 0x00},
     {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}},

    // MARK — v211 TRIO (all three together make Mark work)
    {0xe31a5d,  6, "v211 Mark gate A",
     {0x0F, 0x84, 0x91, 0x04, 0x00, 0x00},
     {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}},
    {0xe31aaf,  6, "v211 Mark gate E",
     {0x0F, 0x84, 0x3F, 0x04, 0x00, 0x00},
     {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}},
    {0xe33cb9,  2, "v211 Delete-wrapper gate A (required for Mark)",
     {0x74, 0x68, 0x00, 0x00, 0x00, 0x00},  // 2-byte short je
     {0x90, 0x90, 0x00, 0x00, 0x00, 0x00}},

    // MARK — v212 (sub_obj2)
    {0xe31ace,  6, "v212 Mark sub_obj2 gate A2",
     {0x0F, 0x84, 0x20, 0x04, 0x00, 0x00},
     {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}},
    {0xe31af8,  6, "v212 Mark sub_obj2 gate C1_2",
     {0x0F, 0x84, 0xF6, 0x03, 0x00, 0x00},
     {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}},
    {0xe31b04,  6, "v212 Mark sub_obj2 gate C2_2",
     {0x0F, 0x84, 0xEA, 0x03, 0x00, 0x00},
     {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}},
    {0xe31b13,  6, "v212 Mark sub_obj2 gate D2",  // jl rel32 (0F 8C), not je
     {0x0F, 0x8C, 0xDB, 0x03, 0x00, 0x00},
     {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}},

    // MARK — v213 (je rel8 → jmp rel8, same displacement)
    {0xe31ab8,  2, "v213 Mark je→jmp short",
     {0x74, 0x5F, 0x00, 0x00, 0x00, 0x00},  // only first 2 checked
     {0xEB, 0x5F, 0x00, 0x00, 0x00, 0x00}},

    // MARK — v214 (sub_obj1)
    {0xe31a7a,  6, "v214 Mark sub_obj1 gate B",
     {0x0F, 0x84, 0x74, 0x04, 0x00, 0x00},
     {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}},
    {0xe31a87,  6, "v214 Mark sub_obj1 gate C1",
     {0x0F, 0x84, 0x67, 0x04, 0x00, 0x00},
     {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}},
    {0xe31a93,  6, "v214 Mark sub_obj1 gate C2",
     {0x0F, 0x84, 0x5B, 0x04, 0x00, 0x00},
     {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}},
    {0xe31aa2,  6, "v214 Mark sub_obj1 gate D",   // jl rel32, not je
     {0x0F, 0x8C, 0x4C, 0x04, 0x00, 0x00},
     {0x90, 0x90, 0x90, 0x90, 0x90, 0x90}},
};

} // namespace

void ApplyViewMarkPatches(uintptr_t eboot_base) {
    if (eboot_base == 0) {
        LOG_ERROR(Core, "[GR2 View/Mark] eboot_base is 0 — cannot patch");
        return;
    }

    int patched = 0, already = 0, mismatched = 0;
    for (const auto& s : kSites) {
        u8* site = reinterpret_cast<u8*>(eboot_base + s.offset);

        if (std::memcmp(site, s.replacement, s.size) == 0) {
            already++;
            continue;
        }
        if (std::memcmp(site, s.expected, s.size) != 0) {
            LOG_WARNING(Core,
                "[GR2 View/Mark] {} @ 0x{:x}: unexpected bytes, NOT patched "
                "(got {:02x} {:02x}..., expected {:02x} {:02x}...)",
                s.label, s.offset, site[0], site[1],
                s.expected[0], s.expected[1]);
            mismatched++;
            continue;
        }
        std::memcpy(site, s.replacement, s.size);
        patched++;
    }

    LOG_INFO(Core,
        "[GR2 View/Mark] Applied View+Mark patches: {} patched, {} already, "
        "{} mismatched (of {} total sites)",
        patched, already, mismatched,
        static_cast<int>(sizeof(kSites) / sizeof(kSites[0])));
}

// ─── Gallery visibility detection ───────────────────────────────────────
// The game maintains a photo-mode state object reachable from BSS
// PHOTO_MODE_ROOT (0x1AA3E78). Field +0x178 of that object is set to 2
// while the film-album gallery UI is visible, and something else (or null
// anywhere in the chain) otherwise.

constexpr u64 kPhotoModeRoot = 0x1AA3E78;

// Pointer-sanity check — filter out obviously bogus values (low system
// addresses and addresses above the ~47-bit user-space ceiling).
static inline bool PtrLooksValid(u64 v) {
    return v > 0x100000ULL && v < 0x800000000000ULL;
}

bool IsGalleryVisible(uintptr_t eboot_base) {
    if (eboot_base == 0) return false;
    const u64 root = *reinterpret_cast<u64*>(eboot_base + kPhotoModeRoot);
    if (!PtrLooksValid(root)) return false;
    const u64 obj = *reinterpret_cast<u64*>(root + 8);
    if (!PtrLooksValid(obj)) return false;
    const u32 state = *reinterpret_cast<u32*>(obj + 0x178);
    return (state == 2);
}

bool PollGalleryStateAndLogEdges(uintptr_t eboot_base) {
    static bool was_visible = false;
    const bool now_visible = IsGalleryVisible(eboot_base);
    if (now_visible != was_visible) {
        LOG_INFO(Core, "[Gallery] {}",
                 now_visible ? "ENTERED film-album" : "EXITED film-album");
        was_visible = now_visible;
    }
    return now_visible;
}

} // namespace Libraries::ScreenshotService
