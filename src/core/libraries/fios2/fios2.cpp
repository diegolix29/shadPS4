// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
#include <string>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "core/libraries/fios2/fios2.h"
#include "core/libraries/kernel/file_system.h"
#include "core/libraries/libs.h"
#include "common/singleton.h"
#include "core/file_sys/fs.h"

namespace Libraries::Fios2 {

struct FiosStat {
    s64 size;
    u64 access_date;
    u64 modification_date;
    u64 creation_date;
    u32 flags;
    u32 reserved;
    s64 uid;
    s64 gid;
    s64 dev;
    s64 ino;
    s64 mode;
};

s32 PS4_SYSV_ABI sceFiosInitialize(const void* parameters) {
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][Initialize] parameters={}", parameters);
    return 0;
}

s32 PS4_SYSV_ABI sceFiosFHOpenSync(const void* op_attr, s32* handle, const char* path,
                                   const void* open_params) {
    if (handle == nullptr || path == nullptr) {
        return -1;
    }
    const s32 fd = Kernel::posix_open(path, 0, 0);
    if (fd < 0) {
        LOG_ERROR(Lib_SysModule, "[FIOS-HLE][FHOpenSync] failed path='{}'", path);
        return fd;
    }
    *handle = fd;
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][FHOpenSync] path='{}' handle={} op_attr={} open_params={}",
             path, fd, op_attr, open_params);
    return 0;
}

s32 PS4_SYSV_ABI sceFiosFHOpenWithModeSync(const void* op_attr, s32* handle, const char* path,
                                           const void* open_params, u16 mode) {
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][FHOpenWithModeSync] path='{}' mode={:#o}", path, mode);
    return sceFiosFHOpenSync(op_attr, handle, path, open_params);
}

s64 PS4_SYSV_ABI sceFiosFHReadSync(const void* op_attr, s32 handle, void* buffer, s64 size) {
    const s64 read = Kernel::sceKernelRead(handle, buffer, static_cast<u64>(size));
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][FHReadSync] handle={} size={} read={} buffer={} op_attr={}",
             handle, size, read, buffer, op_attr);
    return read;
}

s64 PS4_SYSV_ABI sceFiosFHPreadSync(const void* op_attr, s32 handle, void* buffer, s64 size,
                                    s64 offset) {
    const s64 read = Kernel::sceKernelPread(handle, buffer, static_cast<u64>(size), offset);
    LOG_INFO(Lib_SysModule,
             "[FIOS-HLE][FHPreadSync] handle={} offset={:#x} size={} read={} buffer={} op_attr={}",
             handle, offset, size, read, buffer, op_attr);
    return read;
}

s32 PS4_SYSV_ABI sceFiosFHStatSync(const void* op_attr, s32 handle, FiosStat* stat) {
    if (stat == nullptr) {
        return -1;
    }
    Kernel::OrbisKernelStat kernel_stat{};
    const s32 result = Kernel::sceKernelFstat(handle, &kernel_stat);
    if (result >= 0) {
        *stat = {};
        stat->size = kernel_stat.st_size;
        stat->uid = kernel_stat.st_uid;
        stat->gid = kernel_stat.st_gid;
        stat->dev = kernel_stat.st_dev;
        stat->ino = kernel_stat.st_ino;
        stat->mode = kernel_stat.st_mode;
    }
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][FHStatSync] handle={} size={} result={} op_attr={}",
             handle, result >= 0 ? stat->size : -1, result, op_attr);
    return result;
}

s64 PS4_SYSV_ABI sceFiosFHGetSize(s32 handle) {
    Kernel::OrbisKernelStat stat{};
    const s32 result = Kernel::sceKernelFstat(handle, &stat);
    const s64 size = result >= 0 ? stat.st_size : result;
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][FHGetSize] handle={} size={}", handle, size);
    return size;
}

s64 PS4_SYSV_ABI sceFiosFHSeek(s32 handle, s64 offset, s32 whence) {
    const s64 result = Kernel::posix_lseek(handle, offset, whence);
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][FHSeek] handle={} offset={} whence={} result={}", handle,
             offset, whence, result);
    return result;
}

