#include "viewer_pointer_transport.h"

#include <freerdp/peer.h>

BOOL viewer_pointer_transport_send_plan(ViewerPointerTransport *transport,
                                        const ViewerPointerUpdatePlan *plan) {
  freerdp_peer *peer = transport ? transport->peer : NULL;
  BOOL sent = TRUE;

  if (!transport || !plan || !peer || !peer->context ||
      !peer->context->update || !peer->context->update->pointer)
    return FALSE;

  /* PointerPosition is intentionally suppressed to avoid echo lag. */
  if (!plan->send_system && !plan->send_color && !plan->send_new)
    return TRUE;

  (void)viewer_classic_transport_begin_batch(&transport->classic_transport);
  if (plan->send_system) {
    IFCALLRET(peer->context->update->pointer->PointerSystem, sent,
              peer->context, &plan->system_update);
  } else if (plan->send_new && peer->context->update->pointer->PointerNew) {
    IFCALLRET(peer->context->update->pointer->PointerNew, sent, peer->context,
              &plan->pointer_new);
  } else if (plan->send_new || plan->send_color) {
    IFCALLRET(peer->context->update->pointer->PointerColor, sent, peer->context,
              &plan->color);
  }

  viewer_classic_transport_end_batch(&transport->classic_transport);

  return sent;
}
