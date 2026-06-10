#include "viewer_gfx_codec_rfx.h"

#include <freerdp/codec/color.h>
#include <freerdp/codec/rfx.h>
#include <freerdp/settings_types.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct ViewerGfxRfxContext {
  RFX_CONTEXT *rfx;
#ifdef VIEWER_GFX_RFX_TESTING
  UINT32 threading_flags;
#endif
};

#ifdef VIEWER_GFX_RFX_TESTING
static BOOL g_viewer_gfx_rfx_force_context_new_failure = FALSE;
static UINT32 g_viewer_gfx_rfx_last_threading_flags =
    THREADING_FLAGS_DISABLE_THREADS;
#endif

BOOL viewer_gfx_rfx_is_available(void) { return TRUE; }

const char *viewer_gfx_rfx_disabled_reason(void) { return NULL; }

static void
viewer_gfx_rfx_pixel_bounds(const ViewerFramebufferSnapshot *snapshot,
                            UINT32 *origin_x, UINT32 *origin_y,
                            UINT32 *pixel_width, UINT32 *pixel_height) {
  if (!snapshot)
    return;

  if (origin_x)
    *origin_x = snapshot->pixel_width ? snapshot->pixel_origin_x : 0U;
  if (origin_y)
    *origin_y = snapshot->pixel_height ? snapshot->pixel_origin_y : 0U;
  if (pixel_width)
    *pixel_width =
        snapshot->pixel_width ? snapshot->pixel_width : snapshot->width;
  if (pixel_height)
    *pixel_height =
        snapshot->pixel_height ? snapshot->pixel_height : snapshot->height;
}

#ifdef VIEWER_GFX_RFX_TESTING
void viewer_gfx_rfx_test_set_force_context_new_failure(BOOL force_failure) {
  g_viewer_gfx_rfx_force_context_new_failure = force_failure;
}

UINT32 viewer_gfx_rfx_test_last_threading_flags(void) {
  return g_viewer_gfx_rfx_last_threading_flags;
}

UINT32
viewer_gfx_rfx_test_context_threading_flags(ViewerGfxRfxContext *context) {
  return context ? context->threading_flags : UINT32_MAX;
}
#endif

ViewerGfxRfxContext *viewer_gfx_rfx_context_new(void) {
  return viewer_gfx_rfx_context_new_ex(FALSE);
}

ViewerGfxRfxContext *viewer_gfx_rfx_context_new_ex(BOOL threaded) {
  ViewerGfxRfxContext *context = NULL;
  RFX_CONTEXT *rfx = NULL;
  UINT32 threading_flags =
      threaded ? 0U : (UINT32)THREADING_FLAGS_DISABLE_THREADS;

#ifdef VIEWER_GFX_RFX_TESTING
  g_viewer_gfx_rfx_last_threading_flags = threading_flags;
  if (g_viewer_gfx_rfx_force_context_new_failure)
    return NULL;
#endif

  context = (ViewerGfxRfxContext *)calloc(1, sizeof(*context));
  if (!context)
    return NULL;

  rfx = rfx_context_new_ex(TRUE, threading_flags);
  if (!rfx) {
    free(context);
    return NULL;
  }

  if (!rfx_context_set_mode(rfx, RLGR3)) {
    rfx_context_free(rfx);
    free(context);
    return NULL;
  }
  rfx_context_set_pixel_format(rfx, PIXEL_FORMAT_BGRX32);

  context->rfx = rfx;
#ifdef VIEWER_GFX_RFX_TESTING
  context->threading_flags = threading_flags;
#endif
  return context;
}

void viewer_gfx_rfx_context_free(ViewerGfxRfxContext *context) {
  if (!context)
    return;

  rfx_context_free(context->rfx);
  free(context);
}

BOOL viewer_gfx_rfx_context_reset(ViewerGfxRfxContext *context, UINT32 width,
                                  UINT32 height) {
  if (!context || !context->rfx || (width == 0) || (height == 0) ||
      (width > (UINT32)UINT16_MAX) || (height > (UINT32)UINT16_MAX))
    return FALSE;

  return rfx_context_reset(context->rfx, width, height);
}

