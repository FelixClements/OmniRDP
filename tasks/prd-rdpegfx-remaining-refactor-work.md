# PRD: RDPEGFX Remaining Refactor Work

## Introduction

This PRD captures the remaining work required before `RDPEGFX_Implementation_Plan.md` and `VIEWER_SERVER_REFACTOR_PLAN.md` can be considered fully implemented. The prior RDPEGFX Ralph loop completed the framebuffer-backed viewer GFX baseline, uncompressed dirty updates, ACK pacing, no-ACK suspension, and backend GFX decode-only gate. This new loop removes transitional backend-PDU replay from production viewer output, finishes moving viewer-local RDPEGFX protocol state out of `viewer_server.c`, hardens caps/ACK/reset/resize behavior, and records final validation evidence.

This PRD is intended for a Ralph-style loop. Each user story is scoped to one focused implementation iteration and must be committed separately after validation.

## Goals

- Remove backend RDPEGFX PDU replay as an authoritative viewer-output path.
- Finish moving viewer-local RDPEGFX protocol/session/ACK/surface state out of `viewer_server.c` and into `viewer_gfx_pipeline` or narrow internal modules.
- Keep `viewer_server.c` as a high-level coordinator only.
- Add strict RDPEGFX capability selection so OmniRDP confirms only supported, tested capabilities.
- Make frame/ACK handling safe across reset, resize, reconnect, byte backpressure, stale ACKs, and 32-bit frame ID wrap.
- Complete resize/reset and pointer/cursor late-join correctness.
- Add final audit/test evidence proving refactor boundaries are enforced.

## Global constraints and non-goals

- No moving backend replay into another production module.
- No test-only/deprecated backend replay path linked into production binaries.
- No production backend replay symbols such as `ViewerGfxCompleteFrame`, `ViewerGfxFrameBuffer`, or `viewer_gfx_replay_frame`.
- No backend PDU replay to viewers and no backend caps/frame ID/surface mirroring to viewers.
- No RFX/RemoteFX, ClearCodec, H.264, AVC, AVC444, AVC420, or progressive codec implementation.
- No production-default enablement of viewer GFX or backend GFX.
- No removal of the classic viewer path.
- No generated/build artifacts may be committed.

## Common validation for every implementation story

- `clang-format` is run on modified C/H files.
- `git diff --check` passes.
- Debug build passes, or missing local dependency blocker is documented.
- `ctest`/tests pass, or unavailable tests are documented with the reason.
- No generated/build artifacts are committed.

## User Stories

### US-001: Quarantine backend RDPEGFX callbacks

**Description:** As a maintainer, I want backend RDPEGFX callbacks prevented from publishing viewer GFX PDUs so that backend GFX remains decode-only and canonical framebuffer output remains authoritative.

**Acceptance Criteria:**

- Backend RDPEGFX callbacks must not call any `viewer_server_publish_gfx_*` function.
- Backend RDPEGFX callbacks may only chain GDI decode and update canonical framebuffer, layout, refresh, or related backend state.
- Backend RDPEGFX frame IDs, surfaces, caps, and replay events are not used for viewer output.
- Tests or static assertions prove backend GFX PDU publishing to viewers is disabled or unreachable.
- Existing viewer GFX baseline and dirty-update tests pass.
- Common validation passes.

### US-002: Remove backend GFX replay production code

**Description:** As a maintainer, I want backend GFX replay/ring implementation removed from production code so that late join uses framebuffer baselines instead of captured backend PDUs.

**Acceptance Criteria:**

- Backend replay implementation is removed from production code, not merely deprecated, test-only, or unreachable.
- No production symbol named `ViewerGfxCompleteFrame`, `ViewerGfxFrameBuffer`, or `viewer_gfx_replay_frame` remains.
- Late-joining GFX viewers use canonical framebuffer baseline and dirty-update paths only.
- Backend replay is not moved into another production module and no replay path is linked into production binaries.
- Common validation passes.

### US-003: Move FrameAcknowledge handling into pipeline

**Description:** As a maintainer, I want viewer RDPEGFX `FrameAcknowledge` handling owned by `viewer_gfx_pipeline` so that `viewer_server.c` only forwards the event and reacts to high-level results.

**Acceptance Criteria:**

- `viewer_server.c` no longer implements RDPEGFX `FrameAcknowledge` protocol logic directly.
- `viewer_gfx_pipeline` owns last ACK frame ID, dirty ACK state, stale ACK rejection, and ACK result classification.
- `viewer_server.c` receives a narrow result/action from pipeline when coordinator action is needed.
- Existing dirty ACK and no-ACK suspension behavior remains unchanged.
- Tests cover matching ACK, stale ACK, unknown ACK, and reset-boundary ACK rejection.
- Common validation passes.

### US-004: Move late-join GFX state/actions into pipeline

