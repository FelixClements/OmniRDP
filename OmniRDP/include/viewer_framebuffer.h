#ifndef VIEWER_FRAMEBUFFER_H
#define VIEWER_FRAMEBUFFER_H

#include <freerdp/channels/rdpgfx.h>
#include <stddef.h>
#include <winpr/synch.h>
#include <winpr/wtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VIEWER_FRAMEBUFFER_MAX_DIRTY_RECTS 256U

/* Canonical framebuffer contract:
 * - Pixels are stored top-down, row-major, with row 0 at the top of the
 *   desktop. Bottom-up DIB sources must be converted by the caller before
 *   update_pixels.
 * - stride is the byte distance between successive rows and may be larger than
 *   width * bytes_per_pixel. update_pixels copies exactly framebuffer->stride
 *   bytes per row from a source whose stride is at least that large.
 * - pixel_format is the FreeRDP pixel-format value associated with the stored
 *   bytes. The initial RDPEGFX plan expects a 32-bit BGRX/XRGB-style format;
 *   alpha is not authoritative and should be treated as ignored/opaque by
 *   publishers/codecs unless a later story explicitly changes that contract.
 * - Dirty RECTANGLE_16 values use inclusive left/top/right/bottom coordinates.
 *   A full 800x600 framebuffer is represented as left=0, top=0, right=799,
 *   bottom=599. Callers must not pass exclusive right/bottom bounds here.
 * - This module owns canonical pixels, generations, and dirty rectangles only.
 *   It must not call FreeRDP send APIs or own RDPEGFX protocol context state.
 */

typedef struct {
  UINT32 width;
  UINT32 height;
  UINT32 stride;
  UINT32 pixel_format;
  UINT64 generation;
  UINT64 last_update_ts_ms;
  BYTE *pixels;
  size_t pixel_bytes;
  UINT32 pixel_origin_x;
  UINT32 pixel_origin_y;
  UINT32 pixel_width;
  UINT32 pixel_height;
  RECTANGLE_16 dirty_rects[VIEWER_FRAMEBUFFER_MAX_DIRTY_RECTS];
  UINT32 dirty_rect_count;
  BOOL dirty_overflow;
} ViewerFramebufferSnapshot;

typedef struct {
  UINT64 last_update_dirty_bytes;
  UINT64 last_update_copied_bytes;
  UINT64 last_update_full_frame_bytes;
  UINT64 last_update_copy_start_us;
  UINT64 last_update_copy_time_us;
  UINT64 last_update_dirty_rect_count;
  UINT64 last_snapshot_copied_bytes;
  UINT64 last_snapshot_copy_start_us;
  UINT64 last_snapshot_copy_time_us;
} ViewerFramebufferMetrics;

typedef struct {
  BOOL initialized;
  BOOL lock_initialized;
  UINT32 width;
  UINT32 height;
  UINT32 stride;
  UINT32 pixel_format;
  UINT64 generation;
  UINT64 last_update_ts_ms;
  BYTE *pixels;
  size_t pixel_bytes;
  RECTANGLE_16 dirty_rects[VIEWER_FRAMEBUFFER_MAX_DIRTY_RECTS];
  UINT32 dirty_rect_count;
  BOOL dirty_overflow;
  ViewerFramebufferMetrics metrics;
  CRITICAL_SECTION lock;
} ViewerFramebuffer;

BOOL viewer_framebuffer_init(ViewerFramebuffer *framebuffer);
void viewer_framebuffer_uninit(ViewerFramebuffer *framebuffer);

BOOL viewer_framebuffer_resize(ViewerFramebuffer *framebuffer, UINT32 width,
                               UINT32 height, UINT32 stride,
                               UINT32 pixel_format);
BOOL viewer_framebuffer_update_pixels(ViewerFramebuffer *framebuffer,
                                      const BYTE *pixels, UINT32 source_stride,
                                      const RECTANGLE_16 *dirty_rects,
                                      UINT32 dirty_rect_count);
BOOL viewer_framebuffer_dirty_rect_valid(UINT32 width, UINT32 height,
                                         const RECTANGLE_16 *rect);
BOOL viewer_framebuffer_mark_dirty(ViewerFramebuffer *framebuffer,
                                   const RECTANGLE_16 *rect);
BOOL viewer_framebuffer_snapshot(ViewerFramebuffer *framebuffer,
                                 ViewerFramebufferSnapshot *snapshot);
BOOL viewer_framebuffer_dirty_snapshot(ViewerFramebuffer *framebuffer,
                                       UINT32 max_dirty_rect_count,
                                       ViewerFramebufferSnapshot *snapshot);
BOOL viewer_framebuffer_snapshot_dirty_rects(
    ViewerFramebuffer *framebuffer, const RECTANGLE_16 *dirty_rects,
    UINT32 dirty_rect_count, ViewerFramebufferSnapshot *snapshot);
void viewer_framebuffer_snapshot_free(ViewerFramebufferSnapshot *snapshot);
BOOL viewer_framebuffer_get_metrics(ViewerFramebuffer *framebuffer,
                                    ViewerFramebufferMetrics *metrics);

#ifdef __cplusplus
}
#endif

#endif /* VIEWER_FRAMEBUFFER_H */
