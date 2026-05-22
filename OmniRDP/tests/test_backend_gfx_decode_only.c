#include "backend.h"
#include "test_utils.h"

int main(void) {
  BackendClient client = {0};

  test_suppress_crt_dialogs();

  if (client.backend_gfx_decode_only_enabled)
    return 1;

  client.backend_gfx_decode_only_enabled = TRUE;
  if (!client.backend_gfx_decode_only_enabled)
    return 1;

  client.backend_gfx_decode_only_enabled = FALSE;
  if (client.backend_gfx_decode_only_enabled)
    return 1;

  return 0;
}
