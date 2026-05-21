# RDPEGFX linked-plan progress tracker

This tracker links `RDPEGFX_Implementation_Plan.md` to `VIEWER_SERVER_REFACTOR_PLAN.md`.

Primary rule: continue RDPEGFX work from `RDPEGFX_Implementation_Plan.md`, but every phase must obey `VIEWER_SERVER_REFACTOR_PLAN.md` module boundaries. `viewer_server.c` remains a high-level coordinator. Orchestrator agents coordinate stories/progress; bounded fixer/Ralph implementation agents may edit code/tests/CMake within a story.

## Current status

### Completed or substantially in place

- `US-001` is completed/accepted and is the current commit target.
  - Scope completed: RDPEGFX lifecycle and CapsAdvertise/CapsConfirm seam finalized with explicit server-owned fallback/join orchestration from pipeline result flags.
  - Review: oracle accepted the behavior and module-boundary direction.
  - Validation completed:
    - `cmake --build "OmniRDP/build" --config Debug -j`
    - `ctest --test-dir "OmniRDP/build" -C Debug --output-on-failure`
    - `git diff --check`
- `US-002` is completed/accepted.
  - Scope completed: documented canonical framebuffer pixel-format, stride, orientation, alpha, and ownership invariants; added missing tests for mark-dirty accumulation, mark-dirty overflow, snapshot-free reset behavior, and generation non-increment on mark-dirty.
  - Boundary check: no live viewer traffic was routed through the framebuffer; framebuffer remains independent from FreeRDP send paths and RDPEGFX protocol context ownership.
  - Validation completed:
    - `clang-format -i "OmniRDP/include/viewer_framebuffer.h" "OmniRDP/tests/test_viewer_framebuffer.c"`
    - `cmake --build "OmniRDP/build" --config Debug -j`
    - `ctest --test-dir "OmniRDP/build" -C Debug --output-on-failure`
    - `git diff --check`
- `US-003` is completed/accepted.
  - Scope completed: decoded GDI framebuffer ingestion validates dirty rectangles before canonical framebuffer update. Invalid/out-of-bounds rectangles are dropped; if no valid dirty rectangles remain, the update falls back to a full-frame dirty update for correctness and to preserve delivery semantics.
  - Boundary check: no classic viewer delivery behavior, backend/viewer RDPEGFX enablement, publisher/coalescing policy, backend PDU replay authority, or FreeRDP send calls in `viewer_framebuffer` were added.
  - Validation completed:
    - `clang-format -i "OmniRDP/include/viewer_framebuffer.h" "OmniRDP/src/viewer_framebuffer.c" "OmniRDP/src/viewer_server.c" "OmniRDP/tests/test_viewer_framebuffer.c"`
    - `cmake --build "OmniRDP/build" --config Debug -j`
    - `ctest --test-dir "OmniRDP/build" -C Debug --output-on-failure`
    - `python -m json.tool "prd.json" > $null`
    - `git diff --check`
- `US-004` is completed/accepted.
  - Scope completed: publisher metrics updates/reads are guarded by a publisher lock; added a narrow framebuffer-update observation API for latest generation, latest dirty rect count, dirty overflow, observed framebuffer update count, and cumulative observed dirty rect count.
  - Boundary check: `viewer_server.c` only calls the narrow publisher observation API after GDI framebuffer update; no classic delivery behavior, coalescing/latest-state policy, backend/viewer RDPEGFX enablement, protocol state, frame ACK, activation, or codec behavior changed.
  - Validation completed:
    - `clang-format -i "OmniRDP/include/viewer_publisher.h" "OmniRDP/src/viewer_publisher.c" "OmniRDP/src/viewer_server.c" "OmniRDP/tests/test_viewer_publisher.c"`
    - `cmake --build "OmniRDP/build" --config Debug -j`
    - `ctest --test-dir "OmniRDP/build" -C Debug --output-on-failure`
    - `python -m json.tool "prd.json" > $null`
    - `git diff --check`
- `US-005` is completed/accepted.
  - Scope completed: eligible classic activation attempts to enqueue a full-frame classic baseline from a canonical framebuffer snapshot before existing incremental/refresh flow continues. Publisher baseline snapshots normalize dirty metadata to one full-frame rectangle and are not suppressed by global consumed generation.
  - Boundary check: snapshot selection/normalization stays in `viewer_publisher`; canonical pixels stay in `viewer_framebuffer`; `viewer_server.c` coordinates enqueue only. No coalescing/latest-state policy, backend/viewer RDPEGFX enablement, RDPEGFX protocol state, or backend PDU replay authority was added.
  - Validation completed:
    - `clang-format -i "OmniRDP/include/viewer_publisher.h" "OmniRDP/src/viewer_publisher.c" "OmniRDP/src/viewer_server.c" "OmniRDP/tests/test_viewer_publisher.c"`
    - `cmake --build "OmniRDP/build" --config Debug -j`
    - `ctest --test-dir "OmniRDP/build" -C Debug --output-on-failure`
    - `python -m json.tool "prd.json" > $null`
    - `git diff --check`
