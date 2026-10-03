#ifndef PI_VOICE_CHUNKS_H
#define PI_VOICE_CHUNKS_H

#include <stddef.h>

/* Split cleaned speech at sentence, clause, then word boundaries. */
int speech_chunks(const char *text, char **out, size_t *count, size_t cap);

#endif
