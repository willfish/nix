#define _POSIX_C_SOURCE 200809L
#include "chunks.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <glib.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

enum { CHUNK_FIRST = 120, CHUNK_MAX = 260 };

static pcre2_code *sentence_re;
static pcre2_code *clause_re;
static pthread_once_t patterns_once = PTHREAD_ONCE_INIT;

static void compile_patterns(void) {
    int error = 0;
    PCRE2_SIZE offset = 0;
    sentence_re = pcre2_compile(
        (PCRE2_SPTR)"[.!?][\"')\\]]*(?=\\s|$)", PCRE2_ZERO_TERMINATED,
        PCRE2_UTF | PCRE2_UCP, &error, &offset, NULL);
    clause_re = pcre2_compile(
        (PCRE2_SPTR)"[,;:][\"')\\]]*(?=\\s|$)", PCRE2_ZERO_TERMINATED,
        PCRE2_UTF | PCRE2_UCP, &error, &offset, NULL);
}

static int last_boundary(pcre2_code *code, const char *window, size_t window_len,
                         size_t limit, size_t maximum, size_t *end) {
    pcre2_match_data *data = pcre2_match_data_create_from_pattern(code, NULL);
    if (!data) return -1;
    PCRE2_SIZE start = 0;
    size_t shorter = 0;
    size_t first = 0;
    while (start < window_len) {
        int rc = pcre2_match(code, (PCRE2_SPTR)window, window_len, start, 0, data, NULL);
        if (rc < 0) break;
        PCRE2_SIZE *ov = pcre2_get_ovector_pointer(data);
        if (ov[1] <= maximum) {
            if (!first) first = ov[1];
            if (ov[1] <= limit) shorter = ov[1];
        }
        start = ov[1];
    }
    pcre2_match_data_free(data);
    if (!first) return 0;
    *end = shorter ? shorter : first;
    return 1;
}

int speech_chunks(const char *text, char **out, size_t *count, size_t cap) {
    if (!count || (!out && cap)) return -1;
    *count = 0;
    if (!text) return 0;
    if (!g_utf8_validate(text, -1, NULL)) return -1;
    pthread_once(&patterns_once, compile_patterns);
    if (!sentence_re || !clause_re) return -1;
    GString *normalized = g_string_new(NULL);
    int space = 1;
    for (const char *p = text; *p; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if (g_unichar_isspace(c)) {
            if (!space) g_string_append_c(normalized, ' ');
            space = 1;
        } else {
            g_string_append_unichar(normalized, c);
            space = 0;
        }
    }
    if (normalized->len && space) g_string_truncate(normalized, normalized->len - 1);
    const char *remaining = normalized->str;
    int result = 0;
    while (*remaining) {
        if (*count >= cap) { result = -1; break; }
        size_t chars = (size_t)g_utf8_strlen(remaining, -1);
        size_t limit_chars = *count ? CHUNK_MAX : CHUNK_FIRST;
        size_t end = strlen(remaining);
        if (chars > limit_chars) {
            size_t limit = (size_t)(g_utf8_offset_to_pointer(remaining, limit_chars) - remaining);
            size_t maximum = (size_t)(g_utf8_offset_to_pointer(remaining, MIN(chars, CHUNK_MAX)) - remaining);
            size_t window = (size_t)(g_utf8_offset_to_pointer(remaining, MIN(chars, CHUNK_MAX + 1)) - remaining);
            int found = last_boundary(sentence_re, remaining, window, limit, maximum, &end);
            if (!found) found = last_boundary(clause_re, remaining, window, limit, maximum, &end);
            if (found < 0) { result = -1; break; }
            if (!found) {
                end = limit;
                for (size_t i = 1; i <= limit; i++)
                    if (remaining[i] == ' ') end = i;
            }
        }
        size_t cut = end;
        while (cut && remaining[cut - 1] == ' ') cut--;
        out[*count] = strndup(remaining, cut);
        if (!out[*count]) { result = -1; break; }
        (*count)++;
        remaining += end;
        while (*remaining == ' ') remaining++;
    }
    g_string_free(normalized, TRUE);
    return result;
}
