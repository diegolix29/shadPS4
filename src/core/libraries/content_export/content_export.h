// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Core::Loader {
    class SymbolsResolver;
}

namespace Libraries::ContentExport {

    // Probe wrappers (broad ABI on purpose for reverse-mapping GR2 call args)
    int PS4_SYSV_ABI sceContentExportInit(u64 a1 = 0, u64 a2 = 0, u64 a3 = 0, u64 a4 = 0,
                                          u64 a5 = 0, u64 a6 = 0, u64 a7 = 0, u64 a8 = 0);
    int PS4_SYSV_ABI sceContentExportStart(u64 a1 = 0, u64 a2 = 0, u64 a3 = 0, u64 a4 = 0,
                                           u64 a5 = 0, u64 a6 = 0, u64 a7 = 0, u64 a8 = 0);
    int PS4_SYSV_ABI sceContentExportFromData(u64 a1 = 0, u64 a2 = 0, u64 a3 = 0, u64 a4 = 0,
                                              u64 a5 = 0, u64 a6 = 0, u64 a7 = 0, u64 a8 = 0);
    int PS4_SYSV_ABI sceContentExportFromDataWithThumbnail(u64 a1 = 0, u64 a2 = 0, u64 a3 = 0,
                                                           u64 a4 = 0, u64 a5 = 0, u64 a6 = 0,
                                                           u64 a7 = 0, u64 a8 = 0);
    int PS4_SYSV_ABI sceContentExportFinish(u64 a1 = 0, u64 a2 = 0, u64 a3 = 0, u64 a4 = 0,
                                            u64 a5 = 0, u64 a6 = 0, u64 a7 = 0, u64 a8 = 0);
    int PS4_SYSV_ABI sceContentExportTerm(u64 a1 = 0, u64 a2 = 0, u64 a3 = 0, u64 a4 = 0,
                                          u64 a5 = 0, u64 a6 = 0, u64 a7 = 0, u64 a8 = 0);

    void RegisterLib(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries::ContentExport
