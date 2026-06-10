#ifndef OMNIRDP_SAFE_STRING_H
#define OMNIRDP_SAFE_STRING_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#ifdef _MSC_VER
#define OMNI_SAFE_STRING_INLINE static __inline
#else
#define OMNI_SAFE_STRING_INLINE static inline
#endif

/*
 * Central formatting wrappers for fixed-size buffers.
 *
 * Return value matches C99 formatting semantics: the number of characters that
 * would have been written, excluding the trailing NUL, or a negative value on
 * an encoding/formatting error.  The destination is forced to a valid empty or
 * truncated C string whenever dest_size is nonzero.
 */
OMNI_SAFE_STRING_INLINE int omni_vformat(char *dest, size_t dest_size,
                                         const char *format, va_list args) {
  int written;

  if (!dest || dest_size == 0 || !format)
    return -1;

  written = vsnprintf(dest, dest_size, format, args); /* Flawfinder: ignore */
  dest[dest_size - 1] = '\0';
  return written;
}

OMNI_SAFE_STRING_INLINE int omni_format(char *dest, size_t dest_size,
                                        const char *format, ...) {
  int written;
  va_list args;

  va_start(args, format);
  written = omni_vformat(dest, dest_size, format, args);
  va_end(args);

  return written;
}

#undef OMNI_SAFE_STRING_INLINE

#endif /* OMNIRDP_SAFE_STRING_H */
