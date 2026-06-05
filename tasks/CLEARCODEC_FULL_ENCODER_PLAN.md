# Full Production ClearCodec Encoder Plan for OmniRDP

This document captures the investigation and implementation plan for adding a production-grade **ClearCodec encoder** to OmniRDP's viewer-side RDPEGFX output pipeline, including **V-Bar**, **glyph cache**, **RLEX**, and **NSCodec** subcodec support.

The design preserves OmniRDP's current safe architecture:

```text
backend RDP / RDPEGFX / bitmap updates
    -> FreeRDP client/GDI decode
    -> OmniRDP canonical framebuffer
    -> per-viewer encoder
    -> RDPEGFX SurfaceCommand to each viewer
```

It does **not** rely on backend RDPEGFX passthrough. ClearCodec state is connection-specific and stateful; passthrough is fragile for late joiners, multiple viewers, mismatched capabilities, reset behavior, and per-viewer backpressure.

---

## 1. Executive summary

FreeRDP 3.26.0 provides:

- RDPEGFX server/client channel transport.
- ClearCodec **decoder** support via `clear_decompress()`.
- NSCodec **encoder** support via `nsc_compose_message()`.
- Decoder code and tests that can validate OmniRDP-generated ClearCodec payloads.

FreeRDP 3.26.0 does **not** provide a usable ClearCodec encoder:

- `freerdp-3.26.0/include/freerdp/codec/clear.h` declares `clear_compress()`, but marks it broken/not implemented/deprecated.
- `freerdp-3.26.0/libfreerdp/codec/clear.c` implements `clear_compress()` as a TODO stub.

OmniRDP currently provides:

- RDPEGFX viewer-side transport/frame lifecycle.
- Uncompressed RDPEGFX surface command encoding.
- Backend RDPEGFX decode-through-FreeRDP/GDI into a canonical framebuffer.

OmniRDP does **not** currently provide:

- A ClearCodec encoder.
- A generic viewer-side GFX codec selection abstraction.
- Per-viewer ClearCodec state.
- Residual, bands/V-Bar, glyph, RLEX, or ClearCodec-wrapped NSCodec output.

Recommended approach:

1. Add an encoder abstraction while preserving the existing uncompressed path.
2. Add a ClearCodec module behind feature flags.
3. Start with spec-valid raw subcodec and/or residual-only ClearCodec.
4. Add NSCodec subcodec using FreeRDP's `nsc_compose_message()`.
5. Add residual/RLE, RLEX, bands/V-Bar, then glyph cache.
6. Validate every encoded payload by decoding through FreeRDP's `clear_decompress()` and comparing pixels.
7. Keep automatic fallback to uncompressed RDPEGFX and classic bitmap mode.

Estimated scope:

| Deliverable | Rough effort |
|---|---:|
| Encoder abstraction | 1-2 weeks |
| Minimal ClearCodec container | 2-4 weeks |
| NSCodec subcodec support | 2-4 weeks |
| Residual encoder | 3-6 weeks |
| RLEX subcodec | 2-4 weeks |
| Bands/V-Bar encoder | 4-8 weeks |
| Glyph cache | 4-10+ weeks |
| Production hardening | 1-3 months |

Expected total:

- ClearCodec prototype with NSCodec: **1-2 months**.
- Useful partial ClearCodec: **2-4 engineer-months**.
- Full production ClearCodec with V-Bar/glyph/NSCodec: **4-8+ engineer-months**.

---

## 2. Current repository findings

### 2.1 OmniRDP viewer-side RDPEGFX path

Important local files:

```text
OmniRDP/src/viewer_gfx_pipeline.c
OmniRDP/src/viewer_gfx_codec_uncompressed.c
OmniRDP/include/viewer_gfx_codec_uncompressed.h
OmniRDP/src/viewer_gfx_codec_rfx.c
OmniRDP/include/viewer_gfx_codec_rfx.h
OmniRDP/src/viewer_server_internal.h
OmniRDP/tests/CheckRefactorBoundaries.cmake
```

Current behavior:

- `viewer_gfx_pipeline.c` manages RDPEGFX lifecycle: caps negotiation, frame IDs, `ResetGraphics`, `CreateSurface`, `MapSurfaceToOutput`, `StartFrame`, `SurfaceCommand`, `EndFrame`, and frame acknowledgements.
- `viewer_gfx_codec_uncompressed.c` builds `RDPGFX_SURFACE_COMMAND` payloads using `RDPGFX_CODECID_UNCOMPRESSED`.
- Dirty update and baseline paths currently call the uncompressed builder directly.
- `viewer_gfx_codec_rfx.c` is a useful reference for a stateful codec module shape, but is not a complete production compressed-codec implementation.

Boundary rule:

- Codec modules should only build payloads/commands.
- Codec modules must not own RDPEGFX transport, session state, `rdpgfx->` sends, or WTS channel behavior.
- `CheckRefactorBoundaries.cmake` should be updated so any ClearCodec module is kept inside this boundary.

### 2.2 OmniRDP backend RDPEGFX path

Important local file:

```text
OmniRDP/src/backend.c
```

Current behavior:

- Backend RDPEGFX support is decode-oriented.
- FreeRDP/GDI handles backend RDPEGFX decoding, including ClearCodec decode.
- OmniRDP ingests the decoded framebuffer and republishes to viewers.
- This is the correct baseline architecture for multi-viewer proxy behavior.

