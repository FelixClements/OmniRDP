# RDPEGFX RFX Validation Checklist

- Story: US-013
- Status: PARTIAL / NOT_COMPLETE
- Date:
- Run ID:
- Tester:
- Build:
- Backend:
- Client/viewer:
- Config path:

Do not commit credentials, passwords, secrets, private hostnames, private keys, or sensitive screenshots/log excerpts. Redact sensitive values before attaching evidence.

## Required config

```ini
viewer.gfx.enabled = true
viewer.gfx.codec = rfx
viewer.gfx.diagnostic.full_frame_dirty = false
backend.gfx.decode_only_enabled = false
codec.graphics_pipeline = false
```

RFX evidence does not satisfy US-012 uncompressed validation. US-012 requires `viewer.gfx.codec = uncompressed` evidence and remains separate.

## Scenario results

| Scenario | Result (PASS/FAIL/NOT_RUN/PARTIAL) | Lag | Artifacts | Disconnect | Evidence path | Notes |
|---|---|---|---|---|---|---|
| Initial baseline | PARTIAL | NOT_REPORTED | NOT_REPORTED | NO |  | Informal user observation: RFX works much better overall. |
| Start menu open | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN |  | Not separately reported. |
| Start menu close | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN |  | Not separately reported. |
| Window drag | PARTIAL | YES | NOT_REPORTED | NOT_REPORTED |  | Some lag dragging large Explorer windows. |
| Window resize | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN |  | Not separately reported. |
| Overlapping windows | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN |  | Not separately reported. |
| Text editing | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN |  | Not separately reported. |
| Viewer reconnect | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN |  | Not separately reported. |
| Backend desktop resize | NOT_RUN | NOT_RUN | NOT_RUN | NOT_RUN |  | Not separately reported. |

## Observed results

- User reports `viewer.gfx.codec = rfx` works much better than uncompressed RDPEGFX.
- Some lag remains when dragging large Explorer windows.
- Latest logs confirm the run used RFX configuration.
- Log counts from latest RFX run:
  - Area fallback count: `2`
  - Consecutive-defer fallback count: `0`
  - Diagnostic forced full-frame count: `0`
  - Large dirty bursts with `>=90` rectangle batches: `260`
  - Large dirty bursts with `>=128` rectangle batches: `13`
  - Maximum rectangle count in a batch: `157`
  - Maximum payload: `64937`
  - One disconnect/provider ultimatum observed.

## Limitations / retest required

- Formal scenario-by-scenario pass/fail validation is still pending.
- Disconnect/provider ultimatum requires investigation before US-013 can pass.
- Remaining lag during large Explorer window drags requires characterization.
- RFX success must not close US-012 or hide unresolved uncompressed RDPEGFX defects.
