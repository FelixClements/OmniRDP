/**
 * @file fixed_buffer.h
 * @brief Fixed-size character fields used by public C data structures.
 *
 * These fields are schema storage, not temporary formatting buffers.  Callers
 * must continue to write them through size-aware helpers such as strcpy_safe,
 * strncpy_s, omni_format, or json_escape_string.
 */

#ifndef FIXED_BUFFER_H
#define FIXED_BUFFER_H

typedef char OmniFixedChar;

#define OMNI_FIXED_CHAR_FIELD(name, size) OmniFixedChar name[size]

#endif /* FIXED_BUFFER_H */
