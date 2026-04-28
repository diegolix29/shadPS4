// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "common/thread.h"
#include "core/file_sys/fs.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "core/libraries/content_search/content_search.h"
#include "core/libraries/screenshot_service/screenshot_service.h"

// FIX(GR2FORK): Silent-crash fix for the gallery-state monitor thread.
// The monitor at GalleryStateMonitorLoop does raw `*reinterpret_cast<u64*>(...)`
// dereferences of guest memory at 30Hz. On Linux this works because the
// emulator's SIGSEGV handler catches page-fault reads and the process-wide
// signal infrastructure masks them. On Windows, an AV in a detached std::thread
// that wasn't covered by a vectored exception handler takes down the whole
// process — STATUS_ACCESS_VIOLATION, no log, no dialog. That matches exactly
// the GR2 CUSA03694 RTX 3050 Ti "silent crash after 40-70 minutes at the
// guide-hand scene" symptom: the user's game-state transition invalidates
// the PhotoMode root pointer, the monitor's pointer-chain read faults, and
// the process dies silently. All three submitted crash logs had the monitor
// running and zero Vulkan errors.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {
// Safe guest-memory readers. Return false if the page isn't readable (on
// Windows) or — worst case — if an AV fires mid-read. On Linux, delegates
// to the existing raw read since the shadPS4 SIGSEGV handler covers it.
#ifdef _WIN32
static bool IsCommittedReadable(const void* p, size_t n) noexcept {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD prot = mbi.Protect & 0xFF;
    if (prot == 0 || prot == PAGE_NOACCESS) return false;
    const auto* end = reinterpret_cast<const unsigned char*>(p) + n;
    const auto* region_end =
        reinterpret_cast<const unsigned char*>(mbi.BaseAddress) + mbi.RegionSize;
    return end <= region_end;
}

