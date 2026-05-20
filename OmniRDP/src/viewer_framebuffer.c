#include "viewer_framebuffer.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static BOOL viewer_framebuffer_size(UINT32 height, UINT32 stride,
                                    size_t *pixel_bytes) {
  size_t total = 0;

  if (!pixel_bytes || (height == 0) || (stride == 0))
    return FALSE;

  if ((size_t)height > (SIZE_MAX / (size_t)stride))
    return FALSE;

  total = (size_t)height * (size_t)stride;
  if (total == 0)
    return FALSE;

  *pixel_bytes = total;
  return TRUE;
}

static void
viewer_framebuffer_clear_dirty_locked(ViewerFramebuffer *framebuffer) {
  if (!framebuffer)
    return;

  framebuffer->dirty_rect_count = 0;
  framebuffer->dirty_overflow = FALSE;
}

static BOOL viewer_framebuffer_add_dirty_locked(ViewerFramebuffer *framebuffer,
                                                const RECTANGLE_16 *rect) {
  if (!framebuffer || !rect)
    return FALSE;

  if (framebuffer->dirty_rect_count >= VIEWER_FRAMEBUFFER_MAX_DIRTY_RECTS) {
    framebuffer->dirty_overflow = TRUE;
    return TRUE;
  }

  framebuffer->dirty_rects[framebuffer->dirty_rect_count++] = *rect;
  return TRUE;
}

BOOL viewer_framebuffer_dirty_rect_valid(UINT32 width, UINT32 height,
                                         const RECTANGLE_16 *rect) {
  if ((width == 0) || (height == 0) || !rect)
    return FALSE;

  if ((rect->left > rect->right) || (rect->top > rect->bottom))
    return FALSE;

  if (((UINT32)rect->right >= width) || ((UINT32)rect->bottom >= height))
    return FALSE;

  return TRUE;
}

BOOL viewer_framebuffer_init(ViewerFramebuffer *framebuffer) {
  if (!framebuffer)
    return FALSE;

  if (framebuffer->initialized)
    return TRUE;

  memset(framebuffer, 0, sizeof(*framebuffer));
  if (!InitializeCriticalSectionAndSpinCount(&framebuffer->lock, 4000))
    return FALSE;

  framebuffer->lock_initialized = TRUE;
  framebuffer->initialized = TRUE;
  return TRUE;
}

void viewer_framebuffer_uninit(ViewerFramebuffer *framebuffer) {
  if (!framebuffer)
    return;

  if (framebuffer->lock_initialized)
    EnterCriticalSection(&framebuffer->lock);

  free(framebuffer->pixels);
  framebuffer->pixels = NULL;
  framebuffer->pixel_bytes = 0;
  framebuffer->width = 0;
  framebuffer->height = 0;
  framebuffer->stride = 0;
  framebuffer->pixel_format = 0;
  framebuffer->generation = 0;
  viewer_framebuffer_clear_dirty_locked(framebuffer);

  if (framebuffer->lock_initialized)
    LeaveCriticalSection(&framebuffer->lock);

  if (framebuffer->lock_initialized)
    DeleteCriticalSection(&framebuffer->lock);

  memset(framebuffer, 0, sizeof(*framebuffer));
}

BOOL viewer_framebuffer_resize(ViewerFramebuffer *framebuffer, UINT32 width,
                               UINT32 height, UINT32 stride,
                               UINT32 pixel_format) {
  BYTE *pixels = NULL;
  size_t pixel_bytes = 0;
  RECTANGLE_16 full_rect = {0};

  if (!framebuffer || !framebuffer->initialized)
    return FALSE;

  if ((width == 0) || (height == 0) || (stride == 0))
    return FALSE;

  if ((width > (UINT32)UINT16_MAX) || (height > (UINT32)UINT16_MAX))
    return FALSE;

  if (!viewer_framebuffer_size(height, stride, &pixel_bytes))
    return FALSE;

  pixels = (BYTE *)calloc(1, pixel_bytes);
  if (!pixels)
    return FALSE;

  EnterCriticalSection(&framebuffer->lock);
  free(framebuffer->pixels);
  framebuffer->pixels = pixels;
  framebuffer->pixel_bytes = pixel_bytes;
  framebuffer->width = width;
  framebuffer->height = height;
  framebuffer->stride = stride;
  framebuffer->pixel_format = pixel_format;
  framebuffer->generation++;
  viewer_framebuffer_clear_dirty_locked(framebuffer);

  full_rect.left = 0;
  full_rect.top = 0;
  full_rect.right = (UINT16)((width > 0) ? (width - 1U) : 0U);
  full_rect.bottom = (UINT16)((height > 0) ? (height - 1U) : 0U);
  viewer_framebuffer_add_dirty_locked(framebuffer, &full_rect);
  LeaveCriticalSection(&framebuffer->lock);

  return TRUE;
}

