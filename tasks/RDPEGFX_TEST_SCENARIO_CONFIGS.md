# RDPEGFX test scenario config guide

This guide lists practical `config.ini` settings for validating the current
classic, viewer RDPEGFX, backend GFX decode-only, pointer, resize, reconnect,
and slow-viewer scenarios.

Do not commit real credentials. Replace placeholders before running tests.

## Common base config

Use this base for every scenario, then apply the scenario-specific override
block below it.

```ini
[service]
log_level = debug
log_dir = C:\ProgramData\OmniRDP\logs
log_max_size_mb = 10
log_max_files = 5
pipe_name = OmniRDP_ServicePipe

[instances]
names = test

[instance:test]
enabled = true

backend.hostname = <backend-rdp-host>
backend.port = 3389
backend.username = <user>
backend.password = <secret-or-dpapi:value>
backend.domain = <domain-or-empty>
backend.connect_timeout_ms = 30000

backend.security.nla_enabled = true
backend.security.tls_enabled = true
backend.security.rdp_enabled = true
backend.security.server_authentication = true
backend.security.ignore_certificate = false

viewer.bind_address = 127.0.0.1
viewer.port = 3390
viewer.max_viewers = 10
viewer.cert_path =
viewer.key_path =
viewer.security.nla_enabled = true
viewer.security.tls_enabled = true
viewer.security.rdp_enabled = true
viewer.auth.mode = backend_credentials

reconnect.enabled = true
reconnect.max_attempts = 10
reconnect.initial_delay_ms = 1000
reconnect.max_delay_ms = 60000
reconnect.backoff_multiplier = 2.0
```

Connect clients with one of:

```powershell
mstsc /v:127.0.0.1:3390
```

```powershell
wfreerdp /v:127.0.0.1:3390 /u:<user> /p:<secret> /d:<domain>
```

## Scenario matrix

| ID | Scenario | Config override | What to verify |
|---|---|---|---|
| S-001 | Classic first viewer | `viewer.gfx.enabled = false`<br>`backend.gfx.decode_only_enabled = false` | First client sees desktop via classic path. |
| S-002 | Classic late viewer | Same as S-001 | Second client joins and receives current framebuffer. |
| S-003 | Reconnect | Use S-001 or S-005 plus reconnect defaults | Disconnect/reconnect viewer and verify clean recovery. |
| S-004 | Resize | Use S-001 or S-005 | Resize viewer/backend desktop; verify fresh baseline and correct dimensions. |
| S-005 | Viewer RDPEGFX uncompressed | `viewer.gfx.enabled = true`<br>`viewer.gfx.codec = uncompressed`<br>`backend.gfx.decode_only_enabled = false` | RDPEGFX open/caps/baseline/dirty path works. |
| S-006 | Viewer RDPEGFX RFX | `viewer.gfx.enabled = true`<br>`viewer.gfx.codec = rfx`<br>`backend.gfx.decode_only_enabled = false` | Experimental RFX path works; fall back to uncompressed if issues occur. |
| S-007 | Backend GFX decode-only | `backend.gfx.decode_only_enabled = true` plus S-001 or S-005 | Backend may decode GFX, but viewer output still uses canonical framebuffer; no backend replay. |
| S-008 | Slow/no-ACK viewer isolation | See slow-viewer blocks below | One impaired viewer does not block other viewers. |
| S-009 | Pointer/cursor late join | Use S-005, then join second viewer | Late viewer receives current hidden/default/custom cursor and position after baseline. |

## Scenario-specific blocks

### S-001/S-002: Classic first and late viewers

```ini
viewer.gfx.enabled = false
viewer.gfx.codec = uncompressed
backend.gfx.decode_only_enabled = false
viewer.classic_latest_state_enabled = false
```

Run one client for S-001. Keep it connected and run a second client for S-002.

Expected useful log pattern:

```text
RDPEGFX disabled; using classic SurfaceBits path only
```

### S-003: Reconnect

Use either the classic block or an RDPEGFX block, with reconnect enabled:

```ini
reconnect.enabled = true
reconnect.max_attempts = 10
reconnect.initial_delay_ms = 1000
reconnect.max_delay_ms = 60000
reconnect.backoff_multiplier = 2.0
```

Test by disconnecting a viewer and reconnecting it. If possible, also restart or
temporarily disconnect the backend RDP server to exercise backend reconnect.

### S-004: Resize