static BOOL
viewer_gfx_rfx_validate_snapshot(const ViewerFramebufferSnapshot *snapshot) {
  size_t minimum_pixel_bytes = 0;
  size_t tight_row_bytes = 0;
  UINT32 origin_x = 0;
  UINT32 origin_y = 0;
  UINT32 pixel_width = 0;
  UINT32 pixel_height = 0;

  if (!snapshot || !snapshot->pixels || (snapshot->width == 0) ||
      (snapshot->height == 0))
    return FALSE;

  if ((snapshot->width > (UINT32)UINT16_MAX) ||
      (snapshot->height > (UINT32)UINT16_MAX))
    return FALSE;

  if (snapshot->pixel_format != PIXEL_FORMAT_BGRX32)
    return FALSE;

  viewer_gfx_rfx_pixel_bounds(snapshot, &origin_x, &origin_y, &pixel_width,
                              &pixel_height);
  if ((pixel_width == 0) || (pixel_height == 0) ||
      (origin_x >= snapshot->width) || (origin_y >= snapshot->height) ||
      (pixel_width > (snapshot->width - origin_x)) ||
      (pixel_height > (snapshot->height - origin_y)))
    return FALSE;

  if ((size_t)pixel_width > (SIZE_MAX / 4U))
    return FALSE;
  tight_row_bytes = (size_t)pixel_width * 4U;

  if ((snapshot->stride == 0) || ((size_t)snapshot->stride < tight_row_bytes))
    return FALSE;

  if ((size_t)(pixel_height - 1U) >
      ((SIZE_MAX - tight_row_bytes) / (size_t)snapshot->stride))
    return FALSE;
  minimum_pixel_bytes =
      ((size_t)(pixel_height - 1U) * (size_t)snapshot->stride) +
      tight_row_bytes;

  if (snapshot->pixel_bytes < minimum_pixel_bytes)
    return FALSE;

  return TRUE;
}

static BOOL
viewer_gfx_rfx_bounds_available(const ViewerFramebufferSnapshot *snapshot,
                                UINT32 left, UINT32 top, UINT32 right,
                                UINT32 bottom) {
  UINT32 origin_x = 0;
  UINT32 origin_y = 0;
  UINT32 pixel_width = 0;
  UINT32 pixel_height = 0;
  UINT32 pixel_right = 0;
  UINT32 pixel_bottom = 0;

  if (!snapshot || (left >= right) || (top >= bottom))
    return FALSE;

  viewer_gfx_rfx_pixel_bounds(snapshot, &origin_x, &origin_y, &pixel_width,
                              &pixel_height);
  pixel_right = origin_x + pixel_width;
  pixel_bottom = origin_y + pixel_height;
  return (left >= origin_x) && (top >= origin_y) && (right <= pixel_right) &&
         (bottom <= pixel_bottom);
}

static BOOL
viewer_gfx_rfx_validate_rect(const ViewerFramebufferSnapshot *snapshot,
                             const RECTANGLE_16 *dirty_rect, UINT32 *left,
                             UINT32 *top, UINT32 *right, UINT32 *bottom) {
  if (!snapshot || !dirty_rect || !left || !top || !right || !bottom)
    return FALSE;

  if ((dirty_rect->left > dirty_rect->right) ||
      (dirty_rect->top > dirty_rect->bottom))
    return FALSE;

  if (((UINT32)dirty_rect->right >= snapshot->width) ||
      ((UINT32)dirty_rect->bottom >= snapshot->height))
    return FALSE;

  /* Framebuffer dirty RECTANGLE_16 values are inclusive. RDPEGFX surface
   * commands use exclusive bounds, so convert right/bottom with +1 here. */
  *left = (UINT32)dirty_rect->left;
  *top = (UINT32)dirty_rect->top;
  *right = (UINT32)dirty_rect->right + 1U;
  *bottom = (UINT32)dirty_rect->bottom + 1U;

  if ((*left >= *right) || (*top >= *bottom))
    return FALSE;

  return TRUE;
}