BOOL viewer_framebuffer_update_pixels(ViewerFramebuffer *framebuffer,
                                      const BYTE *pixels, UINT32 source_stride,
                                      const RECTANGLE_16 *dirty_rects,
                                      UINT32 dirty_rect_count) {
  UINT32 y = 0;

  if (!framebuffer || !framebuffer->initialized || !pixels)
    return FALSE;

  if ((dirty_rect_count > 0) && !dirty_rects)
    return FALSE;

  EnterCriticalSection(&framebuffer->lock);
  if (!framebuffer->pixels || (source_stride == 0) ||
      (source_stride < framebuffer->stride)) {
    LeaveCriticalSection(&framebuffer->lock);
    return FALSE;
  }

  for (y = 0; y < framebuffer->height; y++) {
    memmove(framebuffer->pixels + ((size_t)y * framebuffer->stride),
            pixels + ((size_t)y * source_stride), framebuffer->stride);
  }

  viewer_framebuffer_clear_dirty_locked(framebuffer);
  for (y = 0; y < dirty_rect_count; y++)
    viewer_framebuffer_add_dirty_locked(framebuffer, &dirty_rects[y]);

  if (dirty_rect_count == 0) {
    RECTANGLE_16 full_rect = {0};
    full_rect.left = 0;
    full_rect.top = 0;
    full_rect.right =
        (UINT16)((framebuffer->width > 0) ? (framebuffer->width - 1U) : 0U);
    full_rect.bottom =
        (UINT16)((framebuffer->height > 0) ? (framebuffer->height - 1U) : 0U);
    viewer_framebuffer_add_dirty_locked(framebuffer, &full_rect);
  }

  framebuffer->generation++;
  LeaveCriticalSection(&framebuffer->lock);
  return TRUE;
}

BOOL viewer_framebuffer_mark_dirty(ViewerFramebuffer *framebuffer,
                                   const RECTANGLE_16 *rect) {
  BOOL result = FALSE;

  if (!framebuffer || !framebuffer->initialized || !rect)
    return FALSE;

  EnterCriticalSection(&framebuffer->lock);
  result = viewer_framebuffer_add_dirty_locked(framebuffer, rect);
  LeaveCriticalSection(&framebuffer->lock);
  return result;
}

BOOL viewer_framebuffer_snapshot(ViewerFramebuffer *framebuffer,
                                 ViewerFramebufferSnapshot *snapshot) {
  if (!framebuffer || !framebuffer->initialized || !snapshot)
    return FALSE;

  memset(snapshot, 0, sizeof(*snapshot));

  EnterCriticalSection(&framebuffer->lock);
  if (!framebuffer->pixels || (framebuffer->pixel_bytes == 0)) {
    LeaveCriticalSection(&framebuffer->lock);
    return FALSE;
  }

  snapshot->pixels = (BYTE *)malloc(framebuffer->pixel_bytes);
  if (!snapshot->pixels) {
    LeaveCriticalSection(&framebuffer->lock);
    return FALSE;
  }

  memmove(snapshot->pixels, framebuffer->pixels, framebuffer->pixel_bytes);
  snapshot->pixel_bytes = framebuffer->pixel_bytes;
  snapshot->width = framebuffer->width;
  snapshot->height = framebuffer->height;
  snapshot->stride = framebuffer->stride;
  snapshot->pixel_format = framebuffer->pixel_format;
  snapshot->generation = framebuffer->generation;
  snapshot->dirty_rect_count = framebuffer->dirty_rect_count;
  snapshot->dirty_overflow = framebuffer->dirty_overflow;
  memmove(snapshot->dirty_rects, framebuffer->dirty_rects,
          sizeof(snapshot->dirty_rects));
  LeaveCriticalSection(&framebuffer->lock);
  return TRUE;
}

void viewer_framebuffer_snapshot_free(ViewerFramebufferSnapshot *snapshot) {
  if (!snapshot)
    return;

  free(snapshot->pixels);
  memset(snapshot, 0, sizeof(*snapshot));
}
