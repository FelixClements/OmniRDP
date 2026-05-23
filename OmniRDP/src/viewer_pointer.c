#include "viewer_pointer.h"

#include <string.h>

BOOL viewer_pointer_plan_from_snapshot(const ViewerPointerSnapshot *snapshot,
                                       UINT64 last_position_generation,
                                       UINT64 last_shape_generation, BOOL force,
                                       ViewerPointerUpdatePlan *plan) {
  BOOL shape_changed = FALSE;
  BOOL position_changed = FALSE;
  const PointerShapeEntry *shape = NULL;

  if (!snapshot || !plan)
    return FALSE;

  memset(plan, 0, sizeof(*plan));
  shape = snapshot->has_active_shape ? snapshot->active_shape : NULL;
  shape_changed =
      force || (snapshot->shape_generation != last_shape_generation);
  position_changed =
      force || (snapshot->position_generation != last_position_generation);

  if (shape_changed) {
    if (!snapshot->visible || !shape) {
      plan->send_system = TRUE;
      plan->system.type = snapshot->visible ? snapshot->type : SYSPTR_NULL;
    } else {
      plan->color.cacheIndex = shape->cacheIndex;
      plan->color.hotSpotX = shape->hotSpotX;
      plan->color.hotSpotY = shape->hotSpotY;
      plan->color.width = shape->width;
      plan->color.height = shape->height;
      plan->color.lengthAndMask = shape->andMaskLength;
      plan->color.lengthXorMask = shape->xorMaskLength;
      plan->color.xorMaskData = shape->xorMaskData;
      plan->color.andMaskData = shape->andMaskData;
      if (shape->xorBpp > 0) {
        plan->send_new = TRUE;
        plan->pointer_new.xorBpp = shape->xorBpp;
        plan->pointer_new.colorPtrAttr = plan->color;
      } else
        plan->send_color = TRUE;
    }
  }

  if (position_changed && snapshot->visible) {
    plan->send_position = TRUE;
    plan->position.xPos = snapshot->x;
    plan->position.yPos = snapshot->y;
  }

  return TRUE;
}
