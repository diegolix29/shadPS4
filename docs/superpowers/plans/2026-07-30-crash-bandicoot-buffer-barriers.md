# Crash Bandicoot GPU Read Barriers Implementation Plan

> **For agentic workers:** Execute this plan test-first. Do not commit. Main agent owns runtime logs, device testing, and final root-cause assessment.

**Goal:** Test whether missing Vulkan barriers for compute-written vertex and index buffers cause CUSA07399's post-logo corruption.

**Architecture:** Port upstream shadPS4 commit `463fe06` narrowly. `BufferCache` appends vertex/index read barriers to the rasterizer's existing `buffer_barriers` collection; `GraphicsPipeline::BindResources` submits them before drawing.

**Tech Stack:** C++20, Vulkan synchronization2, Node.js source-contract tests.

## Global Constraints

- Preserve every pre-existing dirty change, especially current vertex-buffer binding work in `buffer_cache.cpp`.
- No title-ID checks, sleeps, address special cases, broad upstream merge, or unrelated cleanup.
- Subagent edits code and runs focused RED/GREEN tests only; main agent reviews diff and analyzes logs.
- Do not build APK, install, modify device settings, stage, commit, or push.

---

### Task 1: Add compute-to-vertex/index read barriers

**Files:**

- Create: `runtime/tests/bachata-gpu-read-barrier-source.test.mjs`
- Modify: `src/video_core/buffer_cache/buffer_cache.h`
- Modify: `src/video_core/buffer_cache/buffer_cache.cpp`
- Modify: `src/video_core/renderer_vulkan/vk_rasterizer.cpp`

**Interfaces:**

- `BindVertexBuffers(const Vulkan::GraphicsPipeline&, boost::container::small_vector<vk::BufferMemoryBarrier2, 16>&)`
- `BindIndexBuffer(u32, boost::container::small_vector<vk::BufferMemoryBarrier2, 16>&)`
- Both methods append barriers to the caller-owned vector and do not submit barriers themselves.

- [ ] **Step 1: Write failing source-contract test**

Create a Node test that reads the three production files and verifies:

```javascript
assert.match(header, /BindVertexBuffers\([\s\S]*BufferMemoryBarrier2, 16>& barriers/);
assert.match(header, /BindIndexBuffer\([\s\S]*BufferMemoryBarrier2, 16>& barriers/);

assert.match(vertexBody, /IsRegionGpuModified\(range\.base_address, size\)/);
assert.match(vertexBody, /GetBarrier\(vk::AccessFlagBits2::eVertexAttributeRead,[\s\S]*vk::PipelineStageFlagBits2::eVertexAttributeInput\)/);
assert.match(vertexBody, /barriers\.emplace_back\(\*barrier\)/);

assert.match(indexBody, /IsRegionGpuModified\(index_address, index_buffer_size\)/);
assert.match(indexBody, /GetBarrier\(vk::AccessFlagBits2::eIndexRead,[\s\S]*vk::PipelineStageFlagBits2::eIndexInput\)/);
assert.match(indexBody, /barriers\.emplace_back\(\*barrier\)/);

assert.match(rasterizer, /BindVertexBuffers\(\*pipeline, buffer_barriers\)/);
assert.match(rasterizer, /BindIndexBuffer\(index_offset, buffer_barriers\)/);
assert.match(rasterizer, /BindIndexBuffer\(0, buffer_barriers\)/);
```

Also slice both `Draw` functions and assert the bind calls occur before
`pipeline->BindResources(...)`, ensuring the collected barriers are submitted
for the same draw.

- [ ] **Step 2: Verify RED**

Run:

```bash
node --test runtime/tests/bachata-gpu-read-barrier-source.test.mjs
```

Expected: failure because current `BindVertexBuffers` and `BindIndexBuffer`
signatures do not accept the barrier vector.

- [ ] **Step 3: Port minimal upstream behavior**

In `buffer_cache.h`, extend both method signatures with the caller-owned
`boost::container::small_vector<vk::BufferMemoryBarrier2, 16>& barriers`.

In each merged vertex range, after `ObtainBuffer(...)`, append:

```cpp
if (IsRegionGpuModified(range.base_address, size)) {
    if (auto barrier =
            buffer->GetBarrier(vk::AccessFlagBits2::eVertexAttributeRead,
                               vk::PipelineStageFlagBits2::eVertexAttributeInput)) {
        barriers.emplace_back(*barrier);
    }
}
```

Before binding the index buffer, append:

```cpp
if (IsRegionGpuModified(index_address, index_buffer_size)) {
    if (auto barrier = vk_buffer->GetBarrier(vk::AccessFlagBits2::eIndexRead,
                                             vk::PipelineStageFlagBits2::eIndexInput)) {
        barriers.emplace_back(*barrier);
    }
}
```

In both direct and indirect rasterizer draw paths, pass the existing
`buffer_barriers` vector to both bind methods. Preserve current bind-path logic.

- [ ] **Step 4: Verify GREEN and formatting**

Run:

```bash
node --test runtime/tests/bachata-gpu-read-barrier-source.test.mjs
git diff --check -- \
  runtime/tests/bachata-gpu-read-barrier-source.test.mjs \
  src/video_core/buffer_cache/buffer_cache.h \
  src/video_core/buffer_cache/buffer_cache.cpp \
  src/video_core/renderer_vulkan/vk_rasterizer.cpp
```

Expected: test passes and `git diff --check` produces no output.

- [ ] **Step 5: Report**

Report RED failure, GREEN result, exact files changed, and any overlap with
pre-existing dirty changes. Do not commit.

