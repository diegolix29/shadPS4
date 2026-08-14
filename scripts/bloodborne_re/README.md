# Bloodborne reverse-engineering tools

`xref_scan.cpp` scans an unencrypted PS4 SELF, reads function boundaries from
`.eh_frame_hdr`, locates the ASCII and UTF-16 strings in `xref_targets.txt`, and
reports RIP-relative references with their containing, previous, and next
function starts. It also follows one level of absolute string pointers and SELF
relocation triples so code that addresses runtime registration-table entries
can be identified. The adjacent boundaries help associate generated formatter
functions with the native condition/update functions beside them.

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