### 2.3 FreeRDP ClearCodec status

Important FreeRDP files:

```text
freerdp-3.26.0/include/freerdp/codec/clear.h
freerdp-3.26.0/libfreerdp/codec/clear.c
freerdp-3.26.0/libfreerdp/gdi/gfx.c
freerdp-3.26.0/libfreerdp/codec/test/TestFreeRDPCodecClear.c
freerdp-3.26.0/libfreerdp/codec/test/TestFreeRDPCodecProgressive.c
```

Useful decoder details from `clear.c`:

- Decoder context includes `NSC_CONTEXT* nsc`, sequence number, temporary buffers, glyph cache, V-Bar cache, and Short-V-Bar cache.
- The decoder supports residual data, bands/V-Bar data, glyph cache operations, raw BGR24 subcodec ID `0`, NSCodec subcodec ID `1`, and RLEX subcodec ID `2`.
- `clear_decompress()` is the primary validation oracle for encoded payloads.
- `clear_compress()` is not implemented.

### 2.4 FreeRDP NSCodec status

Important FreeRDP files:

```text
freerdp-3.26.0/include/freerdp/codec/nsc.h
freerdp-3.26.0/libfreerdp/codec/nsc_encode.c
freerdp-3.26.0/libfreerdp/codec/nsc_decode.c
```

Reusable encoder pieces:

- `nsc_context_new()`
- `nsc_context_free()`
- `nsc_context_reset()`
- `nsc_context_set_parameters()`
- `nsc_compose_message()`

The ClearCodec encoder should reuse FreeRDP's NSCodec encoder for ClearCodec subcodec ID `0x01` instead of implementing NSCodec from scratch.

---

## 3. Official specifications and references

### 3.1 RDPEGFX / ClearCodec

- MS-RDPEGFX landing page: <https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/da5c75f9-cd99-450c-98c4-014a496942b0>
- RDPEGFX capability/versioning: <https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/31c6e2b1-335b-4a75-9454-bb2309958c21>
- `RDPGFX_WIRE_TO_SURFACE_PDU_1`: <https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/fb919fce-cc97-4d2b-8cf5-a737a00ef1a6>
- ClearCodec compression rules: <https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/6fa49bae-192f-4e25-888a-7cacfae303cf>
- ClearCodec bitmap stream: <https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/f6c8a114-eaba-489f-9626-f41ad27a19b1>
- ClearCodec composite payload: <https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/23253264-20f7-4a85-b6b4-023ca955cb3f>
- ClearCodec residual layer: <https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/4e42306a-ff66-426f-95c4-d53ae5dba900>
- ClearCodec RGB run segment: <https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/c47e61d7-97db-4b35-93b9-3e41077f3ad0>
- ClearCodec bands/V-Bars: <https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/e6f05a9f-0753-4fad-ab47-c01e2f316379> and <https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/8faa4996-5d9a-4fd2-abe9-85a5073b899f>
- ClearCodec subcodec layer: <https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/4720bbf2-b369-43d5-8e50-975e4ab7e29d>
- ClearCodec example dump: <https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/5dbc29b1-88ab-454e-8258-ad8400cd4106>

### 3.2 NSCodec

- MS-RDPNSC overview/compression: <https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpnsc/d1fbd46e-57d7-4a4c-b5b6-d7e7a8d3dcc1>
- NSCodec RLE: <https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpnsc/29dadfaa-aad6-4353-9839-e83e04bba70a> and <https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpnsc/f3b20fba-9dc4-4075-bee9-7cf48f5eef16>
- NSCodec example dump: <https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpnsc/e4d67d59-b541-4546-9a1b-1966da6cfee4>
- AYCoCg / color-plane references inherited from RDPEGDI: <https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegdi/ef530a0a-03d8-482f-989b-57a1036797b2>, <https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegdi/57507b94-41e6-41ff-a35e-c00211945ceb>, and <https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegdi/b4462e64-2286-4e77-9c5e-2046fc490cc0>

### 3.3 Public implementation references

- FreeRDP ClearCodec decoder and stub encoder: <https://github.com/FreeRDP/FreeRDP/blob/3.26.0/libfreerdp/codec/clear.c> and <https://github.com/FreeRDP/FreeRDP/blob/3.26.0/include/freerdp/codec/clear.h>
- FreeRDP NSCodec encoder: <https://github.com/FreeRDP/FreeRDP/blob/3.26.0/libfreerdp/codec/nsc_encode.c> and <https://github.com/FreeRDP/FreeRDP/blob/3.26.0/include/freerdp/codec/nsc.h>
- FreeRDP decoder test/replay harness: <https://github.com/FreeRDP/FreeRDP/blob/3.26.0/libfreerdp/codec/test/TestFreeRDPCodecProgressive.c>
- IronRDP issue discussing ClearCodec/EGFX gaps: <https://github.com/Devolutions/IronRDP/issues/1158>

---

## 4. ClearCodec stream structure

ClearCodec data is sent inside `RDPGFX_WIRE_TO_SURFACE_PDU_1`.

RDPEGFX surface command fields of interest:

