#ifndef POINTER_SHAPE_H
#define POINTER_SHAPE_H

#include <freerdp/pointer.h>
#include <winpr/wtypes.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define POINTER_SHAPE_CACHE_CAPACITY 32u
#define POINTER_SHAPE_MAX_MASK_LENGTH (64u * 64u * 4u)

	typedef struct PointerShapeEntry
	{
		UINT16 width;
		UINT16 height;
		UINT16 hotSpotX;
		UINT16 hotSpotY;
		UINT16 xorBpp;
		BYTE* xorMaskData;
		BYTE* andMaskData;
		UINT32 xorMaskLength;
		UINT32 andMaskLength;
		UINT16 cacheIndex;
	} PointerShapeEntry;

	typedef struct PointerShapeCache
	{
		PointerShapeEntry entries[POINTER_SHAPE_CACHE_CAPACITY];
		UINT32 count;
	} PointerShapeCache;

	PointerShapeCache* pointer_shape_cache_new(void);
	void pointer_shape_cache_free(PointerShapeCache* cache);
	PointerShapeEntry*
	pointer_shape_cache_add_from_color(PointerShapeCache* cache,
	                                   const POINTER_COLOR_UPDATE* pointer_color);
	PointerShapeEntry* pointer_shape_cache_add_from_new(PointerShapeCache* cache,
	                                                    const POINTER_NEW_UPDATE* pointer_new);
	PointerShapeEntry* pointer_shape_cache_get_by_index(PointerShapeCache* cache,
	                                                    UINT16 cacheIndex);

#ifdef __cplusplus
}
#endif

#endif /* POINTER_SHAPE_H */