s32 PS4_SYSV_ABI sceFiosIsValidHandle(s32 handle) {
    Kernel::OrbisKernelStat stat{};
    const bool valid = Kernel::sceKernelFstat(handle, &stat) >= 0;
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][IsValidHandle] handle={} valid={}", handle, valid);
    return valid ? 1 : 0;
}

s32 PS4_SYSV_ABI sceFiosFHCloseSync(const void* op_attr, s32 handle) {
    const s32 result = Kernel::posix_close(handle);
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][FHCloseSync] handle={} result={} op_attr={}", handle,
             result, op_attr);
    return result;
}


s32 PS4_SYSV_ABI sceFiosIOFilterAdd(s32 index, void* filter, void* context) {
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][IOFilterAdd] index={} filter={} context={}", index, filter,
             context);
    return 0;
}

void* PS4_SYSV_ABI sceFiosIOFilterPsarcDearchiver() {
    // Unity often stores the dearchiver function pointer; a non-null dummy is enough
    // for registration. Actual decompression is not implemented here.
    static int dummy_psarc_filter = 1;
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][IOFilterPsarcDearchiver] stub");
    return &dummy_psarc_filter;
}

s64 PS4_SYSV_ABI sceFiosArchiveGetMountBufferSizeSync(const void* op_attr, const char* path,
                                                      const void* params) {
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][ArchiveGetMountBufferSizeSync] path='{}' op={} params={}",
             path ? path : "(null)", op_attr, params);
    // Generous buffer; real PSARC mount needs more work if game depends on archives.
    return 0x100000; // 1 MiB
}

s32 PS4_SYSV_ABI sceFiosArchiveMountSync(const void* op_attr, s32* handle, const char* path,
                                         const char* mount_point, void* mount_buffer,
                                         s64 mount_buffer_size, s32 flags) {
    static_cast<void>(op_attr);
    static_cast<void>(mount_buffer);
    static_cast<void>(mount_buffer_size);
    static_cast<void>(flags);
    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    // Extracted dumps often have no real .psarc; content lives under /app0 (game root).
    // Map the FIOS mount point to the same host folder so asset paths resolve.
    const auto app0_host = mnt->GetHostPath("/app0");
    std::string guest_mount = "/archive/mount/point";
    if (mount_point != nullptr && mount_point[0] != 0) {
        guest_mount = mount_point;
    }
    if (!app0_host.empty()) {
        mnt->Mount(app0_host, guest_mount, true);
        LOG_INFO(Lib_SysModule,
                 "[FIOS-HLE][ArchiveMountSync] mapped '{}' -> host '{}' (path request='{}')",
                 guest_mount, app0_host.string(), path ? path : "(null)");
    } else {
        LOG_ERROR(Lib_SysModule, "[FIOS-HLE][ArchiveMountSync] no /app0 host path for mount '{}'",
                  guest_mount);
    }
    if (handle) {
        *handle = 1; // non-zero fake handle; unmount not implemented
    }
    return 0;
}

s64 PS4_SYSV_ABI sceFiosFileGetSizeSync(const void* op_attr, const char* path) {
    if (path == nullptr) {
        return -1;
    }
    s32 handle = -1;
    const s32 open_result = sceFiosFHOpenSync(op_attr, &handle, path, nullptr);
    if (open_result < 0) {
        return open_result;
    }
    const s64 size = sceFiosFHGetSize(handle);
    sceFiosFHCloseSync(op_attr, handle);
    return size;
}

s32 PS4_SYSV_ABI sceFiosFileExistsSync(const void* op_attr, const char* path) {
    if (path == nullptr) {
        return 0;
    }
    s32 handle = -1;
    const s32 open_result = sceFiosFHOpenSync(op_attr, &handle, path, nullptr);
    if (open_result < 0) {
        return 0;
    }
    sceFiosFHCloseSync(op_attr, handle);
    return 1;
}

s32 PS4_SYSV_ABI sceFiosDirectoryExistsSync(const void* op_attr, const char* path) {
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][DirectoryExistsSync] path='{}'", path ? path : "(null)");
    static_cast<void>(op_attr);
    // Best-effort: treat missing path as absent without full dir APIs.
    return path != nullptr ? 1 : 0;
}