```text
surfaceId
codecId = RDPGFX_CODECID_CLEARCODEC = 0x0008
pixelFormat
destRect / left / top / right / bottom
bitmapDataLength
bitmapData = CLEARCODEC_BITMAP_STREAM
```

OmniRDP command fields should be populated as:

```c
command->codecId = RDPGFX_CODECID_CLEARCODEC;
command->contextId = 0;
command->data = clearcodecPayload;
command->length = clearcodecPayloadLength;
```

The RDPEGFX transport lifecycle remains unchanged:

```text
StartFrame
SurfaceCommand
EndFrame
```

### 4.1 Top-level ClearCodec bitmap stream

```text
flags:       u8
seqNumber:   u8
glyphIndex?: u16, present only if CLEARCODEC_FLAG_GLYPH_INDEX is set
compositePayload?, absent for glyph-hit-only payloads
```

Known flags:

```text
0x01 CLEARCODEC_FLAG_GLYPH_INDEX
0x02 CLEARCODEC_FLAG_GLYPH_HIT
0x04 CLEARCODEC_FLAG_CACHE_RESET
```

Rules:

- `seqNumber` starts at `0` and increments modulo 256.
- `GLYPH_HIT` must not appear without `GLYPH_INDEX`.
- If this is a glyph hit, the payload references an existing glyph cache entry and there is no composite payload.
- If this is not a glyph hit, the composite payload follows.

### 4.2 Composite payload

```text
residualByteCount: u32 LE
bandsByteCount:    u32 LE
subcodecByteCount: u32 LE
residualData...
bandsData...
subcodecData...
```

Decoder application order:

1. Residual layer.
2. Bands/V-Bar layer.
3. Subcodec layer.

Later layers overwrite pixels produced by earlier layers.

---

## 5. Full encoder components

### 5.1 Per-viewer state

ClearCodec state must be **per viewer**.

Do not share ClearCodec state across viewers because each viewer can join late, reconnect, reset graphics independently, fall back independently, acknowledge frames differently, receive different dirty-rect batches, and negotiate different capabilities.

Recommended ownership:

```text
Viewer
 └── ViewerGraphicsContext
      └── ViewerGfxEncoder
           └── ViewerGfxClearContext
```

Proposed state:

```c
typedef struct
{
    BYTE seqNumber;

    /* V-Bar state */
    ClearVBarCache* vbarCache;            /* 32768 entries */
    ClearShortVBarCache* shortVbarCache;  /* 16384 entries */
    UINT32 vbarCursor;
    UINT32 shortVbarCursor;

    /* Glyph state */
    ClearGlyphCache* glyphCache;          /* 4000 entries */

    /* NSCodec subcodec encoder */
    NSC_CONTEXT* nsc;

    /* Scratch buffers */
    BYTE* streamBuffer;
    size_t streamCapacity;
    BYTE* bgr24Buffer;
    size_t bgr24Capacity;
    BYTE* tileBuffer;
    size_t tileCapacity;

    /* State generation */
    UINT64 resetGeneration;
    BOOL cachesValid;
    BOOL clearcodecEnabled;
    BOOL nscodecEnabled;
    BOOL vbarEnabled;
    BOOL glyphEnabled;

    /* Metrics */
    UINT64 rawBytes;
    UINT64 encodedBytes;
    UINT64 fallbackCount;
    UINT64 encodeFailureCount;
    UINT64 nscodecUseCount;
    UINT64 residualUseCount;
    UINT64 rlexUseCount;
    UINT64 vbarHitCount;
    UINT64 vbarMissCount;
    UINT64 shortVbarHitCount;
    UINT64 shortVbarMissCount;
    UINT64 glyphHitCount;
    UINT64 glyphMissCount;
} ViewerGfxClearContext;
```

### 5.2 Residual layer

Residual data is BGR run-length encoding.

Each residual segment:

```text
blue:  u8
green: u8
red:   u8
runLengthFactor1:  u8
runLengthFactor2?: u16 LE
runLengthFactor3?: u32 LE
```

Run length rules:

```text
1..254      -> factor1 = length
255..65534  -> factor1 = 0xFF, factor2 = length
>=65535     -> factor1 = 0xFF, factor2 = 0xFFFF, factor3 = length
```

Implementation notes:

- Input pixels are BGR or BGRX from OmniRDP framebuffer snapshots.
- Residual output is BGR triples plus run length.
- Pixels are ordered left-to-right, top-to-bottom.
- For robust interoperability, emit enough runs to reconstruct exactly `width * height` pixels for the encoded rectangle.
- FreeRDP's decoder validates decoded residual pixel count strictly in practice.

Use residual for solid regions, flat backgrounds, simple UI fills, and low-entropy areas. Avoid it for high-noise/photo/video regions where residual can expand data.

Heuristic:

```text
if residual_size < raw_uncompressed_size * 0.95:
    use residual
else:
    try another subcodec or fallback
```

### 5.3 Subcodec layer

Each subcodec rectangle:

```text
xStart:              u16 LE
yStart:              u16 LE
width:               u16 LE
height:              u16 LE
bitmapDataByteCount: u32 LE
subCodecId:          u8
bitmapData...
```

Supported subcodec IDs:

```text
0x00 raw BGR24
0x01 NSCodec
0x02 RLEX
```

Use subcodecs for regions not well represented by residual or V-Bar.

