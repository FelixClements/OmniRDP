#ifndef VIEWER_POINTER_H
#define VIEWER_POINTER_H

#include "pointer_shape.h"

#include <freerdp/pointer.h>
#include <winpr/wtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  UINT16 x;
  UINT16 y;
  BOOL visible;
  UINT32 type;
  const PointerShapeEntry *active_shape;
  BOOL has_active_shape;
  UINT64 position_generation;
  UINT64 shape_generation;
} ViewerPointerSnapshot;

typedef struct {
  BOOL send_system;
  BOOL send_color;
  BOOL send_new;
  BOOL send_position;
  POINTER_SYSTEM_UPDATE system;
  POINTER_POSITION_UPDATE position;
  POINTER_COLOR_UPDATE color;
  POINTER_NEW_UPDATE pointer_new;
} ViewerPointerUpdatePlan;

BOOL viewer_pointer_plan_from_snapshot(const ViewerPointerSnapshot *snapshot,
                                       UINT64 last_position_generation,
                                       UINT64 last_shape_generation, BOOL force,
                                       ViewerPointerUpdatePlan *plan);

#ifdef __cplusplus
}
#endif

#endif /* VIEWER_POINTER_H */
