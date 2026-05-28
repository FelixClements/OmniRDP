# RDPEGFX Uncompressed Runtime Validation Checklist

- Story: US-012
- Status: FAIL
- Date:
- Run ID:
- Tester:
- Build:
- Backend:
- Client/viewer:
- Config path: C:\ProgramData\OmniRDP\logs\vm2

Do not commit credentials, passwords, secrets, private hostnames, private keys, or sensitive screenshots/log excerpts. Redact sensitive values before attaching evidence.

## Required config

```ini
viewer.gfx.enabled = true
viewer.gfx.codec = uncompressed
viewer.gfx.diagnostic.full_frame_dirty = false
backend.gfx.decode_only_enabled = false
codec.graphics_pipeline = false
```

## Scenario results

| Scenario | Result (PASS/FAIL/NOT_RUN) | Ghosting | Black fragments | Stale trails | Canvas fragmentation | Click responsiveness | Reconnect recovery if applicable | Log/evidence path | Notes |
|---|---|---|---|---|---|---|---|---|---|
| Initial baseline | PASS | PASS | PASS | PASS | PASS | PASS | N/A | `C:\ProgramData\OmniRDP\logs\vm2` | Baseline rendered correctly. Logs show RDPEGFX negotiated successfully and framebuffer baseline sent. |
| Start menu open | PASS | PASS | PASS | PASS | PASS | PASS | N/A | `C:\ProgramData\OmniRDP\logs\vm2` | No visible corruption reported. |
| Start menu close | PASS | PASS | PASS | PASS | PASS | PASS | N/A | `C:\ProgramData\OmniRDP\logs\vm2` | No visible corruption reported. |
| Window drag | FAIL | FAIL | NOT_REPORTED | FAIL | FAIL | FAIL | N/A | `C:\ProgramData\OmniRDP\logs\vm2` | Worked briefly, then screen change hit repeated 60%+ area-threshold full-frame fallback and MSTSC froze. |
| Window resize | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | N/A |  |  |
| Overlapping windows | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | N/A |  |  |
| Text editing | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | N/A |  |  |
| Viewer reconnect | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN |  |  |
| Backend desktop resize | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN |  |  |

## Log evidence checklist

- [x] At least one accumulated pending dirty generation observed.
- [x] At least one sent dirty generation observed.
- [x] Confirmed no unintended diagnostic full-frame forcing occurred.

Runtime evidence summary from `C:\ProgramData\OmniRDP\logs\vm2`:

- Config applied: `viewer.gfx.enabled=true`, `viewer.gfx.codec=uncompressed`, `viewer.gfx.diagnostic.full_frame_dirty=false`.
- RDPEGFX negotiated successfully and framebuffer baseline was sent.
- During window drag, repeated `RDPEGFX threshold full-frame dirty fallback` with `reason=pending area threshold`, `width=1920`, `height=1080`.
- Later logs showed repeated fallback burst for the same generation `255` approximately every 8-16ms.
- Counts from searched logs: threshold fallback `885`, area threshold `885`, diagnostic full-frame dirty forced `0`, consecutive deferred dirty sends `0`.
- No pacing/backpressure/ACK-timeout messages were observed in searched logs.
- Diagnostic full-frame forcing was absent.

Suggested log patterns from current code:

- Pending/start/latest generation fields:
  - `pending_start_generation=`
  - `pending_latest_generation=`
  - `pending_dirty_rects=`
  - `pending_area=`
- Moved/sent generation fields:
  - `moved_batch_generation=`
  - `sent_generation=`
  - `generation=`
  - `last_sent_generation=`
- Diagnostic full-frame forcing should be absent unless deliberately testing the diagnostic flag:
  - Absence of `RDPEGFX diagnostic full-frame dirty forced`

## Limitations / blockers

US-012 remains pending because runtime validation failed. Mitigation implemented: uncompressed RDPEGFX no longer promotes >60% pending area to full-frame, while RFX/non-uncompressed area fallback remains unchanged. Runtime retest is required to confirm the window-drag MSTSC freeze is resolved before marking US-012 complete.
