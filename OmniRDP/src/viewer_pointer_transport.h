#ifndef VIEWER_POINTER_TRANSPORT_H
#define VIEWER_POINTER_TRANSPORT_H

#include "viewer_classic_transport.h"
#include "viewer_pointer.h"

#include <freerdp/freerdp.h>
#include <winpr/wtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  freerdp_peer *peer;
  ViewerClassicTransport classic_transport;
} ViewerPointerTransport;

BOOL viewer_pointer_transport_send_plan(ViewerPointerTransport *transport,
                                        const ViewerPointerUpdatePlan *plan);

#ifdef __cplusplus
}
#endif

#endif /* VIEWER_POINTER_TRANSPORT_H */
