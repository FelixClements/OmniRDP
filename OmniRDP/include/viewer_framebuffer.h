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

typedef struct {
  UINT32 width;
  UINT32 height;
  UINT32 stride;
  UINT32 pixel_format;
  UINT64 generation;
  BYTE *pixels;
  size_t pixel_bytes;
  RECTANGLE_16 dirty_rects[VIEWER_FRAMEBUFFER_MAX_DIRTY_RECTS];
  UINT32 dirty_rect_count;
  BOOL dirty_overflow;
} ViewerFramebufferSnapshot;

typedef struct {
  BOOL initialized;
  BOOL lock_initialized;
  UINT32 width;
  UINT32 height;
  UINT32 stride;
  UINT32 pixel_format;
  UINT64 generation;
  BYTE *pixels;
  size_t pixel_bytes;
  RECTANGLE_16 dirty_rects[VIEWER_FRAMEBUFFER_MAX_DIRTY_RECTS];
  UINT32 dirty_rect_count;
  BOOL dirty_overflow;
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
BOOL viewer_framebuffer_mark_dirty(ViewerFramebuffer *framebuffer,
                                   const RECTANGLE_16 *rect);
BOOL viewer_framebuffer_snapshot(ViewerFramebuffer *framebuffer,
                                 ViewerFramebufferSnapshot *snapshot);
void viewer_framebuffer_snapshot_free(ViewerFramebufferSnapshot *snapshot);

#ifdef __cplusplus
}
#endif

#endif /* VIEWER_FRAMEBUFFER_H */
