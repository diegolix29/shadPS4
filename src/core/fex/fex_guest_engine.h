// SPDX-License-Identifier: MIT
#pragma once

#include "../guest_cpu/guest_cpu.h"
#include "../guest_cpu/hle_call_adapter.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <variant>

#ifndef _WIN32
#include <signal.h>
#endif

namespace Core::Fex {

enum class EngineStage {
  Request,
  Config,
  Context,
  Mapping,
  Thread,
  Execute,
  Bridge,
  Invalidate,
  Teardown,
};

struct EngineFailure final {
  EngineStage Stage;
  int Error;
};

template <typename T>
using EngineResult = std::variant<T, EngineFailure>;

class GuestBridge {
public:
  virtual ~GuestBridge() = default;

  virtual EngineResult<bool> Invoke(GuestCpu::HleCallFrame& frame) = 0;
  virtual std::optional<GuestExecutionRange> QueryExecutableRange(std::uintptr_t) {
    return std::nullopt;
  }
};

struct GuestRunResult final {
  bool Gpr {};
  bool Rflags {};
  bool Xmm {};
  bool Bridge {};
  bool Threads {};
  bool Tls {};
  bool Unaligned {};
  bool Invalidation {};
};

#ifndef _WIN32
bool HandleGuestSignal(int signal, siginfo_t* info, void* rawContext) noexcept;
// Queue Orbis guest exception handler for deferred FEX delivery (ARM64 host).
// orbis_sig is the Orbis signal number (e.g. 30 / SIGUSR1). guest_handler is the
// guest VA from Libraries::Kernel::Handlers. Actual run is HandleCallback at HLE
// boundary — mirrors desktop Windows APC ExceptionHandler without host inject.
bool DeliverGuestOrbisSignal(int orbis_sig, siginfo_t* info, void* rawContext,
                             std::uintptr_t guest_handler) noexcept;
#endif

class GuestEngine final {
public:
  class Thread;

  static EngineResult<std::unique_ptr<GuestEngine>> Create(GuestBridge& bridge);

  GuestEngine(const GuestEngine&) = delete;
  GuestEngine& operator=(const GuestEngine&) = delete;
  ~GuestEngine();

  EngineResult<GuestRunResult> RunControlledHarness();
  EngineResult<Thread*> CreateThread(const GuestExecutionRequest& request);
  EngineResult<GuestExecutionState> Run(Thread& thread);
  EngineResult<GuestExecutionState> CallGuest(std::uintptr_t rip,
                                               std::span<const std::uint64_t> arguments);
  EngineResult<bool> Invalidate(Thread& thread, std::uintptr_t begin, std::size_t size);
  EngineResult<bool> DestroyThread(Thread*& thread);
  EngineResult<bool> Shutdown();
  std::uintptr_t ReturnAddress() const;
  GuestExecutionRange ReturnRange() const;
  GuestExecutionRange CallbackReturnRange() const;

private:
  class Impl;

  explicit GuestEngine(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> ImplState;
};

} // namespace Core::Fex
