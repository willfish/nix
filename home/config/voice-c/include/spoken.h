#ifndef PI_VOICE_SPOKEN_H
#define PI_VOICE_SPOKEN_H

#include <stddef.h>

/* Write the bounded final summary into out. Empty when the reply must stay silent. */
int spoken_text(const char *text, char *out, size_t out_cap);

#endif
