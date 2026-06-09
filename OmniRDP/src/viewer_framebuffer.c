#include "viewer_framebuffer.h"

#include "platform_compat.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VIEWER_FRAMEBUFFER_BYTES_PER_PIXEL 4U

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

static BOOL viewer_framebuffer_dirty_rects_valid(UINT32 width, UINT32 height,
                                                 const RECTANGLE_16 *rects,
                                                 UINT32 rect_count) {
  UINT32 i = 0;

  if (rect_count == 0)
    return TRUE;
  if (!rects)
    return FALSE;

  for (i = 0; i < rect_count; i++) {
    if (!viewer_framebuffer_dirty_rect_valid(width, height, &rects[i]))
      return FALSE;
  }

  return TRUE;
}

static UINT64 viewer_framebuffer_dirty_bytes(const RECTANGLE_16 *rects,
                                             UINT32 rect_count,
                                             UINT64 full_frame_bytes) {
  UINT64 dirty_bytes = 0;
  UINT32 i = 0;

  if ((rect_count == 0) || !rects)
    return full_frame_bytes;

  for (i = 0; i < rect_count; i++) {
    const RECTANGLE_16 *rect = &rects[i];
    const UINT32 width = (UINT32)rect->right - (UINT32)rect->left + 1U;
    const UINT32 height = (UINT32)rect->bottom - (UINT32)rect->top + 1U;
    const UINT64 rect_bytes = (UINT64)width * (UINT64)height *
                              (UINT64)VIEWER_FRAMEBUFFER_BYTES_PER_PIXEL;

    if ((UINT64_MAX - dirty_bytes) < rect_bytes)
      return UINT64_MAX;
    dirty_bytes += rect_bytes;
  }

  return dirty_bytes;
}

static UINT64 viewer_framebuffer_copy_full_frame_locked(
    ViewerFramebuffer *framebuffer, const BYTE *pixels, UINT32 source_stride) {
  UINT32 y = 0;

  for (y = 0; y < framebuffer->height; y++) {
    memmove(framebuffer->pixels + ((size_t)y * framebuffer->stride),
            pixels + ((size_t)y * source_stride), framebuffer->stride);
  }

  return (UINT64)framebuffer->pixel_bytes;
}

static UINT64 viewer_framebuffer_copy_dirty_rects_locked(
    ViewerFramebuffer *framebuffer, const BYTE *pixels, UINT32 source_stride,
    const RECTANGLE_16 *dirty_rects, UINT32 dirty_rect_count) {
  UINT64 copied_bytes = 0;
  UINT32 i = 0;

  for (i = 0; i < dirty_rect_count; i++) {
    const RECTANGLE_16 *rect = &dirty_rects[i];
    const UINT32 rect_width = (UINT32)rect->right - (UINT32)rect->left + 1U;
    const UINT32 rect_height = (UINT32)rect->bottom - (UINT32)rect->top + 1U;
    const size_t row_bytes =
        (size_t)rect_width * (size_t)VIEWER_FRAMEBUFFER_BYTES_PER_PIXEL;
    const size_t row_offset =
        (size_t)rect->left * (size_t)VIEWER_FRAMEBUFFER_BYTES_PER_PIXEL;
    UINT32 y = 0;

    for (y = 0; y < rect_height; y++) {
      const UINT32 source_y = (UINT32)rect->top + y;
      memmove(framebuffer->pixels + ((size_t)source_y * framebuffer->stride) +
                  row_offset,
              pixels + ((size_t)source_y * source_stride) + row_offset,
              row_bytes);
      if ((UINT64_MAX - copied_bytes) < (UINT64)row_bytes)
        copied_bytes = UINT64_MAX;
      else
        copied_bytes += (UINT64)row_bytes;
    }
  }

  return copied_bytes;
}

