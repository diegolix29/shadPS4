// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <memory>
#include <span>
#include <thread>
#include <vector>

#include "common/types.h"

namespace VideoCore {

/// Moves guest RAM -> host staging copies off the GPU command processor thread.
///
/// The command processor (the producer) keeps making every decision it made before: which bytes
/// are uploaded, where they land in the staging rings and which Vulkan commands consume them. Only
/// the byte movement is deferred to worker threads.
///
/// Ordering contract (the "captured" watermark):
///  - The GPU observes staging memory only through a queue submission. Every submission of the
///    draw scheduler waits until the jobs enqueued before it have completed.
///  - The guest observes command processor progress only through completion signals (EOP/EOS/
///    RELEASE_MEM labels and IRQs, flip IRQs, GPU idle, submit retirement). Each of those waits
///    for the jobs enqueued before the signalling packet. A well-formed guest only rewrites memory
///    referenced by submitted work after observing such a signal, so the bytes a pending job reads
///    are stable until the job runs.
///  - Writes the emulator itself performs on guest memory (CE RAM dumps, WRITE_DATA, DMA, fence
///    labels, readback write-backs) first wait for pending jobs that read an overlapping range.
///  - Unmapping guest memory drains all pending jobs.
class GuestCopyEngine {
public:
    enum class OpKind : u8 {
        /// Copies from guest memory through the memory manager.
        Guest,
        /// Fills the destination with zeroes.
        Zero,
        /// Copies from a host pointer that stays valid until the job completes.
        Host,
        /// Copies from guest memory through the backing view, which ignores page protection.
        /// Only for bytes whose guest RAM is current although their page denies reads.
        Backing,
    };

    struct Op {
        VAddr source{};
        u8* destination{};
        u64 size{};
        OpKind kind{OpKind::Guest};
        /// Optional GPU identity of the destination: the VkBuffer handle holding destination and
        /// the offset of destination inside it. Zero when unknown. Lets the protected copy
        /// resolver write the destination with GPU commands.
        u64 dst_buffer{};
        u64 dst_offset{};
    };

    /// Remainder operations a protected copy resolver may leave to the CPU.
    static constexpr u32 MaxResolverRemainder = 16;

    /// Packs a Vulkan handle (vk::Buffer) into Op::dst_buffer.
    template <typename Handle>
    [[nodiscard]] static u64 BufferId(Handle handle) noexcept {
        return std::bit_cast<u64>(static_cast<typename Handle::NativeType>(handle));
    }

    /// Unpacks Op::dst_buffer.
    template <typename Handle>
    [[nodiscard]] static Handle ToHandle(u64 id) noexcept {
        return Handle{std::bit_cast<typename Handle::NativeType>(id)};
    }

    struct Stats {
        u64 jobs{};
        u64 ops{};
        u64 bytes{};
        u64 inline_bytes{};
        u64 worker_ns{};
        u64 help_ns{};
        u64 wait_calls{};
        u64 wait_ns{};
        u64 overlap_waits{};
        u64 slot_full_waits{};
        u64 protected_inline_ops{};
        u64 gpu_served_ops{};
        u64 gpu_served_bytes{};
        u64 backing_bytes{};
    };

    /// Returns true when any page of the range currently denies reads.
    using ReadProtectionProbe = bool (*)(const void* context, VAddr addr, u64 size);

    /// Serves a guest copy whose source denies reads without touching the protected pages, for
    /// example by recording GPU commands that write some of the bytes into op.dst_buffer. On
    /// success stores the parts left for the CPU in remainder (Backing operations for bytes
    /// that must bypass the protection), sets remainder_count and gpu_bytes and returns true.
    /// Returns false when the copy has to run inline.
    using ProtectedCopyResolver = bool (*)(void* context, const Op& op,
                                           std::span<Op, MaxResolverRemainder> remainder,
                                           u32& remainder_count, u64& gpu_bytes);

    static GuestCopyEngine& Instance();

    GuestCopyEngine(const GuestCopyEngine&) = delete;
    GuestCopyEngine& operator=(const GuestCopyEngine&) = delete;

