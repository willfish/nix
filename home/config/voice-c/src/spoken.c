#define _POSIX_C_SOURCE 200809L
#include "spoken.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { SPOKEN_MAX_CHARS = 1500, SPOKEN_MAX_WORDS = 120, SPOKEN_MAX_LINES = 4096 };

static pcre2_code *fence_re;
static pcre2_code *label_re;
static pcre2_code *section_re;
static pcre2_code *link_re;
static pcre2_code *list_re;

static pcre2_code *compile(const char *pattern, uint32_t options) {
    int error = 0;
    PCRE2_SIZE offset = 0;
    pcre2_code *code = pcre2_compile(
        (PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
        options | PCRE2_UTF | PCRE2_UCP, &error, &offset, NULL);
    return code;
}

static void ensure_patterns(void) {
    if (fence_re) return;
    fence_re = compile("^ {0,3}(`{3,}|~{3,})(.*)$", 0);
    label_re = compile(
        "^ {0,3}(?:#{1,6}[ \\t]+)?(?:Spoken summary|Summary|TL;DR|TLDR)"
        "(?::[ \\t]*(.*)|[ \\t]*)$",
        PCRE2_CASELESS);
    section_re = compile("^ {0,3}(?:#{1,6}(?:\\s|$)|(?:=+|-+)\\s*$)", 0);
    link_re = compile("!?\\[([^\\]]+)\\]\\([^\\n)]*\\)", 0);
    list_re = compile("^[ \\t]*(?:[-*+]\\s+|\\d+[.)]\\s+|>\\s*)", PCRE2_MULTILINE);
}

static int match(pcre2_code *code, const char *subject, PCRE2_SIZE *ovector, uint32_t ocount) {
    pcre2_match_data *data = pcre2_match_data_create(ocount, NULL);
    int rc = pcre2_match(code, (PCRE2_SPTR)subject, PCRE2_ZERO_TERMINATED, 0, 0, data, NULL);
    if (rc > 0) {
        PCRE2_SIZE *found = pcre2_get_ovector_pointer(data);
        memcpy(ovector, found, sizeof(PCRE2_SIZE) * ocount * 2);
    }
    pcre2_match_data_free(data);
    return rc;
}

static void strip_bold(const char *src, char *dst, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; src[i] && n + 1 < cap; i++) {
        if (src[i] == '*' && src[i + 1] == '*') {
            i++;
            continue;
        }
        dst[n++] = src[i];
    }
    dst[n] = 0;
}

static int is_word_char(unsigned char c) {
    return isalnum(c) || c >= 128;
}

static int has_alnum(const char *text) {
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (is_word_char(*p)) return 1;
    }
    return 0;
}

static int word_count(const char *text) {
    int words = 0;
    int in = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (isspace(*p)) in = 0;
        else if (!in) {
            in = 1;
            words++;
        }
    }
    return words;
}

static char *replace_links(const char *text) {
    PCRE2_SIZE ovector[8];
    size_t cap = strlen(text) + 1;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t n = 0;
    const char *cursor = text;
    while (*cursor) {
        int rc = match(link_re, cursor, ovector, 4);
        if (rc < 0) {
            size_t rest = strlen(cursor);
            memcpy(out + n, cursor, rest);
            n += rest;
            break;
        }
        memcpy(out + n, cursor, ovector[0]);
        n += ovector[0];
        size_t label_len = ovector[3] - ovector[2];
        memcpy(out + n, cursor + ovector[2], label_len);
        n += label_len;
        cursor += ovector[1];
    }
    out[n] = 0;
    return out;
}

static char *strip_lists(const char *text) {
    size_t cap = strlen(text) + 1;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t n = 0;
    const char *line = text;
    while (*line) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        char saved[4096];
        if (len >= sizeof saved) len = sizeof saved - 1;
        memcpy(saved, line, len);
        saved[len] = 0;
        PCRE2_SIZE ovector[6];
        int rc = match(list_re, saved, ovector, 3);
        size_t skip = rc > 0 ? ovector[1] : 0;
        memcpy(out + n, saved + skip, len - skip);
        n += len - skip;
        if (!end) break;
        out[n++] = '\n';
        line = end + 1;
    }
    out[n] = 0;
    return out;
}