static BOOL viewer_gfx_rfx_build_surface_command_bounds(
    ViewerGfxRfxContext *context, const ViewerFramebufferSnapshot *snapshot,
    UINT16 surface_id, UINT32 left, UINT32 top, UINT32 right, UINT32 bottom,
    const RFX_RECT *rfx_rects, size_t rfx_rect_count,
    RDPGFX_SURFACE_COMMAND *command) {
  BYTE *data = NULL;
  const BYTE *source = NULL;
  RFX_RECT full_rect = {0};
  UINT32 rect_width = 0;
  UINT32 rect_height = 0;
  UINT32 origin_x = 0;
  UINT32 origin_y = 0;
  size_t payload_bytes = 0;
  wStream *stream = NULL;

  if (!context || !context->rfx ||
      !viewer_gfx_rfx_validate_snapshot(snapshot) || !command)
    return FALSE;

  if ((left >= right) || (top >= bottom) || (right > snapshot->width) ||
      (bottom > snapshot->height))
    return FALSE;
  if (!viewer_gfx_rfx_bounds_available(snapshot, left, top, right, bottom))
    return FALSE;

  rect_width = right - left;
  rect_height = bottom - top;
  if ((rect_width == 0) || (rect_height == 0) ||
      (rect_width > (UINT32)UINT16_MAX) || (rect_height > (UINT32)UINT16_MAX))
    return FALSE;

  if (!viewer_gfx_rfx_context_reset(context, rect_width, rect_height))
    return FALSE;

  stream = Stream_New(NULL, 1024);
  if (!stream)
    return FALSE;

  viewer_gfx_rfx_pixel_bounds(snapshot, &origin_x, &origin_y, NULL, NULL);
  source = snapshot->pixels +
           ((size_t)(top - origin_y) * (size_t)snapshot->stride) +
           ((size_t)(left - origin_x) * 4U);

  if (!rfx_rects || (rfx_rect_count == 0)) {
    full_rect.x = 0;
    full_rect.y = 0;
    full_rect.width = (UINT16)rect_width;
    full_rect.height = (UINT16)rect_height;
    rfx_rects = &full_rect;
    rfx_rect_count = 1;
  }

  if (!rfx_compose_message(context->rfx, stream, rfx_rects, rfx_rect_count,
                           source, rect_width, rect_height, snapshot->stride)) {
    Stream_Free(stream, TRUE);
    return FALSE;
  }

  payload_bytes = Stream_GetPosition(stream);
  if ((payload_bytes == 0) || (payload_bytes > UINT32_MAX)) {
    Stream_Free(stream, TRUE);
    return FALSE;
  }

  data = (BYTE *)malloc(payload_bytes);
  if (!data) {
    Stream_Free(stream, TRUE);
    return FALSE;
  }

  memmove(data, Stream_Buffer(stream), payload_bytes);
  Stream_Free(stream, TRUE);

  memset(command, 0, sizeof(*command));
  command->surfaceId = surface_id;
  command->codecId = RDPGFX_CODECID_CAVIDEO;
  command->contextId = 0;
  command->format = PIXEL_FORMAT_BGRX32;
  command->left = (UINT16)left;
  command->top = (UINT16)top;
  command->right = (UINT16)right;
  command->bottom = (UINT16)bottom;
  command->width = (UINT16)rect_width;
  command->height = (UINT16)rect_height;
  command->length = (UINT32)payload_bytes;
  command->data = data;
  return TRUE;
}

BOOL viewer_gfx_rfx_build_surface_command(
    ViewerGfxRfxContext *context, const ViewerFramebufferSnapshot *snapshot,
    UINT16 surface_id, RDPGFX_SURFACE_COMMAND *command) {
  if (!command)
    return FALSE;

  viewer_gfx_rfx_surface_command_reset(command);

  if (!snapshot)
    return FALSE;

  return viewer_gfx_rfx_build_surface_command_bounds(
      context, snapshot, surface_id, 0, 0, snapshot->width, snapshot->height,
      NULL, 0, command);
}

