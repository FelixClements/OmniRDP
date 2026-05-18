#ifndef VIEWER_PUBLISHER_H
#define VIEWER_PUBLISHER_H

#include <winpr/wtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  UINT64 queued_updates;
  UINT64 queued_bytes;
  UINT64 coalesced_updates;
  UINT64 dropped_updates;
  UINT64 last_generation_sent;
  UINT64 latest_generation_available;
} ViewerPublisherMetrics;

typedef struct {
  BOOL initialized;
  ViewerPublisherMetrics metrics;
} ViewerPublisher;

BOOL viewer_publisher_init(ViewerPublisher *publisher);
void viewer_publisher_uninit(ViewerPublisher *publisher);
void viewer_publisher_reset_metrics(ViewerPublisher *publisher);
void viewer_publisher_note_generation(ViewerPublisher *publisher,
                                      UINT64 generation);

#ifdef __cplusplus
}
#endif

#endif /* VIEWER_PUBLISHER_H */