- `US-006` is completed/accepted.
  - Scope completed: classic queue observability/scaffolding only. Publisher metrics now track current classic queue depth, approximate queued bytes, dropped classic events, and max observed depth/bytes through narrow observation APIs.
  - Boundary check: `viewer_server.c` reports observations at existing classic queue mutation points only. FIFO/drop-oldest/full-refresh behavior, byte-limit enforcement, latest-state coalescing, RDPEGFX sends, and delivery policy remain unchanged.
  - Validation completed:
    - `clang-format -i "OmniRDP/include/viewer_publisher.h" "OmniRDP/src/viewer_publisher.c" "OmniRDP/src/viewer_server.c" "OmniRDP/tests/test_viewer_publisher.c"`
    - `python -m json.tool "prd.json" > $null`
    - `git diff --check`
    - `cmake --build "OmniRDP/build" --config Debug -j`
    - `ctest --test-dir "OmniRDP/build" -C Debug --output-on-failure`
- `US-007` is completed/accepted.
  - Scope completed: gated/default-off latest-state replacement policy for slow/backlogged classic viewers. Publisher owns policy configuration, queue replacement decisions, stale suppression, and latest framebuffer snapshot selection. Explicit `viewer.classic_latest_state_*` config keys wire production enablement while defaulting off.
  - Boundary check: `viewer_server.c` calls narrow publisher APIs only, preserves default FIFO/drop-oldest/full-refresh behavior unless the policy is explicitly enabled, clears stale classic backlog only after a newer baseline snapshot/event is available, and does not manipulate pixels or add RDPEGFX/GFX behavior. Per-viewer generation tracking resets with viewer send state/cleanup; normal backend `BITMAP_UPDATE` events remain generation `0`, so stale suppression intentionally applies to framebuffer-backed baseline/latest events only.
  - Validation completed:
    - `clang-format -i "OmniRDP/include/viewer_publisher.h" "OmniRDP/include/viewer_server.h" "OmniRDP/src/viewer_publisher.c" "OmniRDP/src/viewer_server.c" "OmniRDP/tests/test_viewer_publisher.c"`
    - `python -m json.tool "prd.json" > $null`
    - `git diff --check`
    - `cmake --build "OmniRDP/build" --config Debug -j`
    - `ctest --test-dir "OmniRDP/build" -C Debug --output-on-failure`
- `US-008` is completed/accepted.
  - Scope completed: dormant RDPEGFX activation placeholder state/API in `viewer_gfx_pipeline_activate` and `viewer_gfx_pipeline_send_snapshot`. The pipeline validates inputs and records local frame, ACK, active-surface, surface-dimension, full-present, fallback-disabled, negotiation, `use_rdpgfx`, and activation timestamp bookkeeping only.
  - Boundary check: no ResetGraphics/CreateSurface/MapSurfaceToOutput/full-frame PDU sends, codec work, FrameAcknowledge migration, backend GFX enablement, viewer RDPEGFX default enablement, or backend PDU replay authority changes were added. Tests cover null/invalid input, activation placeholders, repeated activation idempotence, fallback-disabled behavior, and snapshot validation/no-send behavior.
  - Validation completed:
    - `clang-format -i "OmniRDP/include/viewer_gfx_pipeline.h" "OmniRDP/src/viewer_gfx_pipeline.c" "OmniRDP/tests/test_viewer_gfx_pipeline.c"`
    - `python -m json.tool "prd.json" > $null`
    - `git diff --check`
    - `cmake --build "OmniRDP/build" --config Debug -j`
    - `ctest --test-dir "OmniRDP/build" -C Debug --output-on-failure`
- Skeleton modules are present and wired:
  - `OmniRDP/include/viewer_framebuffer.h`
  - `OmniRDP/src/viewer_framebuffer.c`
  - `OmniRDP/include/viewer_publisher.h`
  - `OmniRDP/src/viewer_publisher.c`
  - `OmniRDP/include/viewer_gfx_pipeline.h`
  - `OmniRDP/src/viewer_gfx_pipeline.c`
  - `OmniRDP/include/viewer_gfx_codec_uncompressed.h`
  - `OmniRDP/src/viewer_gfx_codec_uncompressed.c`
- Canonical framebuffer and publisher modules exist with tests.
- RDPEGFX viewer lifecycle seam was extracted so `viewer_gfx_pipeline.c/.h` owns narrow lifecycle scaffolding including `RdpgfxServerContext` create/free/init/uninit/open and message handling wrappers.
- RDPEGFX CapsAdvertise/CapsConfirm mechanics were moved to `viewer_gfx_pipeline.c/.h`.
- The module-static orchestration callback design was cleaned up: pipeline reports explicit caps-result actions; `viewer_server.c` performs server-owned fallback/join orchestration explicitly.
- Existing validation after the latest code extraction passed:
  - `cmake --build "OmniRDP/build" --config Debug -j`
  - `ctest --test-dir "OmniRDP/build" -C Debug --output-on-failure`
  - `git diff --check` passed with only LF-to-CRLF warnings.

