#ifndef OMNIRDP_SAFE_STRING_H
#define OMNIRDP_SAFE_STRING_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

/*
 * Central formatting wrappers for fixed-size buffers.
 *
 * Return value matches C99 formatting semantics: the number of characters that
 * would have been written, excluding the trailing NUL, or a negative value on
 * an encoding/formatting error.  The destination is forced to a valid empty or
 * truncated C string whenever dest_size is nonzero.
 */
static inline int omni_vformat(char *dest, size_t dest_size, const char *format,
                               va_list args) {
  int written;

  if (!dest || dest_size == 0 || !format)
    return -1;

  written = vsnprintf(dest, dest_size, format, args); /* Flawfinder: ignore */
  dest[dest_size - 1] = '\0';
  return written;
}

static inline int omni_format(char *dest, size_t dest_size, const char *format,
                              ...) {
  int written;
  va_list args;

  va_start(args, format);
  written = omni_vformat(dest, dest_size, format, args);
  va_end(args);

  return written;
}

#endif /* OMNIRDP_SAFE_STRING_H */