**Description:** As a maintainer, I want remaining viewer-local GFX late-join activation state and actions owned by `viewer_gfx_pipeline` so `viewer_server.c` coordinates only high-level outcomes.

**Acceptance Criteria:**

- Pipeline owns remaining late-join GFX activation/release state and exposes narrow coordinator actions.
- Late-join baseline source remains the canonical framebuffer, not backend replay.
- `viewer_server.c` no longer mutates late-join GFX protocol state directly.
- Existing late-join activation and fallback behavior remains unchanged.
- Tests cover late-join ACK release/action behavior and fallback boundaries.
- Common validation passes.

### US-005: Move viewer-local RDPEGFX surface/reset state into pipeline

**Description:** As a maintainer, I want viewer-local RDPEGFX reset/surface/map/preamble state owned by `viewer_gfx_pipeline` so that `viewer_server.c` does not inspect or mutate protocol surface state.

**Acceptance Criteria:**

- Pipeline reset/surface state is viewer-local and derived from canonical framebuffer dimensions/snapshots.
- Pipeline owns `ResetGraphics`/`CreateSurface`/`MapSurfaceToOutput` preamble state and monitor/surface ownership.
- `viewer_server.c` calls high-level APIs such as ensure GFX surface ready or send baseline/dirty update.
- Backend surface maps, backend frame IDs, backend caps, and backend replay state are not moved into pipeline or used for viewer output.
- Full-frame baseline and dirty update tests pass.
- Common validation passes.

### US-006: Shrink `viewer_server.h` public surface

**Description:** As a maintainer, I want `viewer_server.h` to expose only public lifecycle/config APIs so internals are private to implementation or narrow internal headers.

**Acceptance Criteria:**

- `viewer_server.h` uses opaque/internalization where practical, not just for RDPEGFX structs.
- `viewer_server.h` no longer exposes backend replay structs, RDPEGFX frame-ring structs, or low-level RDPEGFX event internals.
- `viewer_server.h` does not include `<freerdp/server/rdpgfx.h>` unless a documented public API requires it.
- Public APIs remain source-compatible where required by backend/tray/service code.
- Common validation passes.

### US-007: Extract classic queue data structures

**Description:** As a maintainer, I want classic queue storage and mechanical helpers moved out of `viewer_server.c` so later policy extraction can be small and safe.

**Acceptance Criteria:**

- Classic queue node/list storage and push/pop/free helper mechanics live in `viewer_publisher` or a dedicated `viewer_classic_queue` module.
- Backlog/coalescing/stale policy decisions remain behaviorally unchanged in this story.
- `viewer_server.c` coordinates queue operations through narrow APIs.
- No RDPEGFX behavior changes are introduced.
- Common validation passes.

### US-008: Move classic backlog policy out of viewer server

**Description:** As a maintainer, I want classic queue backlog/coalescing/stale policy owned by publisher or classic queue module so `viewer_server.c` does not contain policy algorithms.

**Acceptance Criteria:**

- Queue-depth, byte-count, stale-drop, and coalescing policy decisions are in `viewer_publisher` or a dedicated `viewer_classic_queue` module.
- `viewer_server.c` only coordinates classic enqueue/pump operations through narrow APIs.
- Existing classic latest-state policy and queue metrics tests pass.
- Classic first viewer, late viewer, reconnect, and resize behavior remains unchanged.
- No RDPEGFX behavior changes are introduced.
- Common validation passes.

### US-009: Move classic FreeRDP sends into transport module

**Description:** As a maintainer, I want classic `BitmapUpdate` and `SurfaceBits` FreeRDP send calls moved out of `viewer_server.c` so that `viewer_server.c` remains a coordinator rather than a transport implementation.

**Acceptance Criteria:**

- Direct `BitmapUpdate` and `SurfaceBits` FreeRDP update calls are not in `viewer_server.c`.
- A narrow module such as `viewer_classic_transport.c/.h` owns classic FreeRDP send wrappers.
- Locking and peer lifetime behavior are equivalent to the current implementation.
- No protocol behavior changes, compression changes, or RDPEGFX changes are introduced.
- Common validation passes.

### US-010: Implement strict RDPEGFX capabilities whitelist

**Description:** As a maintainer, I want OmniRDP to confirm only known-good RDPEGFX capabilities so it never advertises support for codecs or feature flags it cannot encode.

**Acceptance Criteria:**

- Caps selection uses a whitelist of supported versions/flags/codecs compatible with current uncompressed-only output.
- Unsupported AVC/H.264/progressive/RFX/ClearCodec capability combinations are rejected or downgraded.
- One viewer's unsupported caps do not poison or restrict unrelated viewers.
- Unit tests cover supported caps, unsupported version, unsupported flags, unsupported codec expectations, and multi-viewer isolation.
- `CapsConfirm` only sends known-good uncompressed-compatible caps.
- No codec implementation is added and backend caps are not mirrored to viewers.
- Common validation passes.