#### 5.3.1 Raw BGR24 subcodec

Raw subcodec is the simplest correctness path.

For a full-rect raw subcodec:

```text
residualByteCount = 0
bandsByteCount = 0
subcodecByteCount = 13 + (3 * width * height)

xStart = 0
yStart = 0
width = rectWidth
height = rectHeight
bitmapDataByteCount = 3 * width * height
subCodecId = 0x00
bitmapData = packed BGR24
```

Pros:

- Easy to implement.
- Useful as an interop proof.
- Useful fallback inside the ClearCodec container.

Cons:

- Usually not much smaller than RDPEGFX uncompressed BGRX32 after wrapper overhead.
- Provides no real ClearCodec text advantage.

#### 5.3.2 NSCodec subcodec

NSCodec subcodec uses `subCodecId = 0x01`.

ClearCodec wrapper:

```text
xStart
yStart
width
height
bitmapDataByteCount = NSCodec message length
subCodecId = 0x01
bitmapData = NSCodec bitmap stream
```

NSCodec message format includes:

```text
LumaPlaneByteCount:         u32 LE
OrangeChromaPlaneByteCount: u32 LE
GreenChromaPlaneByteCount:  u32 LE
AlphaPlaneByteCount:        u32 LE
ColorLossLevel:             u8
ChromaSubsamplingLevel:     u8
Reserved:                   u16
LumaPlane
OrangeChromaPlane
GreenChromaPlane
AlphaPlane
```

Implementation guidance:

- Reuse FreeRDP `NSC_CONTEXT` and `nsc_compose_message()`.
- Start with conservative/lossless settings.
- Do not use lossy NSCodec on text unless explicitly configured.
- Use NSCodec for image-like regions, gradients, icons, or complex subregions.
- Decode output through FreeRDP `clear_decompress()` for validation, not just through raw NSCodec decode, because the ClearCodec wrapper matters.

Potential setup shape:

```c
ctx->nsc = nsc_context_new();
nsc_context_set_parameters(ctx->nsc, ...);

wStream* s = Stream_New(NULL, initialCapacity);
nsc_compose_message(ctx->nsc, s, bmpdata, width, height, scanline);
```

Exact FreeRDP NSCodec function signatures and parameter support should be confirmed against the local `nsc.h` before implementation.

#### 5.3.3 RLEX subcodec

RLEX uses `subCodecId = 0x02`.

RLEX is useful for palette-like regions, low-color icons, simple UI artwork, and repeated indexed-color runs.

Implementation pieces:

- Palette extraction.
- Palette size limits per spec/decoder behavior.
- Mapping BGR colors to palette indexes.
- Suite/depth selection.
- Variable run-length encoding.
- Size comparison against raw, residual, NSCodec, and uncompressed RDPEGFX.

RLEX should come after raw, NSCodec, and residual are stable.

### 5.4 Bands / V-Bar layer

Bands/V-Bar is the main ClearCodec feature for efficient, sharp text/UI.

State:

```text
V-Bar cache:       32768 entries
Short-V-Bar cache: 16384 entries
V-Bar cursor
Short-V-Bar cursor
```

Band structure conceptually contains:

```text
xStart: u16
xEnd:   u16 inclusive
yStart: u16
yEnd:   u16 inclusive
blueBkg:  u8
greenBkg: u8
redBkg:   u8
vBars...
```

Constraints:

- Band height must be <= 52 pixels.
- One V-Bar is emitted for each x-column in the band.
- V-Bars are emitted left-to-right.
- Encoder-side cache updates must exactly match decoder-side cache updates.

High-level encoder algorithm:

```text
for each candidate text/UI tile:
    split into horizontal bands, height <= 52
    choose dominant background color for each band

    for each x-column in band:
        build full V-Bar pixels for that column

        if exact full V-Bar exists in full V-Bar cache:
            emit full V-Bar cache hit
            update metrics
            continue

        find shortest vertical span differing from background

        if short span exists in Short-V-Bar cache:
            emit Short-V-Bar cache hit
            update full V-Bar cache at current cursor as decoder would
            update metrics
            continue

        emit Short-V-Bar cache miss with raw BGR span pixels
        update Short-V-Bar cache at current cursor as decoder would
        update full V-Bar cache at current cursor as decoder would
        update metrics

    compare bands layer size against fallbacks
```

Why this is difficult:

- Cache-hit references must be correct forever.
- Cache cursor wrap must match the client.
- Background color choice can dominate compression ratio.
- ClearType text has colored anti-aliased subpixel fringes that can reduce matching unless carefully handled.
- Bugs often produce visual corruption rather than clean failures.

Conservative policy:

- Do not use cache hits until tests prove cache simulation correctness.
- Prefer misses/raw spans initially.
- Add hit references only when encoder cache state is known valid.
- Disable V-Bar for a viewer after any suspected corruption or repeated decode mismatch in test builds.

### 5.5 Glyph cache

Glyph cache is useful for repeated small image/glyph regions, but it is high risk and should be implemented late.

State:

```text
glyph cache: 4000 entries
```

Rules:

- Valid glyph indexes: `0..3999`.
- `GLYPH_HIT` requires `GLYPH_INDEX`.
- If glyph hit is set, no composite payload is present; client copies the cached glyph/bitmap.
- Glyph index should not be used for bitmap areas greater than 1024 square pixels per spec guidance.

