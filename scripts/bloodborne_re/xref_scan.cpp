// SPDX-License-Identifier: GPL-2.0-or-later

#include <Zydis/Zydis.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t SelfMagic = 0x1D3D154F;
constexpr std::uint32_t ElfMagic = 0x464C457F;
constexpr std::uint32_t PtLoad = 0x1;
constexpr std::uint32_t PtDynamic = 0x2;
constexpr std::uint32_t PtSceDynlibData = 0x61000000;
constexpr std::uint32_t PtGnuEhFrame = 0x6474E550;
constexpr std::uint32_t PfExecute = 0x1;
constexpr std::int64_t DtNull = 0;
constexpr std::int64_t DtSceRela = 0x6100002F;
constexpr std::int64_t DtSceRelaSize = 0x61000031;
constexpr std::uint32_t Rx86Relative = 8;

#pragma pack(push, 1)
struct SelfHeader {
  std::uint32_t magic;
  std::uint8_t version;
  std::uint8_t mode;
  std::uint8_t endian;
  std::uint8_t attributes;
  std::uint8_t category;
  std::uint8_t program_type;
  std::uint16_t padding1;
  std::uint16_t header_size;
  std::uint16_t meta_size;
  std::uint32_t file_size;
  std::uint32_t padding2;
  std::uint16_t segment_count;
  std::uint16_t unknown1a;
  std::uint32_t padding3;
};

struct SelfSegment {
  std::uint64_t flags;
  std::uint64_t file_offset;
  std::uint64_t file_size;
  std::uint64_t memory_size;

  bool IsBlocked() const { return (flags & 0x800) != 0; }

  std::uint32_t ProgramHeaderId() const {
    return static_cast<std::uint32_t>((flags >> 20) & 0xFFF);
  }
};

struct ElfHeader {
  std::uint8_t ident[16];
  std::uint16_t type;
  std::uint16_t machine;
  std::uint32_t version;
  std::uint64_t entry;
  std::uint64_t program_header_offset;
  std::uint64_t section_header_offset;
  std::uint32_t flags;
  std::uint16_t header_size;
  std::uint16_t program_header_entry_size;
  std::uint16_t program_header_count;
  std::uint16_t section_header_entry_size;
  std::uint16_t section_header_count;
  std::uint16_t section_name_index;
};

struct ProgramHeader {
  std::uint32_t type;
  std::uint32_t flags;
  std::uint64_t offset;
  std::uint64_t virtual_address;
  std::uint64_t physical_address;
  std::uint64_t file_size;
  std::uint64_t memory_size;
  std::uint64_t alignment;
};

struct DynamicEntry {
  std::int64_t tag;
  std::uint64_t value;
};

struct Relocation {
  std::uint64_t offset;
  std::uint64_t info;
  std::int64_t addend;
};
#pragma pack(pop)

static_assert(sizeof(SelfHeader) == 0x20);
static_assert(sizeof(SelfSegment) == 0x20);
static_assert(sizeof(ElfHeader) == 0x40);
static_assert(sizeof(ProgramHeader) == 0x38);
static_assert(sizeof(DynamicEntry) == 0x10);
static_assert(sizeof(Relocation) == 0x18);

template <typename T>
const T *At(const std::vector<std::uint8_t> &bytes, std::uint64_t offset) {
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
    return nullptr;
  }
  return reinterpret_cast<const T *>(bytes.data() + offset);
}

std::vector<std::uint8_t> ReadFile(const std::string &path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    return {};
  }
  const auto size = input.tellg();
  if (size <= 0) {
    return {};
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  input.seekg(0);
  input.read(reinterpret_cast<char *>(bytes.data()), size);
  return input ? bytes : std::vector<std::uint8_t>{};
}

std::vector<std::string> ReadTargets(const std::string &path) {
  std::ifstream input(path);
  std::vector<std::string> targets;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (!line.empty() && line.front() != '#') {
      targets.push_back(line);
    }
  }
  return targets;
}

std::vector<std::uint8_t> EncodeTarget(const std::string &target, bool wide) {
  const bool prefix = target.ends_with('*');
  const std::string_view text =
      prefix ? std::string_view{target}.substr(0, target.size() - 1) : target;
  std::vector<std::uint8_t> encoded;
  encoded.reserve((text.size() + 1) * (wide ? 2 : 1));
  for (const unsigned char ch : text) {
    encoded.push_back(ch);
    if (wide) {
      encoded.push_back(0);
    }
  }
  if (!prefix) {
    encoded.push_back(0);
    if (wide) {
      encoded.push_back(0);
    }
  }
  return encoded;
}

std::string Hex(std::uint64_t value) {
  std::ostringstream out;
  out << "0x" << std::hex << std::setfill('0') << std::setw(8) << value;
  return out.str();
}

class SelfImage {
public:
  explicit SelfImage(std::vector<std::uint8_t> bytes)
      : bytes_(std::move(bytes)) {}

