// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <string>
#include "common/types.h"

namespace VideoCore {

/// True when the user enabled "Collect debug logs" (or SHADPS4_DEBUG_LOGS=1): frame rate and cost
/// counter lines, freeze watchdog and extra texture cache logging. Off by default.
bool DebugLogsEnabled();

/// Logs the game frame rate ("GameFPS", sceGnmSubmitDone calls) and the cost counters
/// ("PerfStats") every 120 game frames. The first call starts the freeze watchdog.
void NotifyGameFrame();

/// Logs the presented game frame rate ("PresentFPS") every 300 frames.
void NotifyPresent();

/// Automated testing: counts dispatches of the shader named in SHADPS4_WATCH_SHADER (hex hash).
void NotifyComputeDispatch(u64 pgm_hash);

/// Cost counters logged with GameFPS as "PerfStats" (per 120 game frames).
enum class PerfCounter : u32 {
    FceCalls,           ///< EliminateFastClear runs (FCE and FMASK decompress passes)
    FceClears,          ///< ... that cleared the image
    FceUs,              ///< CPU time spent in them
    FlushCpu,           ///< CPU writes that waited for GPU images to be flushed first
    FlushWaitUs,        ///< time guest threads waited for those flushes
    FlushWaitMaxUs,     ///< longest single wait
    TileToBuffer,       ///< images tiled into the buffer cache (write backs)
    TileBytes,          ///< ... their size
    WriteBacksDeferred, ///< storage alias write backs deferred (lazy write back)
    TileMbWriteBack,    ///< write back bytes by reason (same order as TextureCache::TileReason)
    TileMbView,
    TileMbPending,
    TileMbCpuWrite,
    ImagesCreated,   ///< new texture cache images
    AudioOutputs,    ///< audio buffers the game submitted (all ports)
    AudioDry,        ///< ... arriving after the device queue ran empty (audible gap)
    AudioClears,     ///< backed-up queue dropped (audible skip)
    AudioMaxGapUs,   ///< longest interval between two submits of one port
    AjmBatches,      ///< audio decode batches (MP3/AT9/AAC) processed by the AJM worker
    AjmBusyUs,       ///< time the worker spent decoding
    AjmMaxLatencyUs, ///< longest submit-to-done time of a batch (queueing + decoding)
    Count,
};

/// Counters only count while debug logs are enabled.
void PerfAdd(PerfCounter counter, u64 value = 1);
void PerfMax(PerfCounter counter, u64 value);

/// Freeze watchdog: describes an address (e.g. buffer cache state) in the freeze dump.
void SetFreezeAddressInspector(std::function<std::string(u64)> inspector);

} // namespace VideoCore