### US-011: Add frame epoch and backpressure-safe ACK handling

**Description:** As a maintainer, I want frame IDs paired with epochs and bounded byte-based pacing so stale ACKs and slow viewers cannot corrupt dirty pacing across reset, resize, reconnect, or 32-bit frame ID wrap.

**Acceptance Criteria:**

- Per-viewer GFX epoch is incremented on baseline reset, resize, reconnect, and session reset.
- Dirty in-flight map stores frame ID, framebuffer generation, epoch, timestamp, and queued/sent byte accounting needed for backpressure.
- ACK handling rejects frame IDs from old epochs.
- Frame ID wrap from `UINT32_MAX` to a valid next frame ID is explicit and tested.
- Byte-based GFX backpressure prevents unbounded in-flight dirty payload bytes per viewer.
- Tests cover stale ACKs, wrap, reset/resize/reconnect epochs, byte-limit denial, and byte accounting cleanup on ACK/reset.
- Common validation passes.

### US-012: Harden resize/reset GFX sequencing

**Description:** As a maintainer, I want resize/reset to be treated as a strict GFX epoch boundary so that no stale surface commands are sent after dimensions change.

**Acceptance Criteria:**

- Framebuffer resize increments or triggers a GFX epoch boundary for each GFX viewer.
- Dirty maps, in-flight frames, byte accounting, ACK state, and old snapshots are cleared or invalidated on resize.
- Each active GFX viewer receives a fresh `ResetGraphics`/`CreateSurface`/`MapSurfaceToOutput`/full-frame baseline after resize before dirty updates resume.
- No dirty update with old dimensions is sent after resize.
- Stale ACKs from old dimensions/epoch are ignored.
- Tests cover resize during idle, resize with in-flight dirty frames, and resize during late join.
- Common validation passes.

### US-013: Add pointer and cursor late-join baseline

**Description:** As a user, I want a late-joining GFX viewer to receive the current pointer shape, visibility, and position so the remote desktop cursor state is correct immediately after join.

**Acceptance Criteria:**

- The source of truth is OmniRDP's backend pointer snapshot/cache state: current shape, position, visibility, and generation.
- GFX late-join activation sends or schedules current pointer state after framebuffer baseline.
- Hidden/default cursor state is handled explicitly.
- Pointer baseline has no backend RDPEGFX PDU replay dependency.
- Existing classic pointer behavior remains unchanged.
- Tests cover late-join pointer shape, position, hidden/default cursor, and forced pointer baseline on join.
- Common validation passes.

### US-014: Add final boundary audit enforcement

**Description:** As a maintainer, I want automated/static audit checks proving refactor boundaries so future changes cannot reintroduce backend replay or protocol code into `viewer_server.c`.

**Acceptance Criteria:**

- Add a script or CTest-backed static check for forbidden production patterns.
- Check forbids `viewer_server_publish_gfx_*` in backend RDPEGFX callback paths.
- Check forbids `ViewerGfxCompleteFrame`, `ViewerGfxFrameBuffer`, and `viewer_gfx_replay_frame` in production code.
- Check forbids `rdpgfx->StartFrame`, `rdpgfx->SurfaceCommand`, and `rdpgfx->EndFrame` in `viewer_server.c`.
- Check forbids direct `BitmapUpdate` and `SurfaceBits` sends in `viewer_server.c` after classic transport extraction.
- Check verifies codec and framebuffer modules contain no send/context ownership.
- `VIEWER_SERVER_REFACTOR_PLAN.md` and progress docs are updated with accurate remaining/implemented status.
- Common validation passes.

### US-015: Record end-to-end RDPEGFX validation evidence

**Description:** As a maintainer, I want documented real-client validation evidence so RDPEGFX is not considered complete based only on unit tests and stubbed callbacks.

**Acceptance Criteria:**

- Add a validation document under `tasks/` or docs with exact manual test commands, config snippets, expected logs, and pass/fail results.
- Validate classic first viewer, late viewer, reconnect, and resize.
- Validate viewer GFX full-frame baseline and dirty updates with `viewer.gfx.enabled=true`.
- Validate backend GFX decode-only with classic and GFX viewers where available in the test environment, documenting unavailable environment blockers explicitly.
- Validate no-ACK/slow-viewer behavior does not block other viewers.
- Validate late-join pointer/cursor state.
- Any failures are converted into follow-up PRD stories instead of being silently deferred.
- Common validation passes.

## Success Metrics

- `viewer_server.c` contains no backend RDPEGFX PDU replay state or protocol send sequencing.
- `viewer_server.h` no longer exposes unnecessary internals.
- Static boundary audit passes locally and in the normal test suite.
- All existing RDPEGFX/framebuffer/publisher/config tests pass.
- Real-client validation evidence is recorded for classic and GFX paths.