  bool Parse() {
    header_ = At<SelfHeader>(bytes_, 0);
    if (!header_ || header_->magic != SelfMagic) {
      return false;
    }
    const std::uint64_t elf_offset =
        sizeof(SelfHeader) + header_->segment_count * sizeof(SelfSegment);
    elf_ = At<ElfHeader>(bytes_, elf_offset);
    if (!elf_ || std::memcmp(elf_->ident, &ElfMagic, sizeof(ElfMagic)) != 0 ||
        elf_->program_header_entry_size != sizeof(ProgramHeader)) {
      return false;
    }
    if (const auto *first = At<SelfSegment>(bytes_, sizeof(SelfHeader))) {
      self_segments_.assign(first, first + header_->segment_count);
    } else {
      return false;
    }
    const std::uint64_t program_headers_offset =
        elf_offset + elf_->program_header_offset;
    if (const auto *first = At<ProgramHeader>(bytes_, program_headers_offset)) {
      program_headers_.assign(first, first + elf_->program_header_count);
    } else {
      return false;
    }
    return true;
  }

  const std::vector<std::uint8_t> &Bytes() const { return bytes_; }

  std::optional<std::uint64_t>
  SelfOffsetToVirtualAddress(std::uint64_t offset) const {
    for (const auto &segment : self_segments_) {
      if (!segment.IsBlocked() || offset < segment.file_offset ||
          offset >= segment.file_offset + segment.file_size) {
        continue;
      }
      const auto id = segment.ProgramHeaderId();
      if (id >= program_headers_.size()) {
        return std::nullopt;
      }
      const auto &program = program_headers_[id];
      if (program.type != PtLoad) {
        continue;
      }
      return program.virtual_address + offset - segment.file_offset;
    }
    return std::nullopt;
  }

  std::optional<std::uint64_t>
  VirtualAddressToSelfOffset(std::uint64_t address) const {
    for (const auto &segment : self_segments_) {
      if (!segment.IsBlocked()) {
        continue;
      }
      const auto id = segment.ProgramHeaderId();
      if (id >= program_headers_.size()) {
        continue;
      }
      const auto &program = program_headers_[id];
      if (program.type != PtLoad) {
        continue;
      }
      if (address >= program.virtual_address &&
          address < program.virtual_address + segment.file_size) {
        return segment.file_offset + address - program.virtual_address;
      }
    }
    return std::nullopt;
  }

  bool ContainsVirtualAddress(std::uint64_t address) const {
    return std::ranges::any_of(
        program_headers_, [address](const auto &program) {
          return program.type == PtLoad && address >= program.virtual_address &&
                 address < program.virtual_address + program.memory_size;
        });
  }

  const ProgramHeader *ExecutableProgram() const {
    const auto it = std::ranges::find_if(
        program_headers_, [](const ProgramHeader &program) {
          return program.type == PtLoad && (program.flags & PfExecute) != 0;
        });
    return it == program_headers_.end() ? nullptr : &*it;
  }

  const ProgramHeader *EhFrameProgram() const {
    const auto it = std::ranges::find_if(program_headers_,
                                         [](const ProgramHeader &program) {
                                           return program.type == PtGnuEhFrame;
                                         });
    return it == program_headers_.end() ? nullptr : &*it;
  }

  std::optional<std::pair<std::uint64_t, std::uint64_t>>
  ProgramData(std::uint32_t type) const {
    for (const auto &program : program_headers_) {
      if (program.type != type) {
        continue;
      }
      const auto segment = std::ranges::find_if(
          self_segments_, [this, &program](const SelfSegment &entry) {
            if (!entry.IsBlocked() ||
                entry.ProgramHeaderId() >= program_headers_.size()) {
              return false;
            }
            const auto &owner = program_headers_[entry.ProgramHeaderId()];
            return program.offset >= owner.offset &&
                   program.offset < owner.offset + owner.file_size;
          });
      if (segment != self_segments_.end()) {
        const auto &owner = program_headers_[segment->ProgramHeaderId()];
        const auto relative = program.offset - owner.offset;
        return std::pair{
            segment->file_offset + relative,
            std::min(segment->file_size - relative, program.file_size)};
      }
    }
    return std::nullopt;
  }

private:
  std::vector<std::uint8_t> bytes_;
  const SelfHeader *header_{};
  const ElfHeader *elf_{};
  std::vector<SelfSegment> self_segments_;
  std::vector<ProgramHeader> program_headers_;
};

struct TargetLabel {
  std::string name;
  std::string encoding;
  std::uint64_t self_offset;
  bool allow_near;
};

using TargetMap = std::unordered_map<std::uint64_t, std::vector<TargetLabel>>;

