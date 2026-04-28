// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"
#include "core/libraries/share_utility/share_utility.h"

namespace Libraries::ShareUtility {

    namespace {
        void DumpProbe(const char* fn, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7, u64 a8) {
            LOG_ERROR(Core,
                      "[GR2PhotoProbe][ShareUtility] {} args: a1={:#x} a2={:#x} a3={:#x} a4={:#x} "
                      "a5={:#x} a6={:#x} a7={:#x} a8={:#x}",
                      fn, a1, a2, a3, a4, a5, a6, a7, a8);
        }
    } // namespace

    int PS4_SYSV_ABI sceShareUtilityInitialize(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7,
                                               u64 a8) {
        DumpProbe(__func__, a1, a2, a3, a4, a5, a6, a7, a8);
        return ORBIS_OK;
                                               }

                                               int PS4_SYSV_ABI sceShareUtilityTerminate(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7,
                                                                                         u64 a8) {
                                                   DumpProbe(__func__, a1, a2, a3, a4, a5, a6, a7, a8);
                                                   return ORBIS_OK;
                                                                                         }

                                                                                         int PS4_SYSV_ABI sceShareUtilityOpenShareMenu(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7,
                                                                                                                                       u64 a8) {
                                                                                             DumpProbe(__func__, a1, a2, a3, a4, a5, a6, a7, a8);
                                                                                             // Keep explicit failure for now; not needed for core camera/gallery testing.
                                                                                             return ORBIS_KERNEL_ERROR_ENOSYS;
                                                                                                                                       }

                                                                                                                                       int PS4_SYSV_ABI sceShareUtilityOpenShareMenuDefault(u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6,
                                                                                                                                                                                            u64 a7, u64 a8) {
                                                                                                                                           DumpProbe(__func__, a1, a2, a3, a4, a5, a6, a7, a8);
                                                                                                                                           return ORBIS_KERNEL_ERROR_ENOSYS;
                                                                                                                                                                                            }

                                                                                                                                                                                            void RegisterLib(Core::Loader::SymbolsResolver* sym) {
                                                                                                                                                                                                LIB_FUNCTION("Jlv-lYxEnvM", "libSceShareUtility", 1, "libSceShareUtility", sceShareUtilityInitialize);
                                                                                                                                                                                                LIB_FUNCTION("DUWhxkyVPj4", "libSceShareUtility", 1, "libSceShareUtility", sceShareUtilityTerminate);
                                                                                                                                                                                                LIB_FUNCTION("Lqp4EDIRXSo", "libSceShareUtility", 1, "libSceShareUtility", sceShareUtilityOpenShareMenu);
                                                                                                                                                                                                LIB_FUNCTION("8hZ2EEl2Tto", "libSceShareUtility", 1, "libSceShareUtility",
                                                                                                                                                                                                             sceShareUtilityOpenShareMenuDefault);
                                                                                                                                                                                            }

} // namespace Libraries::ShareUtility
