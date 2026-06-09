#include "viewer_gfx_codec_rfx.h"

#include <freerdp/codec/color.h>
#include <freerdp/codec/rfx.h>
#include <freerdp/settings_types.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct ViewerGfxRfxContext {
  RFX_CONTEXT *rfx;
};

#ifdef VIEWER_GFX_RFX_TESTING
static BOOL g_viewer_gfx_rfx_force_context_new_failure = FALSE;
#endif

BOOL viewer_gfx_rfx_is_available(void) { return TRUE; }

const char *viewer_gfx_rfx_disabled_reason(void) { return NULL; }

#ifdef VIEWER_GFX_RFX_TESTING
void viewer_gfx_rfx_test_set_force_context_new_failure(BOOL force_failure) {
  g_viewer_gfx_rfx_force_context_new_failure = force_failure;
}
#endif

ViewerGfxRfxContext *viewer_gfx_rfx_context_new(void) {
  ViewerGfxRfxContext *context = NULL;
  RFX_CONTEXT *rfx = NULL;

#ifdef VIEWER_GFX_RFX_TESTING
  if (g_viewer_gfx_rfx_force_context_new_failure)
    return NULL;
#endif

  context = (ViewerGfxRfxContext *)calloc(1, sizeof(*context));
  if (!context)
    return NULL;

  rfx = rfx_context_new_ex(TRUE, THREADING_FLAGS_DISABLE_THREADS);
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

  if (!snapshot || !snapshot->pixels || (snapshot->width == 0) ||
      (snapshot->height == 0))
    return FALSE;

  if ((snapshot->width > (UINT32)UINT16_MAX) ||
      (snapshot->height > (UINT32)UINT16_MAX))
    return FALSE;

  if (snapshot->pixel_format != PIXEL_FORMAT_BGRX32)
    return FALSE;

  if ((size_t)snapshot->width > (SIZE_MAX / 4U))
    return FALSE;
  tight_row_bytes = (size_t)snapshot->width * 4U;

  if ((snapshot->stride == 0) || ((size_t)snapshot->stride < tight_row_bytes))
    return FALSE;

  if ((size_t)(snapshot->height - 1U) >
      ((SIZE_MAX - tight_row_bytes) / (size_t)snapshot->stride))
    return FALSE;
  minimum_pixel_bytes =
      ((size_t)(snapshot->height - 1U) * (size_t)snapshot->stride) +
      tight_row_bytes;

  if (snapshot->pixel_bytes < minimum_pixel_bytes)
    return FALSE;

  return TRUE;
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
    RDPGFX_SURFACE_COMMAND *command) {
  BYTE *data = NULL;
  const BYTE *source = NULL;
  RFX_RECT rfx_rect = {0};
  UINT32 rect_width = 0;
  UINT32 rect_height = 0;
  size_t payload_bytes = 0;
  wStream *stream = NULL;

  if (!context || !context->rfx ||
      !viewer_gfx_rfx_validate_snapshot(snapshot) || !command)
    return FALSE;

  if ((left >= right) || (top >= bottom) || (right > snapshot->width) ||
      (bottom > snapshot->height))
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

  rfx_rect.x = 0;
  rfx_rect.y = 0;
  rfx_rect.width = (UINT16)rect_width;
  rfx_rect.height = (UINT16)rect_height;
  source = snapshot->pixels + ((size_t)top * (size_t)snapshot->stride) +
           ((size_t)left * 4U);

  if (!rfx_compose_message(context->rfx, stream, &rfx_rect, 1, source,
                           rect_width, rect_height, snapshot->stride)) {
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
      command);
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
      context, snapshot, surface_id, left, top, right, bottom, command);
}

void viewer_gfx_rfx_surface_command_reset(RDPGFX_SURFACE_COMMAND *command) {
  if (!command)
    return;

  free(command->data);
  memset(command, 0, sizeof(*command));
}
