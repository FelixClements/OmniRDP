# US-015 RDPEGFX validation evidence checkpoint

## Summary

- Date: 2026-05-24
- Branch/commit: `refactor/viewer-server-pipeline-seams` at `056a27d` (`Enforce viewer refactor boundaries`)
- Environment: local Windows development workspace at `C:\Users\localadmin\Documents\Github\OmniRDP`; existing `OmniRDP/build` Debug tree; automated build and CTest available.
- Runtime resources available: no real MSTSC/FreeRDP viewer session, backend Windows RDP server, multi-viewer lab, or controlled slow/no-ACK viewer environment was available/provided for this checkpoint.
- Overall status: **BLOCKED** for US-015 completion. Automated validation passed, but required real-client/runtime evidence was **NOT_RUN**.

## Automated validation

These checks were run in PowerShell from the repository root.

| Area | Exact command | Result |
| --- | --- | --- |
| PRD JSON syntax | `python -m json.tool "prd.json" > $null` | PASS; no output |
| Diff whitespace | `git diff --check` | PASS; no whitespace errors. Git emitted line-ending warnings that `prd.json` and `tasks/RDPEGFX_REMAINING_REFACTOR_PROGRESS.md` will be converted from LF to CRLF the next time Git touches them. |
| Debug build | `cmake --build "OmniRDP/build" --config Debug -j` | PASS; Debug binaries/tests built successfully |
| CTest suite | `ctest --test-dir "OmniRDP/build" -C Debug --output-on-failure` | PASS; 15/15 tests passed |
| Static boundary checks in CTest | `ctest --test-dir "OmniRDP/build" -C Debug --output-on-failure` covering `test_backend_gfx_quarantine`, `test_no_backend_gfx_replay`, `test_public_viewer_server_facade`, `test_viewer_server_classic_transport_boundary`, and `test_refactor_boundaries` | PASS; all listed boundary tests passed |

CTest boundary test results observed:

- `test_backend_gfx_quarantine` — PASS
- `test_no_backend_gfx_replay` — PASS
- `test_public_viewer_server_facade` — PASS
- `test_viewer_server_classic_transport_boundary` — PASS
- `test_refactor_boundaries` — PASS

## Intended runtime configuration snippets

Secrets are intentionally redacted. These snippets describe the intended runtime validation configuration; they were **not** used to produce runtime pass evidence in this environment.

### Classic viewer path with backend security keys

```ini
[instance:us015-classic]
backend.hostname = <backend-rdp-host>
backend.port = 3389
backend.username = <redacted-user>
backend.password = <redacted-secret>
backend.domain = <redacted-domain-or-empty>
backend.connect_timeout_ms = 30000
backend.security.nla_enabled = true
backend.security.tls_enabled = true
backend.security.rdp_enabled = true
backend.security.server_authentication = true
backend.security.ignore_certificate = false

viewer.bind_address = 127.0.0.1
viewer.port = 3390
viewer.max_viewers = 10
viewer.gfx.enabled = false
backend.gfx.decode_only_enabled = false
```

### Viewer RDPEGFX enabled, backend GFX still off

```ini
[instance:us015-viewer-gfx]
backend.hostname = <backend-rdp-host>
backend.port = 3389
backend.username = <redacted-user>
backend.password = <redacted-secret>
backend.domain = <redacted-domain-or-empty>
backend.security.nla_enabled = true
backend.security.tls_enabled = true
backend.security.rdp_enabled = true
backend.security.server_authentication = true
backend.security.ignore_certificate = false

viewer.bind_address = 127.0.0.1
viewer.port = 3391
viewer.max_viewers = 10
viewer.gfx.enabled = true
backend.gfx.decode_only_enabled = false
```

### Backend RDPEGFX decode-only enabled

```ini
[instance:us015-backend-gfx-decode-only]
backend.hostname = <backend-rdp-host>
backend.port = 3389
backend.username = <redacted-user>
backend.password = <redacted-secret>
backend.domain = <redacted-domain-or-empty>
backend.security.nla_enabled = true
backend.security.tls_enabled = true
backend.security.rdp_enabled = true
backend.security.server_authentication = true
backend.security.ignore_certificate = false

viewer.bind_address = 127.0.0.1
viewer.port = 3392
viewer.max_viewers = 10
viewer.gfx.enabled = true
backend.gfx.decode_only_enabled = true
```

