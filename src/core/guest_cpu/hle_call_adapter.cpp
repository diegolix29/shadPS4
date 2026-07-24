// SPDX-License-Identifier: MIT

#include "hle_call_adapter.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace Core::GuestCpu {

std::shared_ptr<HleCallAdapter> HleCallRegistry::Register(std::shared_ptr<HleCallAdapter> adapter,
                                                           std::string_view name) {
    std::unique_lock lock{registry_mutex};
    if (adapter == nullptr || next_operation == 0) {
        return {};
    }
    adapter->AssignOperation(next_operation++, name);
    adapters.emplace_back(adapter);
    return adapter;
}

std::shared_ptr<HleCallAdapter> HleCallRegistry::Find(u64 operation) const {
    std::shared_lock lock{registry_mutex};
    if (operation == 0 || operation > adapters.size()) {
        return {};
    }
    return adapters.at(operation - 1);
}

HleVeneerAllocator::~HleVeneerAllocator() {
    for (const auto& allocation : allocations) {
        if (allocation.page != nullptr && munmap(allocation.page, allocation.size) != 0) {
            std::abort();
        }
    }
}

HleVeneerResult HleVeneerAllocator::Allocate(const HleCallAdapter& adapter) {
    std::scoped_lock lock{allocator_mutex};
    if (adapter.Operation() == 0) {
        return HleVeneerFailure{EINVAL};
    }
    if (const auto cached = veneers.find(adapter.Operation()); cached != veneers.end()) {
        return cached->second;
    }
    const auto page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        return HleVeneerFailure{errno == 0 ? EIO : errno};
    }
    const auto size = static_cast<std::size_t>(page_size);
    void* const page = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        return HleVeneerFailure{errno};
    }

    constexpr std::size_t veneer_size = 16;
    auto* const code = static_cast<u8*>(page);
    // mov r10, rcx preserves the fourth SysV argument across syscall's RCX clobber.
    // mov rax, operation; syscall; ret
    constexpr u8 preserve_fourth_argument[]{0x49, 0x89, 0xca};
    constexpr u8 prefix[]{0x48, 0xb8};
    constexpr u8 suffix[]{0x0f, 0x05, 0xc3};
    std::memcpy(code, preserve_fourth_argument, sizeof(preserve_fourth_argument));
    std::memcpy(code + sizeof(preserve_fourth_argument), prefix, sizeof(prefix));
    const auto operation = adapter.Operation();
    const auto operation_offset = sizeof(preserve_fourth_argument) + sizeof(prefix);
    std::memcpy(code + operation_offset, &operation, sizeof(operation));
    std::memcpy(code + operation_offset + sizeof(operation), suffix, sizeof(suffix));
    __builtin___clear_cache(reinterpret_cast<char*>(code), reinterpret_cast<char*>(code + veneer_size));
    if (mprotect(page, size, PROT_READ | PROT_EXEC) != 0) {
        const int error = errno;
        if (munmap(page, size) != 0) {
            std::abort();
        }
        return HleVeneerFailure{error};
    }

    allocations.emplace_back(page, size);
    executable_ranges.push_back({reinterpret_cast<std::uintptr_t>(page), size, true, false});
    const auto address = reinterpret_cast<u64>(page);
    std::fprintf(stderr, "BACHATA_FEX_VENEER address=%#llx operation=%llu name=%.*s\n",
                 static_cast<unsigned long long>(address),
                 static_cast<unsigned long long>(adapter.Operation()),
                 static_cast<int>(adapter.Name().size()), adapter.Name().data());
    veneers.emplace(adapter.Operation(), address);
    return address;
}

std::vector<GuestExecutionRange> HleVeneerAllocator::GetExecutableRanges() const {
    std::scoped_lock lock{allocator_mutex};
    return executable_ranges;
}

std::optional<GuestExecutionRange> HleVeneerAllocator::QueryExecutableRange(
    std::uintptr_t address) const {
    if (address == 0) {
        return std::nullopt;
    }
    std::scoped_lock lock{allocator_mutex};
    for (const auto& range : executable_ranges) {
        if (!range.Executable || range.Begin == 0 || range.Size == 0) {
            continue;
        }
        if (address >= range.Begin && address < range.Begin + range.Size) {
            return range;
        }
    }
    return std::nullopt;
}

} // namespace Core::GuestCpu
