// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
// tlhelp32.h needs the types from windows.h
#include <tlhelp32.h>
#endif

#include "common/config.h"
#include "common/logging/log.h"
#include "video_core/perf_stats.h"

namespace VideoCore {

bool DebugLogsEnabled() {
    // "Collect debug logs" in the settings; SHADPS4_DEBUG_LOGS=1/0 overrides it for one run.
    static const bool enabled = [] {
        if (const char* env = std::getenv("SHADPS4_DEBUG_LOGS"); env && *env) {
            return env[0] == '1';
        }
        return Config::collectDebugLogs();
    }();
    return enabled;
}

static bool FreezeWatchdogEnabled() {
    static const bool enabled = [] {
        const char* dump = std::getenv("SHADPS4_FREEZE_DUMP_S");
        const char* exit = std::getenv("SHADPS4_FREEZE_EXIT");
        return DebugLogsEnabled() || (dump && *dump) || (exit && *exit == '1');
    }();
    return enabled;
}

// Dispatches of the SHADPS4_WATCH_SHADER shader (automated tests check that a scene was reached).
static std::atomic<u64> watch_count{0};

static std::array<std::atomic<u64>, static_cast<size_t>(PerfCounter::Count)> perf_counters{};

// Freeze watchdog (with debug logs, or SHADPS4_FREEZE_DUMP_S / SHADPS4_FREEZE_EXIT set): when the
// game submits no frame for SHADPS4_FREEZE_DUMP_S seconds (default 10) after it started
// rendering, the call stack of every thread is logged ("Freeze:" lines, exe+offset frames,
// resolve with the linker map). SHADPS4_FREEZE_EXIT=1 then ends the process (automated runs).
// Stacks are walked with the x64 unwind tables, so they stop at guest code.
static std::atomic<s64> last_game_frame_ns{0};
static std::function<std::string(u64)> freeze_inspector;

void SetFreezeAddressInspector(std::function<std::string(u64)> inspector) {
    freeze_inspector = std::move(inspector);
}

#ifdef _WIN32
static void DumpThreadStacks() {
    const DWORD self = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }
    const auto exe = reinterpret_cast<u64>(GetModuleHandleW(nullptr));
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Thread32First(snap, &entry); ok; ok = Thread32Next(snap, &entry)) {
        if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) {
            continue;
        }
        const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                             THREAD_QUERY_LIMITED_INFORMATION,
                                         FALSE, entry.th32ThreadID);
        if (!thread) {
            continue;
        }
        // Nothing that allocates or logs while the thread is suspended (it may hold a lock).
        std::array<u64, 48> pcs{};
        u32 count = 0;
        CONTEXT start{}; // registers where the thread stopped, for guest code analysis
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(thread, &ctx)) {
                start = ctx;
                while (count < pcs.size() && ctx.Rip != 0) {
                    pcs[count++] = ctx.Rip;
                    DWORD64 image_base = 0;
                    const auto* function = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
                    if (!function) {
                        if (count != 1) {
                            break; // guest code or JIT: no unwind info
                        }
                        // Leaf function (e.g. a syscall stub): the return address is on top.
                        ctx.Rip = *reinterpret_cast<const u64*>(ctx.Rsp);
                        ctx.Rsp += 8;
                        continue;
                    }
                    PVOID handler_data = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip,
                                     const_cast<PRUNTIME_FUNCTION>(function), &ctx, &handler_data,
                                     &establisher, nullptr);
                }
            }
            ResumeThread(thread);
        }
        std::string name;
        PWSTR wide_name = nullptr;
        if (SUCCEEDED(GetThreadDescription(thread, &wide_name)) && wide_name) {
            for (const wchar_t* c = wide_name; *c; ++c) {
                name += *c < 128 ? static_cast<char>(*c) : '?';
            }
            LocalFree(wide_name);
        }
        CloseHandle(thread);
        std::string frames;
        for (u32 i = 0; i < count; ++i) {
            HMODULE module = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(pcs[i]), &module) &&
                module) {
                const auto base = reinterpret_cast<u64>(module);
                if (base == exe) {
                    frames += fmt::format(" exe+{:#x}", pcs[i] - base);
                } else {
                    char path[MAX_PATH]{};
                    GetModuleFileNameA(module, path, MAX_PATH);
                    const char* file = std::strrchr(path, '\\');
                    frames += fmt::format(" {}+{:#x}", file ? file + 1 : path, pcs[i] - base);
                }
            } else {
                frames += fmt::format(" {:#x}", pcs[i]);
            }
        }
        LOG_CRITICAL(Render, "Freeze: thread {} '{}':{}", entry.th32ThreadID, name, frames);
        HMODULE start_module = nullptr;
        if (count > 0 && start.Rip != 0 &&
            !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(start.Rip), &start_module)) {
            // Stopped in guest code (e.g. polling a fence): registers, code around RIP and what
            // the pointer-like registers point to.
            const std::array<std::pair<const char*, u64>, 17> regs{{
                {"rip", start.Rip},
                {"rax", start.Rax},
                {"rbx", start.Rbx},
                {"rcx", start.Rcx},
                {"rdx", start.Rdx},
                {"rsi", start.Rsi},
                {"rdi", start.Rdi},
                {"rbp", start.Rbp},
                {"rsp", start.Rsp},
                {"r8", start.R8},
                {"r9", start.R9},
                {"r10", start.R10},
                {"r11", start.R11},
                {"r12", start.R12},
                {"r13", start.R13},
                {"r14", start.R14},
                {"r15", start.R15},
            }};
            std::string text;
            for (const auto& [reg, value] : regs) {
                text += fmt::format(" {}={:#x}", reg, value);
            }
            LOG_CRITICAL(Render, "Freeze: thread {} regs:{}", entry.th32ThreadID, text);
            std::array<u8, 96> code{};
            SIZE_T read = 0;
            if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(start.Rip - 48),
                                  code.data(), code.size(), &read)) {
                std::string hex;
                for (SIZE_T i = 0; i < read; ++i) {
                    hex += fmt::format("{:02x}", code[i]);
                }
                LOG_CRITICAL(Render, "Freeze: thread {} code at rip-48: {}", entry.th32ThreadID,
                             hex);
            }
            for (const auto& [reg, value] : regs) {
                if (value < 0x10000 || value > 0x7fffffffffffull || value == start.Rip) {
                    continue;
                }
                u64 qwords[2]{};
                if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(value), qwords,
                                      sizeof(qwords), &read)) {
                    LOG_CRITICAL(Render, "Freeze: thread {} [{}={:#x}] = {:#018x} {:#018x} {}",
                                 entry.th32ThreadID, reg, value, qwords[0], qwords[1],
                                 freeze_inspector ? freeze_inspector(value) : std::string{});
                }
            }
        }
    }
    CloseHandle(snap);
}
#endif