TargetMap FindTargets(const SelfImage &image,
                      const std::vector<std::string> &names) {
  TargetMap targets;
  const auto &bytes = image.Bytes();
  for (const auto &name : names) {
    for (const bool wide : {false, true}) {
      const auto encoded = EncodeTarget(name, wide);
      if (encoded.empty()) {
        continue;
      }
      auto position = bytes.cbegin();
      while ((position = std::search(position, bytes.cend(), encoded.cbegin(),
                                     encoded.cend())) != bytes.cend()) {
        const auto offset =
            static_cast<std::uint64_t>(position - bytes.cbegin());
        if (const auto address = image.SelfOffsetToVirtualAddress(offset)) {
          targets[*address].push_back({.name = name,
                                       .encoding = wide ? "utf16" : "ascii",
                                       .self_offset = offset,
                                       .allow_near = name.ends_with('*')});
        }
        ++position;
      }
    }
  }
  return targets;
}

std::size_t AddPointerTargets(const SelfImage &image, TargetMap &targets) {
  const auto direct_targets = targets;
  const auto &bytes = image.Bytes();
  std::size_t pointer_count = 0;
  for (const auto &[target_address, labels] : direct_targets) {
    std::array<std::uint8_t, sizeof(target_address)> encoded{};
    std::memcpy(encoded.data(), &target_address, encoded.size());

    auto position = bytes.cbegin();
    while ((position = std::search(position, bytes.cend(), encoded.cbegin(),
                                   encoded.cend())) != bytes.cend()) {
      const auto offset = static_cast<std::uint64_t>(position - bytes.cbegin());
      auto address = image.SelfOffsetToVirtualAddress(offset);
      std::string encoding_prefix = "pointer-to-";
      if (!address) {
        const auto *destination = At<std::uint64_t>(bytes, offset + 8);
        const auto *size = At<std::uint64_t>(bytes, offset + 16);
        if (destination && size && *size == sizeof(target_address) &&
            image.ContainsVirtualAddress(*destination)) {
          address = *destination;
          encoding_prefix = "relocated-pointer-to-";
        }
      }
      for (const auto &label : labels) {
        if (!address) {
          continue;
        }
        targets[*address].push_back(
            {.name = label.name,
             .encoding = encoding_prefix + label.encoding,
             .self_offset = offset,
             .allow_near = label.allow_near});
        std::cerr << "pointer=" << Hex(*address)
                  << " target=" << Hex(target_address) << " label=\""
                  << label.name << "\" encoding=" << encoding_prefix
                  << label.encoding << " self_offset=" << Hex(offset) << '\n';
        ++pointer_count;
      }
      ++position;
    }
  }
  return pointer_count;
}

std::vector<std::uint64_t> ReadFunctionStarts(const SelfImage &image) {
  const auto *eh = image.EhFrameProgram();
  if (!eh) {
    return {};
  }
  const auto self_offset =
      image.VirtualAddressToSelfOffset(eh->virtual_address);
  if (!self_offset) {
    return {};
  }
  const auto &bytes = image.Bytes();
  const auto *header = At<std::uint8_t>(bytes, *self_offset);
  const auto *count = At<std::uint32_t>(bytes, *self_offset + 8);
  if (!header || !count || header[0] != 1 || header[1] != 0x1B ||
      header[2] != 0x03 || header[3] != 0x3B) {
    std::cerr << "Unsupported .eh_frame_hdr encoding\n";
    return {};
  }
  std::vector<std::uint64_t> starts;
  starts.reserve(*count);
  for (std::uint32_t index = 0; index < *count; ++index) {
    const auto *relative =
        At<std::int32_t>(bytes, *self_offset + 12 + index * 8);
    if (!relative) {
      return {};
    }
    starts.push_back(eh->virtual_address + *relative);
  }
  if (!std::ranges::is_sorted(starts)) {
    std::cerr << "Unsorted .eh_frame_hdr function table\n";
    return {};
  }
  return starts;
}

