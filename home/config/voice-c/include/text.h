#ifndef PI_VOICE_TEXT_H
#define PI_VOICE_TEXT_H

#include <stddef.h>

int has_control_characters(const char *text);
/* Returns 0 and writes cleaned text, 1 for invalid input, -1 on allocation failure. */
int dictation_text(const char *text, char *out, size_t cap);

#endif