## Manual/runtime scenario matrix

| Scenario | Status | Evidence / blocker |
| --- | --- | --- |
| Classic first viewer | NOT_RUN | Requires a real viewer client and reachable backend Windows RDP server; not available in this environment. |
| Classic late viewer | NOT_RUN | Requires at least two real viewer clients and a live backend session; not available in this environment. |
| Reconnect | NOT_RUN | Requires a real viewer and backend session with controlled disconnect/reconnect; not available in this environment. |
| Resize | NOT_RUN | Requires real viewer resize against a live backend session; not available in this environment. |
| RDPEGFX baseline/dirty with `viewer.gfx.enabled=true` | NOT_RUN | Requires an RDPEGFX-capable real client plus backend desktop changes; not available in this environment. |
| Backend GFX decode-only | NOT_RUN | Requires backend Windows RDP server/session capable of exercising backend GFX while confirming no viewer replay; not available in this environment. |
| No-ACK/slow-viewer isolation | NOT_RUN | Requires multi-viewer runtime setup and a controllable slow/no-ACK client or network impairment; not available in this environment. |
| Pointer/cursor late join: hidden | NOT_RUN | Requires real backend pointer state change and late-joining viewer; not available in this environment. |
| Pointer/cursor late join: default | NOT_RUN | Requires real backend pointer state and late-joining viewer; not available in this environment. |
| Pointer/cursor late join: custom | NOT_RUN | Requires real backend custom cursor state and late-joining viewer; not available in this environment. |
| Pointer/cursor late join: position | NOT_RUN | Requires real backend pointer movement and late-joining viewer; not available in this environment. |

## Expected log patterns from current source

The following are expected/source-derived patterns only. They are **not actual runtime observations** from this checkpoint.

- RDPEGFX disabled classic path: `Viewer %u RDPEGFX disabled; using classic SurfaceBits path only`
- RDPEGFX channel open: `Viewer %u attempting RDPEGFX Open`, `Viewer %u RDPEGFX Open succeeded`, or `Viewer %u RDPEGFX Open failed`
- Capability negotiation: `Viewer %u RDPEGFX caps confirm progressed negotiation`, `Viewer %u RDPEGFX caps confirmed after activation; gating`, or `Viewer %u advertised incompatible RDPEGFX caps; staying on`
- Canonical framebuffer baseline: `RDPEGFX framebuffer full-frame baseline sent`, `RDPEGFX framebuffer baseline failed`, `RDPEGFX pending canonical baseline`, `RDPEGFX live canonical resize baseline`
- Late-join pointer baseline: `RDPEGFX pointer baseline after late-join framebuffer baseline`
- Activation/fallback: `peer activated for RDPEGFX late join`, `peer activated waiting for RDPEGFX caps confirmation`, `RDPEGFX caps negotiation fallback`, `incompatible RDPEGFX caps`

## Follow-up items before US-015 can pass

1. Provision or document a reachable backend Windows RDP server with non-secret `backend.security.*` configuration and test credentials handled outside committed files.
2. Run classic first-viewer, late-viewer, reconnect, and resize scenarios with MSTSC and/or FreeRDP viewer clients; record commands, non-secret config, observable results, and relevant logs.
3. Run `viewer.gfx.enabled=true` with an RDPEGFX-capable viewer and capture baseline plus dirty-update evidence from visible desktop changes and logs.
4. Run `backend.gfx.decode_only_enabled=true` while confirming backend GFX remains decode-only and viewer output still comes from canonical framebuffer paths, not backend PDU replay.
5. Run a multi-viewer slow/no-ACK isolation scenario and record that one slow/no-ACK viewer does not block other viewers.
6. Run pointer/cursor late-join coverage for hidden, default, custom, and position states.
7. Convert any runtime failure into follow-up PRD stories before marking US-015 complete.

## Conclusion

US-015 should remain **blocked/pending** with `passes: false`. Automated validation and static boundary checks passed, but the end-to-end real-client/runtime evidence required by US-015 is unavailable and was not run in this environment.