int Scan(const SelfImage &image, const TargetMap &targets,
         const std::vector<std::uint64_t> &function_starts) {
  const auto *executable = image.ExecutableProgram();
  if (!executable || function_starts.empty()) {
    return 1;
  }

  ZydisDecoder decoder;
  ZydisFormatter formatter;
  ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
  ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);

  std::size_t match_count = 0;
  for (std::size_t function_index = 0; function_index < function_starts.size();
       ++function_index) {
    const auto function_start = function_starts[function_index];
    const auto function_end =
        function_index + 1 < function_starts.size()
            ? function_starts[function_index + 1]
            : std::min(executable->virtual_address + executable->file_size,
                       function_start + 0x4000);
    if (function_end <= function_start) {
      continue;
    }
    const auto self_offset = image.VirtualAddressToSelfOffset(function_start);
    if (!self_offset || *self_offset >= image.Bytes().size()) {
      continue;
    }
    const auto available = std::min<std::uint64_t>(
        function_end - function_start, image.Bytes().size() - *self_offset);
    std::uint64_t offset = 0;
    while (offset < available) {
      ZydisDecodedInstruction instruction;
      ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
      const auto status = ZydisDecoderDecodeFull(
          &decoder, image.Bytes().data() + *self_offset + offset,
          available - offset, &instruction, operands);
      if (!ZYAN_SUCCESS(status)) {
        ++offset;
        continue;
      }
      const auto instruction_address = function_start + offset;
      for (std::uint8_t operand_index = 0;
           operand_index < instruction.operand_count_visible; ++operand_index) {
        const auto &operand = operands[operand_index];
        if (operand.type != ZYDIS_OPERAND_TYPE_MEMORY ||
            operand.mem.base != ZYDIS_REGISTER_RIP) {
          continue;
        }
        ZyanU64 target_address{};
        if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction, &operand,
                                                   instruction_address,
                                                   &target_address))) {
          continue;
        }
        const std::vector<TargetLabel> *labels = nullptr;
        std::uint64_t label_address = target_address;
        if (const auto target = targets.find(target_address);
            target != targets.end()) {
          labels = &target->second;
        } else {
          std::uint64_t closest_distance = 0x101;
          for (const auto &[candidate_address, candidate_labels] : targets) {
            if (!std::ranges::any_of(candidate_labels, [](const auto &label) {
                  return label.allow_near &&
                         label.encoding.starts_with("relocated-pointer-to-");
                })) {
              continue;
            }
            const auto distance = candidate_address > target_address
                                      ? candidate_address - target_address
                                      : target_address - candidate_address;
            if (distance < closest_distance) {
              closest_distance = distance;
              label_address = candidate_address;
              labels = &candidate_labels;
            }
          }
        }
        if (!labels) {
          continue;
        }
        char formatted[256]{};
        ZydisFormatterFormatInstruction(&formatter, &instruction, operands,
                                        instruction.operand_count_visible,
                                        formatted, sizeof(formatted),
                                        instruction_address, nullptr);
        for (const auto &label : *labels) {
          std::cout << "xref=" << Hex(instruction_address)
                    << " function=" << Hex(function_start)
                    << " previous_function="
                    << Hex(function_index == 0
                               ? function_start
                               : function_starts[function_index - 1])
                    << " next_function="
                    << Hex(function_index + 1 < function_starts.size()
                               ? function_starts[function_index + 1]
                               : function_end)
                    << " target=" << Hex(target_address);
          if (target_address != label_address) {
            const auto delta = static_cast<std::int64_t>(target_address) -
                               static_cast<std::int64_t>(label_address);
            std::cout << " near=" << Hex(label_address) << " delta=" << delta;
          }
          std::cout << " " << formatted << " label=\"" << label.name
                    << "\" encoding=" << label.encoding
                    << " self_offset=" << Hex(label.self_offset) << '\n';
          ++match_count;
        }
      }
      offset += instruction.length;
    }
  }
  std::cerr << "Scanned " << function_starts.size() << " functions; found "
            << match_count << " xrefs\n";
  return 0;
}

std::optional<std::uint64_t> ParseAddress(const char *text) {
  try {
    std::size_t consumed{};
    const auto value = std::stoull(text, &consumed, 0);
    if (consumed == std::strlen(text)) {
      return value;
    }
  } catch (const std::exception &) {
  }
  return std::nullopt;
}

int DisassembleFunction(const SelfImage &image,
                        const std::vector<std::uint64_t> &function_starts,
                        std::uint64_t requested_address) {
  if (function_starts.empty()) {
    return 1;
  }

  const auto following =
      std::ranges::upper_bound(function_starts, requested_address);
  if (following == function_starts.begin()) {
    std::cerr << "No function contains " << Hex(requested_address) << '\n';
    return 1;
  }
  const auto function = std::prev(following);
  const auto function_start = *function;
  const auto function_end =
      following == function_starts.end() ? function_start + 0x4000 : *following;
  const auto self_offset = image.VirtualAddressToSelfOffset(function_start);
  if (!self_offset || *self_offset >= image.Bytes().size()) {
    std::cerr << "Function " << Hex(function_start)
              << " is not backed by SELF data\n";
    return 1;
  }

  const auto available = std::min<std::uint64_t>(
      function_end - function_start, image.Bytes().size() - *self_offset);
  ZydisDecoder decoder;
  ZydisFormatter formatter;
  ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
  ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);

  std::cout << "function=" << Hex(function_start)
            << " end=" << Hex(function_end)
            << " requested=" << Hex(requested_address) << '\n';
  std::uint64_t offset{};
  while (offset < available) {
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    const auto status = ZydisDecoderDecodeFull(
        &decoder, image.Bytes().data() + *self_offset + offset,
        available - offset, &instruction, operands);
    if (!ZYAN_SUCCESS(status)) {
      ++offset;
      continue;
    }
    const auto instruction_address = function_start + offset;
    char formatted[256]{};
    ZydisFormatterFormatInstruction(
        &formatter, &instruction, operands, instruction.operand_count_visible,
        formatted, sizeof(formatted), instruction_address, nullptr);
    std::cout << Hex(instruction_address) << ' ' << formatted << '\n';
    offset += instruction.length;
  }
  return 0;
}

