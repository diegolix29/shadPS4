# Crash Bandicoot Turnip View-Dimension Implementation Plan

> Execute with subagent-driven development on the current branch. Do not create
> worktrees, branches, commits, or pushes.

**Goal:** Remove post-Activision striping by correcting the confirmed
Color1D-view/Color2D-backing mismatch without changing 2D tiled backing images.

**Architecture:** First add bounded diagnostic metadata and reproduce on the
device. Then use the captured tuple to write a pure descriptor predicate,
specialize matching guest 1D/2D-tiled resources as host 2D, and make backing,
Vulkan view type, SPIR-V sampled-image dimension, and coordinates agree.

**Constraints:** Use context-mode for large output. Preserve user Turnip
selection. Build/package runtime before Gradle. Install only an APK containing
both managed-runtime assets. Leave game open after final capture.

---

## Task 1: Capture exact incompatible-view tuple

**Files:**
- Modify: `src/video_core/texture_cache/texture_cache.cpp`
- Modify: `src/video_core/texture_cache/image_view.cpp`
- Possibly modify: `src/video_core/texture_cache/image_view.h`

1. Inspect real image/view structs and call sites; list fields available at
   construction and any unavailable usage classification.
2. Add a bounded, uniquely tagged diagnostic record at `FindTexture`, before
   binding provenance and requested-image metadata are lost. Keep the existing
   `ImageView` incompatibility error. Do not change behavior.
3. Build the smallest native target covering `image_view.cpp`.
4. Build/package managed runtime, verify it, then run Gradle test/lint/package.
5. Verify the APK contains both runtime assets and does not bundle Turnip.
6. Install, launch `CUSA07399`, reproduce through the first striped scene,
   capture screenshot/logs, and leave the game running.
7. Write the exact tuple and root-cause decision to
   `.superpowers/sdd/task-13-diagnostic-report.md`.

## Task 2: Add failing resolver tests

**Files:**
- Create or modify: focused texture-cache test chosen after Task 1
- Modify: `tests/CMakeLists.txt`
- Create or modify: resolver header/source beside `image_view`

1. Define a value-only host-2D predicate from the captured tuple.
2. Write a regression test for the exact tuple; confirm red.
3. Add rejection tests for attachment, storage, MSAA, layered, height > 1,
   format mismatch, true 1D backing, and unknown usage; confirm red.

## Task 3: Implement minimal view resolver

**Files:**
- Modify: resolver header/source
- Modify: `src/video_core/texture_cache/image_view.cpp`

1. Implement the smallest predicate set that makes accepted tests green.
2. Carry the result in `ImageResource` and shader specialization identity.
3. Use host 2D for matching texture `ImageInfo` and `ImageViewInfo`.
4. Preserve guest tiling, detiling, synchronization, and existing backing
   identity.
5. Run focused tests repeatedly and existing texture-cache tests.

## Task 4: Add failing shader-agreement tests

**Files:**
- Modify: focused GCN/SPIR-V test
- Modify: relevant shader image-dimension source selected after Task 1

1. Prove the accepted resolver result requests a 2D sampled type.
2. Prove a guest 1D sampled coordinate gains a row-center Y component, while
   integer fetch/offset and derivative Y components remain zero.
3. Cover lod/offset operation forms present in captured shaders.
4. Confirm tests fail before implementation.

## Task 5: Implement shader agreement

**Files:**
- Modify: shader image type/coordinate sources identified by Task 1

1. Thread the resolved dimension through descriptor/shader metadata.
2. Emit a 2D sampled-image type and `(x, 0.5)` sampled coordinates only for
   accepted remaps; use zero Y for integer fetch/offset and derivatives.
3. Reject unsupported operation forms rather than silently changing semantics.
4. Run focused tests repeatedly and all GCN tests.

## Task 6: Full verification and device acceptance

1. Run native focused tests, full Release CTest, and repeated regression tests.
2. Build/package runtime and run runtime verification.
3. Run Android `test`, `lintDebug`, and `assembleDebug`.
4. Verify runtime assets and absence of bundled Turnip in install APK.
5. Install and reproduce past Activision logo.
6. Capture logs/screenshots; verify acceptance criteria from design.
7. Leave app/game running.
8. Record commands, hashes, results, and artifact paths in
   `.superpowers/sdd/task-14-renderer-report.md`.

## Task 7: Subagent reviews

1. Run specification-compliance review against design and captured tuple.
2. Run code-quality review over only task-owned renderer/test diffs.
3. Fix findings with failing tests first.
4. Re-run relevant verification and review until no blocking findings remain.