    /// Starts the worker pool. A worker count of zero keeps every copy inline.
    void Start(u32 num_workers);

    /// Drains and joins the worker pool.
    void Stop();

    /// Marks the calling thread as the single producer allowed to defer copies.
    void SetProducerThread() noexcept;

    /// Installs the query used to keep read-protected guest ranges off the workers. A worker
    /// touching such a range would run the emulator fault handlers outside the command processor.
    void SetReadProtectionProbe(ReadProtectionProbe probe, const void* context) noexcept {
        read_probe_context = context;
        read_probe.store(probe, std::memory_order_release);
    }

    /// Brackets revoking read access to guest memory (arming read watchers). Begin waits for the
    /// pending copies of the range; copies enqueued until End observe the revocation and run inline.
    void BeginReadProtect(VAddr addr, u64 size);

    /// Installs the resolver tried before running a read-protected copy inline. Called on the
    /// producer thread only.
    void SetProtectedCopyResolver(ProtectedCopyResolver resolver, void* context) noexcept {
        protected_resolver_context = context;
        protected_resolver.store(resolver, std::memory_order_release);
    }

    [[nodiscard]] bool HasProtectedCopyResolver() const noexcept {
        return protected_resolver.load(std::memory_order_acquire) != nullptr;
    }
    void EndReadProtect(VAddr addr, u64 size) noexcept;