int FindAddressXrefs(const SelfImage &image,
                     const std::vector<std::uint64_t> &function_starts,
                     std::uint64_t target_address) {
  if (function_starts.empty()) {
    return 1;
  }

  ZydisDecoder decoder;
  ZydisFormatter formatter;
  ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
  ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);

  std::size_t match_count{};
  for (std::size_t function_index = 0; function_index < function_starts.size();
       ++function_index) {
    const auto function_start = function_starts[function_index];
    const auto function_end = function_index + 1 < function_starts.size()
                                  ? function_starts[function_index + 1]
                                  : function_start + 0x4000;
    const auto self_offset = image.VirtualAddressToSelfOffset(function_start);
    if (!self_offset || function_end <= function_start ||
        *self_offset >= image.Bytes().size()) {
      continue;
    }
    const auto available = std::min<std::uint64_t>(
        function_end - function_start, image.Bytes().size() - *self_offset);

    std::uint64_t offset{};
    while (offset < available) {
      ZydisDecodedInstruction instruction;
      ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
      const auto status = ZydisDecoderDecodeFull(
          &decoder, image.Bytes().data() + *self_offset + offset,
          available - offset, &instruction, operands);
      if (!ZYAN_SUCCESS(status)) {
        ++offset;
        continue;
      }
      const auto instruction_address = function_start + offset;
      bool matched{};
      for (std::uint8_t operand_index = 0;
           operand_index < instruction.operand_count_visible; ++operand_index) {
        const auto &operand = operands[operand_index];
        const bool direct_control_flow =
            operand_index == 0 &&
            (instruction.mnemonic == ZYDIS_MNEMONIC_CALL ||
             instruction.mnemonic == ZYDIS_MNEMONIC_JMP) &&
            operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE;
        const bool rip_relative_memory =
            operand.type == ZYDIS_OPERAND_TYPE_MEMORY &&
            operand.mem.base == ZYDIS_REGISTER_RIP;
        if (!direct_control_flow && !rip_relative_memory) {
          continue;
        }
        ZyanU64 resolved{};
        if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(
                &instruction, &operand, instruction_address, &resolved)) &&
            resolved == target_address) {
          matched = true;
          break;
        }
      }
      if (matched) {
        char formatted[256]{};
        ZydisFormatterFormatInstruction(&formatter, &instruction, operands,
                                        instruction.operand_count_visible,
                                        formatted, sizeof(formatted),
                                        instruction_address, nullptr);
        std::cout << "xref=" << Hex(instruction_address)
                  << " function=" << Hex(function_start) << ' ' << formatted
                  << '\n';
        ++match_count;
      }
      offset += instruction.length;
    }
  }
  std::cerr << "Scanned " << function_starts.size() << " functions; found "
            << match_count << " xrefs to " << Hex(target_address) << '\n';
  return 0;
}

int FindIndirectControlFlow(const SelfImage &image,
                            const std::vector<std::uint64_t> &function_starts,
                            std::uint64_t requested_displacement) {
  if (function_starts.empty()) {
    return 1;
  }

  ZydisDecoder decoder;
  ZydisFormatter formatter;
  ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
  ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);

  std::size_t match_count{};
  for (std::size_t function_index = 0; function_index < function_starts.size();
       ++function_index) {
    const auto function_start = function_starts[function_index];
    const auto function_end = function_index + 1 < function_starts.size()
                                  ? function_starts[function_index + 1]
                                  : function_start + 0x4000;
    const auto self_offset = image.VirtualAddressToSelfOffset(function_start);
    if (!self_offset || function_end <= function_start ||
        *self_offset >= image.Bytes().size()) {
      continue;
    }
    const auto available = std::min<std::uint64_t>(
        function_end - function_start, image.Bytes().size() - *self_offset);

    std::uint64_t offset{};
    while (offset < available) {
      ZydisDecodedInstruction instruction;
      ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
      const auto status = ZydisDecoderDecodeFull(
          &decoder, image.Bytes().data() + *self_offset + offset,
          available - offset, &instruction, operands);
      if (!ZYAN_SUCCESS(status)) {
        ++offset;
        continue;
      }
      const auto &target = operands[0];
      const bool matched = (instruction.mnemonic == ZYDIS_MNEMONIC_CALL ||
                            instruction.mnemonic == ZYDIS_MNEMONIC_JMP) &&
                           target.type == ZYDIS_OPERAND_TYPE_MEMORY &&
                           target.mem.disp.size != 0 &&
                           static_cast<std::uint64_t>(target.mem.disp.value) ==
                               requested_displacement;
      if (matched) {
        const auto instruction_address = function_start + offset;
        char formatted[256]{};
        ZydisFormatterFormatInstruction(&formatter, &instruction, operands,
                                        instruction.operand_count_visible,
                                        formatted, sizeof(formatted),
                                        instruction_address, nullptr);
        std::cout << "indirect=" << Hex(instruction_address)
                  << " function=" << Hex(function_start) << ' ' << formatted
                  << '\n';
        ++match_count;
      }
      offset += instruction.length;
    }
  }
  std::cerr << "Scanned " << function_starts.size() << " functions; found "
            << match_count << " indirect calls or jumps at displacement "
            << Hex(requested_displacement) << '\n';
  return 0;
}