static BOOL viewer_framebuffer_dirty_bounds(const RECTANGLE_16 *dirty_rects,
                                            UINT32 dirty_rect_count,
                                            RECTANGLE_16 *bounds) {
  UINT32 i = 0;

  if (!dirty_rects || (dirty_rect_count == 0) || !bounds)
    return FALSE;

  *bounds = dirty_rects[0];
  for (i = 1; i < dirty_rect_count; i++) {
    const RECTANGLE_16 *rect = &dirty_rects[i];
    if (rect->left < bounds->left)
      bounds->left = rect->left;
    if (rect->top < bounds->top)
      bounds->top = rect->top;
    if (rect->right > bounds->right)
      bounds->right = rect->right;
    if (rect->bottom > bounds->bottom)
      bounds->bottom = rect->bottom;
  }

  return TRUE;
}

static BOOL
viewer_framebuffer_snapshot_copy_locked(ViewerFramebuffer *framebuffer,
                                        const RECTANGLE_16 *copy_rect,
                                        ViewerFramebufferSnapshot *snapshot) {
  UINT32 y = 0;
  UINT64 copy_start_us = 0;
  UINT64 copy_time_us = 0;
  UINT32 copy_width = 0;
  UINT32 copy_height = 0;
  UINT32 copy_stride = 0;
  UINT32 copy_row_bytes = 0;
  size_t pixel_bytes = 0;
  BYTE *pixels = NULL;

  if (!framebuffer || !copy_rect || !snapshot)
    return FALSE;

  if (!viewer_framebuffer_dirty_rect_valid(framebuffer->width,
                                           framebuffer->height, copy_rect))
    return FALSE;

  copy_width = (UINT32)copy_rect->right - (UINT32)copy_rect->left + 1U;
  copy_height = (UINT32)copy_rect->bottom - (UINT32)copy_rect->top + 1U;
  if ((copy_width == 0) || (copy_height == 0) ||
      (copy_width > (UINT32)UINT16_MAX) || (copy_height > (UINT32)UINT16_MAX))
    return FALSE;
  if (copy_width > (UINT32_MAX / VIEWER_FRAMEBUFFER_BYTES_PER_PIXEL))
    return FALSE;
  copy_row_bytes = copy_width * VIEWER_FRAMEBUFFER_BYTES_PER_PIXEL;
  if (((UINT32)copy_rect->left == 0) && (copy_width == framebuffer->width))
    copy_stride = framebuffer->stride;
  else
    copy_stride = copy_row_bytes;
  if (!viewer_framebuffer_size(copy_height, copy_stride, &pixel_bytes))
    return FALSE;

  pixels = (BYTE *)malloc(pixel_bytes);
  if (!pixels)
    return FALSE;

  copy_start_us = platform_get_timestamp_us();
  for (y = 0; y < copy_height; y++) {
    const UINT32 source_y = (UINT32)copy_rect->top + y;
    const size_t source_offset =
        ((size_t)source_y * (size_t)framebuffer->stride) +
        ((size_t)copy_rect->left * (size_t)VIEWER_FRAMEBUFFER_BYTES_PER_PIXEL);
    const size_t destination_offset = (size_t)y * (size_t)copy_stride;
    memmove(pixels + destination_offset, framebuffer->pixels + source_offset,
            copy_stride);
  }
  copy_time_us = platform_get_timestamp_us() - copy_start_us;

  snapshot->pixels = pixels;
  snapshot->pixel_bytes = pixel_bytes;
  snapshot->pixel_origin_x = (UINT32)copy_rect->left;
  snapshot->pixel_origin_y = (UINT32)copy_rect->top;
  snapshot->pixel_width = copy_width;
  snapshot->pixel_height = copy_height;
  snapshot->width = framebuffer->width;
  snapshot->height = framebuffer->height;
  snapshot->stride = copy_stride;
  snapshot->pixel_format = framebuffer->pixel_format;
  snapshot->generation = framebuffer->generation;
  snapshot->last_update_ts_ms = framebuffer->last_update_ts_ms;
  snapshot->dirty_rect_count = framebuffer->dirty_rect_count;
  snapshot->dirty_overflow = framebuffer->dirty_overflow;
  memmove(snapshot->dirty_rects, framebuffer->dirty_rects,
          sizeof(snapshot->dirty_rects));

  framebuffer->metrics.last_snapshot_copied_bytes = (UINT64)pixel_bytes;
  framebuffer->metrics.last_snapshot_copy_start_us = copy_start_us;
  framebuffer->metrics.last_snapshot_copy_time_us = copy_time_us;
  return TRUE;
}

