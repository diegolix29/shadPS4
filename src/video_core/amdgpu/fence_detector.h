// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <vector>

#include "common/types.h"
#include "video_core/amdgpu/pm4_cmds.h"

namespace AmdGpu {

class FenceDetector {
public:
    explicit FenceDetector(std::span<const u32> cmd) {
        DetectFences(cmd);
    }

    bool IsFence(const PM4Header* header) const {
        return std::ranges::contains(fences, header, &LabelWrite::packet);
    }

private:
    static std::span<const u32> NextPacket(std::span<const u32> cmd, size_t offset) noexcept {
        return offset < cmd.size() ? cmd.subspan(offset) : std::span<const u32>{};
    }

void DetectFences(std::span<const u32> cmd) {
        while (!cmd.empty()) {
            const auto* header = reinterpret_cast<const PM4Header*>(cmd.data());

            switch (header->type) {
            case 0:
                UNREACHABLE_MSG("Unimplemented PM4 type 0, base reg: {}, size: {}",
                                header->type0.base.Value(), header->type0.NumWords());
                break;
            case 2:
                cmd = NextPacket(cmd, 1);
                continue;
            case 3:
                const PM4ItOpcode opcode = header->type3.opcode;
                switch (opcode) {
                case PM4ItOpcode::EventWriteEos: {
                    const auto* event_eos = reinterpret_cast<const PM4CmdEventWriteEos*>(header);
                    if (event_eos->command == PM4CmdEventWriteEos::Command::SignalFence) {
                        fences.emplace_back(header, event_eos->Address<VAddr>(),
                                            event_eos->DataDWord());
                    }
                    break;
                }
                case PM4ItOpcode::EventWriteEop: {
                    const auto* e = reinterpret_cast<const PM4CmdEventWriteEop*>(header);
                    if (e->int_sel != InterruptSelect::None) {
                        fences.emplace_back(header);
                    }
                    if (e->data_sel == DataSelect::Data32Low) {
                        fences.emplace_back(header, e->Address<VAddr>(), e->DataDWord());
                    } else if (e->data_sel == DataSelect::Data64) {
                        fences.emplace_back(header, e->Address<VAddr>(), e->DataQWord());
                    }
                    break;
                }
                case PM4ItOpcode::ReleaseMem: {
                    const auto* e = reinterpret_cast<const PM4CmdReleaseMem*>(header);
                    if (e->data_sel == DataSelect::Data32Low) {
                        fences.emplace_back(header, e->Address<VAddr>(), e->DataDWord());
                    } else if (e->data_sel == DataSelect::Data64) {
                        fences.emplace_back(header, e->Address<VAddr>(), e->DataQWord());
                    }
                    break;
                }
                case PM4ItOpcode::WriteData: {
                    const auto* e = reinterpret_cast<const PM4CmdWriteData*>(header);
                    ASSERT(e->dst_sel.Value() == 2 || e->dst_sel.Value() == 5);
                    const u32 size = (header->type3.count.Value() - 2) * 4;
                    if (size <= sizeof(u64) && e->wr_confirm) {
                        u64 value{};
                        std::memcpy(&value, e->data, size);
                        fences.emplace_back(header, e->Address<VAddr>(), value);
                    }
                    break;
                }
                case PM4ItOpcode::WaitRegMem: {
                    const auto* wait_reg_mem = reinterpret_cast<const PM4CmdWaitRegMem*>(header);
                    if (wait_reg_mem->mem_space == PM4CmdWaitRegMem::MemSpace::Register) {
                        break;
                    }
                    const VAddr wait_addr = wait_reg_mem->Address<VAddr>();
                    using Function = PM4CmdWaitRegMem::Function;
                    const u32 mask = wait_reg_mem->mask;
                    const u32 ref = wait_reg_mem->ref;
                    std::erase_if(fences, [&](const LabelWrite& write) {
                        if (wait_addr != write.label) {
                            return false;
                        }
                        const u32 value = static_cast<u32>(write.value);
                        switch (wait_reg_mem->function.Value()) {
                        case Function::LessThan:
                            return (value & mask) < ref;
                        case Function::LessThanEqual:
                            return (value & mask) <= ref;
                        case Function::Equal:
                            return (value & mask) == ref;
                        case Function::NotEqual:
                            return (value & mask) != ref;
                        case Function::GreaterThanEqual:
                            return (value & mask) >= ref;
                        case Function::GreaterThan:
                            return (value & mask) > ref;
                        default:
                            UNREACHABLE();
                        }
                    });
                    break;
                }
                default:
                    break;
                }
                cmd = NextPacket(cmd, header->type3.NumWords() + 1);
                break;
            }
        }
    }

private:
    struct LabelWrite {
        const PM4Header* packet;
        VAddr label;
        u64 value;
    };
    std::vector<LabelWrite> fences;
};

} // namespace AmdGpu