int FindImmediate(const SelfImage &image,
                  const std::vector<std::uint64_t> &function_starts,
                  std::uint64_t requested_value) {
  if (function_starts.empty()) {
    return 1;
  }

  ZydisDecoder decoder;
  ZydisFormatter formatter;
  ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
  ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);

  std::size_t match_count{};
  for (std::size_t function_index = 0; function_index < function_starts.size();
       ++function_index) {
    const auto function_start = function_starts[function_index];
    const auto function_end = function_index + 1 < function_starts.size()
                                  ? function_starts[function_index + 1]
                                  : function_start + 0x4000;
    const auto self_offset = image.VirtualAddressToSelfOffset(function_start);
    if (!self_offset || function_end <= function_start ||
        *self_offset >= image.Bytes().size()) {
      continue;
    }
    const auto available = std::min<std::uint64_t>(
        function_end - function_start, image.Bytes().size() - *self_offset);

    std::uint64_t offset{};
    while (offset < available) {
      ZydisDecodedInstruction instruction;
      ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
      const auto status = ZydisDecoderDecodeFull(
          &decoder, image.Bytes().data() + *self_offset + offset,
          available - offset, &instruction, operands);
      if (!ZYAN_SUCCESS(status)) {
        ++offset;
        continue;
      }
      const bool matched = std::ranges::any_of(
          std::span{operands, instruction.operand_count_visible},
          [requested_value](const ZydisDecodedOperand &operand) {
            return operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
                   !operand.imm.is_relative &&
                   operand.imm.value.u == requested_value;
          });
      if (matched) {
        const auto instruction_address = function_start + offset;
        char formatted[256]{};
        ZydisFormatterFormatInstruction(&formatter, &instruction, operands,
                                        instruction.operand_count_visible,
                                        formatted, sizeof(formatted),
                                        instruction_address, nullptr);
        std::cout << "immediate=" << Hex(instruction_address)
                  << " function=" << Hex(function_start) << ' ' << formatted
                  << '\n';
        ++match_count;
      }
      offset += instruction.length;
    }
  }
  std::cerr << "Scanned " << function_starts.size() << " functions; found "
            << match_count << " instructions containing immediate "
            << Hex(requested_value) << '\n';
  return 0;
}

int FindMemoryDisplacement(const SelfImage &image,
                           const std::vector<std::uint64_t> &function_starts,
                           std::uint64_t requested_displacement) {
  if (function_starts.empty()) {
    return 1;
  }

  ZydisDecoder decoder;
  ZydisFormatter formatter;
  ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
  ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);

  std::size_t match_count{};
  for (std::size_t function_index = 0; function_index < function_starts.size();
       ++function_index) {
    const auto function_start = function_starts[function_index];
    const auto function_end = function_index + 1 < function_starts.size()
                                  ? function_starts[function_index + 1]
                                  : function_start + 0x4000;
    const auto self_offset = image.VirtualAddressToSelfOffset(function_start);
    if (!self_offset || function_end <= function_start ||
        *self_offset >= image.Bytes().size()) {
      continue;
    }
    const auto available = std::min<std::uint64_t>(
        function_end - function_start, image.Bytes().size() - *self_offset);

    std::uint64_t offset{};
    while (offset < available) {
      ZydisDecodedInstruction instruction;
      ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
      const auto status = ZydisDecoderDecodeFull(
          &decoder, image.Bytes().data() + *self_offset + offset,
          available - offset, &instruction, operands);
      if (!ZYAN_SUCCESS(status)) {
        ++offset;
        continue;
      }
      const bool matched = std::ranges::any_of(
          std::span{operands, instruction.operand_count_visible},
          [requested_displacement](const ZydisDecodedOperand &operand) {
            return operand.type == ZYDIS_OPERAND_TYPE_MEMORY &&
                   operand.mem.disp.size != 0 &&
                   static_cast<std::uint64_t>(operand.mem.disp.value) ==
                       requested_displacement;
          });
      if (matched) {
        const auto instruction_address = function_start + offset;
        char formatted[256]{};
        ZydisFormatterFormatInstruction(&formatter, &instruction, operands,
                                        instruction.operand_count_visible,
                                        formatted, sizeof(formatted),
                                        instruction_address, nullptr);
        std::cout << "memory=" << Hex(instruction_address)
                  << " function=" << Hex(function_start) << ' ' << formatted
                  << '\n';
        ++match_count;
      }
      offset += instruction.length;
    }
  }
  std::cerr << "Scanned " << function_starts.size() << " functions; found "
            << match_count << " memory operands at displacement "
            << Hex(requested_displacement) << '\n';
  return 0;
}

