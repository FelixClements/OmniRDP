#include "viewer_gfx_codec_clearcodec.h"

#include <freerdp/codec/clear.h>
#include <freerdp/codec/color.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct ViewerGfxClearCodecContext {
  CLEAR_CONTEXT *clear;
};

BOOL viewer_gfx_clearcodec_is_available(void) { return TRUE; }

const char *viewer_gfx_clearcodec_disabled_reason(void) { return NULL; }

static void
viewer_gfx_clearcodec_pixel_bounds(const ViewerFramebufferSnapshot *snapshot,
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

ViewerGfxClearCodecContext *viewer_gfx_clearcodec_context_new(void) {
  ViewerGfxClearCodecContext *context = NULL;
  CLEAR_CONTEXT *clear = NULL;

  context = (ViewerGfxClearCodecContext *)calloc(1, sizeof(*context));
  if (!context)
    return NULL;

  clear = clear_context_new(TRUE);
  if (!clear) {
    free(context);
    return NULL;
  }

  context->clear = clear;
  return context;
}

void viewer_gfx_clearcodec_context_free(ViewerGfxClearCodecContext *context) {
  if (!context)
    return;

  clear_context_free(context->clear);
  free(context);
}

static BOOL viewer_gfx_clearcodec_validate_snapshot(
    const ViewerFramebufferSnapshot *snapshot) {
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

  viewer_gfx_clearcodec_pixel_bounds(snapshot, &origin_x, &origin_y,
                                     &pixel_width, &pixel_height);
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

static BOOL viewer_gfx_clearcodec_bounds_available(
    const ViewerFramebufferSnapshot *snapshot, UINT32 left, UINT32 top,
    UINT32 right, UINT32 bottom) {
  UINT32 origin_x = 0;
  UINT32 origin_y = 0;
  UINT32 pixel_width = 0;
  UINT32 pixel_height = 0;
  UINT32 pixel_right = 0;
  UINT32 pixel_bottom = 0;

  if (!snapshot || (left >= right) || (top >= bottom))
    return FALSE;

  viewer_gfx_clearcodec_pixel_bounds(snapshot, &origin_x, &origin_y,
                                     &pixel_width, &pixel_height);
  pixel_right = origin_x + pixel_width;
  pixel_bottom = origin_y + pixel_height;
  return (left >= origin_x) && (top >= origin_y) && (right <= pixel_right) &&
         (bottom <= pixel_bottom);
}

static BOOL viewer_gfx_clearcodec_validate_rect(
    const ViewerFramebufferSnapshot *snapshot, const RECTANGLE_16 *dirty_rect,
    UINT32 *left, UINT32 *top, UINT32 *right, UINT32 *bottom) {
  if (!snapshot || !dirty_rect || !left || !top || !right || !bottom)
    return FALSE;

  if ((dirty_rect->left > dirty_rect->right) ||
      (dirty_rect->top > dirty_rect->bottom))
    return FALSE;

  if (((UINT32)dirty_rect->right >= snapshot->width) ||
      ((UINT32)dirty_rect->bottom >= snapshot->height))
    return FALSE;

  *left = (UINT32)dirty_rect->left;
  *top = (UINT32)dirty_rect->top;
  *right = (UINT32)dirty_rect->right + 1U;
  *bottom = (UINT32)dirty_rect->bottom + 1U;

  if ((*left >= *right) || (*top >= *bottom))
    return FALSE;

  return TRUE;
}

static BOOL viewer_gfx_clearcodec_build_surface_command_bounds(
    ViewerGfxClearCodecContext *context,
    const ViewerFramebufferSnapshot *snapshot, UINT16 surface_id, UINT32 left,
    UINT32 top, UINT32 right, UINT32 bottom, UINT32 dest_left, UINT32 dest_top,
    RDPGFX_SURFACE_COMMAND *command) {
  BYTE *data = NULL;
  UINT32 payload_bytes = 0;
  UINT32 rect_width = 0;
  UINT32 rect_height = 0;
  UINT32 dest_right = 0;
  UINT32 dest_bottom = 0;
  UINT32 origin_x = 0;
  UINT32 origin_y = 0;
  INT32 encode_status = 0;

  if (!context || !context->clear ||
      !viewer_gfx_clearcodec_validate_snapshot(snapshot) || !command)
    return FALSE;

  if ((left >= right) || (top >= bottom) || (right > snapshot->width) ||
      (bottom > snapshot->height))
    return FALSE;
  if (!viewer_gfx_clearcodec_bounds_available(snapshot, left, top, right,
                                              bottom))
    return FALSE;

  rect_width = right - left;
  rect_height = bottom - top;
  if ((rect_width == 0) || (rect_height == 0) ||
      (rect_width > (UINT32)UINT16_MAX) || (rect_height > (UINT32)UINT16_MAX))
    return FALSE;
  if (((UINT32)UINT16_MAX - dest_left) < rect_width)
    return FALSE;
  if (((UINT32)UINT16_MAX - dest_top) < rect_height)
    return FALSE;
  dest_right = dest_left + rect_width;
  dest_bottom = dest_top + rect_height;

  viewer_gfx_clearcodec_pixel_bounds(snapshot, &origin_x, &origin_y, NULL,
                                     NULL);
  encode_status =
      clear_encode(context->clear, snapshot->pixels, snapshot->pixel_format,
                   snapshot->stride, left - origin_x, top - origin_y,
                   rect_width, rect_height, &data, &payload_bytes, NULL);
  if ((encode_status < 0) || !data || (payload_bytes == 0))
    return FALSE;

  memset(command, 0, sizeof(*command));
  command->surfaceId = surface_id;
  command->codecId = RDPGFX_CODECID_CLEARCODEC;
  command->contextId = 0;
  command->format = PIXEL_FORMAT_BGRX32;
  command->left = (UINT16)dest_left;
  command->top = (UINT16)dest_top;
  command->right = (UINT16)dest_right;
  command->bottom = (UINT16)dest_bottom;
  command->width = (UINT16)rect_width;
  command->height = (UINT16)rect_height;
  command->length = payload_bytes;
  command->data = data;
  return TRUE;
}

BOOL viewer_gfx_clearcodec_build_surface_command(
    ViewerGfxClearCodecContext *context,
    const ViewerFramebufferSnapshot *snapshot, UINT16 surface_id,
    RDPGFX_SURFACE_COMMAND *command) {
  if (!command)
    return FALSE;

  viewer_gfx_clearcodec_surface_command_reset(command);

  if (!snapshot)
    return FALSE;

  return viewer_gfx_clearcodec_build_surface_command_bounds(
      context, snapshot, surface_id, 0, 0, snapshot->width, snapshot->height, 0,
      0, command);
}

BOOL viewer_gfx_clearcodec_build_surface_command_rect(
    ViewerGfxClearCodecContext *context,
    const ViewerFramebufferSnapshot *snapshot, UINT16 surface_id,
    const RECTANGLE_16 *dirty_rect, RDPGFX_SURFACE_COMMAND *command) {
  UINT32 left = 0;
  UINT32 top = 0;
  UINT32 right = 0;
  UINT32 bottom = 0;

  if (!command)
    return FALSE;

  viewer_gfx_clearcodec_surface_command_reset(command);

  if (!viewer_gfx_clearcodec_validate_snapshot(snapshot) ||
      !viewer_gfx_clearcodec_validate_rect(snapshot, dirty_rect, &left, &top,
                                           &right, &bottom))
    return FALSE;

  return viewer_gfx_clearcodec_build_surface_command_bounds(
      context, snapshot, surface_id, left, top, right, bottom, left, top,
      command);
}

BOOL viewer_gfx_clearcodec_build_surface_command_region(
    ViewerGfxClearCodecContext *context,
    const ViewerFramebufferSnapshot *snapshot, UINT16 surface_id,
    UINT32 source_left, UINT32 source_top, UINT32 source_right,
    UINT32 source_bottom, UINT32 dest_left, UINT32 dest_top,
    RDPGFX_SURFACE_COMMAND *command) {
  if (!command)
    return FALSE;

  viewer_gfx_clearcodec_surface_command_reset(command);

  return viewer_gfx_clearcodec_build_surface_command_bounds(
      context, snapshot, surface_id, source_left, source_top, source_right,
      source_bottom, dest_left, dest_top, command);
}

void viewer_gfx_clearcodec_surface_command_reset(
    RDPGFX_SURFACE_COMMAND *command) {
  if (!command)
    return;

  free(command->data);
  memset(command, 0, sizeof(*command));
}
