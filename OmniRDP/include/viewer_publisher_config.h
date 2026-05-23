#ifndef VIEWER_PUBLISHER_CONFIG_H
#define VIEWER_PUBLISHER_CONFIG_H

#include <winpr/wtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  VIEWER_PUBLISHER_CLASSIC_POLICY_FIFO = 0,
  VIEWER_PUBLISHER_CLASSIC_POLICY_LATEST_STATE = 1
} ViewerPublisherClassicPolicy;

typedef struct {
  BOOL enabled;
  ViewerPublisherClassicPolicy policy;
  UINT32 max_queue_depth;
  UINT64 max_queue_bytes;
} ViewerPublisherClassicPolicyConfig;

#ifdef __cplusplus
}
#endif

#endif /* VIEWER_PUBLISHER_CONFIG_H */