int FindRelativeRelocationsTo(const SelfImage &image,
                              std::uint64_t requested_addend) {
  const auto dynamic = image.ProgramData(PtDynamic);
  const auto dynamic_data = image.ProgramData(PtSceDynlibData);
  if (!dynamic || !dynamic_data) {
    std::cerr << "SELF dynamic relocation data is missing\n";
    return 1;
  }

  std::uint64_t relocation_offset{};
  std::uint64_t relocation_size{};
  for (std::uint64_t offset = 0;
       offset + sizeof(DynamicEntry) <= dynamic->second;
       offset += sizeof(DynamicEntry)) {
    const auto *entry =
        At<DynamicEntry>(image.Bytes(), dynamic->first + offset);
    if (!entry || entry->tag == DtNull) {
      break;
    }
    if (entry->tag == DtSceRela) {
      relocation_offset = entry->value;
    } else if (entry->tag == DtSceRelaSize) {
      relocation_size = entry->value;
    }
  }
  if (relocation_offset > dynamic_data->second ||
      relocation_size > dynamic_data->second - relocation_offset) {
    std::cerr << "SELF relative relocation table is invalid\n";
    return 1;
  }

  std::size_t match_count{};
  for (std::uint64_t offset = 0; offset + sizeof(Relocation) <= relocation_size;
       offset += sizeof(Relocation)) {
    const auto *relocation = At<Relocation>(
        image.Bytes(), dynamic_data->first + relocation_offset + offset);
    if (relocation &&
        static_cast<std::uint32_t>(relocation->info) == Rx86Relative &&
        static_cast<std::uint64_t>(relocation->addend) == requested_addend) {
      std::cout << "relocation=" << Hex(relocation->offset)
                << " addend=" << Hex(relocation->addend) << '\n';
      ++match_count;
    }
  }
  std::cerr << "Found " << match_count << " relative relocations with addend "
            << Hex(requested_addend) << '\n';
  return 0;
}

int DumpQwords(const SelfImage &image, std::uint64_t address) {
  const auto self_offset = image.VirtualAddressToSelfOffset(address);
  if (!self_offset) {
    std::cerr << "Virtual address is not backed by SELF data: " << Hex(address)
              << '\n';
    return 1;
  }
  std::unordered_map<std::uint64_t, std::uint64_t> relative_relocations;
  const auto dynamic = image.ProgramData(PtDynamic);
  const auto dynamic_data = image.ProgramData(PtSceDynlibData);
  if (dynamic && dynamic_data) {
    std::uint64_t relocation_offset{};
    std::uint64_t relocation_size{};
    for (std::uint64_t offset = 0;
         offset + sizeof(DynamicEntry) <= dynamic->second;
         offset += sizeof(DynamicEntry)) {
      const auto *entry =
          At<DynamicEntry>(image.Bytes(), dynamic->first + offset);
      if (!entry || entry->tag == DtNull) {
        break;
      }
      if (entry->tag == DtSceRela) {
        relocation_offset = entry->value;
      } else if (entry->tag == DtSceRelaSize) {
        relocation_size = entry->value;
      }
    }
    if (relocation_offset <= dynamic_data->second &&
        relocation_size <= dynamic_data->second - relocation_offset) {
      for (std::uint64_t offset = 0;
           offset + sizeof(Relocation) <= relocation_size;
           offset += sizeof(Relocation)) {
        const auto *relocation = At<Relocation>(
            image.Bytes(), dynamic_data->first + relocation_offset + offset);
        if (relocation &&
            static_cast<std::uint32_t>(relocation->info) == Rx86Relative) {
          relative_relocations[relocation->offset] = relocation->addend;
        }
      }
    }
  }

  for (std::size_t index = 0; index < 32; ++index) {
    const auto virtual_address = address + index * sizeof(std::uint64_t);
    const auto *value = At<std::uint64_t>(
        image.Bytes(), *self_offset + index * sizeof(std::uint64_t));
    if (!value) {
      break;
    }
    const auto relocated = relative_relocations.find(virtual_address);
    std::cout << Hex(virtual_address) << ' '
              << Hex(relocated == relative_relocations.end()
                         ? *value
                         : relocated->second)
              << (relocated == relative_relocations.end() ? "" : " relative")
              << '\n';
  }
  return 0;
}

