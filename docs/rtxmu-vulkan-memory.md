# Vulkan RTXMU integration

CER enables RTXMU by default for static, compactable Vulkan BLAS. Dynamic BLAS
and TLAS retain NVRHI's native allocation/update path. DX12 does not enable RTXMU.

## Configuration and repositories

- `CER_WITH_RTXMU=ON` enables the Vulkan integration.
- `CER_VULKAN_BLAS_COMPACTION=ON` lets CER mark static BLAS for compaction.
- Setting either option to `OFF` makes CER use native, update-capable BLAS.
- NVRHI branch: `codex/rtxmu-vram`.
- Nested RTXMU branch: `codex/vulkan-compaction-memory`.

The implementation spans CER, `extern/NVRHI`, and `extern/NVRHI/rtxmu`.
Working-tree changes in the nested repositories must also be preserved; a CER
patch or branch alone does not contain them.

## Memory behavior

Static BLAS use RTXMU pools for both the original and compacted storage.
Compaction preserves the geometry. A compacted result is skipped when its
aligned allocation would not be smaller than the original. Pool allocations
split free ranges, merge adjacent free ranges, reclaim the free tail, and release
fully empty blocks.

RTXMU static builds borrow NVRHI scratch rather than retain a scratch allocation
per BLAS. NVRHI inserts a GPU memory barrier before reusing scratch from the same
command-list recording after a 64 MiB batch. Submitted/in-flight scratch is not
eligible for this reset. An individual larger operation is allowed to exceed the
batch size.

Completed scratch chunks unused for 120 queue submissions are released. Smaller
requests avoid repeatedly selecting oversized chunks, so loading peaks can age
out. Completed scratch chunks can also be reclaimed when the configured memory
limit would otherwise prevent allocation.

Each compaction call processes up to 64 MiB of original BLAS allocation sizes,
with one oversized BLAS allowed. The original and compacted allocations coexist
until the compaction submission completes. Pending build records and compaction
commands retain BLAS handles so that IDs and storage cannot be recycled early.
CER submits this sequence on its graphics queue.

These batch sizes are not total VRAM caps. Queued original BLAS, pool slack,
multiple in-flight frames, dynamic geometry and render resources remain additional
costs. Throttling compaction can leave a backlog after a large scene load.

## Update behavior

CER creates a new handle when rebuilding a compactable BLAS. If previously static
geometry starts receiving vertex, skinning or local-transform updates, it switches
to the native update-capable path. Geometry inputs and GPU transform composition
are preserved. NVRHI rejects rebuilding an already-built RTXMU handle; callers
must replace it, as CER does.

## Inspection and validation

`[VRAM] Scene BLAS` reports live scene AS allocation sizes against their original
sizes, excluding pool slack, scratch and retired resources. `[RTXMU]` periodically
reports the original-size baseline and the current result, transient, compacted,
scratch and update pool buffer capacities. The baseline is not resident VRAM;
pool capacities do not include driver overhead or NVRHI scratch.

Only source/static review and whitespace checks were performed. No C++ build,
shader compilation, game run, GPU validation or VRAM measurement was performed.
Runtime comparison should use the same scene, texture set, resolution and camera,
and distinguish scene-load peaks from steady state after compaction drains.
Include animated geometry, changing visibility, scene transitions and both game
targets when validating the integration.

This change targets acceleration-structure storage and build memory. Texture
formats, mip levels and material sampling are unchanged; it does not establish a
measured reduction in total CER + Community Shaders VRAM.
