#include "viewer_gfx_codec_uncompressed.h"

#include <freerdp/codec/color.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static BOOL viewer_gfx_uncompressed_validate_snapshot(
    const ViewerFramebufferSnapshot *snapshot) {
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

static BOOL viewer_gfx_uncompressed_validate_rect(
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

static BOOL viewer_gfx_uncompressed_build_surface_command_bounds(
    const ViewerFramebufferSnapshot *snapshot, UINT16 surface_id, UINT32 left,
    UINT32 top, UINT32 right, UINT32 bottom, RDPGFX_SURFACE_COMMAND *command) {
  BYTE *data = NULL;
  UINT32 row = 0;
  UINT32 rect_width = 0;
  UINT32 rect_height = 0;
  size_t destination_row_bytes = 0;
  size_t payload_bytes = 0;

  if (!viewer_gfx_uncompressed_validate_snapshot(snapshot) || !command)
    return FALSE;

  if ((left >= right) || (top >= bottom) || (right > snapshot->width) ||
      (bottom > snapshot->height))
    return FALSE;

  rect_width = right - left;
  rect_height = bottom - top;
  if ((rect_width == 0) || (rect_height == 0))
    return FALSE;

  if ((size_t)rect_width > (SIZE_MAX / 4U))
    return FALSE;
  destination_row_bytes = (size_t)rect_width * 4U;

  if ((size_t)rect_height > (SIZE_MAX / destination_row_bytes))
    return FALSE;
  payload_bytes = (size_t)rect_height * destination_row_bytes;
  if ((payload_bytes == 0) || (payload_bytes > UINT32_MAX))
    return FALSE;

  data = (BYTE *)malloc(payload_bytes);
  if (!data)
    return FALSE;

  for (row = 0; row < rect_height; row++) {
    const BYTE *source = snapshot->pixels +
                         ((size_t)(top + row) * (size_t)snapshot->stride) +
                         ((size_t)left * 4U);
    BYTE *destination = data + ((size_t)row * destination_row_bytes);
    memmove(destination, source, destination_row_bytes);
  }

  memset(command, 0, sizeof(*command));
  command->surfaceId = surface_id;
  command->codecId = RDPGFX_CODECID_UNCOMPRESSED;
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

BOOL viewer_gfx_uncompressed_build_surface_command(
    const ViewerFramebufferSnapshot *snapshot, UINT16 surface_id,
    RDPGFX_SURFACE_COMMAND *command) {
  if (!command)
    return FALSE;

  viewer_gfx_uncompressed_surface_command_reset(command);

  if (!snapshot)
    return FALSE;

  return viewer_gfx_uncompressed_build_surface_command_bounds(
      snapshot, surface_id, 0, 0, snapshot->width, snapshot->height, command);
}

BOOL viewer_gfx_uncompressed_build_surface_command_rect(
    const ViewerFramebufferSnapshot *snapshot, UINT16 surface_id,
    const RECTANGLE_16 *dirty_rect, RDPGFX_SURFACE_COMMAND *command) {
  UINT32 left = 0;
  UINT32 top = 0;
  UINT32 right = 0;
  UINT32 bottom = 0;

  if (!command)
    return FALSE;

  viewer_gfx_uncompressed_surface_command_reset(command);

  if (!viewer_gfx_uncompressed_validate_snapshot(snapshot) ||
      !viewer_gfx_uncompressed_validate_rect(snapshot, dirty_rect, &left, &top,
                                             &right, &bottom))
    return FALSE;

  return viewer_gfx_uncompressed_build_surface_command_bounds(
      snapshot, surface_id, left, top, right, bottom, command);
}

void viewer_gfx_uncompressed_surface_command_reset(
    RDPGFX_SURFACE_COMMAND *command) {
  if (!command)
    return;

  free(command->data);
  memset(command, 0, sizeof(*command));
}
