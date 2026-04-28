// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::ScreenShot {

int PS4_SYSV_ABI _Z5dummyv();
int PS4_SYSV_ABI sceScreenShotCapture();
int PS4_SYSV_ABI sceScreenShotDisable();
int PS4_SYSV_ABI sceScreenShotDisableNotification();
int PS4_SYSV_ABI sceScreenShotEnable();
int PS4_SYSV_ABI sceScreenShotEnableNotification();
int PS4_SYSV_ABI sceScreenShotGetAppInfo();
int PS4_SYSV_ABI sceScreenShotGetDrcParam();
int PS4_SYSV_ABI sceScreenShotIsDisabled();
int PS4_SYSV_ABI sceScreenShotIsVshScreenCaptureDisabled();
int PS4_SYSV_ABI sceScreenShotSetOverlayImage();
int PS4_SYSV_ABI sceScreenShotSetOverlayImageWithOrigin();
int PS4_SYSV_ABI sceScreenShotSetParam();
int PS4_SYSV_ABI sceScreenShotSetDrcParam();

int PS4_SYSV_ABI sceScreenShotUnknownZ(u64 a1 = 0, u64 a2 = 0, u64 a3 = 0, u64 a4 = 0,
                                        u64 a5 = 0, u64 a6 = 0, u64 a7 = 0, u64 a8 = 0);

void RegisterLib(Core::Loader::SymbolsResolver* sym);
} // namespace Libraries::ScreenShot
// Note: added before closing namespace brace - need to fix
