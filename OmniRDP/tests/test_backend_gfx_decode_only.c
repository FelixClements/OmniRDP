#include "backend.h"
#include "test_utils.h"

int main(void) {
  BackendClient client = {0};

  test_suppress_crt_dialogs();

  if (backend_gfx_pdu_publish_allowed(NULL))
    return 1;

  if (!backend_gfx_pdu_publish_allowed(&client))
    return 1;

  client.backend_gfx_decode_only_enabled = TRUE;
  if (backend_gfx_pdu_publish_allowed(&client))
    return 1;

  client.backend_gfx_decode_only_enabled = FALSE;
  if (!backend_gfx_pdu_publish_allowed(&client))
    return 1;

  return 0;
}
