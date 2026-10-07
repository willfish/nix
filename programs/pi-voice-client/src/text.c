#define _POSIX_C_SOURCE 200809L
#include "text.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <glib.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static pcre2_code *marker_re;
static pthread_once_t patterns_once = PTHREAD_ONCE_INIT;

static void compile_patterns(void) {
    int error = 0;
    PCRE2_SIZE offset = 0;
    marker_re = pcre2_compile(
        (PCRE2_SPTR)"[\\[(]\\s*(?:blank[_ ]audio|no[_ ]speech|silence|silent|"
        "music|inaudible|noise|applause|laughter|breathing)\\s*[\\])]",
        PCRE2_ZERO_TERMINATED, PCRE2_CASELESS | PCRE2_UTF | PCRE2_UCP,
        &error, &offset, NULL);
}

int has_control_characters(const char *text) {
    if (!text || !g_utf8_validate(text, -1, NULL)) return 1;
    for (const char *p = text; *p; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if ((c < 32 && c != '\n' && c != '\r' && c != '\t') || (c >= 127 && c < 160))
            return 1;
    }
    return 0;
}

int dictation_text(const char *text, char *out, size_t cap) {
    if (!out || cap == 0) return -1;
    out[0] = 0;
    if (!text || !g_utf8_validate(text, -1, NULL)) return 1;
    pthread_once(&patterns_once, compile_patterns);
    if (!marker_re) return -1;
    char *work = strdup(text);
    if (!work) return -1;
    pcre2_match_data *data = pcre2_match_data_create_from_pattern(marker_re, NULL);
    if (!data) { free(work); return -1; }
    for (;;) {
        int rc = pcre2_match(marker_re, (PCRE2_SPTR)work, PCRE2_ZERO_TERMINATED, 0, 0, data, NULL);
        if (rc < 0) break;
        PCRE2_SIZE *ov = pcre2_get_ovector_pointer(data);
        work[ov[0]] = ' ';
        memmove(work + ov[0] + 1, work + ov[1], strlen(work + ov[1]) + 1);
    }
    pcre2_match_data_free(data);
    char *dst = work;
    int space = 0;
    for (const char *p = work; *p;) {
        const char *next = g_utf8_next_char(p);
        gunichar c = g_utf8_get_char(p);
        if (c != 0x200b && c != 0xfeff) {
            if (c == ' ' || c == '\t') {
                if (!space) *dst++ = ' ';
                space = 1;
            } else {
                memmove(dst, p, (size_t)(next - p));
                dst += next - p;
                space = 0;
            }
        }
        p = next;
    }
    *dst = 0;
    const char *start = work;
    while (*start && g_unichar_isspace(g_utf8_get_char(start))) start = g_utf8_next_char(start);
    char *end = dst;
    while (end > start) {
        char *prev = g_utf8_find_prev_char(start, end);
        if (!g_unichar_isspace(g_utf8_get_char(prev))) break;
        end = prev;
    }
    *end = 0;
    int alnum = 0;
    for (const char *p = start; *p; p = g_utf8_next_char(p))
        if (g_unichar_isalnum(g_utf8_get_char(p))) alnum = 1;
    int result = 0;
    if (alnum) {
        size_t n = (size_t)(end - start);
        if (n >= cap) result = -1;
        else memcpy(out, start, n + 1);
    }
    free(work);
    return result;
}