static void StartFreezeWatchdog() {
    std::thread([] {
        const char* env = std::getenv("SHADPS4_FREEZE_DUMP_S");
        const s64 limit_ns = (env && *env ? std::atoll(env) : 10) * 1'000'000'000LL;
        const char* exit_env = std::getenv("SHADPS4_FREEZE_EXIT");
        const bool exit_on_freeze = exit_env && *exit_env == '1';
        u32 dumps = 0;
        s64 dumped_frame_ns = 0;
        while (limit_ns > 0 && dumps < 3) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            const s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
            const s64 last = last_game_frame_ns.load(std::memory_order_relaxed);
            if (last == 0 || last == dumped_frame_ns || now - last < limit_ns) {
                continue;
            }
            dumped_frame_ns = last;
            ++dumps;
            LOG_CRITICAL(Render, "Freeze: no game frame for {:.1f} s, dumping thread stacks",
                         (now - last) / 1e9);
#ifdef _WIN32
            DumpThreadStacks();
#endif
            if (exit_on_freeze) {
                LOG_CRITICAL(Render, "Freeze: exiting (SHADPS4_FREEZE_EXIT=1)");
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                std::_Exit(3);
            }
        }
    }).detach();
}

void PerfAdd(PerfCounter counter, u64 value) {
    if (!DebugLogsEnabled()) {
        return;
    }
    perf_counters[static_cast<size_t>(counter)].fetch_add(value, std::memory_order_relaxed);
}

void PerfMax(PerfCounter counter, u64 value) {
    if (!DebugLogsEnabled()) {
        return;
    }
    auto& slot = perf_counters[static_cast<size_t>(counter)];
    u64 old = slot.load(std::memory_order_relaxed);
    while (old < value && !slot.compare_exchange_weak(old, value, std::memory_order_relaxed)) {
    }
}

