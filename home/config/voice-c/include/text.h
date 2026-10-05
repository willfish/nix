#ifndef PI_VOICE_TEXT_H
#define PI_VOICE_TEXT_H

#include <stddef.h>

int has_control_characters(const char *text);
/* Returns 0 for cleaned text, 1 for invalid UTF-8/null input, -1 on failure
 * (including insufficient capacity). Errors leave out empty, never truncated. */
int dictation_text(const char *text, char *out, size_t cap);

#endif