BOOL viewer_gfx_rfx_build_surface_command_rect(
    ViewerGfxRfxContext *context, const ViewerFramebufferSnapshot *snapshot,
    UINT16 surface_id, const RECTANGLE_16 *dirty_rect,
    RDPGFX_SURFACE_COMMAND *command) {
  UINT32 left = 0;
  UINT32 top = 0;
  UINT32 right = 0;
  UINT32 bottom = 0;

  if (!command)
    return FALSE;

  viewer_gfx_rfx_surface_command_reset(command);

  if (!viewer_gfx_rfx_validate_snapshot(snapshot) ||
      !viewer_gfx_rfx_validate_rect(snapshot, dirty_rect, &left, &top, &right,
                                    &bottom))
    return FALSE;

  return viewer_gfx_rfx_build_surface_command_bounds(
      context, snapshot, surface_id, left, top, right, bottom, NULL, 0,
      command);
}

BOOL viewer_gfx_rfx_build_surface_command_rects(
    ViewerGfxRfxContext *context, const ViewerFramebufferSnapshot *snapshot,
    UINT16 surface_id, const RECTANGLE_16 *dirty_rects, UINT32 dirty_rect_count,
    RDPGFX_SURFACE_COMMAND *command, BOOL *batched) {
  RFX_RECT *rfx_rects = NULL;
  UINT32 bound_left = UINT32_MAX;
  UINT32 bound_top = UINT32_MAX;
  UINT32 bound_right = 0;
  UINT32 bound_bottom = 0;
  UINT32 i = 0;
  BOOL ok = FALSE;

  if (batched)
    *batched = FALSE;
  if (!command)
    return FALSE;

  viewer_gfx_rfx_surface_command_reset(command);

  if (!viewer_gfx_rfx_validate_snapshot(snapshot) || !dirty_rects ||
      (dirty_rect_count < 2U))
    return FALSE;

  if ((size_t)dirty_rect_count > (SIZE_MAX / sizeof(*rfx_rects)))
    return FALSE;
  rfx_rects = (RFX_RECT *)calloc(dirty_rect_count, sizeof(*rfx_rects));
  if (!rfx_rects)
    return FALSE;

  for (i = 0; i < dirty_rect_count; i++) {
    UINT32 left = 0;
    UINT32 top = 0;
    UINT32 right = 0;
    UINT32 bottom = 0;

    if (!viewer_gfx_rfx_validate_rect(snapshot, &dirty_rects[i], &left, &top,
                                      &right, &bottom) ||
        !viewer_gfx_rfx_bounds_available(snapshot, left, top, right, bottom))
      goto cleanup;

    if (left < bound_left)
      bound_left = left;
    if (top < bound_top)
      bound_top = top;
    if (right > bound_right)
      bound_right = right;
    if (bottom > bound_bottom)
      bound_bottom = bottom;
  }

  if ((bound_left >= bound_right) || (bound_top >= bound_bottom) ||
      ((bound_right - bound_left) > (UINT32)UINT16_MAX) ||
      ((bound_bottom - bound_top) > (UINT32)UINT16_MAX))
    goto cleanup;

  for (i = 0; i < dirty_rect_count; i++) {
    UINT32 left = 0;
    UINT32 top = 0;
    UINT32 right = 0;
    UINT32 bottom = 0;

    if (!viewer_gfx_rfx_validate_rect(snapshot, &dirty_rects[i], &left, &top,
                                      &right, &bottom))
      goto cleanup;

    rfx_rects[i].x = (UINT16)(left - bound_left);
    rfx_rects[i].y = (UINT16)(top - bound_top);
    rfx_rects[i].width = (UINT16)(right - left);
    rfx_rects[i].height = (UINT16)(bottom - top);
  }

  ok = viewer_gfx_rfx_build_surface_command_bounds(
      context, snapshot, surface_id, bound_left, bound_top, bound_right,
      bound_bottom, rfx_rects, dirty_rect_count, command);
  if (ok && batched)
    *batched = TRUE;

cleanup:
  free(rfx_rects);
  return ok;
}

void viewer_gfx_rfx_surface_command_reset(RDPGFX_SURFACE_COMMAND *command) {
  if (!command)
    return;

  free(command->data);
  memset(command, 0, sizeof(*command));
}