void NotifyGameFrame() {
    const bool stats = DebugLogsEnabled();
    if (!stats && !FreezeWatchdogEnabled()) {
        return;
    }
    using Clock = std::chrono::steady_clock;
    static Clock::time_point window_start = Clock::now();
    static Clock::time_point last_frame = window_start;
    static u64 frames = 0;
    static double max_frame_ms = 0.0;
    static u32 hitches = 0;
    const auto now = Clock::now();
    const s64 now_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    if (last_game_frame_ns.exchange(now_ns, std::memory_order_relaxed) == 0 &&
        FreezeWatchdogEnabled()) {
        StartFreezeWatchdog();
    }
    if (!stats) {
        return;
    }
    const double frame_ms = std::chrono::duration<double, std::milli>(now - last_frame).count();
    last_frame = now;
    max_frame_ms = std::max(max_frame_ms, frame_ms);
    hitches += frame_ms > 50.0;
    if (++frames % 120 == 0) {
        const double secs = std::chrono::duration<double>(now - window_start).count();
        LOG_INFO(Render, "GameFPS: {:.1f} (watched shader seen: {})", 120.0 / secs,
                 watch_count.load() > 0 ? "yes" : "no");
        const auto take = [](PerfCounter counter) {
            return perf_counters[static_cast<size_t>(counter)].exchange(0,
                                                                        std::memory_order_relaxed);
        };
        const u64 fce = take(PerfCounter::FceCalls), fce_clears = take(PerfCounter::FceClears);
        const u64 fce_us = take(PerfCounter::FceUs), flushes = take(PerfCounter::FlushCpu);
        const u64 wait_us = take(PerfCounter::FlushWaitUs);
        const u64 wait_max_us = take(PerfCounter::FlushWaitMaxUs);
        const u64 tiles = take(PerfCounter::TileToBuffer),
                  tile_bytes = take(PerfCounter::TileBytes);
        const u64 created = take(PerfCounter::ImagesCreated);
        const u64 deferred = take(PerfCounter::WriteBacksDeferred);
        const double mb_wb = take(PerfCounter::TileMbWriteBack) / 1048576.0;
        const double mb_view = take(PerfCounter::TileMbView) / 1048576.0;
        const double mb_pending = take(PerfCounter::TileMbPending) / 1048576.0;
        const double mb_cpu = take(PerfCounter::TileMbCpuWrite) / 1048576.0;
        const u64 audio = take(PerfCounter::AudioOutputs), audio_dry = take(PerfCounter::AudioDry);
        const u64 audio_clears = take(PerfCounter::AudioClears);
        const u64 audio_gap_us = take(PerfCounter::AudioMaxGapUs);
        const u64 ajm = take(PerfCounter::AjmBatches), ajm_busy_us = take(PerfCounter::AjmBusyUs);
        const u64 ajm_latency_us = take(PerfCounter::AjmMaxLatencyUs);
        LOG_INFO(Render,
                 "PerfStats: max_frame_ms={:.1f} frames_over_50ms={} fce={} fce_clears={} "
                 "fce_ms={:.2f} cpu_write_flushes={} flush_wait_ms={:.1f} flush_wait_max_ms={:.1f} "
                 "write_backs={} write_back_mb={:.1f} (immediate {:.0f} view {:.0f} deferred "
                 "{:.0f} cpu_write {:.0f}) "
                 "write_backs_deferred={} new_images={} audio_outputs={} audio_dry={} "
                 "audio_clears={} audio_max_gap_ms={:.1f} ajm_batches={} ajm_busy_pct={:.1f} "
                 "ajm_max_latency_ms={:.1f}",
                 max_frame_ms, hitches, fce, fce_clears, fce_us / 1000.0, flushes, wait_us / 1000.0,
                 wait_max_us / 1000.0, tiles, tile_bytes / 1048576.0, mb_wb, mb_view, mb_pending,
                 mb_cpu, deferred, created, audio, audio_dry, audio_clears, audio_gap_us / 1000.0,
                 ajm, secs > 0 ? ajm_busy_us / (secs * 1e4) : 0.0, ajm_latency_us / 1000.0);
        max_frame_ms = 0.0;
        hitches = 0;
        window_start = now;
    }
}

void NotifyPresent() {
    if (!DebugLogsEnabled()) {
        return;
    }
    using Clock = std::chrono::steady_clock;
    static Clock::time_point window_start = Clock::now();
    static u64 frames = 0;
    if (++frames % 300 == 0) {
        const auto now = Clock::now();
        const double secs = std::chrono::duration<double>(now - window_start).count();
        LOG_INFO(Render, "PresentFPS: {:.1f} (watched shader seen: {})", 300.0 / secs,
                 watch_count.load() > 0 ? "yes" : "no");
        window_start = now;
    }
}

void NotifyComputeDispatch(u64 pgm_hash) {
    static const u64 watch_hash = [] {
        const char* env = std::getenv("SHADPS4_WATCH_SHADER");
        return env && *env ? std::strtoull(env, nullptr, 16) : 0ull;
    }();
    if (watch_hash == 0 || pgm_hash != watch_hash) {
        return;
    }
    if (watch_count.fetch_add(1) == 0) {
        LOG_INFO(Render, "WatchShader: {:#x} dispatched for the first time", pgm_hash);
    }
}

} // namespace VideoCore
