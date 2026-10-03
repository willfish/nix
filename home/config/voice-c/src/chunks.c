#define _POSIX_C_SOURCE 200809L
#include "chunks.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

enum { CHUNK_FIRST = 120, CHUNK_MAX = 260 };

static pcre2_code *sentence_re;
static pcre2_code *clause_re;

static void ensure(void) {
    if (sentence_re) return;
    int error = 0;
    PCRE2_SIZE offset = 0;
    sentence_re = pcre2_compile(
        (PCRE2_SPTR)"[.!?][\"')\\]]*(?=\\s|$)", PCRE2_ZERO_TERMINATED,
        PCRE2_UTF, &error, &offset, NULL);
    clause_re = pcre2_compile(
        (PCRE2_SPTR)"[,;:][\"')\\]]*(?=\\s|$)", PCRE2_ZERO_TERMINATED,
        PCRE2_UTF, &error, &offset, NULL);
}

static void normalize(const char *src, char *dst, size_t cap) {
    size_t n = 0;
    int space = 1;
    for (const unsigned char *p = (const unsigned char *)src; *p && n + 1 < cap; p++) {
        if (isspace(*p)) {
            if (!space) dst[n++] = ' ';
            space = 1;
        } else {
            dst[n++] = *p;
            space = 0;
        }
    }
    if (n && dst[n - 1] == ' ') n--;
    dst[n] = 0;
}

static int last_boundary(pcre2_code *code, const char *window, size_t window_len, size_t limit, size_t *end) {
    pcre2_match_data *data = pcre2_match_data_create(4, NULL);
    PCRE2_SIZE start = 0;
    size_t shorter = 0;
    size_t first = 0;
    int found = 0;
    while (start < window_len) {
        int rc = pcre2_match(code, (PCRE2_SPTR)window, window_len, start, 0, data, NULL);
        if (rc < 0) break;
        PCRE2_SIZE *ov = pcre2_get_ovector_pointer(data);
        if (ov[1] <= CHUNK_MAX) {
            if (!found) first = ov[1];
            found = 1;
            if (ov[1] <= limit) shorter = ov[1];
        }
        start = ov[1] > start ? ov[1] : start + 1;
    }
    pcre2_match_data_free(data);
    if (!found) return 0;
    *end = shorter ? shorter : first;
    return 1;
}

int speech_chunks(const char *text, char **out, size_t *count, size_t cap) {
    *count = 0;
    if (!text) return 0;
    ensure();
    char remaining[8192];
    normalize(text, remaining, sizeof remaining);
    while (remaining[0]) {
        if (*count >= cap) return -1;
        size_t len = strlen(remaining);
        size_t limit = *count ? CHUNK_MAX : CHUNK_FIRST;
        if (len <= limit) {
            out[*count] = strdup(remaining);
            if (!out[*count]) return -1;
            (*count)++;
            break;
        }
        size_t window = len < CHUNK_MAX + 1 ? len : CHUNK_MAX + 1;
        char probe[CHUNK_MAX + 2];
        memcpy(probe, remaining, window);
        probe[window] = 0;
        size_t end = 0;
        if (!last_boundary(sentence_re, probe, window, limit, &end)
            && !last_boundary(clause_re, probe, window, limit, &end)) {
            char *space = NULL;
            for (size_t i = 0; i < limit + 1 && i < len; i++) {
                if (remaining[i] == ' ') space = remaining + i;
            }
            end = space && space > remaining ? (size_t)(space - remaining) : limit;
        }
        size_t cut = end;
        while (cut && isspace((unsigned char)remaining[cut - 1])) cut--;
        out[*count] = strndup(remaining, cut);
        if (!out[*count]) return -1;
        (*count)++;
        const char *next = remaining + end;
        while (*next && isspace((unsigned char)*next)) next++;
        memmove(remaining, next, strlen(next) + 1);
    }
    return 0;
}