s64 PS4_SYSV_ABI sceFiosFHTell(s32 handle) {
    return sceFiosFHSeek(handle, 0, 1 /*SEEK_CUR*/);
}

s64 PS4_SYSV_ABI sceFiosFHWriteSync(const void* op_attr, s32 handle, const void* buffer, s64 size) {
    static_cast<void>(op_attr);
    const s64 written = Kernel::sceKernelWrite(handle, buffer, static_cast<u64>(size));
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][FHWriteSync] handle={} size={} written={}", handle, size,
             written);
    return written;
}

s32 PS4_SYSV_ABI sceFiosStatSync(const void* op_attr, const char* path, FiosStat* stat) {
    if (path == nullptr || stat == nullptr) {
        return -1;
    }
    s32 handle = -1;
    const s32 open_result = sceFiosFHOpenSync(op_attr, &handle, path, nullptr);
    if (open_result < 0) {
        return open_result;
    }
    const s32 result = sceFiosFHStatSync(op_attr, handle, stat);
    sceFiosFHCloseSync(op_attr, handle);
    return result;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("wAKZ-det+yo", "libSceFios2", 1, "libSceFios2", sceFiosInitialize);
    LIB_FUNCTION("b44anV2D7K0", "libSceFios2", 1, "libSceFios2", sceFiosFHOpenSync);
    LIB_FUNCTION("w13Ojm7ON9o", "libSceFios2", 1, "libSceFios2", sceFiosFHOpenWithModeSync);
    LIB_FUNCTION("Bn2ZF4ZjeuQ", "libSceFios2", 1, "libSceFios2", sceFiosFHReadSync);
    LIB_FUNCTION("2m9+Opco-hk", "libSceFios2", 1, "libSceFios2", sceFiosFHPreadSync);
    LIB_FUNCTION("xP45eIntEis", "libSceFios2", 1, "libSceFios2", sceFiosFHStatSync);
    LIB_FUNCTION("FdjoqFQOlt0", "libSceFios2", 1, "libSceFios2", sceFiosFHGetSize);
    LIB_FUNCTION("xReSebwKApA", "libSceFios2", 1, "libSceFios2", sceFiosFHSeek);
    LIB_FUNCTION("8IGjwtnvYwI", "libSceFios2", 1, "libSceFios2", sceFiosIsValidHandle);
    LIB_FUNCTION("AOujSGqU+ms", "libSceFios2", 1, "libSceFios2", sceFiosFHCloseSync);
    LIB_FUNCTION("lgITuBsRo2o", "libSceFios2", 1, "libSceFios2", sceFiosIOFilterAdd);
    LIB_FUNCTION("OIGbkgGOu6E", "libSceFios2", 1, "libSceFios2", sceFiosIOFilterPsarcDearchiver);
    LIB_FUNCTION("UUriaXy7G90", "libSceFios2", 1, "libSceFios2", sceFiosArchiveGetMountBufferSizeSync);
    LIB_FUNCTION("xutLbQdqyb4", "libSceFios2", 1, "libSceFios2", sceFiosArchiveMountSync);
    LIB_FUNCTION("zF8-CRvRXnM", "libSceFios2", 1, "libSceFios2", sceFiosFileGetSizeSync);
    LIB_FUNCTION("NwOHMRM2Ppw", "libSceFios2", 1, "libSceFios2", sceFiosFileExistsSync);
    LIB_FUNCTION("OOuvHKTu4Oc", "libSceFios2", 1, "libSceFios2", sceFiosDirectoryExistsSync);
    LIB_FUNCTION("MrRFrdgpsx8", "libSceFios2", 1, "libSceFios2", sceFiosFHTell);
    LIB_FUNCTION("Kl-TbrDU9YM", "libSceFios2", 1, "libSceFios2", sceFiosFHWriteSync);
    LIB_FUNCTION("jayvY07C5dk", "libSceFios2", 1, "libSceFios2", sceFiosStatSync);
}

} // namespace Libraries::Fios2
