#define _POSIX_C_SOURCE 200809L
#include "spoken.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <glib.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { SPOKEN_MAX_CHARS = 1500, SPOKEN_MAX_WORDS = 120 };

static pcre2_code *fence_re;
static pcre2_code *label_re;
static pcre2_code *section_re;
static pcre2_code *link_re;
static pcre2_code *list_re;
static pthread_once_t patterns_once = PTHREAD_ONCE_INIT;

static pcre2_code *compile(const char *pattern, uint32_t options) {
    int error = 0;
    PCRE2_SIZE offset = 0;
    return pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
                        options | PCRE2_UTF | PCRE2_UCP, &error, &offset, NULL);
}

static void compile_patterns(void) {
    fence_re = compile("^ {0,3}(`{3,}|~{3,})(.*)$", 0);
    label_re = compile(
        "^ {0,3}(?:#{1,6}[ \\t]+)?(?:Spoken summary|Summary|TL;DR|TLDR)"
        "(?::[ \\t]*(.*)|[ \\t]*)$", PCRE2_CASELESS);
    section_re = compile("^ {0,3}(?:#{1,6}(?:\\s|$)|(?:=+|-+)\\s*$)", 0);
    link_re = compile("!?\\[([^\\]]+)\\]\\([^\\n)]*\\)", 0);
    list_re = compile("^\\s*(?:[-*+]\\s+|\\d+[.)]\\s+|>\\s*)", PCRE2_MULTILINE);
}

static int match(pcre2_code *code, const char *subject, PCRE2_SIZE *ovector, uint32_t ocount) {
    pcre2_match_data *data = pcre2_match_data_create(ocount, NULL);
    if (!data) return PCRE2_ERROR_NOMEMORY;
    int rc = pcre2_match(code, (PCRE2_SPTR)subject, PCRE2_ZERO_TERMINATED, 0, 0, data, NULL);
    if (rc > 0) memcpy(ovector, pcre2_get_ovector_pointer(data), sizeof(PCRE2_SIZE) * ocount * 2);
    pcre2_match_data_free(data);
    return rc;
}

static void strip_markup(char *text, int bold_only) {
    char *dst = text;
    for (const char *p = text; *p; p++) {
        if ((*p == '*' && p[1] == '*') || (!bold_only && *p == '_' && p[1] == '_')) {
            p++;
            continue;
        }
        if (!bold_only && *p == '`') continue;
        *dst++ = *p;
    }
    *dst = 0;
}

static int whitespace_only(const char *text) {
    for (const char *p = text; *p; p = g_utf8_next_char(p))
        if (!g_unichar_isspace(g_utf8_get_char(p))) return 0;
    return 1;
}

static char *replace_matches(pcre2_code *code, const char *text, int keep_label) {
    pcre2_match_data *data = pcre2_match_data_create_from_pattern(code, NULL);
    if (!data) return NULL;
    GString *out = g_string_new(NULL);
    size_t cursor = 0, len = strlen(text);
    while (cursor < len) {
        /* Preserve the original subject so ^ never matches a removed prefix. */
        int rc = pcre2_match(code, (PCRE2_SPTR)text, len, cursor, 0, data, NULL);
        if (rc == PCRE2_ERROR_NOMATCH) { g_string_append(out, text + cursor); break; }
        if (rc < 0) { g_string_free(out, TRUE); pcre2_match_data_free(data); return NULL; }
        PCRE2_SIZE *ov = pcre2_get_ovector_pointer(data);
        g_string_append_len(out, text + cursor, ov[0] - cursor);
        if (keep_label) g_string_append_len(out, text + ov[2], ov[3] - ov[2]);
        cursor = ov[1];
    }
    pcre2_match_data_free(data);
    return g_string_free(out, FALSE);
}

int spoken_text(const char *text, char *out, size_t out_cap) {
    if (!out || out_cap == 0) return -1;
    out[0] = 0;
    if (!text || !g_utf8_validate(text, -1, NULL)) return 0;
    pthread_once(&patterns_once, compile_patterns);
    if (!fence_re || !label_re || !section_re || !link_re || !list_re) return -1;

    gchar **split = g_strsplit(text, "\n", -1);
    GPtrArray *lines = g_ptr_array_new();
    char *fence = NULL;
    for (gchar **line = split; *line; line++) {
        size_t len = strlen(*line);
        if (len && (*line)[len - 1] == '\r') (*line)[len - 1] = 0;
        PCRE2_SIZE ov[8];
        int marked = match(fence_re, *line, ov, 4) > 0;
        if (fence) {
            if (marked && (*line)[ov[2]] == fence[0] && ov[3] - ov[2] >= strlen(fence)
                && whitespace_only(*line + ov[4])) {
                g_free(fence);
                fence = NULL;
            }
        } else if (marked) {
            fence = g_strndup(*line + ov[2], ov[3] - ov[2]);
        } else {
            g_ptr_array_add(lines, *line);
        }
    }
    g_free(fence);

    int start = -1;
    char *inline_text = g_strdup("");
    for (guint i = 0; i < lines->len; i++) {
        char *stripped = g_strdup(g_ptr_array_index(lines, i));
        strip_markup(stripped, 1);
        PCRE2_SIZE ov[8];
        if (match(label_re, stripped, ov, 4) > 0) {
            start = (int)i + 1;
            g_free(inline_text);
            inline_text = ov[2] != PCRE2_UNSET ? g_strndup(stripped + ov[2], ov[3] - ov[2]) : g_strdup("");
        }
        g_free(stripped);
    }
    GString *joined = g_string_new(inline_text);
    g_free(inline_text);
    int valid = start >= 0;
    if (valid) {
        for (guint i = (guint)start; i < lines->len; i++) {
            const char *line = g_ptr_array_index(lines, i);
            PCRE2_SIZE ov[4];
            if (match(section_re, line, ov, 2) > 0) { valid = 0; break; }
            g_string_append_c(joined, '\n');
            g_string_append(joined, line);
        }
    }
    g_ptr_array_free(lines, TRUE);
    g_strfreev(split);
    if (!valid) { g_string_free(joined, TRUE); return 0; }

    char *links = replace_matches(link_re, joined->str, 1);
    char *clean = links ? replace_matches(list_re, links, 0) : NULL;
    g_free(links);
    g_string_free(joined, TRUE);
    if (!clean) return -1;
    strip_markup(clean, 0);
    GString *normalized = g_string_new(NULL);
    int space = 1, alnum = 0, words = 0;
    size_t chars = 0;
    for (const char *p = clean; *p; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if (g_unichar_isspace(c)) {
            if (!space) { g_string_append_c(normalized, ' '); chars++; }
            space = 1;
        } else {
            if (space) words++;
            g_string_append_unichar(normalized, c);
            chars++;
            if (g_unichar_isalnum(c)) alnum = 1;
            space = 0;
        }
    }
    g_free(clean);
    if (normalized->len && space) { g_string_truncate(normalized, normalized->len - 1); chars--; }
    int result = 0;
    if (chars <= SPOKEN_MAX_CHARS && words <= SPOKEN_MAX_WORDS && alnum) {
        if (normalized->len >= out_cap) result = -1;
        else memcpy(out, normalized->str, normalized->len + 1);
    }
    g_string_free(normalized, TRUE);
    return result;
}