// Defined out-of-line so __try/__except works under /EHsc. Non-inline; must
// not contain C++ objects with non-trivial destructors.
static bool SafeRead64(const u64* p, u64& out) noexcept {
    if (!IsCommittedReadable(p, sizeof(u64))) return false;
    __try {
        out = *p;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeRead32(const u32* p, u32& out) noexcept {
    if (!IsCommittedReadable(p, sizeof(u32))) return false;
    __try {
        out = *p;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#else
static bool SafeRead64(const u64* p, u64& out) noexcept {
    out = *p;
    return true;
}
static bool SafeRead32(const u32* p, u32& out) noexcept {
    out = *p;
    return true;
}
#endif
} // namespace

namespace Libraries::ContentSearch {

namespace {

constexpr u32 ENTRY_SIZE = 0x960;
constexpr u32 MAX_GALLERY_PHOTOS = 999;

static std::mutex s_ids_mutex;
static std::vector<std::string> s_exported_ids;
static bool s_ids_loaded = false;
static bool s_screenshot_mounted = false;

// Metadata API state
static u32 s_metadata_handle_counter = 0x4D440001;
static std::mutex s_metadata_mutex;

struct MetadataSession {
    u64 record_id;
    bool valid;
};
static MetadataSession s_metadata_sessions[16] = {};

static u32 AllocMetadataHandle(u64 record_id) {
    std::lock_guard lock(s_metadata_mutex);
    u32 handle = s_metadata_handle_counter++;
    u32 slot = handle & 0xF;
    s_metadata_sessions[slot].record_id = record_id;
    s_metadata_sessions[slot].valid = true;
    return handle;
}

static u64 LookupMetadataRecordId(u32 handle) {
    std::lock_guard lock(s_metadata_mutex);
    u32 slot = handle & 0xF;
    if (s_metadata_sessions[slot].valid)
        return s_metadata_sessions[slot].record_id;
    return 0;
}

static void FreeMetadataHandle(u32 handle) {
    std::lock_guard lock(s_metadata_mutex);
    u32 slot = handle & 0xF;
    s_metadata_sessions[slot].valid = false;
}

// Screenshot directory + mount
std::filesystem::path GetScreenshotDir() {
    std::error_code ec;
    // RENAME(GR2FORK v1.0): "GR2_PhotoApp_HLE" -> "Gravity Rush 2". Must stay
    // in sync with jpegenc.cpp::GetGR2PhotoDir().
    const auto dir =
        Common::FS::GetUserPath(Common::FS::PathType::ScreenshotsDir) / "Gravity Rush 2";
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void MountScreenshotDir() {
    if (s_screenshot_mounted) return;
    s_screenshot_mounted = true;

    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    const auto dir = GetScreenshotDir();

    mnt->Mount(dir, "/screenshot", true);
    mnt->Mount(dir, "/photo", true);
    mnt->Mount(dir, "/app0/photo", true);
    LOG_INFO(Core, "[GR2PhotoHLE] Mounted /screenshot, /photo, /app0/photo -> {}", dir.string());
}

// PURGED(GR2FORK v1.0): exported_ids.txt is obsolete. The on-disk .jpg files
// ARE the source of truth. s_exported_ids is rebuilt at runtime from a
// directory scan and kept in sync in memory for O(1) indexed access by the
// gallery fiber. No text file is read or written anywhere.
//
// Helpers below:
//   LoadIdsFromDisk()      : populate s_exported_ids from *.jpg filenames.
//                            Called lazily; idempotent (s_ids_loaded gate).
//   RescanIdsFromDisk()    : force-rebuild, used after external file deletion.

static void ScanJpegsInto(const std::filesystem::path& dir,
                          std::vector<std::string>& out) {
    out.clear();
    std::set<std::string> seen;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        const auto ext = entry.path().extension().string();
        if (ext != ".jpg" && ext != ".jpeg" && ext != ".JPG" && ext != ".JPEG") continue;
        auto stem = entry.path().stem().string();
        // Accept well-formed PS4 content ids (contain "SCREENSHOT") OR any
        // stem that looks like a user-dropped photo. Games that use the HLE
        // gallery look up by index, not by name, so format-flexibility here
        // only helps when the user drops their own .jpgs into the folder.
        if (stem.empty()) continue;
        if (seen.insert(stem).second) {
            out.push_back(std::move(stem));
        }
    }
    // Stable ordering so gallery index N always maps to the same photo across
    // runs. directory_iterator order is fs-dependent; sort lexicographically.
    std::sort(out.begin(), out.end());
}

void LoadIdsFromDisk() {
    if (s_ids_loaded) return;
    s_ids_loaded = true;
    const auto dir = GetScreenshotDir();
    ScanJpegsInto(dir, s_exported_ids);
    LOG_INFO(Core, "[GR2PhotoHLE] Scanned {} -> {} photo(s) (runtime index, no .txt)",
             dir.string(), s_exported_ids.size());
}

void RescanIdsFromDisk() {
    s_ids_loaded = false;
    LoadIdsFromDisk();
}

// NAMING(GR2FORK v1.0): previously returned a global monotonic counter that
// jpegenc/content_export used to build "UP9000-CUSA04943_00-SCREENSHOT%05u"
// filenames. The naming scheme has been replaced with a human-readable
// timestamp (see GenerateTimestampContentId below, defined with external
// linkage in the Public API section), so this helper is kept only to
// satisfy the legacy header export — it now returns the total photo count,
// which is what most callers actually wanted.
u32 GetNextCounterValue() {
    return static_cast<u32>(s_exported_ids.size());
}

// Search output helpers
void WriteSearchOutputs(u64 a7, u64 hit) {
    if (a7 == 0) return;
    u64* pair0 = reinterpret_cast<u64*>(a7 - 0x10);
    u64* pair1 = reinterpret_cast<u64*>(a7 - 0x08);
    u64* adv   = reinterpret_cast<u64*>(a7);
    *pair0 = hit;
    *pair1 = hit;
    *adv   = hit;
}

// Populate a single ContentSearch entry (0x960 bytes)
void PopulateEntry(u8* entry, u32 index, const std::string& cid) {
    u64 record_id = static_cast<u64>(index + 1);
    std::memcpy(entry + 0x000, &record_id, 8);

    u32 ctype = 1;
    std::memcpy(entry + 0x00C, &ctype, 4);

    std::memset(entry + 0x018, 0, 0x401);
    std::strncpy(reinterpret_cast<char*>(entry + 0x018), cid.c_str(), 0x400);

    char title[64] = {};
    std::snprintf(title, sizeof(title), "Photo %04u", index);
    std::memset(entry + 0x424, 0, 0x101);
    std::strncpy(reinterpret_cast<char*>(entry + 0x424), title, 0x100);

    char filepath[512] = {};
    std::snprintf(filepath, sizeof(filepath), "/screenshot/%s.jpg", cid.c_str());
    std::memset(entry + 0x52B, 0, 0x401);
    std::strncpy(reinterpret_cast<char*>(entry + 0x52B), filepath, 0x400);
}

} // namespace

// Public API

// GR2FORK v1.0: generate a new content id of the form "YYYYMMDD_HHMMSS_NNN",
// where NNN is a 3-digit per-second disambiguation counter. Stems are pure
// ASCII + underscore so the PS4 side can pass them through opaque string
// APIs without surprise. The NNN search uses the in-memory id list (rebuilt
// at boot by LoadIdsFromDisk from actual .jpg files), so the resulting
// filename is guaranteed unique on disk without consulting a separate
// counter file.
//
// NOTE: defined at namespace scope (not inside the anonymous namespace)
// because jpegenc.cpp and content_export.cpp call it across TUs.
std::string GenerateTimestampContentId() {
    std::lock_guard lock(s_ids_mutex);
    LoadIdsFromDisk();

    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char ts[20] = {};
    std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_buf);

    // Most seconds will have exactly 1 capture, so n=1 usually hits.
    for (u32 n = 1; n < 1000; n++) {
        char candidate[40] = {};
        std::snprintf(candidate, sizeof(candidate), "%s_%03u", ts, n);
        std::string id(candidate);
        if (std::find(s_exported_ids.begin(), s_exported_ids.end(), id) ==
            s_exported_ids.end()) {
            return id;
        }
    }
    // > 999 captures within a single second: spill to epoch-ms fallback.
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch())
                        .count();
    char fallback[56] = {};
    std::snprintf(fallback, sizeof(fallback), "%s_FB%06lld", ts,
                  static_cast<long long>(ms & 0xFFFFFF));
    return std::string(fallback);
}

u32 GetExportedCount() {
    std::lock_guard lock(s_ids_mutex);
    return static_cast<u32>(s_exported_ids.size());
}

std::string GetContentIdByIndex(u32 index) {
    std::lock_guard lock(s_ids_mutex);
    if (index >= s_exported_ids.size()) return {};
    return s_exported_ids[index];
}

bool DeleteContentById(const std::string& content_id) {
    {
        std::lock_guard lock(s_ids_mutex);
        auto it = std::find(s_exported_ids.begin(), s_exported_ids.end(), content_id);
        if (it == s_exported_ids.end()) return false;
        s_exported_ids.erase(it);
    }
    auto path = GetScreenshotDir() / (content_id + ".jpg");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    // PURGED(GR2FORK v1.0): no SaveIdsToDisk — filesystem is source of truth.
    return true;
}

void NotifyExportedContentId(const std::string& content_id) {
    std::lock_guard lock(s_ids_mutex);
    LoadIdsFromDisk();
    for (const auto& existing : s_exported_ids) {
        if (existing == content_id) return;
    }
    // PURGED(GR2FORK v1.0): in-memory append only, no .txt write. The JPEG
    // file itself (written by jpegenc.cpp before this call) is the canonical
    // record. Keep sorted so indices remain stable for the gallery fiber.
    s_exported_ids.push_back(content_id);
    std::sort(s_exported_ids.begin(), s_exported_ids.end());
    LOG_INFO(Core, "[GR2PhotoHLE] ContentSearch: registered id '{}' (total={})",
             content_id, s_exported_ids.size());
}

std::filesystem::path GetScreenshotHostDir() {
    return GetScreenshotDir();
}

u32 GetNextPhotoCounter() {
    std::lock_guard lock(s_ids_mutex);
    LoadIdsFromDisk();
    return GetNextCounterValue();
}

static std::string s_last_saved_content_id;
static std::mutex s_last_saved_mutex;

void SetLastSavedContentId(const std::string& content_id) {
    std::lock_guard lock(s_last_saved_mutex);
    s_last_saved_content_id = content_id;
}

std::string GetLastSavedContentId() {
    std::lock_guard lock(s_last_saved_mutex);
    return s_last_saved_content_id;
}

// HLE API

// ─── Gallery visibility background monitor ─────────────────────────────
// A dedicated 30Hz thread polls the game's photo-mode state object and
// logs ENTER/EXIT transitions. Decoupled from pad, GPU, and all other
// subsystems — runs as long as the emulator process does.
//
//   root  = *(u64*)(eboot_base + 0x1AA3E78)   [BSS PHOTO_MODE_ROOT]
//   obj   = *(u64*)(root + 8)                 if root sane
//   state = *(u32*)(obj  + 0x178)             if obj  sane
//   visible = (state == 2)
//
// Thread is started lazily on the first sceContentSearchInit call,
// idempotent, and detached (OS reaps on process exit).
static std::atomic<bool> g_gallery_monitor_started{false};

static void GalleryStateMonitorLoop() {
    Common::SetCurrentThreadName("shadPS4:GalleryStateMonitor");
    auto ptr_ok = [](u64 v) -> bool {
        return v > 0x100000ULL && v < 0x800000000000ULL;
    };
    bool was_visible = false;

    // Poll period 33ms (~30 Hz). Negligible CPU: three pointer loads and
    // a compare per tick. Sub-33ms worst-case edge-detection latency.
    constexpr auto kPeriod = std::chrono::milliseconds(33);

    // FIX(GR2FORK): circuit-breaker for pathological Windows faults. If the
    // monitor fails to read guest memory consistently (e.g. layout drift or
    // a game-state transition we don't understand), stop polling rather
    // than spin on failed reads at 30Hz forever. The monitor is a
    // best-effort logger, not a correctness requirement.
    constexpr u32 kMaxConsecutiveFaults = 300; // ~10 seconds at 30Hz
    u32 consecutive_faults = 0;
    bool monitor_disabled = false;

    while (true) {
        if (monitor_disabled) {
            std::this_thread::sleep_for(std::chrono::seconds(60));
            continue;
        }
        const uintptr_t base = MemoryPatcher::g_eboot_address;
        if (base != 0) {
            bool now_visible = false;
            bool read_ok = true;
            u64 root = 0;
            // FIX(GR2FORK): was `root = *reinterpret_cast<u64*>(base + 0x1AA3E78);`.
            // Unsafe on Windows — a guest-memory AV here silently terminates
            // the whole emulator process (STATUS_ACCESS_VIOLATION in a detached
            // std::thread). Use SEH-protected read.
            if (!SafeRead64(reinterpret_cast<const u64*>(base + 0x1AA3E78), root)) {
                read_ok = false;
            } else if (ptr_ok(root)) {
                u64 obj = 0;
                if (!SafeRead64(reinterpret_cast<const u64*>(root + 8), obj)) {
                    read_ok = false;
                } else if (ptr_ok(obj)) {
                    u32 state = 0;
                    if (!SafeRead32(reinterpret_cast<const u32*>(obj + 0x178), state)) {
                        read_ok = false;
                    } else {
                        now_visible = (state == 2);
                    }
                }
            }
            if (read_ok) {
                consecutive_faults = 0;
            } else if (++consecutive_faults >= kMaxConsecutiveFaults) {
                LOG_WARNING(Core,
                            "[Gallery] monitor disabled — {} consecutive guest-memory "
                            "faults at BSS 0x{:X} / offsets +0x8 / +0x178. "
                            "Game-state probe is stale. Gallery ENTER/EXIT logging is off.",
                            consecutive_faults, 0x1AA3E78);
                monitor_disabled = true;
                continue;
            }
            if (read_ok && now_visible != was_visible) {
                LOG_INFO(Core, "[Gallery] {}",
                         now_visible ? "ENTERED film-album"
                                     : "EXITED film-album");
                was_visible = now_visible;
            }
        }
        std::this_thread::sleep_for(kPeriod);
    }
}

static void StartGalleryStateMonitor() {
    bool expected = false;
    if (g_gallery_monitor_started.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        std::thread(GalleryStateMonitorLoop).detach();
        LOG_INFO(Core,
                 "[Gallery] background state monitor started (30 Hz poll)");
    }
}

int PS4_SYSV_ABI sceContentSearchInit(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7,
                                      u64 a8) {
    // Apply the View+Mark gate NOPs so Cross and Square work on HLE photos.
    // Safe to call multiple times — verifies bytes before writing.
    {
        const uintptr_t base = MemoryPatcher::g_eboot_address;
        if (base != 0) {
            Libraries::ScreenshotService::ApplyViewMarkPatches(base);
        }
    }

    // Start the background gallery-state monitor (idempotent — first call
    // only). Runs as long as the process does; decoupled from pad/GPU.
    StartGalleryStateMonitor();

    MountScreenshotDir();
    {
        std::lock_guard lock(s_ids_mutex);
        LoadIdsFromDisk();
    }

    if (a1 != 0) {
        *reinterpret_cast<u32*>(a1) = 0x43530001;
    }

    LOG_INFO(Core, "[GR2PhotoHLE] ContentSearchInit: mode={}", a2);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceContentSearchSearchContent(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6,
                                               u64 a7, u64 a8) {
    std::lock_guard lock(s_ids_mutex);

    MountScreenshotDir();
    LoadIdsFromDisk();

    u32 total = static_cast<u32>(s_exported_ids.size());
    if (total > MAX_GALLERY_PHOTOS) total = MAX_GALLERY_PHOTOS;

    u32 start = static_cast<u32>(a5);
    u32 limit = static_cast<u32>(a6);
    u32 available = (start < total) ? (total - start) : 0;
    u32 hit = std::min(available, limit);

    if (a8 != 0 && hit > 0) {
        u64 buf_size = static_cast<u64>(hit) * ENTRY_SIZE;
        std::memset(reinterpret_cast<void*>(a8), 0, buf_size);

        for (u32 i = 0; i < hit; i++) {
            u8* entry = reinterpret_cast<u8*>(a8) + static_cast<u64>(i) * ENTRY_SIZE;
            u32 global_idx = start + i;
            PopulateEntry(entry, global_idx, s_exported_ids[global_idx]);
        }
    }

    WriteSearchOutputs(a7, hit);

    LOG_INFO(Core, "[GR2PhotoHLE] SearchContent: start={} limit={} hit={} total={}",
             start, limit, hit, total);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceContentSearchTerm(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7,
                                      u64 a8) {
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceContentSearchSearchApplication(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5,
                                                   u64 a6, u64 a7, u64 a8) {
    if (a3 != 0) *reinterpret_cast<u32*>(a3) = 0;
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceContentSearchGetMyApplicationIndex(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5,
                                                       u64 a6, u64 a7, u64 a8) {
    if (a1 != 0) *reinterpret_cast<u32*>(a1) = 0;
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceContentSearchGetNumOfContent(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6,
                                                 u64 a7, u64 a8) {
    std::lock_guard lock(s_ids_mutex);
    LoadIdsFromDisk();
    u32 count = static_cast<u32>(s_exported_ids.size());
    if (count > MAX_GALLERY_PHOTOS) count = MAX_GALLERY_PHOTOS;

    if (a3 != 0) *reinterpret_cast<u32*>(a3) = count;
    if (a4 != 0) *reinterpret_cast<u32*>(a4) = count;
    return ORBIS_OK;
}

// Metadata API

int PS4_SYSV_ABI sceContentSearchOpenMetadata(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6,
                                              u64 a7, u64 a8) {
    u32 handle = AllocMetadataHandle(a1);
    if (a2 != 0) *reinterpret_cast<u32*>(a2) = handle;
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceContentSearchOpenMetadataByContentId(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5,
                                                         u64 a6, u64 a7, u64 a8) {
    u32 handle = AllocMetadataHandle(0);
    if (a2 != 0) *reinterpret_cast<u32*>(a2) = handle;
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceContentSearchGetMetadataValue(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6,
                                                  u64 a7, u64 a8) {
    if (a3 == 0) return -1;

    u8* params = reinterpret_cast<u8*>(a3);
    u32 buf_size = *reinterpret_cast<u32*>(params + 0);
    u64 buf_ptr = *reinterpret_cast<u64*>(params + 8);

    if (buf_ptr == 0 || buf_size == 0) return -1;

    char* dest = reinterpret_cast<char*>(buf_ptr);
    std::memset(dest, 0, std::min(buf_size, 0x101u));
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceContentSearchGetMetadataFieldInfo(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5,
                                                      u64 a6, u64 a7, u64 a8) {
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceContentSearchCloseMetadata(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6,
                                               u64 a7, u64 a8) {
    FreeMetadataHandle(static_cast<u32>(a1));
    return ORBIS_OK;
}

// RegisterLib

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("dPj4ZtRcIWk", "libSceContentSearch", 1, "libSceContentSearch",
                 sceContentSearchInit);
    LIB_FUNCTION("TEW3IKxYfXc", "libSceContentSearch", 1, "libSceContentSearch",
                 sceContentSearchSearchContent);
    LIB_FUNCTION("V4A4AGzxA0g", "libSceContentSearch", 1, "libSceContentSearch",
                 sceContentSearchSearchApplication);
    LIB_FUNCTION("FRT4EYtZU1Y", "libSceContentSearch", 1, "libSceContentSearch",
                 sceContentSearchGetMyApplicationIndex);
    LIB_FUNCTION("o-RBPV0qr8c", "libSceContentSearch", 1, "libSceContentSearch",
                 sceContentSearchGetNumOfContent);
    LIB_FUNCTION("5opI5D0LQjg", "libSceContentSearch", 1, "libSceContentSearch",
                 sceContentSearchOpenMetadata);
    LIB_FUNCTION("bjAlYWwRTJA", "libSceContentSearch", 1, "libSceContentSearch",
                 sceContentSearchOpenMetadataByContentId);
    LIB_FUNCTION("ruNe-FgCzO8", "libSceContentSearch", 1, "libSceContentSearch",
                 sceContentSearchGetMetadataValue);
    LIB_FUNCTION("0DqfN9r3jmA", "libSceContentSearch", 1, "libSceContentSearch",
                 sceContentSearchGetMetadataFieldInfo);
    LIB_FUNCTION("-YbpaF0XS-I", "libSceContentSearch", 1, "libSceContentSearch",
                 sceContentSearchCloseMetadata);
    LIB_FUNCTION("1xSZodB2geA", "libSceContentSearch", 1, "libSceContentSearch",
                 sceContentSearchTerm);
}

} // namespace Libraries::ContentSearch