int DumpString(const SelfImage &image, std::uint64_t address) {
  const auto self_offset = image.VirtualAddressToSelfOffset(address);
  if (!self_offset) {
    std::cerr << "Virtual address is not backed by SELF data: " << Hex(address)
              << '\n';
    return 1;
  }

  constexpr std::size_t MaxLength = 0x400;
  std::cout << Hex(address) << " \"";
  for (std::size_t index = 0;
       index < MaxLength && *self_offset + index < image.Bytes().size();
       ++index) {
    const auto value = image.Bytes()[*self_offset + index];
    if (value == 0) {
      std::cout << "\"\n";
      return 0;
    }
    if (value >= 0x20 && value <= 0x7e && value != '"' && value != '\\') {
      std::cout << static_cast<char>(value);
    } else {
      std::cout << "\\x" << std::hex << std::setfill('0') << std::setw(2)
                << static_cast<unsigned int>(value);
    }
  }
  std::cout << "\" (unterminated)\n";
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 3 && argc != 4) {
    std::cerr << "usage: bloodborne-xref-scan <eboot.bin> <targets.txt>\n"
                 "       bloodborne-xref-scan <eboot.bin> --disassemble "
                 "<address>\n"
                 "       bloodborne-xref-scan <eboot.bin> --xrefs-to "
                 "<address>\n"
                 "       bloodborne-xref-scan <eboot.bin> "
                 "--indirect-displacement <offset>\n"
                 "       bloodborne-xref-scan <eboot.bin> "
                 "--memory-displacement <offset>\n"
                 "       bloodborne-xref-scan <eboot.bin> "
                 "--relocations-to <address>\n"
                 "       bloodborne-xref-scan <eboot.bin> --immediate "
                 "<value>\n"
                 "       bloodborne-xref-scan <eboot.bin> --dump-qwords "
                 "<address>\n"
                 "       bloodborne-xref-scan <eboot.bin> --dump-string "
                 "<address>\n";
    return 2;
  }
  SelfImage image(ReadFile(argv[1]));
  if (!image.Parse()) {
    std::cerr << "Could not parse Bloodborne SELF image: " << argv[1] << '\n';
    return 1;
  }
  const auto function_starts = ReadFunctionStarts(image);
  if (argc == 4 && std::string_view{argv[2]} == "--disassemble") {
    const auto address = ParseAddress(argv[3]);
    if (!address) {
      std::cerr << "Invalid virtual address: " << argv[3] << '\n';
      return 2;
    }
    return DisassembleFunction(image, function_starts, *address);
  }
  if (argc == 4 && std::string_view{argv[2]} == "--xrefs-to") {
    const auto address = ParseAddress(argv[3]);
    if (!address) {
      std::cerr << "Invalid virtual address: " << argv[3] << '\n';
      return 2;
    }
    return FindAddressXrefs(image, function_starts, *address);
  }
  if (argc == 4 && std::string_view{argv[2]} == "--indirect-displacement") {
    const auto displacement = ParseAddress(argv[3]);
    if (!displacement) {
      std::cerr << "Invalid displacement: " << argv[3] << '\n';
      return 2;
    }
    return FindIndirectControlFlow(image, function_starts, *displacement);
  }
  if (argc == 4 && std::string_view{argv[2]} == "--memory-displacement") {
    const auto displacement = ParseAddress(argv[3]);
    if (!displacement) {
      std::cerr << "Invalid displacement: " << argv[3] << '\n';
      return 2;
    }
    return FindMemoryDisplacement(image, function_starts, *displacement);
  }
  if (argc == 4 && std::string_view{argv[2]} == "--relocations-to") {
    const auto address = ParseAddress(argv[3]);
    if (!address) {
      std::cerr << "Invalid relocation addend: " << argv[3] << '\n';
      return 2;
    }
    return FindRelativeRelocationsTo(image, *address);
  }
  if (argc == 4 && std::string_view{argv[2]} == "--dump-qwords") {
    const auto address = ParseAddress(argv[3]);
    if (!address) {
      std::cerr << "Invalid virtual address: " << argv[3] << '\n';
      return 2;
    }
    return DumpQwords(image, *address);
  }
  if (argc == 4 && std::string_view{argv[2]} == "--dump-string") {
    const auto address = ParseAddress(argv[3]);
    if (!address) {
      std::cerr << "Invalid virtual address: " << argv[3] << '\n';
      return 2;
    }
    return DumpString(image, *address);
  }
  if (argc == 4 && std::string_view{argv[2]} == "--immediate") {
    const auto value = ParseAddress(argv[3]);
    if (!value) {
      std::cerr << "Invalid immediate: " << argv[3] << '\n';
      return 2;
    }
    return FindImmediate(image, function_starts, *value);
  }
  if (argc != 3) {
    std::cerr
        << "Expected --disassemble, --xrefs-to, --indirect-displacement, "
           "--memory-displacement, --immediate, --dump-qwords, or "
           "--dump-string before the value\n";
    return 2;
  }
  const auto target_names = ReadTargets(argv[2]);
  if (target_names.empty()) {
    std::cerr << "No target strings found in: " << argv[2] << '\n';
    return 1;
  }
  auto targets = FindTargets(image, target_names);
  const auto direct_target_count = targets.size();
  const auto pointer_target_count = AddPointerTargets(image, targets);
  std::cerr << "Resolved " << direct_target_count << " string addresses and "
            << pointer_target_count << " pointer slots\n";
  return Scan(image, targets, function_starts);
}