### Still disabled by default

- Viewer RDPEGFX output remains disabled by default.
- Backend RDPEGFX remains disabled by default.
- No new codec behavior should be assumed active.

## Module boundary rules

- `viewer_server.c`: high-level coordination only: listener setup, peer connect/disconnect, authentication, viewer list/lifecycle, and calls into modules.
- `viewer_framebuffer.c/.h`: canonical pixels, dimensions, stride/pixel format, generation, dirty regions, snapshots. No FreeRDP send calls.
- `viewer_publisher.c/.h`: coalescing, pacing, generation tracking, queued update/byte policy, per-viewer dispatch decisions. No `RdpgfxServerContext` ownership.
- `viewer_gfx_pipeline.c/.h`: RDPEGFX session/protocol sequencing: context lifecycle, channel open/message handling, caps confirm, reset/create/map, frames, resize/reset session state, fallback state. Frame acknowledgement callback migration is not allowed until a separate reviewed story explicitly scopes it; today it remains outside the current pipeline ownership boundary.
- `viewer_gfx_codec_uncompressed.c/.h`: uncompressed RDPEGFX payload construction only. No protocol/session ownership.

## Non-goals for the linked-plan sequence

- Do not enable viewer or backend GFX by default.
- Do not mirror backend RDPEGFX capabilities to viewers.
- Do not use backend RDPEGFX PDU replay as the authoritative late-join model.
- Existing backend-PDU replay/ring code remains transitional in `viewer_server.c`; do not mix it into the canonical framebuffer/publisher path.
- Do not rewrite listener, authentication, classic lifecycle, frame queues, or backend activation outside a specific bounded story.
- Do not implement RFX, ClearCodec, H.264, AVC420, AVC444, AVC444v2, or progressive codecs before the uncompressed GFX MVP is correct.
- Do not hold framebuffer locks while encoding or sending.
- Do not allow one slow viewer to block backend decode or other viewers.

## Ralph story order

The Ralph-ready story backlog is in repository root `prd.json` because no Ralph-specific location was present in this repository. It uses the Ralph schema with `project`, `name`, `branchName`, `description`, and `userStories` fields. Story IDs use `US-001` style and include `dependsOn` arrays.

Recommended next stories:

1. `US-001` — completed/accepted; current commit target.
2. `US-002` — completed/accepted; framebuffer invariants documented and tested.
3. `US-003` — completed/accepted; decoded GDI framebuffer dirty rects are validated before canonical framebuffer update.
4. `US-004` — completed/accepted; publisher generation and dirty metadata metrics are thread-safe and observability-only.
5. `US-005` — completed/accepted; classic late join attempts a framebuffer-backed full-frame baseline.
6. `US-006` — completed/accepted; classic queue observability/scaffolding only.
7. `US-007` — completed/accepted; gated/default-off classic latest-state replacement policy.
8. `US-008` — completed/accepted; dormant viewer RDPEGFX activation state placeholders.
9. `US-009` — completed/accepted; uncompressed RDPEGFX surface commands are built from snapshots with full-frame and single dirty-rect payload tests.
10. `US-010` — completed/accepted; disabled-by-default viewer RDPEGFX full-frame baseline sends from canonical framebuffer snapshots behind `viewer.gfx.enabled`.
11. `US-011` — completed/accepted; dirty-update planning state and publisher dirty snapshot coalescing added with no incremental sends.
12. `US-012` — send ACK-paced uncompressed dirty RDPEGFX frames.
13. `US-013` — add experimental backend GFX decode-only gate.

Backend GFX (`US-013`) is intentionally deferred later than the original implementation-plan order. The current sequence prioritizes the canonical framebuffer/viewer MVP and avoids mixing backend PDU replay with the new publisher path.

## Per-story validation checklist

- Review the story's source documents and module boundary notes.
- Keep the story small enough for one Ralph iteration.
- Run `clang-format -i` on modified C/H files.
- Run `git diff --check`.
- Run `cmake --build "OmniRDP/build" --config Debug -j`; if it cannot run, document the missing local dependency/build blocker.
- Run `ctest --test-dir "OmniRDP/build" -C Debug --output-on-failure`; if tests are unavailable, document the reason.
- Update this progress tracker after each completed story.

## Open risks and follow-ups

- `viewer_server.h` still exposes substantial transitional GFX structures. Prefer narrow/opaque APIs in future bounded refactors, but do not churn public internals without a concrete story.
- Existing backend-PDU replay/ring code remains transitional. Leave it isolated until the framebuffer/publisher model replaces the late-join path.
- Mid-session GFX-to-classic fallback may not be protocol-safe for all clients; prefer pre-activation fallback or per-viewer reconnect/degrade behavior unless explicitly tested.
- Backend GFX must remain decode-only and independent from viewer GFX negotiation.
