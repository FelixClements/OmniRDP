#include <assert.h>

#include <freerdp/pointer.h>

#include "test_utils.h"
#include "viewer_pointer.h"

static void test_forced_custom_shape_and_position_when_generations_match(void) {
  BYTE xorMask[] = {1, 2, 3, 4};
  BYTE andMask[] = {5, 6};
  PointerShapeEntry shape = {
      .width = 16,
      .height = 20,
      .hotSpotX = 2,
      .hotSpotY = 3,
      .xorBpp = 32,
      .xorMaskData = xorMask,
      .andMaskData = andMask,
      .xorMaskLength = sizeof(xorMask),
      .andMaskLength = sizeof(andMask),
      .cacheIndex = 7,
  };
  ViewerPointerSnapshot snapshot = {
      .x = 44,
      .y = 55,
      .visible = TRUE,
      .type = SYSPTR_DEFAULT,
      .active_shape = &shape,
      .has_active_shape = TRUE,
      .position_generation = 9,
      .shape_generation = 10,
  };
  ViewerPointerUpdatePlan plan = {0};

  assert(viewer_pointer_plan_from_snapshot(&snapshot, 9, 10, TRUE, &plan));
  assert(plan.send_new);
  assert(!plan.send_color);
  assert(!plan.send_system);
  assert(plan.pointer_new.xorBpp == 32);
  assert(plan.pointer_new.colorPtrAttr.cacheIndex == 7);
  assert(plan.pointer_new.colorPtrAttr.xorMaskData == xorMask);
  assert(plan.send_position);
  assert(plan.position.xPos == 44);
  assert(plan.position.yPos == 55);
}

static void test_forced_default_cursor_and_position(void) {
  ViewerPointerSnapshot snapshot = {
      .x = 11,
      .y = 22,
      .visible = TRUE,
      .type = SYSPTR_DEFAULT,
      .has_active_shape = FALSE,
      .position_generation = 4,
      .shape_generation = 5,
  };
  ViewerPointerUpdatePlan plan = {0};

  assert(viewer_pointer_plan_from_snapshot(&snapshot, 4, 5, TRUE, &plan));
  assert(plan.send_system);
  assert(plan.system.type == SYSPTR_DEFAULT);
  assert(!plan.send_color);
  assert(!plan.send_new);
  assert(plan.send_position);
  assert(plan.position.xPos == 11);
  assert(plan.position.yPos == 22);
}

static void test_hidden_cursor_sends_null_and_no_position(void) {
  ViewerPointerSnapshot snapshot = {
      .x = 1,
      .y = 2,
      .visible = FALSE,
      .type = SYSPTR_DEFAULT,
      .has_active_shape = FALSE,
      .position_generation = 3,
      .shape_generation = 4,
  };
  ViewerPointerUpdatePlan plan = {0};

  assert(viewer_pointer_plan_from_snapshot(&snapshot, 3, 4, TRUE, &plan));
  assert(plan.send_system);
  assert(plan.system.type == SYSPTR_NULL);
  assert(!plan.send_position);
}

static void test_forced_visible_position_when_generation_matches(void) {
  ViewerPointerSnapshot snapshot = {
      .x = 123,
      .y = 234,
      .visible = TRUE,
      .type = SYSPTR_DEFAULT,
      .has_active_shape = FALSE,
      .position_generation = 99,
      .shape_generation = 100,
  };
  ViewerPointerUpdatePlan plan = {0};

  assert(viewer_pointer_plan_from_snapshot(&snapshot, 99, 100, TRUE, &plan));
  assert(plan.send_position);
  assert(plan.position.xPos == 123);
  assert(plan.position.yPos == 234);
}

static void test_unforced_matching_generations_send_nothing(void) {
  ViewerPointerSnapshot snapshot = {
      .x = 5,
      .y = 6,
      .visible = TRUE,
      .type = SYSPTR_DEFAULT,
      .has_active_shape = FALSE,
      .position_generation = 7,
      .shape_generation = 8,
  };
  ViewerPointerUpdatePlan plan = {0};

  assert(viewer_pointer_plan_from_snapshot(&snapshot, 7, 8, FALSE, &plan));
  assert(!plan.send_system);
  assert(!plan.send_color);
  assert(!plan.send_new);
  assert(!plan.send_position);
}

int main(void) {
  test_suppress_crt_dialogs();
  test_forced_custom_shape_and_position_when_generations_match();
  test_forced_default_cursor_and_position();
  test_hidden_cursor_sends_null_and_no_position();
  test_forced_visible_position_when_generation_matches();
  test_unforced_matching_generations_send_nothing();
  return 0;
}