Recommended strategy:

- Use only for conservative candidates: small repeated glyph-like regions, repeated icons, repeated UI symbols.
- Use strong hashes and dimensions to avoid false hits.
- Store exact pixel content for validation in debug/test builds.
- Avoid aggressive glyph detection until V-Bar is stable.

Glyph miss/update flow:

```text
emit GLYPH_INDEX without GLYPH_HIT
emit composite payload for the bitmap
client decodes composite payload
client stores decoded bitmap at glyphIndex
```

Glyph hit flow:

```text
emit GLYPH_INDEX | GLYPH_HIT
emit glyphIndex
no composite payload
client copies cached glyph bitmap
```

---

## 6. Proposed OmniRDP design

### 6.1 New files

```text
OmniRDP/include/viewer_gfx_encoder.h
OmniRDP/src/viewer_gfx_encoder.c

OmniRDP/include/viewer_gfx_codec_clear.h
OmniRDP/src/viewer_gfx_codec_clear.c

OmniRDP/tests/test_viewer_gfx_codec_clear.c
```

Optional later split if the file grows too large:

```text
OmniRDP/src/viewer_gfx_codec_clear_stream.c
OmniRDP/src/viewer_gfx_codec_clear_residual.c
OmniRDP/src/viewer_gfx_codec_clear_subcodec.c
OmniRDP/src/viewer_gfx_codec_clear_nsc.c
OmniRDP/src/viewer_gfx_codec_clear_rlex.c
OmniRDP/src/viewer_gfx_codec_clear_vbar.c
OmniRDP/src/viewer_gfx_codec_clear_glyph.c
```

### 6.2 Generic encoder API

```c
typedef enum
{
    VIEWER_GFX_CODEC_UNCOMPRESSED = 0,
    VIEWER_GFX_CODEC_CLEARCODEC = 1
} ViewerGfxCodecKind;

typedef struct ViewerGfxEncoder ViewerGfxEncoder;

ViewerGfxEncoder* viewer_gfx_encoder_new(ViewerGfxCodecKind kind);
void viewer_gfx_encoder_free(ViewerGfxEncoder* encoder);
BOOL viewer_gfx_encoder_reset(ViewerGfxEncoder* encoder, BOOL resetCaches);

BOOL viewer_gfx_encoder_build_surface_command_rect(
    ViewerGfxEncoder* encoder,
    const ViewerFramebufferSnapshot* snapshot,
    UINT16 surfaceId,
    const RECTANGLE_16* rect,
    RDPGFX_SURFACE_COMMAND* command);

void viewer_gfx_encoder_surface_command_reset(RDPGFX_SURFACE_COMMAND* command);
```

### 6.3 ClearCodec-specific API

```c
typedef struct ViewerGfxClearContext ViewerGfxClearContext;

ViewerGfxClearContext* viewer_gfx_clear_context_new(void);
void viewer_gfx_clear_context_free(ViewerGfxClearContext* ctx);
BOOL viewer_gfx_clear_context_reset(ViewerGfxClearContext* ctx, BOOL resetCaches);

BOOL viewer_gfx_clear_build_surface_command_rect(
    ViewerGfxClearContext* ctx,
    const ViewerFramebufferSnapshot* snapshot,
    UINT16 surfaceId,
    const RECTANGLE_16* rect,
    RDPGFX_SURFACE_COMMAND* command);

void viewer_gfx_clear_surface_command_reset(RDPGFX_SURFACE_COMMAND* command);
```

### 6.4 Integration points

Modify:

```text
OmniRDP/src/viewer_server_internal.h
OmniRDP/src/viewer_gfx_pipeline.c
OmniRDP/src/viewer_server.c
OmniRDP/CMakeLists.txt or OmniRDP/tests/CMakeLists.txt as applicable
OmniRDP/tests/CheckRefactorBoundaries.cmake
```

`ViewerGraphicsContext` should own the per-viewer encoder state.

Example conceptual ownership:

```c
typedef struct
{
    /* existing graphics state */
    ViewerGfxEncoder* encoder;
    ViewerGfxCodecKind preferredCodec;
    BOOL clearcodecEnabled;
} ViewerGraphicsContext;
```

`viewer_gfx_pipeline.c` should select a codec and build commands, but it should remain the owner of RDPEGFX sends:

```text
pipeline:
    choose encoder
    build RDPGFX_SURFACE_COMMAND
    StartFrame
    SurfaceCommand
    EndFrame
    reset/free command
```

Codec modules must not call `rdpgfx->StartFrame`, `rdpgfx->SurfaceCommand`, `rdpgfx->EndFrame`, WTS send APIs, or peer/session transport APIs.

---

## 7. Codec selection and tile classifier

A full ClearCodec encoder needs more than serialization. It needs a classifier that decides which layer/subcodec to use for each region.

Recommended tile sizes:

```text
64x64
128x64
128x128
```

Start simple and tune with metrics.

### 7.1 Classification categories

```text
solid / flat
    -> residual

text-like / high-contrast vertical strokes
    -> bands/V-Bar

small repeated glyph-like
    -> glyph cache

image-like / gradient / complex natural image
    -> NSCodec

low-color palette-like
    -> RLEX

incompressible or uncertain
    -> raw subcodec or uncompressed RDPEGFX
```