Use either classic or RDPEGFX. The display keys may be present but are currently
reserved/no-op for negotiated runtime geometry:

```ini
display.monitor_count = 1
display.monitor_width = 1920
display.monitor_height = 1080
display.color_depth = 32
```

For RDPEGFX resize testing, prefer:

```ini
viewer.gfx.enabled = true
viewer.gfx.codec = uncompressed
backend.gfx.decode_only_enabled = false
```

Expected useful log patterns:

```text
RDPEGFX live canonical resize baseline
RDPEGFX framebuffer full-frame baseline sent
```

### S-005: Viewer RDPEGFX uncompressed

```ini
viewer.gfx.enabled = true
viewer.gfx.codec = uncompressed
backend.gfx.decode_only_enabled = false
```

This is the safest viewer RDPEGFX mode. It uses uncompressed RDPEGFX surface
commands, which are bandwidth-heavy but easiest to validate.

Expected useful log patterns:

```text
attempting RDPEGFX Open
RDPEGFX Open succeeded
RDPEGFX caps confirm progressed negotiation
RDPEGFX framebuffer full-frame baseline sent
```

### S-006: Viewer RDPEGFX RFX

```ini
viewer.gfx.enabled = true
viewer.gfx.codec = rfx
backend.gfx.decode_only_enabled = false
```

Accepted values for `viewer.gfx.codec`:

```ini
viewer.gfx.codec = uncompressed
viewer.gfx.codec = rfx
viewer.gfx.codec = remote_fx
```

`remote_fx` is an alias for `rfx`. Invalid values fall back to
`uncompressed`. Treat RFX as experimental; if the client shows rendering
problems, switch back to `uncompressed`.

### S-007: Backend GFX decode-only

Run this with either classic viewers:

```ini
viewer.gfx.enabled = false
backend.gfx.decode_only_enabled = true
```

or with viewer RDPEGFX:

```ini
viewer.gfx.enabled = true
viewer.gfx.codec = uncompressed
backend.gfx.decode_only_enabled = true
```

Expected behavior: backend GFX is decode-only. Viewer output must still come
from the canonical framebuffer path, not backend RDPEGFX PDU replay.

### S-008: Slow/no-ACK viewer isolation

For classic slow-viewer/latest-state policy testing:

```ini
viewer.gfx.enabled = false
viewer.classic_latest_state_enabled = true
viewer.classic_latest_state_max_queue_depth = 7
viewer.classic_latest_state_max_queue_bytes = 4096
viewer.slow_disconnect_enabled = true
viewer.slow_disconnect_after_ms = 30000
```

For RDPEGFX no-ACK/backpressure testing:

```ini
viewer.gfx.enabled = true
viewer.gfx.codec = uncompressed
backend.gfx.decode_only_enabled = false
```

Use at least two clients. Impair one client with a firewall rule, network
throttle, suspend, or blocked output and confirm other viewers continue to get
updates.

### S-009: Pointer/cursor late join

```ini
viewer.gfx.enabled = true
viewer.gfx.codec = uncompressed
backend.gfx.decode_only_enabled = false
```

Steps:

1. Connect first viewer.
2. Move the pointer to a known position.
3. Test default cursor, hidden cursor, and a custom cursor shape if possible.
4. Connect a second late-joining viewer.
5. Confirm the second viewer receives the current cursor shape/system state and
   position after the framebuffer baseline.

Expected useful log pattern:

```text
RDPEGFX pointer baseline after late-join framebuffer baseline
```

## Codec notes

Viewer RDPEGFX codec selection is controlled by:

```ini
viewer.gfx.enabled = true
viewer.gfx.codec = uncompressed
```

or:

```ini
viewer.gfx.enabled = true
viewer.gfx.codec = rfx
```

Classic/backend codec flags are separate:

```ini
codec.nscodec = true
codec.remote_fx = true
codec.graphics_pipeline = false
codec.h264 = false
codec.avc444 = false
codec.avc444v2 = false
```

Keep H.264/AVC options disabled unless explicitly adding support. Viewer GFX
does not replay backend GFX PDUs.

## Evidence to record

For each scenario, record:

- Exact config used, with secrets redacted.
- Client command(s).
- Backend OS/RDP server version.
- Viewer client version.
- Relevant log snippets.
- Pass/fail result.
- Follow-up issue/story for any failure or `NOT_RUN` scenario.
