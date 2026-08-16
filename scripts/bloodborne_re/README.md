# Bloodborne reverse-engineering tools

`xref_scan.cpp` scans an unencrypted PS4 SELF, reads function boundaries from
`.eh_frame_hdr`, locates the ASCII and UTF-16 strings in `xref_targets.txt`, and
reports RIP-relative references with their containing, previous, and next
function starts. It also follows one level of absolute string pointers and SELF
relocation triples so code that addresses runtime registration-table entries
can be identified. The adjacent boundaries help associate generated formatter
functions with the native condition/update functions beside them.

It can also disassemble the complete exception-table function containing a
known virtual address. This is useful because normal ELF tools do not directly
understand the PS4 SELF container:

```sh
/tmp/bloodborne-xref-scan /path/to/CUSA03173/eboot.bin \
  --disassemble 0x01388680
```

Direct calls, tail jumps, and RIP-relative references to a known virtual
address can be listed without a full disassembler project as well:

```sh
/tmp/bloodborne-xref-scan /path/to/CUSA03173/eboot.bin \
  --xrefs-to 0x0131E5C0
```

An indirect virtual call or jump can be found by its table displacement. This
is useful after recovering a function from a relocated vtable entry:

```sh
/tmp/bloodborne-xref-scan /path/to/CUSA03173/eboot.bin \
  --indirect-displacement 0x90
```

All decoded memory operands using a particular structure displacement can be
listed as well. This is useful for finding every reader and writer of a
recovered class member:

```sh
/tmp/bloodborne-xref-scan /path/to/CUSA03173/eboot.bin \
  --memory-displacement 0x124
```

Search for a non-relative immediate when recovering state values, task types,
event codes, or reason constants:

```sh
/tmp/bloodborne-xref-scan /path/to/CUSA03173/eboot.bin \
  --immediate 0xFF000023
```

SELF-relative relocations identify every data slot that resolves to a known
function or object address:

```sh
/tmp/bloodborne-xref-scan /path/to/CUSA03173/eboot.bin \
  --relocations-to 0x00CC5390
```

To inspect a relocated vtable or adjacent pointer table, dump 32 qwords from
its virtual address. Entries backed by `R_X86_64_RELATIVE` are printed as their
resolved addends and marked `relative`:

```sh
/tmp/bloodborne-xref-scan /path/to/CUSA03173/eboot.bin \
  --dump-qwords 0x052C6700
```

To recover an ASCII label at a known virtual address used by an assertion or
registration site:

```sh
/tmp/bloodborne-xref-scan /path/to/CUSA03173/eboot.bin \
  --dump-string 0x0493A906
```

Build it against the Zydis libraries from an existing shadPS4 build:

```sh
g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic \
  scripts/bloodborne_re/xref_scan.cpp \
  -Iexternals/zydis/include \
  -Iexternals/zydis/dependencies/zycore/include \
  build/externals/zydis/libZydis.a \
  build/externals/zydis/zycore/libZycore.a \
  -pthread -o /tmp/bloodborne-xref-scan
```

Run it against the exact executable being investigated:

```sh
/tmp/bloodborne-xref-scan /path/to/CUSA03173/eboot.bin \
  scripts/bloodborne_re/xref_targets.txt
```

Target lines are exact null-terminated strings by default. End a line with `*`
to search for a non-terminated prefix, which is useful for padded action names.
Only prefix targets report RIP-relative references up to `0x100` bytes from a
relocated pointer slot. This exposes table-base references without labeling
unrelated neighboring registry entries as matches.

The current runtime trace offsets are for CUSA03173 app version 01.09 only.
shadPS4 verifies every patched prologue before installing any trace hooks.