### 7.2 Always compare against fallback

Before sending ClearCodec, compare encoded size to uncompressed RDPEGFX.

```text
if clearcodec_bytes >= uncompressed_bytes * threshold:
    use uncompressed RDPEGFX
```

Suggested threshold:

```text
0.90 to 0.95
```

### 7.3 Avoid blocking presentation

ClearCodec must be opportunistic. If encoding is too expensive or uncertain, send uncompressed.

Suggested policy:

```text
if encode_time_budget_exceeded:
    fallback to uncompressed for remaining tiles/frame
```

---

## 8. Capability negotiation and configuration

ClearCodec should be disabled by default until interoperability is proven.

Suggested config:

```ini
viewer.gfx.clearcodec_enabled = false
viewer.gfx.clearcodec_nscodec_enabled = false
viewer.gfx.clearcodec_residual_enabled = true
viewer.gfx.clearcodec_rlex_enabled = false
viewer.gfx.clearcodec_vbar_enabled = false
viewer.gfx.clearcodec_glyph_enabled = false
viewer.gfx.clearcodec_fallback_uncompressed = true
viewer.gfx.clearcodec_min_savings_percent = 10
viewer.gfx.clearcodec_experimental = true
```

Enable ClearCodec only if:

- RDPEGFX is active.
- Client caps are compatible.
- Config enables ClearCodec.
- Encoder initialized successfully.
- Viewer is not in degraded/fallback mode.
- No recent ClearCodec failures occurred for that viewer.

ClearCodec is a codec ID carried in `WIRE_TO_SURFACE_1`; it is not a replacement for RDPEGFX caps negotiation.

---

## 9. Late join, reset, resize, and reconnect behavior

ClearCodec state is history-dependent. Late joiners and resets must be handled conservatively.

### 9.1 New viewer / late joiner

Recommended sequence:

```text
allocate fresh ClearCodec state
send ResetGraphics
CreateSurface
MapSurfaceToOutput
send full baseline frame uncompressed
wait for frame acknowledgement if practical
enable ClearCodec for subsequent dirty updates
```

Why uncompressed baseline first:

- It avoids cache dependency during join.
- It establishes known client pixels.
- It simplifies debugging and visual correctness.

Alternative:

- Send a ClearCodec baseline with cache reset and self-contained payload.
- This is possible but should come later.

### 9.2 Reset/resize/reconnect

On any viewer graphics reset, resize, reconnect, or suspected corruption:

```text
disable ClearCodec temporarily
discard/reinitialize ClearCodec state
send uncompressed baseline
re-enable ClearCodec only after clean baseline
```

### 9.3 Cache reset flag

`CLEARCODEC_FLAG_CACHE_RESET` should be used intentionally.

Caveat from practical decoder behavior:

- It resets cache cursors in FreeRDP decoder behavior.
- It may not clear all storage content in the way an implementer might assume.
- Encoder should not depend on stale cache entries after reset.

Recommended policy:

- Treat reset as making all cache-hit references invalid from the encoder point of view.
- Rebuild cache state through misses/self-contained payloads.

---

## 10. Fallback and safety policy

ClearCodec must never be a required path.

Fallback hierarchy:

```text
ClearCodec encode succeeds and is worthwhile
    -> send ClearCodec

ClearCodec encode fails, is too large, or exceeds CPU budget
    -> send uncompressed RDPEGFX

RDPEGFX fails or client becomes incompatible
    -> classic bitmap fallback
```

Per-viewer disable conditions:

- repeated encode failures,
- repeated send failures,
- missing/late frame acknowledgements beyond threshold,
- suspected visual corruption,
- reset/reconnect loop,
- client-specific incompatibility.

Debug/development builds should expose aggressive validation:

```text
encode ClearCodec
decode with FreeRDP clear_decompress locally
compare pixels
only send if roundtrip succeeds
```

This is too expensive for production on every frame, but very valuable while developing each layer.

---

## 11. Implementation phases

### Phase 1: Encoder abstraction

Objective:

- Add `ViewerGfxEncoder` abstraction.
- Preserve existing uncompressed behavior exactly.
- Add metrics/fallback plumbing.

Deliverables:

- `viewer_gfx_encoder.h/.c`.
- `ViewerGraphicsContext` owns encoder state.
- `viewer_gfx_pipeline.c` uses generic encoder calls.
- Tests proving uncompressed output is unchanged.

Exit criteria:

- Existing RDPEGFX uncompressed tests pass.
- Existing viewer behavior unchanged.
- Boundary tests pass.

### Phase 2: Minimal ClearCodec container

Objective:

- Produce spec-valid ClearCodec payloads without meaningful compression.

Implementation:

- Write top-level ClearCodec stream header.
- Write composite payload counts.
- Write one full-rect raw BGR24 subcodec block.
- Set `codecId = RDPGFX_CODECID_CLEARCODEC`.
- Maintain `seqNumber`.

Exit criteria:

- FreeRDP `clear_decompress()` roundtrip succeeds.
- MSTSC accepts a simple ClearCodec frame.
- Fallback to uncompressed works.

### Phase 3: NSCodec subcodec

Objective:

- Wrap FreeRDP-generated NSCodec messages as ClearCodec subcodec ID `0x01`.

Implementation:

- Create/configure per-viewer `NSC_CONTEXT`.
- Encode selected tiles with `nsc_compose_message()`.
- Wrap output in ClearCodec subcodec block.
- Compare size against raw/uncompressed.

Exit criteria:

- NSCodec subcodec roundtrips through `clear_decompress()`.
- MSTSC and FreeRDP client display NSCodec subcodec regions correctly.
- Text regions are not degraded under default settings.

### Phase 4: Residual encoder

Objective:

- Add lossless BGR RLE residual encoding.

Implementation:

- Detect runs over BGR pixels.
- Emit residual run segments.
- Validate run length edge cases: 1, 254, 255, 65534, 65535, larger than 65535.

Exit criteria:

- Residual-only frames roundtrip exactly.
- Solid/flat UI regions compress meaningfully.

### Phase 5: RLEX subcodec

Objective:

- Add ClearCodec subcodec ID `0x02` for palette-like regions.

Implementation:

- Palette builder.
- Color-to-index mapping.
- RLEX run encoder.
- Size heuristic vs residual/raw/NSCodec.

Exit criteria:

- RLEX subcodec roundtrips exactly.
- Palette-like test images compress better than raw.

### Phase 6: Bands/V-Bar encoder

Objective:

- Add text/UI-optimized ClearCodec bands/V-Bar path.

Implementation:

- Band segmentation.
- Dominant background color selection.
- Full V-Bar and Short-V-Bar representation.
- Cache lookup and insert.
- Cursor wrap.
- Cache reset handling.
- Conservative hit emission.

Exit criteria:

- Miss-only V-Bar streams roundtrip.
- Miss-then-hit streams roundtrip.
- Cursor wrap tests pass.
- Text scenarios compress better than residual/raw/uncompressed.
- Long-session text rendering remains visually stable.

### Phase 7: Glyph cache

Objective:

- Add repeated small glyph/icon caching.

Implementation:

- Conservative glyph candidate detection.
- Hash/dimension/pixel-content match.
- Glyph miss/update emission.
- Glyph hit emission.
- Cache replacement policy.

Exit criteria:

- Glyph miss then glyph hit roundtrips exactly.
- No false hits in large synthetic corpus.
- Real typing scenarios remain visually correct.

### Phase 8: Production hardening

Objective:

- Make ClearCodec safe for opt-in production usage.

Work:

- MSTSC compatibility matrix.
- FreeRDP client compatibility matrix.
- Long-session soak tests.
- Multi-viewer tests.
- Late join tests.
- Resize/reconnect tests.
- WAN/high-latency tests.
- CPU/bandwidth metrics.
- Robust fallback tuning.

Exit criteria:

- ClearCodec can be enabled for known-good client/workload combinations.
- Automatic fallback protects correctness.
- Metrics show meaningful savings on target workloads.

---

## 12. Test strategy

### 12.1 Unit tests

Add:

```text
OmniRDP/tests/test_viewer_gfx_codec_clear.c
```

Test cases:

- stream header writing,
- byte counts,
- sequence starts at 0,
- sequence increments,
- sequence wraps 255 -> 0,
- raw subcodec block layout,
- NSCodec subcodec wrapper layout,
- residual run encoding,
- residual edge run lengths,
- RLEX palette/run encoding,
- V-Bar miss,
- V-Bar hit,
- Short-V-Bar miss,
- Short-V-Bar hit,
- V-Bar cursor wrap,
- cache reset,
- glyph miss,
- glyph hit,
- invalid rectangles,
- allocation failure,
- fallback behavior.

### 12.2 Roundtrip validation

Every codec layer should support this validation pattern:

```text
source pixels
    -> OmniRDP ClearCodec encoder
    -> FreeRDP clear_decompress()
    -> decoded pixels
    -> byte-for-byte compare with source
```

Synthetic image corpus:

- 1x1,
- narrow/tall rectangles,
- odd widths,
- odd heights,
- full desktop,
- solid colors,
- checkerboards,
- gradients,
- random noise,
- icons,
- black text on white,
- ClearType text samples,
- terminal output,
- browser text,
- window chrome,
- video-like frames.

### 12.3 Stateful tests

Required state tests:

- multiple sequential frames,
- sequence wrap,
- reset with no cache hits afterwards,
- V-Bar miss then hit,
- Short-V-Bar miss then hit,
- glyph miss then hit,
- cache cursor wrap,
- surface recreate,
- full baseline after dirty updates,
- late join fresh state,
- two viewers with independent state.

### 12.4 Interop tests

Clients:

- Microsoft MSTSC.
- FreeRDP client.
- Optional: IronRDP/Devolutions client.

Scenarios:

- idle desktop,
- typing in Notepad,
- terminal output,
- browser scrolling,
- window dragging,
- opening/closing menus,
- resizing remote desktop,
- reconnect,
- multi-viewer join/leave,
- long idle session,
- high latency / packet delay,
- bandwidth-constrained link.

### 12.5 Safety and fuzz tests

- Invalid dirty rect bounds.
- Huge dimensions / overflow attempts.
- Allocation failures.
- Forced NSCodec failure.
- Forced ClearCodec encode failure.
- Repeated reset loops.
- Randomized tile splits.
- Corrupted generated payloads passed to decoder in isolated tests.

---