static BOOL viewer_framebuffer_full_rect_locked(ViewerFramebuffer *framebuffer,
                                                RECTANGLE_16 *full_rect) {
  if (!framebuffer || !full_rect || (framebuffer->width == 0) ||
      (framebuffer->height == 0))
    return FALSE;

  full_rect->left = 0;
  full_rect->top = 0;
  full_rect->right = (UINT16)(framebuffer->width - 1U);
  full_rect->bottom = (UINT16)(framebuffer->height - 1U);
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
  memset(&framebuffer->metrics, 0, sizeof(framebuffer->metrics));
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
  framebuffer->last_update_ts_ms = platform_get_timestamp_ms();
  memset(&framebuffer->metrics, 0, sizeof(framebuffer->metrics));
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
  UINT64 copied_bytes = 0;
  UINT64 copy_start_us = 0;
  UINT64 copy_time_us = 0;

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

  if (!viewer_framebuffer_dirty_rects_valid(framebuffer->width,
                                            framebuffer->height, dirty_rects,
                                            dirty_rect_count)) {
    LeaveCriticalSection(&framebuffer->lock);
    return FALSE;
  }

  framebuffer->metrics.last_update_dirty_bytes = viewer_framebuffer_dirty_bytes(
      dirty_rects, dirty_rect_count, (UINT64)framebuffer->pixel_bytes);
  framebuffer->metrics.last_update_full_frame_bytes =
      (UINT64)framebuffer->pixel_bytes;
  framebuffer->metrics.last_update_dirty_rect_count = dirty_rect_count;

  copy_start_us = platform_get_timestamp_us();
  if (dirty_rect_count == 0)
    copied_bytes = viewer_framebuffer_copy_full_frame_locked(
        framebuffer, pixels, source_stride);
  else
    copied_bytes = viewer_framebuffer_copy_dirty_rects_locked(
        framebuffer, pixels, source_stride, dirty_rects, dirty_rect_count);
  copy_time_us = platform_get_timestamp_us() - copy_start_us;
  framebuffer->metrics.last_update_copied_bytes = copied_bytes;
  framebuffer->metrics.last_update_copy_start_us = copy_start_us;
  framebuffer->metrics.last_update_copy_time_us = copy_time_us;

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
  framebuffer->last_update_ts_ms = platform_get_timestamp_ms();
  LeaveCriticalSection(&framebuffer->lock);
  return TRUE;
}

BOOL viewer_framebuffer_mark_dirty(ViewerFramebuffer *framebuffer,
                                   const RECTANGLE_16 *rect) {
  BOOL result = FALSE;

  if (!framebuffer || !framebuffer->initialized || !rect)
    return FALSE;

  EnterCriticalSection(&framebuffer->lock);
  if (!viewer_framebuffer_dirty_rect_valid(framebuffer->width,
                                           framebuffer->height, rect)) {
    LeaveCriticalSection(&framebuffer->lock);
    return FALSE;
  }

  result = viewer_framebuffer_add_dirty_locked(framebuffer, rect);
  LeaveCriticalSection(&framebuffer->lock);
  return result;
}

BOOL viewer_framebuffer_snapshot(ViewerFramebuffer *framebuffer,
                                 ViewerFramebufferSnapshot *snapshot) {
  RECTANGLE_16 full_rect = {0};
  BOOL result = FALSE;

  if (!framebuffer || !framebuffer->initialized || !snapshot)
    return FALSE;

  memset(snapshot, 0, sizeof(*snapshot));

  EnterCriticalSection(&framebuffer->lock);
  if (!framebuffer->pixels || (framebuffer->pixel_bytes == 0)) {
    LeaveCriticalSection(&framebuffer->lock);
    return FALSE;
  }

  result = viewer_framebuffer_full_rect_locked(framebuffer, &full_rect) &&
           viewer_framebuffer_snapshot_copy_locked(framebuffer, &full_rect,
                                                   snapshot);
  LeaveCriticalSection(&framebuffer->lock);
  return result;
}

