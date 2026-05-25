#include "viewer_pointer_transport.h"

#include <freerdp/peer.h>

BOOL viewer_pointer_transport_send_plan(ViewerPointerTransport *transport,
                                        const ViewerPointerUpdatePlan *plan) {
  freerdp_peer *peer = transport ? transport->peer : NULL;
  BOOL sent = TRUE;

  if (!transport || !plan || !peer || !peer->context ||
      !peer->context->update || !peer->context->update->pointer)
    return FALSE;

  if (!plan->send_system && !plan->send_color && !plan->send_new &&
      !plan->send_position)
    return TRUE;

  (void)viewer_classic_transport_begin_batch(&transport->classic_transport);
  if (plan->send_system) {
    IFCALLRET(peer->context->update->pointer->PointerSystem, sent,
              peer->context, &plan->system);
  } else if (plan->send_new && peer->context->update->pointer->PointerNew) {
    IFCALLRET(peer->context->update->pointer->PointerNew, sent, peer->context,
              &plan->pointer_new);
  } else if (plan->send_new || plan->send_color) {
    IFCALLRET(peer->context->update->pointer->PointerColor, sent, peer->context,
              &plan->color);
  }

  if (sent && plan->send_position &&
      peer->context->update->pointer->PointerPosition)
    IFCALLRET(peer->context->update->pointer->PointerPosition, sent,
              peer->context, &plan->position);
  viewer_classic_transport_end_batch(&transport->classic_transport);

  return sent;
}
