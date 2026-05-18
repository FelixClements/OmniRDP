#include "viewer_publisher.h"

#include <string.h>

BOOL viewer_publisher_init(ViewerPublisher *publisher) {
  if (!publisher)
    return FALSE;

  if (publisher->initialized)
    return TRUE;

  memset(publisher, 0, sizeof(*publisher));
  publisher->initialized = TRUE;
  return TRUE;
}

void viewer_publisher_uninit(ViewerPublisher *publisher) {
  if (!publisher)
    return;

  memset(publisher, 0, sizeof(*publisher));
}

void viewer_publisher_reset_metrics(ViewerPublisher *publisher) {
  if (!publisher)
    return;

  memset(&publisher->metrics, 0, sizeof(publisher->metrics));
}

void viewer_publisher_note_generation(ViewerPublisher *publisher,
                                      UINT64 generation) {
  if (!publisher)
    return;

  publisher->metrics.latest_generation_available = generation;
}