BOOL viewer_framebuffer_dirty_snapshot(ViewerFramebuffer *framebuffer,
                                       UINT32 max_dirty_rect_count,
                                       ViewerFramebufferSnapshot *snapshot) {
  RECTANGLE_16 copy_rect = {0};
  BOOL result = FALSE;

  if (!framebuffer || !framebuffer->initialized || !snapshot)
    return FALSE;

  memset(snapshot, 0, sizeof(*snapshot));

  EnterCriticalSection(&framebuffer->lock);
  if (!framebuffer->pixels || (framebuffer->pixel_bytes == 0)) {
    LeaveCriticalSection(&framebuffer->lock);
    return FALSE;
  }

  if (framebuffer->dirty_overflow || (framebuffer->dirty_rect_count == 0) ||
      ((max_dirty_rect_count > 0) &&
       (framebuffer->dirty_rect_count > max_dirty_rect_count))) {
    result = viewer_framebuffer_full_rect_locked(framebuffer, &copy_rect);
  } else {
    result = viewer_framebuffer_dirty_bounds(
        framebuffer->dirty_rects, framebuffer->dirty_rect_count, &copy_rect);
  }

  if (result)
    result = viewer_framebuffer_snapshot_copy_locked(framebuffer, &copy_rect,
                                                     snapshot);
  LeaveCriticalSection(&framebuffer->lock);
  return result;
}

BOOL viewer_framebuffer_snapshot_dirty_rects(
    ViewerFramebuffer *framebuffer, const RECTANGLE_16 *dirty_rects,
    UINT32 dirty_rect_count, ViewerFramebufferSnapshot *snapshot) {
  RECTANGLE_16 copy_rect = {0};
  BOOL result = FALSE;

  if (!framebuffer || !framebuffer->initialized || !snapshot || !dirty_rects ||
      (dirty_rect_count == 0) ||
      (dirty_rect_count > VIEWER_FRAMEBUFFER_MAX_DIRTY_RECTS))
    return FALSE;

  memset(snapshot, 0, sizeof(*snapshot));

  EnterCriticalSection(&framebuffer->lock);
  if (!framebuffer->pixels || (framebuffer->pixel_bytes == 0)) {
    LeaveCriticalSection(&framebuffer->lock);
    return FALSE;
  }

  if (!viewer_framebuffer_dirty_rects_valid(framebuffer->width,
                                            framebuffer->height, dirty_rects,
                                            dirty_rect_count)) {
    LeaveCriticalSection(&framebuffer->lock);
    return FALSE;
  }

  result = viewer_framebuffer_dirty_bounds(dirty_rects, dirty_rect_count,
                                           &copy_rect);
  if (result)
    result = viewer_framebuffer_snapshot_copy_locked(framebuffer, &copy_rect,
                                                     snapshot);
  if (result) {
    snapshot->dirty_rect_count = dirty_rect_count;
    snapshot->dirty_overflow = FALSE;
    memmove(snapshot->dirty_rects, dirty_rects,
            (size_t)dirty_rect_count * sizeof(snapshot->dirty_rects[0]));
  }
  LeaveCriticalSection(&framebuffer->lock);
  return result;
}

void viewer_framebuffer_snapshot_free(ViewerFramebufferSnapshot *snapshot) {
  if (!snapshot)
    return;

  free(snapshot->pixels);
  memset(snapshot, 0, sizeof(*snapshot));
}

BOOL viewer_framebuffer_get_metrics(ViewerFramebuffer *framebuffer,
                                    ViewerFramebufferMetrics *metrics) {
  if (!framebuffer || !framebuffer->initialized || !metrics)
    return FALSE;

  EnterCriticalSection(&framebuffer->lock);
  *metrics = framebuffer->metrics;
  LeaveCriticalSection(&framebuffer->lock);
  return TRUE;
}
