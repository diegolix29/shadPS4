// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <unordered_map>
#include <vector>

#include "common/config.h"
#include "common/types.h"
#include "video_core/amdgpu/pm4_cmds.h"

namespace AmdGpu {

class FenceDetector {
public:
    explicit FenceDetector(std::span<const u32> cmd) {
        DetectFences(cmd);
    }

    // Check if a packet is a known fence
    bool IsFence(const PM4Header* header) const {
        return fences.contains(reinterpret_cast<VAddr>(header));
    }

private:
    static std::span<const u32> NextPacket(std::span<const u32> cmd, size_t offset) {
        return (offset > cmd.size()) ? std::span<const u32>{} : cmd.subspan(offset);
    }

    void DetectFences(std::span<const u32> cmd) {
        while (!cmd.empty()) {
            const auto* header = reinterpret_cast<const PM4Header*>(cmd.data());
            const u32 type = header->type;

            switch (type) {
            default:
            case 0:
                LOG_ERROR(Lib_GnmDriver, "Unsupported PM4 type 0");
                cmd = NextPacket(cmd, header->type0.NumWords() + 1);
                continue;

            case 2:
                cmd = NextPacket(cmd, 1);
                break;

            case 3:
                const PM4ItOpcode opcode = header->type3.opcode;
                switch (opcode) {
                case PM4ItOpcode::EventWriteEos: {
                    const auto* event_eos = reinterpret_cast<const PM4CmdEventWriteEos*>(header);
                    if (event_eos->command == PM4CmdEventWriteEos::Command::SignalFence) {
                        fences[reinterpret_cast<VAddr>(header)] = event_eos->DataDWord();
                    }
                    break;
                }

                case PM4ItOpcode::EventWriteEop: {
                    const auto* event_eop = reinterpret_cast<const PM4CmdEventWriteEop*>(header);
                    if (event_eop->int_sel != InterruptSelect::None) {
                        fences[reinterpret_cast<VAddr>(header)] = 0;
                    }
                    if (event_eop->data_sel == DataSelect::Data32Low) {
                        fences[reinterpret_cast<VAddr>(header)] = event_eop->DataDWord();
                    } else if (event_eop->data_sel == DataSelect::Data64) {
                        fences[reinterpret_cast<VAddr>(header)] = event_eop->DataQWord();
                    }
                    break;
                }

                case PM4ItOpcode::ReleaseMem: {
                    const auto* release_mem = reinterpret_cast<const PM4CmdReleaseMem*>(header);
                    if (release_mem->data_sel == DataSelect::Data32Low) {
                        fences[reinterpret_cast<VAddr>(header)] = release_mem->DataDWord();
                    } else if (release_mem->data_sel == DataSelect::Data64) {
                        fences[reinterpret_cast<VAddr>(header)] = release_mem->DataQWord();
                    }
                    break;
                }

                case PM4ItOpcode::WriteData: {
                    const auto* write_data = reinterpret_cast<const PM4CmdWriteData*>(header);
                    const u32 data_size = (header->type3.count.Value() - 2) * 4;
                    if (data_size <= sizeof(u64) && write_data->wr_confirm) {
                        u64 value = 0;
                        std::memcpy(&value, write_data->data, data_size);
                        fences[reinterpret_cast<VAddr>(header)] = value;
                    }
                    break;
                }

                case PM4ItOpcode::WaitRegMem: {
                    const auto* wait_reg_mem = reinterpret_cast<const PM4CmdWaitRegMem*>(header);
                    if (wait_reg_mem->mem_space == PM4CmdWaitRegMem::MemSpace::Register) {
                        break;
                    }

                    const VAddr wait_addr = wait_reg_mem->Address<VAddr>();
                    const u32 mask = wait_reg_mem->mask;
                    const u32 ref = wait_reg_mem->ref;

                    auto it = fences.find(wait_addr);
                    if (it != fences.end()) {
                        const u32 value = static_cast<u32>(it->second);
                        bool remove = false;

                        switch (wait_reg_mem->function.Value()) {
                        case PM4CmdWaitRegMem::Function::LessThan:
                            remove = (value & mask) < ref;
                            break;
                        case PM4CmdWaitRegMem::Function::LessThanEqual:
                            remove = (value & mask) <= ref;
                            break;
                        case PM4CmdWaitRegMem::Function::Equal:
                            remove = (value & mask) == ref;
                            break;
                        case PM4CmdWaitRegMem::Function::NotEqual:
                            remove = (value & mask) != ref;
                            break;
                        case PM4CmdWaitRegMem::Function::GreaterThanEqual:
                            remove = (value & mask) >= ref;
                            break;
                        case PM4CmdWaitRegMem::Function::GreaterThan:
                            remove = (value & mask) > ref;
                            break;
                        default:
                            UNREACHABLE();
                        }

                        if (remove)
                            fences.erase(it);
                    }
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
    // Address -> fence value (u32/u64)
    std::unordered_map<VAddr, u64> fences;
};

} // namespace AmdGpu
