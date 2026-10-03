#define _POSIX_C_SOURCE 200809L
#include "text.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static pcre2_code *marker_re;

static void ensure(void) {
    if (marker_re) return;
    int error = 0;
    PCRE2_SIZE offset = 0;
    marker_re = pcre2_compile(
        (PCRE2_SPTR)"[\\[(]\\s*(?:blank[_ ]audio|no[_ ]speech|silence|silent|"
        "music|inaudible|noise|applause|laughter|breathing)\\s*[\\])]",
        PCRE2_ZERO_TERMINATED, PCRE2_CASELESS | PCRE2_UTF, &error, &offset, NULL);
}

int has_control_characters(const char *text) {
    if (!text) return 1;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if ((*p < 32 && *p != '\n' && *p != '\r' && *p != '\t') || (*p >= 127 && *p < 160))
            return 1;
    }
    return 0;
}

int dictation_text(const char *text, char *out, size_t cap) {
    if (!out || cap == 0) return -1;
    out[0] = 0;
    if (!text) return 1;
    ensure();
    if (!marker_re) return -1;
    char *work = strdup(text);
    if (!work) return -1;
    pcre2_match_data *data = pcre2_match_data_create(4, NULL);
    for (;;) {
        int rc = pcre2_match(marker_re, (PCRE2_SPTR)work, PCRE2_ZERO_TERMINATED, 0, 0, data, NULL);
        if (rc < 0) break;
        PCRE2_SIZE *ov = pcre2_get_ovector_pointer(data);
        work[ov[0]] = ' ';
        memmove(work + ov[0] + 1, work + ov[1], strlen(work + ov[1]) + 1);
    }
    pcre2_match_data_free(data);
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)work; *p; p++) {
        if (*p == 0xE2 && p[1] == 0x80 && p[2] == 0x8B) { p += 2; continue; }
        if (*p == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { p += 2; continue; }
        if (n + 1 < cap) work[n++] = *p;
    }
    work[n] = 0;
    char *dst = out;
    int space = 1;
    int alnum = 0;
    for (unsigned char *p = (unsigned char *)work; *p && (size_t)(dst - out) + 1 < cap; p++) {
        if (*p == ' ' || *p == '\t') {
            if (!space) *dst++ = ' ';
            space = 1;
        } else {
            if (isalnum(*p) || *p >= 128) alnum = 1;
            *dst++ = *p;
            space = 0;
        }
    }
    if (dst > out && dst[-1] == ' ') dst--;
    *dst = 0;
    free(work);
    if (!alnum) out[0] = 0;
    return 0;
}