static void collapse(const char *src, char *dst, size_t cap) {
    size_t n = 0;
    int space = 1;
    for (const unsigned char *p = (const unsigned char *)src; *p && n + 1 < cap; p++) {
        if (isspace(*p)) {
            if (!space && n + 1 < cap) dst[n++] = ' ';
            space = 1;
        } else {
            dst[n++] = *p;
            space = 0;
        }
    }
    if (n && dst[n - 1] == ' ') n--;
    dst[n] = 0;
}

int spoken_text(const char *text, char *out, size_t out_cap) {
    if (!out || out_cap == 0) return -1;
    out[0] = 0;
    if (!text) return 0;
    ensure_patterns();
    if (!fence_re || !label_re || !section_re || !link_re || !list_re) return -1;

    char *copy = strdup(text);
    if (!copy) return -1;
    char *lines[SPOKEN_MAX_LINES];
    int count = 0;
    char *fence = NULL;
    char *cursor = copy;
    while (cursor && count < SPOKEN_MAX_LINES) {
        char *nl = strchr(cursor, '\n');
        if (nl) *nl = 0;
        size_t len = strlen(cursor);
        if (len && cursor[len - 1] == '\r') cursor[len - 1] = 0;
        PCRE2_SIZE ovector[8];
        int marked = match(fence_re, cursor, ovector, 4) > 0;
        if (fence) {
            if (marked && cursor[ovector[2]] == fence[0]
                && (ovector[3] - ovector[2]) >= strlen(fence)
                && cursor[ovector[4]] == 0) {
                free(fence);
                fence = NULL;
            }
        } else if (marked) {
            size_t flen = ovector[3] - ovector[2];
            fence = strndup(cursor + ovector[2], flen);
        } else {
            lines[count++] = cursor;
        }
        if (!nl) break;
        cursor = nl + 1;
    }

    int start = -1;
    char inline_text[4096] = "";
    for (int i = 0; i < count; i++) {
        char stripped[4096];
        strip_bold(lines[i], stripped, sizeof stripped);
        PCRE2_SIZE ovector[8] = {0};
        if (match(label_re, stripped, ovector, 4) > 0) {
            start = i + 1;
            inline_text[0] = 0;
            if (ovector[2] != PCRE2_UNSET && ovector[3] > ovector[2]) {
                size_t n = ovector[3] - ovector[2];
                if (n >= sizeof inline_text) n = sizeof inline_text - 1;
                memcpy(inline_text, stripped + ovector[2], n);
                inline_text[n] = 0;
            }
        }
    }
    if (start < 0) {
        free(fence);
        free(copy);
        return 0;
    }
    for (int i = start; i < count; i++) {
        PCRE2_SIZE ovector[4];
        if (match(section_re, lines[i], ovector, 2) > 0) {
            free(fence);
            free(copy);
            return 0;
        }
    }

    size_t joined_cap = strlen(inline_text) + 2;
    for (int i = start; i < count; i++) joined_cap += strlen(lines[i]) + 1;
    char *joined = malloc(joined_cap);
    if (!joined) {
        free(fence);
        free(copy);
        return -1;
    }
    strcpy(joined, inline_text);
    for (int i = start; i < count; i++) {
        strcat(joined, "\n");
        strcat(joined, lines[i]);
    }
    char *links = replace_links(joined);
    char *lists = links ? strip_lists(links) : NULL;
    if (lists) {
        char *star = lists;
        for (char *p = lists; *p; p++) {
            if ((*p == '*' && p[1] == '*') || (*p == '_' && p[1] == '_')) {
                p++;
                continue;
            }
            if (*p == '`') continue;
            *star++ = *p;
        }
        *star = 0;
        collapse(lists, out, out_cap);
    }
    free(lists);
    free(links);
    free(joined);
    free(fence);
    free(copy);
    if (strlen(out) > SPOKEN_MAX_CHARS || word_count(out) > SPOKEN_MAX_WORDS || !has_alnum(out))
        out[0] = 0;
    return 0;
}
