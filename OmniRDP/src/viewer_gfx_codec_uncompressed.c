#include "viewer_gfx_codec_uncompressed.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

BOOL viewer_gfx_uncompressed_build_surface_command(
    const ViewerFramebufferSnapshot *snapshot, UINT16 surface_id,
    RDPGFX_SURFACE_COMMAND *command) {
  if (!snapshot || !snapshot->pixels || !command ||
      (snapshot->pixel_bytes == 0))
    return FALSE;

  if (snapshot->pixel_bytes > UINT32_MAX)
    return FALSE;

  memset(command, 0, sizeof(*command));
  command->surfaceId = surface_id;
  command->codecId = RDPGFX_CODECID_UNCOMPRESSED;
  command->contextId = 0;
  command->format = GFX_PIXEL_FORMAT_XRGB_8888;
  command->left = 0;
  command->top = 0;
  command->right = snapshot->width;
  command->bottom = snapshot->height;
  command->width = snapshot->width;
  command->height = snapshot->height;
  command->length = (UINT32)snapshot->pixel_bytes;
  command->data = (BYTE *)malloc(snapshot->pixel_bytes);
  if (!command->data) {
    memset(command, 0, sizeof(*command));
    return FALSE;
  }

  memmove(command->data, snapshot->pixels, snapshot->pixel_bytes);
  return TRUE;
}

void viewer_gfx_uncompressed_surface_command_reset(
    RDPGFX_SURFACE_COMMAND *command) {
  if (!command)
    return;

  free(command->data);
  memset(command, 0, sizeof(*command));
}
