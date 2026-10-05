#ifndef PI_VOICE_CHUNKS_H
#define PI_VOICE_CHUNKS_H

#include <stddef.h>

/* Split UTF-8 speech at sentence, clause, then word boundaries.
 * Limits count code points. Returns -1 on invalid input or exhausted capacity.
 * Free all *count output strings, including partial results on failure. */
int speech_chunks(const char *text, char **out, size_t *count, size_t cap);

#endif
