#include <assert.h>
#include <string.h>

#include <freerdp/pointer.h>

#include "pointer_shape.h"

static void test_deep_copy_success_for_color_shape(void)
{
    BYTE xorMask[] = { 1, 2, 3, 4 };
    BYTE andMask[] = { 5, 6, 7 };
    POINTER_COLOR_UPDATE update = {
        .cacheIndex = 7,
        .hotSpotX = 2,
        .hotSpotY = 3,
        .width = 16,
        .height = 16,
        .lengthAndMask = sizeof(andMask),
        .lengthXorMask = sizeof(xorMask),
        .xorMaskData = xorMask,
        .andMaskData = andMask,
    };
    PointerShapeCache* cache = pointer_shape_cache_new();
    PointerShapeEntry* entry = NULL;

    assert(cache);
    entry = pointer_shape_cache_add_from_color(cache, &update);
    assert(entry);
    assert(entry->cacheIndex == 7);
    assert(entry->width == 16);
    assert(entry->height == 16);
    assert(entry->hotSpotX == 2);
    assert(entry->hotSpotY == 3);
    assert(entry->xorBpp == 0);
    assert(entry->xorMaskLength == sizeof(xorMask));
    assert(entry->andMaskLength == sizeof(andMask));
    assert(entry->xorMaskData != xorMask);
    assert(entry->andMaskData != andMask);
    assert(memcmp(entry->xorMaskData, xorMask, sizeof(xorMask)) == 0);
    assert(memcmp(entry->andMaskData, andMask, sizeof(andMask)) == 0);

    xorMask[0] = 42;
    andMask[0] = 24;
    assert(entry->xorMaskData[0] == 1);
    assert(entry->andMaskData[0] == 5);

    pointer_shape_cache_free(cache);
}

static void test_deep_copy_success_for_new_shape(void)
{
    BYTE xorMask[] = { 8, 9, 10, 11 };
    BYTE andMask[] = { 12, 13 };
    POINTER_NEW_UPDATE update = {
        .xorBpp = 24,
        .colorPtrAttr = {
            .cacheIndex = 9,
            .hotSpotX = 4,
            .hotSpotY = 5,
            .width = 32,
            .height = 32,
            .lengthAndMask = sizeof(andMask),
            .lengthXorMask = sizeof(xorMask),
            .xorMaskData = xorMask,
            .andMaskData = andMask,
        },
    };
    PointerShapeCache* cache = pointer_shape_cache_new();
    PointerShapeEntry* entry = NULL;

    assert(cache);
    entry = pointer_shape_cache_add_from_new(cache, &update);
    assert(entry);
    assert(entry->cacheIndex == 9);
    assert(entry->xorBpp == 24);
    assert(entry->width == 32);
    assert(entry->height == 32);
    assert(entry->hotSpotX == 4);
    assert(entry->hotSpotY == 5);
    assert(entry->xorMaskData != xorMask);
    assert(entry->andMaskData != andMask);
    assert(memcmp(entry->xorMaskData, xorMask, sizeof(xorMask)) == 0);
    assert(memcmp(entry->andMaskData, andMask, sizeof(andMask)) == 0);

    xorMask[1] = 99;
    andMask[1] = 88;
    assert(entry->xorMaskData[1] == 9);
    assert(entry->andMaskData[1] == 13);

    pointer_shape_cache_free(cache);
}

static void test_cache_selection_by_index(void)
{
    BYTE firstXor[] = { 1 };
    BYTE firstAnd[] = { 2 };
    BYTE secondXor[] = { 3 };
    BYTE secondAnd[] = { 4 };
    POINTER_COLOR_UPDATE first = {
        .cacheIndex = 1,
        .width = 8,
        .height = 8,
        .lengthAndMask = sizeof(firstAnd),
        .lengthXorMask = sizeof(firstXor),
        .xorMaskData = firstXor,
        .andMaskData = firstAnd,
    };
    POINTER_NEW_UPDATE second = {
        .xorBpp = 32,
        .colorPtrAttr = {
            .cacheIndex = 11,
            .width = 16,
            .height = 16,
            .lengthAndMask = sizeof(secondAnd),
            .lengthXorMask = sizeof(secondXor),
            .xorMaskData = secondXor,
            .andMaskData = secondAnd,
        },
    };
    PointerShapeCache* cache = pointer_shape_cache_new();
    PointerShapeEntry* entry = NULL;

    assert(cache);
    assert(pointer_shape_cache_add_from_color(cache, &first));
    assert(pointer_shape_cache_add_from_new(cache, &second));

    entry = pointer_shape_cache_get_by_index(cache, 11);
    assert(entry);
    assert(entry->cacheIndex == 11);
    assert(entry->xorBpp == 32);
    assert(entry->xorMaskData[0] == 3);

    entry = pointer_shape_cache_get_by_index(cache, 99);
    assert(!entry);

    pointer_shape_cache_free(cache);
}

static void test_oversized_mask_rejected(void)
{
    POINTER_COLOR_UPDATE update = {
        .cacheIndex = 5,
        .width = 64,
        .height = 64,
        .lengthAndMask = 1,
        .lengthXorMask = POINTER_SHAPE_MAX_MASK_LENGTH + 1u,
        .xorMaskData = (BYTE*)"x",
        .andMaskData = (BYTE*)"y",
    };
    PointerShapeCache* cache = pointer_shape_cache_new();

    assert(cache);
    assert(!pointer_shape_cache_add_from_color(cache, &update));
    assert(cache->count == 0);

    pointer_shape_cache_free(cache);
}

static void test_cleanup_is_idempotent(void)
{
    PointerShapeCache* cache = pointer_shape_cache_new();

    assert(cache);
    pointer_shape_cache_free(cache);
    cache = NULL;
    pointer_shape_cache_free(cache);
}

int main(void)
{
    test_deep_copy_success_for_color_shape();
    test_deep_copy_success_for_new_shape();
    test_cache_selection_by_index();
    test_oversized_mask_rejected();
    test_cleanup_is_idempotent();
    return 0;
}