    /// Returns true when the calling thread may defer copies through Enqueue.
    [[nodiscard]] bool CanDefer() const noexcept {
        return is_producer_thread && active.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool IsActive() const noexcept {
        return active.load(std::memory_order_relaxed);
    }

    /// Enqueues copy operations and returns the sequence of the last job created. When the
    /// calling thread cannot defer, the operations run inline and the current submitted sequence
    /// is returned.
    u64 Enqueue(std::span<const Op> ops);

    [[nodiscard]] u64 SubmittedSeq() const noexcept {
        return submitted.load(std::memory_order_acquire);
    }

    [[nodiscard]] u64 CompletedSeq() const noexcept {
        return completed.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool HasPending() const noexcept {
        return completed.load(std::memory_order_acquire) !=
               submitted.load(std::memory_order_acquire);
    }

    /// Blocks until every job up to and including seq has completed. Callable from any thread;
    /// the caller executes queued jobs while it waits.
    void WaitCompleted(u64 seq);

    /// Blocks until every job enqueued so far has completed.
    void Drain() {
        if (HasPending()) [[unlikely]] {
            WaitCompleted(SubmittedSeq());
        }
    }

    /// Must be called before the emulator writes or unmaps [addr, addr + size) of guest memory.
    /// Returns once no queued job reads the range.
    void WaitForGuestWrite(VAddr addr, u64 size) {
        if (HasPending()) [[unlikely]] {
            WaitForGuestWriteSlow(addr, size);
        }
    }

    [[nodiscard]] Stats GetStats() const noexcept;

    [[nodiscard]] u32 NumWorkers() const noexcept {
        return static_cast<u32>(workers.size());
    }

    /// Stress-tests ordering, completion waits and overlap waits on host memory. Must run before
    /// the command processor produces work. Returns true when every check passed.
    bool RunSelfTest();

private:
    static constexpr u64 SlotCount = 1024;
    static constexpr u64 SlotMask = SlotCount - 1;
    static constexpr u32 MaxOpsPerJob = 32;
    static constexpr u64 SplitBytes = 256 * 1024;
    static constexpr u64 GranuleBits = 16;
    static constexpr u64 PendingTableSize = 8192;
    /// Pieces whose pending marks are published before a single read protection fence.
    static constexpr u32 MarkBatchSize = 32;
    static_assert((SlotCount & SlotMask) == 0);
    static_assert((PendingTableSize & (PendingTableSize - 1)) == 0);

    struct alignas(64) Slot {
        std::atomic<u64> done_seq{0};
        u64 bytes{};
        u32 num_ops{};
        std::array<Op, MaxOpsPerJob> ops{};
    };

    GuestCopyEngine();
    ~GuestCopyEngine();

    void WorkerLoop(std::stop_token stoken, u32 index);
    bool TryRunOne(bool from_worker);
    void RunJob(u64 seq, bool from_worker);
    void AdvanceCompleted();
    void PublishJob(u32 num_ops, u64 bytes);
    void WaitForSlot();
    void WaitForGuestWriteSlow(VAddr addr, u64 size);
    void EnqueueOp(const Op& op, bool allow_resolve);
    void AppendMarkedPieces(std::span<const Op> pieces, bool allow_resolve);
    void AppendPiece(const Op& piece, bool allow_resolve);
    /// Returns the piece of op that starts at offset, at most SplitBytes long.
    [[nodiscard]] static Op MakePiece(const Op& op, u64 offset) noexcept;
    [[nodiscard]] bool TryResolveProtected(const Op& op);
    void ExecuteInline(std::span<const Op> ops);
    void ExecuteOps(std::span<const Op> ops);
    void MarkPending(const Op& op, bool add) noexcept;
    [[nodiscard]] bool OverlapsPending(VAddr addr, u64 size) const noexcept;
    [[nodiscard]] bool IsReadProtected(VAddr addr, u64 size) const noexcept;
    void AddReadIntent(VAddr addr, u64 size, bool add) noexcept;
    void DrainRange(VAddr addr, u64 size);

    [[nodiscard]] static u64 PendingIndex(u64 granule) noexcept {
        u64 value = granule * 0x9E3779B97F4A7C15ULL;
        value ^= value >> 29;
        return value & (PendingTableSize - 1);
    }

    static thread_local bool is_producer_thread;

    std::unique_ptr<std::array<Slot, SlotCount>> slots;
    std::unique_ptr<std::array<std::atomic<u32>, PendingTableSize>> pending_reads;
    std::unique_ptr<std::array<std::atomic<u32>, PendingTableSize>> read_protect_intents;
    std::atomic<ReadProtectionProbe> read_probe{nullptr};
    const void* read_probe_context{};
    std::atomic<ProtectedCopyResolver> protected_resolver{nullptr};
    void* protected_resolver_context{};

    alignas(64) std::atomic<u64> submitted{0};
    alignas(64) std::atomic<u64> claimed{0};
    alignas(64) std::atomic<u64> completed{0};
    alignas(64) std::atomic<u64> wake_signal{0};
    std::atomic<u32> parked_workers{0};
    /// Workers polling for jobs. One of them picks up the next job without being woken.
    std::atomic<u32> spinning_workers{0};
    std::atomic<u32> completion_waiters{0};
    std::atomic<bool> active{false};

    // Self-test only: a range, packed as (offset << 32 | size) relative to self_test_base, that
    // queued jobs must not read. Zero when unused.
    std::atomic<u64> self_test_forbidden{0};
    VAddr self_test_base{};
    std::atomic<u64> self_test_violations{0};

    // Producer-side staging for the job being built.
    u32 building_ops{};
    u64 building_bytes{};

    /// Counters written only by the producer thread. They are read concurrently, so they stay
    /// atomic, but they are updated with plain loads and stores on a cache line of their own.
    struct alignas(64) ProducerStats {
        std::atomic<u64> jobs{};
        std::atomic<u64> ops{};
        std::atomic<u64> bytes{};
        std::atomic<u64> slot_full_waits{};
        std::atomic<u64> protected_inline_ops{};
        std::atomic<u64> gpu_served_ops{};
        std::atomic<u64> gpu_served_bytes{};
        std::atomic<u64> backing_bytes{};
    };
    ProducerStats producer_stats;

    /// Counters any thread may update.
    struct alignas(64) AtomicStats {
        std::atomic<u64> inline_bytes{};
        std::atomic<u64> worker_ns{};
        std::atomic<u64> help_ns{};
        std::atomic<u64> wait_calls{};
        std::atomic<u64> wait_ns{};
        std::atomic<u64> overlap_waits{};
    };
    AtomicStats stats;

    std::vector<std::jthread> workers;
};

} // namespace VideoCore
