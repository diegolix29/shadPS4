# Crash Bandicoot Turnip View-Dimension Compatibility Design

## Problem

Crash Bandicoot now passes the Activision logo and remains alive after the
guest-backed mspace fix, but the first later scene contains severe horizontal
striping on the Android Turnip Vulkan path. The touch overlay remains correct,
which localizes the failure below guest presentation and above Android surface
composition.

The matching runtime log reports:

```text
ImageView: image view type Color1D is incompatible with image type Color2D
```

The first events occur while thin, height-one resources use `ARRAY_MODE=2`.
That array mode is a 2D tiled layout, so changing every height-one backing image
to Vulkan 1D is unsafe. This repository already reverted that global approach
after regressions.

## Goals

- Capture the complete resource tuple at the first incompatible view event.
- Fix only the confirmed sampled height-one view/shader mismatch.
- Keep the backing image and its 2D tile interpretation unchanged.
- Preserve valid render-target, storage, MSAA, layered, and ordinary 1D paths.
- Run without validation errors or visible striping on the user-selected
  Turnip driver.

## Non-goals

- No game-title or CUSA-specific workaround.
- No global `Nx1` 2D-to-1D image downgrade.
- No replacement of the user's Turnip driver.
- No broad texture-cache redesign.
- No claim of general renderer compatibility from one title.

## Evidence Gate

Before changing rendering behavior, enrich the existing incompatibility log.
Emit one diagnostic record per unique mismatch tuple, bounded to prevent log
flooding. The record must include all data available at view construction:

- guest address and mapped size;
- image width, height, depth, pitch, bits per pixel, mip and layer ranges;
- tile mode, array mode, and micro-tile mode;
- guest-requested view type and format;
- backing image type, Vulkan image type, format, samples, and usage flags;
- view usage/range and, when available, shader stage, pipeline hash, and image
  operation class.

One Android reproduction must show the exact tuple that begins after the
Activision logo. Behavior remains unchanged during this capture.

## Selected Architecture

### Descriptor-local host representation

Device evidence confirms that pipeline compilation happens before the texture
cache discovers whether the sharp aliases an existing render target. A
view-local runtime decision alone therefore cannot keep the already-compiled
SPIR-V image dimension synchronized.

Add a pure compatibility predicate at shader resource tracking. It may mark a
guest 1D resource as hosted in 2D only when captured metadata proves all of
these predicates:

1. guest type is 1D, resource height/depth are one, and tile mode is the
   captured 2D layout `Thin2DThin` / `Array2DTiledThin1`;
2. access is sampled/read-only;
3. resource is single-sample, single-mip, and non-layered;
4. resource is not depth, atomic, storage, array, R128, or MSAA;
5. format and subresource range remain compatible.

Unknown or ambiguous cases reject the host-2D mark. The mark becomes part of
shader specialization identity and is consumed by both shader compilation and
texture-cache descriptors.

For the observed render-target alias, the existing Vulkan 2D backing remains
unchanged. If a matching sharp has no prior backing, create its host image as
2D so the compiled SPIR-V, bound view, and backing cannot disagree. This is a
narrow 1D-sharp/2D-layout promotion, not the rejected global height-one
2D-to-1D downgrade. Guest tile interpretation and detiling remain unchanged.

### Shader agreement

A remapped Vulkan 2D sampled view requires a matching SPIR-V sampled-image
dimension. For that resolved case only, compile the image as 2D and adapt a 1D
guest sampled coordinate to `(x, 0.5)` so the single host row is addressed at
its texel center. Preserve lod, gradients, offsets, gather behavior,
and integer/normalized coordinate semantics. If an operation cannot be adapted
without changing semantics, the resolver must reject that path.

Detiling, synchronization, and existing render-target identity stay unchanged.

### Rejected alternatives

- **Global height-one 1D backing:** violates 2D tiling assumptions and regresses
  attachments/MSAA.
- **Aliased 1D image plus copies:** Vulkan-clean but adds copy synchronization,
  ownership, and cache-coherency complexity. Keep as fallback only if view-local
  shader agreement cannot be represented safely.
- **Ignore the incompatibility:** leaves undefined Vulkan behavior and does not
  meet visual acceptance.

## Test Design

Use test-driven development for behavior changes:

1. resolver accepts the exact captured sampled height-one tuple;
2. resolver rejects height greater than one, true 1D backing, attachments,
   storage, MSAA, layered ranges, incompatible formats, and unknown usage;
3. shader tests prove only accepted remaps use a 2D sampled type, row-center
   sampled Y, and zero integer/derivative Y;
4. existing GCN and texture-cache tests remain unchanged and pass;
5. repeated test runs expose lifetime or dispatcher teardown regressions.

The diagnostic-only patch is verified by compilation and log inspection before
the resolver tests are written.

## Android Acceptance

Build runtime assets before Gradle, verify both managed-runtime assets exist in
the F-Droid debug APK, then install that APK without bundling a replacement
Turnip driver. Launch `CUSA07399`, capture logs and screenshots through the
first post-Activision scene, and leave the game running afterward.

Acceptance requires:

- no exit `133`, `SIGTRAP`, or mspace `ENOSYS`;
- no incompatible 1D/2D view event on the fixed path;
- no new Vulkan validation errors;
- striping absent in a screenshot from the previously corrupt scene;
- touch overlay and ordinary rendering remain intact.

## Rollback

The resolver is isolated and predicate-gated. If device validation regresses,
disable the remap without changing backing-image allocation or the diagnostic
record. No cache format or persistent user data changes are involved.

## Self-review

- Root cause remains a hypothesis until the diagnostic tuple is captured.
- The design rejects known-dangerous global image-type conversion.
- Shader and Vulkan view dimensions are changed together.
- Ambiguous or writable paths fail closed.
- No title-specific condition appears in the renderer.
