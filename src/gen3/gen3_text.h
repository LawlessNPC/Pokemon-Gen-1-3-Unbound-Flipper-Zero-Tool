#pragma once

#include <stddef.h>
#include <stdint.h>

#define GEN3_EOS 0xFF

/* Encodes up to max_chars chars of ascii, then fills the rest of field_len with GEN3_EOS.
 * Unsupported chars encode as '?' (0xAC). Returns chars encoded. Requires max_chars <= field_len. */
size_t gen3_text_encode(const char* ascii, uint8_t* out, size_t field_len, size_t max_chars);

/* Decodes until GEN3_EOS or in_len. Unknown bytes become '?'. Always NUL-terminates. Returns strlen(out). */
size_t gen3_text_decode(const uint8_t* in, size_t in_len, char* out, size_t out_size);
