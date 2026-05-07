#include "pointer_shape.h"

#include <stdlib.h>
#include <string.h>

static void pointer_shape_entry_reset(PointerShapeEntry *entry) {
  if (!entry)
    return;

  free(entry->xorMaskData);
  free(entry->andMaskData);
  memset(entry, 0, sizeof(*entry));
}

static BOOL pointer_shape_copy_mask(BYTE **destination, const BYTE *source,
                                    UINT32 length) {
  BYTE *buffer = NULL;

  if (!destination)
    return FALSE;

  *destination = NULL;

  if (length == 0)
    return TRUE;

  if (!source || (length > POINTER_SHAPE_MAX_MASK_LENGTH))
    return FALSE;

  buffer = (BYTE *)malloc(length);
  if (!buffer)
    return FALSE;

  memcpy(buffer, source, length);
  *destination = buffer;
  return TRUE;
}

static PointerShapeEntry *
pointer_shape_cache_reserve_entry(PointerShapeCache *cache, UINT16 cacheIndex) {
  UINT32 index = 0;

  if (!cache)
    return NULL;

  for (index = 0; index < cache->count; index++) {
    if (cache->entries[index].cacheIndex == cacheIndex) {
      pointer_shape_entry_reset(&cache->entries[index]);
      cache->entries[index].cacheIndex = cacheIndex;
      return &cache->entries[index];
    }
  }

  if (cache->count >= POINTER_SHAPE_CACHE_CAPACITY)
    return NULL;

  cache->entries[cache->count].cacheIndex = cacheIndex;
  cache->count++;
  return &cache->entries[cache->count - 1];
}

static PointerShapeEntry *pointer_shape_cache_add(
    PointerShapeCache *cache, UINT16 cacheIndex, UINT16 width, UINT16 height,
    UINT16 hotSpotX, UINT16 hotSpotY, UINT16 xorBpp, const BYTE *xorMaskData,
    UINT32 xorMaskLength, const BYTE *andMaskData, UINT32 andMaskLength) {
  PointerShapeEntry *entry = NULL;
  BOOL is_new_entry = FALSE;

  if (!cache)
    return NULL;

  if ((xorMaskLength > POINTER_SHAPE_MAX_MASK_LENGTH) ||
      (andMaskLength > POINTER_SHAPE_MAX_MASK_LENGTH))
    return NULL;

  is_new_entry = (pointer_shape_cache_get_by_index(cache, cacheIndex) == NULL);
  entry = pointer_shape_cache_reserve_entry(cache, cacheIndex);
  if (!entry)
    return NULL;

  entry->cacheIndex = cacheIndex;
  entry->width = width;
  entry->height = height;
  entry->hotSpotX = hotSpotX;
  entry->hotSpotY = hotSpotY;
  entry->xorBpp = xorBpp;
  entry->xorMaskLength = xorMaskLength;
  entry->andMaskLength = andMaskLength;

  if (!pointer_shape_copy_mask(&entry->xorMaskData, xorMaskData,
                               xorMaskLength) ||
      !pointer_shape_copy_mask(&entry->andMaskData, andMaskData,
                               andMaskLength)) {
    pointer_shape_entry_reset(entry);
    if (is_new_entry && (cache->count > 0))
      cache->count--;
    return NULL;
  }

  return entry;
}

PointerShapeCache *pointer_shape_cache_new(void) {
  return (PointerShapeCache *)calloc(1, sizeof(PointerShapeCache));
}

void pointer_shape_cache_free(PointerShapeCache *cache) {
  UINT32 index = 0;

  if (!cache)
    return;

  for (index = 0; index < cache->count; index++)
    pointer_shape_entry_reset(&cache->entries[index]);

  free(cache);
}

PointerShapeEntry *
pointer_shape_cache_add_from_color(PointerShapeCache *cache,
                                   const POINTER_COLOR_UPDATE *pointer_color) {
  if (!pointer_color)
    return NULL;

  return pointer_shape_cache_add(
      cache, pointer_color->cacheIndex, pointer_color->width,
      pointer_color->height, pointer_color->hotSpotX, pointer_color->hotSpotY,
      0, pointer_color->xorMaskData, pointer_color->lengthXorMask,
      pointer_color->andMaskData, pointer_color->lengthAndMask);
}

PointerShapeEntry *
pointer_shape_cache_add_from_new(PointerShapeCache *cache,
                                 const POINTER_NEW_UPDATE *pointer_new) {
  const POINTER_COLOR_UPDATE *pointer_color = NULL;

  if (!pointer_new || (pointer_new->xorBpp > UINT16_MAX))
    return NULL;

  pointer_color = &pointer_new->colorPtrAttr;
  return pointer_shape_cache_add(
      cache, pointer_color->cacheIndex, pointer_color->width,
      pointer_color->height, pointer_color->hotSpotX, pointer_color->hotSpotY,
      (UINT16)pointer_new->xorBpp, pointer_color->xorMaskData,
      pointer_color->lengthXorMask, pointer_color->andMaskData,
      pointer_color->lengthAndMask);
}

PointerShapeEntry *pointer_shape_cache_get_by_index(PointerShapeCache *cache,
                                                    UINT16 cacheIndex) {
  UINT32 index = 0;

  if (!cache)
    return NULL;

  for (index = 0; index < cache->count; index++) {
    if (cache->entries[index].cacheIndex == cacheIndex)
      return &cache->entries[index];
  }

  return NULL;
}
