// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef _MSC_VER
#define BREAKPOINT __debugbreak
#elif defined(__GNUC__)
#define BREAKPOINT __builtin_trap
#else
#error What the fuck is this compiler
#endif

// Tracy disabled for tests
static inline bool IsProfilerConnected() {
    return false;
}

#define TRACY_GPU_ENABLED 0

#define CUSTOM_LOCK(type, varname)

#define TRACK_ALLOC(ptr, size, pool)
#define TRACK_FREE(ptr, pool)

enum MarkersPalette : int {
    EmulatorMarkerColor = 0x264653,
    RendererMarkerColor = 0x2a9d8f,
    HleMarkerColor = 0xe9c46a,
    GpuMarkerColor = 0xf4a261,
    Reserved1 = 0xe76f51,
};

#define EMULATOR_TRACE
#define RENDERER_TRACE
#define HLE_TRACE

#define TRACE_HINT(str)

#define TRACE_WARN(msg)
#define TRACE_ERROR(msg)
#define TRACE_CRIT(msg)

#define GPU_SCOPE_LOCATION(name, color)

#define MUTEX_LOCATION(name)

#define FRAME_END

#define FIBER_ENTER(name)
#define FIBER_EXIT
