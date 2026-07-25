// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
#include <mutex>
#include <string>
#include <unordered_map>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "core/libraries/fios2/fios2.h"
#include "core/libraries/kernel/file_system.h"
#include "core/libraries/libs.h"
#include "common/singleton.h"
#include "core/file_sys/fs.h"

namespace Libraries::Fios2 {

// The guest uses the async Fios2 op API (submit op -> OpWait -> OpGetActualCount).
// shadPS4 services I/O synchronously, so each async submitter runs the work inline and
// records the result under a fake op handle that the Op* queries read back. Fake handles
// start above zero so they never collide with the "invalid op" sentinel (-1 / 0).
namespace {
std::mutex g_op_mutex;
std::unordered_map<s32, s64> g_op_actual_counts;
s32 g_next_op_handle = 1;

s32 AllocateOpHandle(s64 actual_count) {
    std::scoped_lock lock{g_op_mutex};
    const s32 op = g_next_op_handle++;
    g_op_actual_counts[op] = actual_count;
    return op;
}

s64 QueryOpActualCount(s32 op) {
    std::scoped_lock lock{g_op_mutex};
    const auto it = g_op_actual_counts.find(op);
    return it != g_op_actual_counts.end() ? it->second : 0;
}
} // namespace

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

// Async Fios2 op API. shadPS4 has no real async I/O queue, so async submitters do the
// work inline and hand back a fake op handle. The Op* waiters then report the op as
// already complete and return the recorded result. This mirrors what the desktop x86
// path achieves via AeroLib's benign no-op stubs, while keeping the byte counts intact.

s32 PS4_SYSV_ABI sceFiosOverlayAdd(void* overlay, s32* out_id) {
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][OverlayAdd] overlay={} out_id={}", overlay,
             static_cast<void*>(out_id));
    // No overlay filesystem is implemented; accept and ignore the registration.
    if (out_id != nullptr) {
        *out_id = 1; // fake overlay id
    }
    return 0;
}

s32 PS4_SYSV_ABI sceFiosFHPread(const void* op_attr, s32 handle, void* buffer, s64 size,
                                s64 offset) {
    // Non-blocking positioned read: perform it synchronously and return a fake op handle
    // so a following OpWait/OpGetActualCount returns the real byte count. The op handle is
    // the return value (positive on success, negative errno on failure), matching the Vita/
    // PS4 SceFiosOp convention rather than a separate out-param.
    const s64 read = Kernel::sceKernelPread(handle, buffer, static_cast<u64>(size), offset);
    const s32 op = read >= 0 ? AllocateOpHandle(read) : static_cast<s32>(read);
    LOG_INFO(Lib_SysModule,
             "[FIOS-HLE][FHPread] handle={} offset={:#x} size={} read={} op={} op_attr={}", handle,
             offset, size, read, op, op_attr);
    return op;
}

s32 PS4_SYSV_ABI sceFiosFHOpen(const void* op_attr, s32* out_handle, const char* path,
                               const void* open_params) {
    // Non-blocking open: run it synchronously (shadPS4 has no async queue), store the file
    // descriptor in *out_handle for the caller, and return a positive op handle so the
    // following OpWait/OpGetActualCount report success. Mirrors FHOpenSync but yields an op.
    s32 op = -1;
    if (out_handle != nullptr && path != nullptr) {
        const s32 fd = Kernel::posix_open(path, 0, 0);
        if (fd >= 0) {
            *out_handle = fd;
            op = AllocateOpHandle(0);
            LOG_INFO(Lib_SysModule,
                     "[FIOS-HLE][FHOpen] path='{}' handle={} op={} op_attr={} open_params={}", path,
                     fd, op, op_attr, open_params);
        } else {
            LOG_ERROR(Lib_SysModule, "[FIOS-HLE][FHOpen] failed path='{}'", path);
            op = fd;
        }
    }
    return op;
}

s32 PS4_SYSV_ABI sceFiosOpWait(s32 op) {
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][OpWait] op={} (synchronous, already complete)", op);
    return 0;
}

s32 PS4_SYSV_ABI sceFiosOpSyncWait(s32 op) {
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][OpSyncWait] op={} (synchronous, already complete)", op);
    return 0;
}

s64 PS4_SYSV_ABI sceFiosOpGetActualCount(s32 op) {
    const s64 count = QueryOpActualCount(op);
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][OpGetActualCount] op={} count={}", op, count);
    return count;
}

s32 PS4_SYSV_ABI sceFiosOpIsDone(s32 op) {
    // All ops complete synchronously, so every submitted op is already done.
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][OpIsDone] op={} (always done)", op);
    return 1;
}

s32 PS4_SYSV_ABI sceFiosOpGetError(s32 op) {
    // Ops store their outcome in the byte count: negative counts encode the errno.
    const s64 count = QueryOpActualCount(op);
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][OpGetError] op={} error={}", op, count < 0 ? count : 0);
    return count < 0 ? static_cast<s32>(count) : 0;
}

s32 PS4_SYSV_ABI sceFiosOpDelete(s32 op) {
    // Drop the recorded count so fake op handles don't leak across long sessions.
    {
        std::scoped_lock lock{g_op_mutex};
        g_op_actual_counts.erase(op);
    }
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][OpDelete] op={}", op);
    return 0;
}

s32 PS4_SYSV_ABI sceFiosFHClose(const void* op_attr, s32 handle) {
    // Async close: run synchronously and return a completed op handle.
    const s32 result = Kernel::posix_close(handle);
    const s32 op = result >= 0 ? AllocateOpHandle(0) : result;
    LOG_INFO(Lib_SysModule, "[FIOS-HLE][FHClose] handle={} result={} op={} op_attr={}", handle,
             result, op, op_attr);
    return op;
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
    LIB_FUNCTION("TXABsmiiqto", "libSceFios2", 1, "libSceFios2", sceFiosOverlayAdd);
    LIB_FUNCTION("rR8wq7YFRZs", "libSceFios2", 1, "libSceFios2", sceFiosFHPread);
    LIB_FUNCTION("er6TkQFUvp0", "libSceFios2", 1, "libSceFios2", sceFiosFHOpen);
    LIB_FUNCTION("SnoQQWnGK9I", "libSceFios2", 1, "libSceFios2", sceFiosOpWait);
    LIB_FUNCTION("+FRvKknUj1I", "libSceFios2", 1, "libSceFios2", sceFiosOpGetActualCount);
    LIB_FUNCTION("2wvqS7Odb6M", "libSceFios2", 1, "libSceFios2", sceFiosOpSyncWait);
    LIB_FUNCTION("bfgo2Otmqz0", "libSceFios2", 1, "libSceFios2", sceFiosOpIsDone);
    LIB_FUNCTION("X+7rIfY97Ps", "libSceFios2", 1, "libSceFios2", sceFiosOpGetError);
    LIB_FUNCTION("5cyEcilO-J0", "libSceFios2", 1, "libSceFios2", sceFiosOpDelete);
    LIB_FUNCTION("5sYNBNK+W3g", "libSceFios2", 1, "libSceFios2", sceFiosFHClose);
}

} // namespace Libraries::Fios2