## 13. Metrics and observability

Track per viewer:

```text
raw bytes
encoded bytes
compression ratio
encode time
send time
frame ack latency
fallback count
ClearCodec disable reason
NSCodec use count
Residual use count
RLEX use count
V-Bar cache hit/miss count
Short-V-Bar cache hit/miss count
glyph hit/miss count
average dirty area
dirty rect count
frames skipped/coalesced
```

Useful logs:

```text
viewer=<id> gfx codec=clear rect=<x,y,w,h> raw=<bytes> encoded=<bytes> ratio=<r> time_ms=<t>
viewer=<id> gfx clear fallback reason=<reason>
viewer=<id> gfx clear disabled reason=<reason>
viewer=<id> gfx clear reset generation=<n>
```

Go/no-go metrics:

- ClearCodec should beat uncompressed by at least 30-50% on target text/UI workloads before enabling broadly.
- Encoder CPU must fit within frame budget.
- Fallback rate must be low for supported clients.
- No visual corruption in long-session tests.

---

## 14. Risks and mitigations

### 14.1 State drift

Risk: encoder and decoder cache state diverge, causing persistent visual corruption.

Mitigation: roundtrip tests, conservative cache-hit emission, reset on suspicion, and per-viewer independent state.

### 14.2 MSTSC strictness

Risk: FreeRDP may decode streams that MSTSC rejects or renders differently.

Mitigation: test MSTSC from the minimal-container phase onward, avoid ambiguous spec behavior, and prefer self-contained baseline frames.

### 14.3 CPU cost

Risk: compression cost exceeds bandwidth savings.

Mitigation: tile budgets, size/cost heuristics, opportunistic fallback, and metrics-driven enablement.

### 14.4 NSCodec quality loss

Risk: lossy NSCodec settings degrade text, undermining ClearCodec's purpose.

Mitigation: conservative/lossless defaults, avoid NSCodec for text-classified tiles, and make loss settings configurable.

### 14.5 Late joiners

Risk: new viewers miss cache history.

Mitigation: fresh state per viewer, uncompressed baseline first, and enable ClearCodec only after baseline.

### 14.6 Multi-viewer complexity

Risk: shared state or mixed frame histories corrupt one viewer.

Mitigation: never share ClearCodec state; track per-viewer generation and fallback state.

---

## 15. Alternatives and priority guidance

Before committing to full ClearCodec, continue improving lower-risk paths:

1. Dirty-rect coalescing.
2. Frame pacing and backpressure.
3. Full-frame vs dirty-rect heuristics.
4. Uncompressed RDPEGFX stability.
5. Classic bitmap fallback reliability.

ClearCodec is most valuable for:

- text,
- terminal/admin workloads,
- Windows UI,
- icons,
- static or low-motion desktop usage,
- bandwidth-constrained links where text sharpness matters.

ClearCodec is less valuable for:

- video,
- high-motion animation,
- photo-heavy workloads,
- cases where LAN bandwidth is abundant.

For motion-heavy workloads, AVC/H.264 or AVC444 may eventually be a better investment, but they have their own dependencies and text-quality tradeoffs.

---

## 16. Recommended implementation order

Do not jump directly to full ClearCodec.

Recommended sequence:

```text
1. Encoder abstraction
2. Minimal ClearCodec raw-subcodec container
3. NSCodec subcodec wrapper
4. Residual/RLE encoder
5. RLEX subcodec
6. Bands/V-Bar miss-only mode
7. Bands/V-Bar cache-hit mode
8. Glyph miss/update mode
9. Glyph hit mode
10. Production hardening and client matrix
```

Default state throughout development:

```ini
viewer.gfx.clearcodec_enabled = false
```

Only enable by default after:

- MSTSC compatibility is proven.
- FreeRDP client compatibility is proven.
- Multi-viewer state is stable.
- Late join/reset behavior is stable.
- Compression ratio justifies CPU cost.
- Fallback behavior is reliable.

---

## 17. Definition of done for production ClearCodec

A production ClearCodec implementation should satisfy all of the following:

- Supports raw subcodec.
- Supports NSCodec subcodec.
- Supports residual layer.
- Supports RLEX subcodec or explicitly documents why it is excluded.
- Supports bands/V-Bar with cache hits/misses and reset behavior.
- Supports glyph cache hits/misses for conservative candidates.
- Maintains independent per-viewer state.
- Handles late join with clean baseline.
- Handles resize/reset/reconnect safely.
- Falls back to uncompressed RDPEGFX automatically.
- Falls back to classic bitmap if RDPEGFX fails.
- Has roundtrip tests through FreeRDP `clear_decompress()`.
- Has MSTSC interop evidence.
- Has FreeRDP client interop evidence.
- Has long-session soak test evidence.
- Has metrics proving value on target workloads.

---

## 18. Short conclusion

A full ClearCodec encoder with V-Bar, glyph cache, and NSCodec is feasible for OmniRDP, but it is a substantial stateful codec implementation. FreeRDP can be used for RDPEGFX transport, NSCodec encoding, and ClearCodec decode validation, but the ClearCodec encoder itself must be written in OmniRDP or upstreamed into FreeRDP.

The safest strategy is incremental, feature-flagged development with uncompressed RDPEGFX as the correctness baseline and classic bitmap as the final escape hatch.
