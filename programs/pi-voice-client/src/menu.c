#ifndef VOICE_MENU_NO_MAIN
#define voice_menu_rows_free voice_menu_rows_free_exe
#define voice_session_description voice_session_description_exe
#define voice_menu_rows voice_menu_rows_exe
#define voice_menu_empty_reason voice_menu_empty_reason_exe
#define voice_menu_prompt voice_menu_prompt_exe
#define voice_menu_run voice_menu_run_exe
#define voice_menu_fuzzel_plan voice_menu_fuzzel_plan_exe
#define voice_menu_interpret_fuzzel voice_menu_interpret_fuzzel_exe
#define voice_menu_pick_fuzzel voice_menu_pick_fuzzel_exe
#endif
#include "menu.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *xstrdup(const char *text) {
    size_t n;
    char *copy;
    if (!text) return NULL;
    n = strlen(text) + 1;
    copy = malloc(n);
    if (!copy) return NULL;
    memcpy(copy, text, n);
    return copy;
}

static int fail(char **error, const char *message) {
    if (error && !*error) *error = xstrdup(message ? message : "Voice menu failed");
    return -1;
}

static int utf8_next(const char *s, unsigned *cp, size_t *len) {
    const unsigned char *u = (const unsigned char *)s;
    if (!u || !u[0]) return 0;
    if (u[0] < 0x80) { *cp = u[0]; *len = 1; return 1; }
    if ((u[0] & 0xE0) == 0xC0 && (u[1] & 0xC0) == 0x80) {
        *cp = ((u[0] & 0x1F) << 6) | (u[1] & 0x3F); *len = 2; return 1;
    }
    if ((u[0] & 0xF0) == 0xE0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80) {
        *cp = ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F); *len = 3; return 1;
    }
    if ((u[0] & 0xF8) == 0xF0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80 && (u[3] & 0xC0) == 0x80) {
        *cp = ((u[0] & 0x07) << 18) | ((u[1] & 0x3F) << 12) | ((u[2] & 0x3F) << 6) | (u[3] & 0x3F);
        *len = 4; return 1;
    }
    *cp = u[0]; *len = 1; return 1;
}

static size_t cp_len(const char *text) {
    size_t n = 0;
    unsigned cp; size_t len;
    if (!text) return 0;
    while (*text && utf8_next(text, &cp, &len)) { n++; text += len; }
    return n;
}

static void cp_slice(const char *text, size_t count, char *out, size_t cap) {
    size_t used = 0, n = 0;
    unsigned cp; size_t len;
    if (!out || !cap) return;
    out[0] = 0;
    if (!text) return;
    while (*text && n < count && utf8_next(text, &cp, &len)) {
        if (used + len + 1 > cap) break;
        memcpy(out + used, text, len);
        used += len;
        out[used] = 0;
        text += len;
        n++;
    }
}

static int is_utf8_space(unsigned cp) {
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0xA0 || cp == 0x3000
        || (cp >= 0x2000 && cp <= 0x200A);
}

static void rstrip_cp(char *text) {
    size_t bytes;
    if (!text) return;
    bytes = strlen(text);
    while (bytes) {
        size_t start = bytes - 1;
        unsigned cp; size_t len;
        while (start > 0 && (text[start] & 0xC0) == 0x80) start--;
        if (!utf8_next(text + start, &cp, &len)) break;
        if (!is_utf8_space(cp)) break;
        text[start] = 0;
        bytes = start;
    }
}

static int rsplit_dot(const char *text, char *head, size_t head_cap, char *tail, size_t tail_cap) {
    const char *sep = " \xc2\xb7 ";
    const char *found = NULL;
    const char *cursor = text;
    size_t head_len;
    if (!text) return 0;
    while ((cursor = strstr(cursor, sep))) {
        found = cursor;
        cursor += 1;
    }
    if (!found) return 0;
    head_len = (size_t)(found - text);
    if (head_len + 1 > head_cap || strlen(found + 4) + 1 > tail_cap) return 0;
    memcpy(head, text, head_len);
    head[head_len] = 0;
    snprintf(tail, tail_cap, "%s", found + 4);
    return 1;
}

static char *public_owned(const char *value) {
    size_t cap = (value ? strlen(value) : 0) * 4 + 8;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    if (voice_public_label(value, VOICE_LABEL_UNLIMITED, buf, cap)) {
        free(buf);
        return xstrdup("");
    }
    return buf;
}

static char *finish_clip(char *full, const char *out) {
    char *kept = xstrdup(out);
    free(full);
    return kept;
}

static char *clip_label(const char *value, size_t limit) {
    char *full = public_owned(value);
    char head[4096];
    char tail[1024];
    char out[4096];
    if (!full) return NULL;
    if (cp_len(full) <= limit) return full;
    if (rsplit_dot(full, head, sizeof head, tail, sizeof tail)) {
        size_t tail_n = cp_len(tail);
        if (limit >= tail_n + 3) {
            size_t room = limit - tail_n - 3;
            if (room >= 8) {
                cp_slice(head, room - 1, out, sizeof out);
                rstrip_cp(out);
                snprintf(out + strlen(out), sizeof out - strlen(out), "\xe2\x80\xa6 \xc2\xb7 %s", tail);
                return finish_clip(full, out);
            }
        }
    }
    cp_slice(full, limit ? limit - 1 : 0, out, sizeof out);
    rstrip_cp(out);
    snprintf(out + strlen(out), sizeof out - strlen(out), "\xe2\x80\xa6");
    return finish_clip(full, out);
}

static char *fit_prompt(const char *value, size_t limit) {
    char *full = public_owned(value);
    char head[4096];
    char tail[1024];
    char out[4096];
    size_t full_n;
    if (!full) return NULL;
    full_n = cp_len(full);
    if (full_n <= limit) return full;
    if (rsplit_dot(full, head, sizeof head, tail, sizeof tail)) {
        size_t tail_n = cp_len(tail);
        size_t head_n = cp_len(head);
        if (limit >= tail_n + 3) {
            size_t room = limit - tail_n - 3;
            if (room >= 8 && room < head_n) {
                cp_slice(head, room - 1, out, sizeof out);
                rstrip_cp(out);
                snprintf(out + strlen(out), sizeof out - strlen(out), "\xe2\x80\xa6 \xc2\xb7 %s", tail);
                return finish_clip(full, out);
            }
        }
    }
    cp_slice(full, limit ? limit - 1 : 0, out, sizeof out);
    rstrip_cp(out);
    snprintf(out + strlen(out), sizeof out - strlen(out), "\xe2\x80\xa6");
    return finish_clip(full, out);
}

static int append_row(VoiceMenuRow **rows, size_t *count, const char *action, const char *label) {
    VoiceMenuRow *next = realloc(*rows, (*count + 1) * sizeof *next);
    if (!next) return -1;
    *rows = next;
    next[*count].action = xstrdup(action);
    next[*count].label = xstrdup(label ? label : "");
    if (!next[*count].action || !next[*count].label) return -1;
    (*count)++;
    return 0;
}

void voice_menu_rows_free(VoiceMenuRow *rows, size_t count) {
    size_t i;
    if (!rows) return;
    for (i = 0; i < count; i++) {
        free(rows[i].action);
        free(rows[i].label);
    }
    free(rows);
}

static const VoiceIdentity *identity_for(const VoicePresentation *view, const char *action) {
    size_t i;
    if (!view || !action) return NULL;
    for (i = 0; i < view->identity_count; i++) {
        if (strcmp(view->identities[i].action, action) == 0) return &view->identities[i];
    }
    return NULL;
}

static char *identity_source(const VoicePresentation *view, const char *action) {
    const VoiceIdentity *identity = identity_for(view, action);
    char raw[1024];
    const char *source = action ? action : "";
    if (identity) {
        if (identity->id && identity->id[0]) source = identity->id;
        else if (identity->token && identity->token[0]) source = identity->token;
    }
    if (voice_public_label(source, VOICE_LABEL_UNLIMITED, raw, sizeof raw) || !raw[0]) return xstrdup("session");
    return xstrdup(raw);
}

static int same_prefix(const char *left, const char *right, size_t size) {
    char a[512], b[512];
    cp_slice(left, size, a, sizeof a);
    cp_slice(right, size, b, sizeof b);
    return strcmp(a, b) == 0;
}

int voice_session_description(const VoiceSessionRow *row, char **out, char **error) {
    char full[4096];
    char parts[16][512];
    int count = 0;
    char *cursor;
    char joined[4096];
    size_t used = 0;
    int i;
    const char *thinking;
    if (!out) return fail(error, "Session description needs an output");
    *out = NULL;
    if (!row || !row->full_label || !row->full_label[0]) {
        *out = xstrdup(row && row->label ? row->label : "");
        return *out ? 0 : fail(error, "Out of memory");
    }
    if (voice_public_label(row->full_label, VOICE_LABEL_UNLIMITED, full, sizeof full)) return fail(error, "Session label is too long");
    cursor = full;
    while (count < 16) {
        char *sep = strstr(cursor, " \xc2\xb7 ");
        size_t n = sep ? (size_t)(sep - cursor) : strlen(cursor);
        if (n >= sizeof parts[0]) n = sizeof parts[0] - 1;
        memcpy(parts[count], cursor, n);
        parts[count][n] = 0;
        count++;
        if (!sep) break;
        cursor = sep + 4;
    }
    if (count && (strcmp(parts[0], "pi") == 0 || strcmp(parts[0], "qwen-pi") == 0)) {
        memmove(parts, parts + 1, (size_t)(count - 1) * sizeof parts[0]);
        count--;
    }
    if (row->selected) {
        for (i = 0; i < count; i++) {
            if (strcmp(parts[i], "selected") == 0) {
                memmove(parts + i, parts + i + 1, (size_t)(count - i - 1) * sizeof parts[0]);
                count--;
                break;
            }
        }
    }
    thinking = row->thinking;
    if (thinking && thinking[0]) {
        for (i = 0; i < count; i++) {
            if (strcmp(parts[i], thinking) == 0) {
                memmove(parts + i, parts + i + 1, (size_t)(count - i - 1) * sizeof parts[0]);
                count--;
                break;
            }
        }
    }
    joined[0] = 0;
    for (i = 0; i < count; i++) {
        int wrote = snprintf(joined + used, sizeof joined - used, "%s%s", i ? " \xc2\xb7 " : "", parts[i]);
        if (wrote < 0 || (size_t)wrote >= sizeof joined - used) return fail(error, "Session label is too long");
        used += (size_t)wrote;
    }
    *out = xstrdup(joined);
    return *out ? 0 : fail(error, "Out of memory");
}

static int present_labels(VoiceMenuRow **shown, size_t *shown_count, const char *const *actions,
    const char *const *labels, const int *marked, size_t count, const VoicePresentation *view) {
    char **clipped = calloc(count ? count : 1, sizeof *clipped);
    size_t *colliding = calloc(count ? count : 1, sizeof *colliding);
    char **suffixes = calloc(count ? count : 1, sizeof *suffixes);
    size_t collisions = 0;
    size_t i, j;
    int rc = 0;
    if (!clipped || !colliding || !suffixes) {
        free(clipped); free(colliding); free(suffixes);
        return -1;
    }
    for (i = 0; i < count; i++) {
        clipped[i] = clip_label(labels[i], 50);
        if (!clipped[i]) rc = -1;
    }
    for (i = 0; i < count && !rc; i++) {
        size_t copies = 0;
        for (j = 0; j < count; j++) if (strcmp(clipped[i], clipped[j]) == 0) copies++;
        if (copies > 1) colliding[collisions++] = i;
    }
    if (collisions && !rc) {
        char **sources = calloc(collisions, sizeof *sources);
        size_t longest = 0;
        size_t size = 4;
        int unique = 0;
        if (!sources) rc = -1;
        for (i = 0; i < collisions && !rc; i++) {
            sources[i] = identity_source(view, actions[colliding[i]]);
            if (!sources[i]) rc = -1;
            else if (cp_len(sources[i]) > longest) longest = cp_len(sources[i]);
        }
        while (!rc && size < longest && !unique) {
            unique = 1;
            for (i = 0; i < collisions && unique; i++) {
                for (j = i + 1; j < collisions; j++) {
                    if (same_prefix(sources[i], sources[j], size)) unique = 0;
                }
            }
            if (!unique) size++;
        }
        for (i = 0; i < collisions && !rc; i++) {
            char cut[512];
            cp_slice(sources[i], size, cut, sizeof cut);
            suffixes[colliding[i]] = xstrdup(cut);
            if (!suffixes[colliding[i]]) rc = -1;
        }
        for (i = 0; i < collisions; i++) free(sources ? sources[i] : NULL);
        free(sources);
    }
    for (i = 0; i < count && !rc; i++) {
        char line[1024];
        char *label = clipped[i];
        char *reshaped = NULL;
        if (suffixes[i]) {
            size_t room = 50 - cp_len(suffixes[i]) - 3;
            if (room < 8) room = 8;
            reshaped = clip_label(label, room);
            if (!reshaped) rc = -1;
            else {
                snprintf(line, sizeof line, "%s \xc2\xb7 %s", reshaped, suffixes[i]);
                free(reshaped);
                label = line;
            }
        }
        if (!rc) {
            char marked_line[1200];
            const char *final_label = label;
            if (marked[i]) {
                snprintf(marked_line, sizeof marked_line, "* %s", label);
                final_label = marked_line;
            }
            if (append_row(shown, shown_count, actions[i], final_label)) rc = -1;
        }
    }
    for (i = 0; i < count; i++) free(clipped[i]);
    for (i = 0; i < count; i++) free(suffixes[i]);
    free(clipped);
    free(colliding);
    free(suffixes);
    return rc;
}

static const VoiceSessionRow *session_by_action(const VoiceMenuStatus *status, const char *action) {
    size_t i;
    if (!status || !action || strncmp(action, "select:", 7) != 0) return NULL;
    for (i = 0; i < status->session_count; i++) {
        if (status->sessions[i].token && strcmp(action + 7, status->sessions[i].token) == 0)
            return &status->sessions[i];
    }
    return NULL;
}

static int needs_confirmation(const VoiceMenuStatus *status) {
    return status && status->retained && status->harness
        && (strcmp(status->harness, "pi") == 0 || strcmp(status->harness, "qwen-pi") == 0)
        && !(status->selection_explicit_missing || status->selection_explicit)
        && status->pane && status->pane[0];
}

static int live_stop(const VoiceMenuStatus *status) {
    const char *connection = status ? status->connection_state : NULL;
    return voice_menu_busy(status) || (status && status->speaking)
        || (connection && (strcmp(connection, "connecting") == 0 || strcmp(connection, "reconnecting") == 0))
        || (status && status->models && strcmp(status->models, "loading") == 0);
}

static int grow_row_slots(const char ***actions, const char ***labels, char ***owned, int **marked,
    size_t *cap, size_t need) {
    const char **next_actions;
    const char **next_labels;
    char **next_owned;
    int *next_marked;
    size_t next = *cap ? *cap : 8;
    while (next < need) next *= 2;
    next_actions = realloc(*actions, next * sizeof *next_actions);
    if (!next_actions && next) return -1;
    *actions = next_actions;
    next_labels = realloc(*labels, next * sizeof *next_labels);
    if (!next_labels && next) return -1;
    *labels = next_labels;
    if (owned) {
        next_owned = realloc(*owned, next * sizeof *next_owned);
        if (!next_owned && next) return -1;
        *owned = next_owned;
    }
    next_marked = realloc(*marked, next * sizeof *next_marked);
    if (!next_marked && next) return -1;
    *marked = next_marked;
    *cap = next;
    return 0;
}

static int session_pairs(const VoiceMenuStatus *status, const VoicePresentation *view,
    VoiceMenuRow **rows, size_t *count) {
    const char **actions = NULL;
    const char **labels_store = NULL;
    char **owned = NULL;
    int *marked = NULL;
    size_t n = 0, cap = 0, i;
    int rc = 0;
    if (!view) return -1;
    for (i = 0; i < view->action_count; i++) {
        const VoiceAction *action = &view->actions[i];
        const VoiceSessionRow *row;
        int selected_row = 0;
        if (strncmp(action->action, "select:", 7) != 0) continue;
        row = session_by_action(status, action->action);
        selected_row = row && row->selected && !voice_menu_busy(status);
        if (!action->enabled && !selected_row) continue;
        if (n == cap && grow_row_slots(&actions, &labels_store, &owned, &marked, &cap, n + 1)) {
            rc = -1;
            break;
        }
        owned[n] = NULL;
        if (row && row->full_label && row->full_label[0] && !needs_confirmation(status)) {
            char *described = NULL;
            if (voice_session_description(row, &described, NULL)) described = xstrdup(row->label ? row->label : "");
            owned[n] = described ? fit_prompt(described, 50) : NULL;
            free(described);
            labels_store[n] = owned[n] ? owned[n] : action->label;
        } else labels_store[n] = action->label;
        actions[n] = action->action;
        marked[n] = selected_row || (view->selected_session && strcmp(action->action, view->selected_session) == 0);
        n++;
    }
    if (!rc && present_labels(rows, count, actions, labels_store, marked, n, view)) rc = -1;
    for (i = 0; i < n; i++) free(owned[i]);
    free(actions);
    free(labels_store);
    free(owned);
    free(marked);
    return rc;
}

static int prefixed_rows(const VoicePresentation *view, const char *prefix, const char *marked_action,
    VoiceMenuRow **rows, size_t *count) {
    const char **actions = NULL;
    const char **labels = NULL;
    int *marked = NULL;
    size_t n = 0, cap = 0, i;
    int rc = 0;
    size_t prefix_len = prefix ? strlen(prefix) : 0;
    if (!view) return -1;
    for (i = 0; i < view->action_count; i++) {
        if (strncmp(view->actions[i].action, prefix, prefix_len) != 0 || !view->actions[i].enabled) continue;
        if (n == cap && grow_row_slots(&actions, &labels, NULL, &marked, &cap, n + 1)) {
            rc = -1;
            break;
        }
        actions[n] = view->actions[i].action;
        labels[n] = view->actions[i].label;
        marked[n] = marked_action && strcmp(view->actions[i].action, marked_action) == 0;
        n++;
    }
    if (!rc && present_labels(rows, count, actions, labels, marked, n, view)) rc = -1;
    free(actions);
    free(labels);
    free(marked);
    return rc;
}

static char *menu_label(const VoiceMenuStatus *status, const char *action, const char *label) {
    if (strcmp(action, "discard") == 0 && status->pending) return xstrdup("Discard waiting dictation");
    if (strcmp(action, "recover-discard") == 0) return xstrdup("Discard unrecovered dictation");
    if (strcmp(action, "recover-stage") == 0) {
        const char *destination = NULL;
        char bounded[256];
        char line[320];
        size_t i;
        for (i = 0; i < status->session_count; i++) {
            if (status->sessions[i].selected && status->sessions[i].label && status->sessions[i].label[0]) {
                destination = status->sessions[i].label;
                break;
            }
        }
        if (!destination || !destination[0]) destination = status->session_label && status->session_label[0]
            ? status->session_label : "selected Pi session";
        if (voice_public_label(destination, 40, bounded, sizeof bounded)) return xstrdup("Stage in selected Pi session");
        snprintf(line, sizeof line, "Stage in %s", bounded);
        return xstrdup(line);
    }
    return xstrdup(label);
}

static int row_has(const VoiceMenuRow *rows, size_t count, const char *action) {
    size_t i;
    for (i = 0; i < count; i++) if (strcmp(rows[i].action, action) == 0) return 1;
    return 0;
}

static const char *row_label(const VoiceMenuRow *rows, size_t count, const char *action) {
    size_t i;
    for (i = 0; i < count; i++) if (strcmp(rows[i].action, action) == 0) return rows[i].label;
    return NULL;
}

static int known_root(const char *action) {
    static const char *names[] = {
        "stop", "append", "retry", "read", "rebind", "recover-stage", "recover-copy",
        "menu:sessions", "menu:speech", "menu:voices", "menu:dictation", "auto-toggle",
        "team-toggle", "discard", "recover-discard", NULL
    };
    size_t i;
    for (i = 0; names[i]; i++) if (strcmp(names[i], action) == 0) return 1;
    return 0;
}

static int order_root(const VoiceMenuStatus *status, VoiceMenuRow **rows, size_t *count) {
    VoiceMenuRow *ordered = NULL;
    size_t ordered_count = 0;
    const char *primary = NULL;
    const char *order[20];
    size_t order_n = 0, i;
    int live = live_stop(status);
    VoiceMenuRow *extras = NULL;
    size_t extra_count = 0;
    if (live && row_has(*rows, *count, "stop")) primary = "stop";
    else if (needs_confirmation(status) && row_has(*rows, *count, "menu:sessions")) primary = "menu:sessions";
    else if (row_has(*rows, *count, "recover-stage")) primary = "recover-stage";
    else if (row_has(*rows, *count, "append")) primary = "append";
    else if (row_has(*rows, *count, "read")) primary = "read";
    if (live) order[order_n++] = "stop";
    order[order_n++] = "append";
    order[order_n++] = "retry";
    order[order_n++] = "read";
    order[order_n++] = "rebind";
    order[order_n++] = "recover-stage";
    order[order_n++] = "recover-copy";
    order[order_n++] = "menu:sessions";
    order[order_n++] = "menu:speech";
    order[order_n++] = "menu:voices";
    order[order_n++] = "menu:dictation";
    order[order_n++] = "auto-toggle";
    order[order_n++] = "team-toggle";
    if (!live) order[order_n++] = "stop";
    order[order_n++] = "discard";
    order[order_n++] = "recover-discard";
    if (primary && append_row(&ordered, &ordered_count, primary, row_label(*rows, *count, primary))) return -1;
    for (i = 0; i < order_n; i++) {
        if (primary && strcmp(order[i], primary) == 0) continue;
        if (!row_has(*rows, *count, order[i])) continue;
        if (append_row(&ordered, &ordered_count, order[i], row_label(*rows, *count, order[i]))) {
            voice_menu_rows_free(ordered, ordered_count);
            return -1;
        }
    }
    for (i = 0; i < *count; i++) {
        if (known_root((*rows)[i].action)) continue;
        if (primary && strcmp((*rows)[i].action, primary) == 0) continue;
        if (append_row(&extras, &extra_count, (*rows)[i].action, (*rows)[i].label)) {
            voice_menu_rows_free(ordered, ordered_count);
            voice_menu_rows_free(extras, extra_count);
            return -1;
        }
    }
    for (i = 0; i < extra_count; i++) {
        if (append_row(&ordered, &ordered_count, extras[i].action, extras[i].label)) {
            voice_menu_rows_free(ordered, ordered_count);
            voice_menu_rows_free(extras, extra_count);
            return -1;
        }
    }
    voice_menu_rows_free(extras, extra_count);
    voice_menu_rows_free(*rows, *count);
    *rows = ordered;
    *count = ordered_count;
    return 0;
}

int voice_menu_rows(const VoiceMenuStatus *status, const char *section, VoiceMenuRow **rows, size_t *count, char **error) {
    VoicePresentation *view = NULL;
    VoiceMenuRow *built = NULL;
    size_t built_count = 0;
    char marked[128];
    if (!rows || !count) return fail(error, "Menu rows need an output");
    *rows = NULL;
    *count = 0;
    if (!section) section = "menu";
    if (voice_presentation(status, &view, error)) return -1;
    if (strcmp(section, "voices") == 0) {
        snprintf(marked, sizeof marked, "voice:%s", view->selected_voice);
        if (prefixed_rows(view, "voice:", marked, &built, &built_count)) goto oom;
    } else if (strcmp(section, "sessions") == 0) {
        if (session_pairs(status, view, &built, &built_count)) goto oom;
    } else if (strcmp(section, "speech") == 0) {
        snprintf(marked, sizeof marked, "speech:%s", status->speech_backend && status->speech_backend[0] ? status->speech_backend : "none");
        if (prefixed_rows(view, "speech:", marked, &built, &built_count)) goto oom;
    } else if (strcmp(section, "dictation") == 0) {
        snprintf(marked, sizeof marked, "stt:%s", view->selected_stt);
        if (prefixed_rows(view, "stt:", marked, &built, &built_count)) goto oom;
    } else {
        VoiceMenuRow *sessions = NULL, *voices = NULL, *dictation = NULL, *speech = NULL;
        size_t ns = 0, nv = 0, nd = 0, np = 0, i;
        static const char *launchers[] = {"menu:sessions", "menu:voices", "menu:dictation", "menu:speech"};
        static const char *titles[] = {"Choose session", "Choose voice", "Choose dictation", "Choose speech"};
        if (session_pairs(status, view, &sessions, &ns) || prefixed_rows(view, "voice:", NULL, &voices, &nv)
            || prefixed_rows(view, "stt:", NULL, &dictation, &nd) || prefixed_rows(view, "speech:", NULL, &speech, &np)) {
            voice_menu_rows_free(sessions, ns);
            voice_menu_rows_free(voices, nv);
            voice_menu_rows_free(dictation, nd);
            voice_menu_rows_free(speech, np);
            goto oom;
        }
        for (i = 0; i < 4; i++) {
            size_t have = i == 0 ? ns : i == 1 ? nv : i == 2 ? nd : np;
            if (have && append_row(&built, &built_count, launchers[i], titles[i])) {
                voice_menu_rows_free(sessions, ns);
                voice_menu_rows_free(voices, nv);
                voice_menu_rows_free(dictation, nd);
                voice_menu_rows_free(speech, np);
                goto oom;
            }
        }
        voice_menu_rows_free(sessions, ns);
        voice_menu_rows_free(voices, nv);
        voice_menu_rows_free(dictation, nd);
        voice_menu_rows_free(speech, np);
        for (i = 0; i < view->action_count; i++) {
            const VoiceAction *action = &view->actions[i];
            char *wording;
            char *clipped;
            if (!action->enabled) continue;
            if (strncmp(action->action, "voice:", 6) == 0 || strncmp(action->action, "select:", 7) == 0
                || strncmp(action->action, "stt:", 4) == 0 || strncmp(action->action, "speech:", 7) == 0) continue;
            if (strcmp(action->action, "stop") == 0 && !live_stop(status)) continue;
            wording = menu_label(status, action->action, action->label);
            if (!wording) goto oom;
            if (strcmp(action->action, "auto-toggle") == 0 || strcmp(action->action, "team-toggle") == 0) {
                char toggled[256];
                int on = strcmp(action->action, "auto-toggle") == 0 ? view->auto_on : view->show_team;
                snprintf(toggled, sizeof toggled, "%s: %s", wording, on ? "on" : "off");
                free(wording);
                wording = xstrdup(toggled);
            }
            clipped = wording ? clip_label(wording, 52) : NULL;
            free(wording);
            if (!clipped || append_row(&built, &built_count, action->action, clipped)) {
                free(clipped);
                goto oom;
            }
            free(clipped);
        }
        if (order_root(status, &built, &built_count)) goto oom;
    }
    voice_presentation_free(view);
    *rows = built;
    *count = built_count;
    return 0;
oom:
    voice_presentation_free(view);
    voice_menu_rows_free(built, built_count);
    return fail(error, "Out of memory");
}

const char *voice_menu_empty_reason(const VoiceMenuStatus *status, const char *section) {
    if (section && strcmp(section, "sessions") == 0) {
        if (voice_menu_busy(status)) return "Session switching is locked until this take finishes";
        return "No other selectable sessions";
    }
    if (section && strcmp(section, "dictation") == 0) {
        if (status && status->stt_count && voice_menu_busy(status)) return "Dictation is locked until this take finishes";
        return "No dictation backends are available";
    }
    return "No voices are available";
}

int voice_menu_prompt(const VoiceMenuStatus *status, const char *section, char **out, char **error) {
    char line[1024];
    const char *voice;
    const char *stt;
    if (!out) return fail(error, "Prompt needs an output");
    *out = NULL;
    if (!status) status = NULL;
    voice = status && status->selected_voice && status->selected_voice[0] ? status->selected_voice : "none";
    stt = status && status->selected_stt && status->selected_stt[0] ? status->selected_stt : "none";
    if (!section) section = "menu";
    if (strcmp(section, "menu") != 0) {
        if (strcmp(section, "voices") == 0) snprintf(line, sizeof line, "Voice (%s)", voice);
        else if (strcmp(section, "dictation") == 0) snprintf(line, sizeof line, "Dictation (%s)", stt);
        else if (strcmp(section, "speech") == 0) snprintf(line, sizeof line, "Speech backend");
        else if (strcmp(section, "sessions") == 0) snprintf(line, sizeof line, "Sessions");
        else return fail(error, "Unknown voice menu section");
        *out = xstrdup(line);
        return *out ? 0 : fail(error, "Out of memory");
    }
    if (!(status && ((status->pane && status->pane[0])
        || (status->connection_state && strcmp(status->connection_state, "reconnecting") == 0)
        || (voice_menu_busy(status) && status->recording_label && status->recording_label[0])))) {
        *out = xstrdup("Voice | No session");
        return *out ? 0 : fail(error, "Out of memory");
    }
    if (status->phase && strcmp(status->phase, "error") == 0) {
        *out = xstrdup("Voice | Unavailable");
        return *out ? 0 : fail(error, "Out of memory");
    }
    if (status->retained) {
        char source[1024];
        char from[1200];
        const char *raw = status->retained_source && status->retained_source[0] ? status->retained_source : "previous destination";
        if (voice_public_label(raw, VOICE_LABEL_UNLIMITED, source, sizeof source)) return fail(error, "Retained source is too long");
        snprintf(from, sizeof from, "From %s", source);
        *out = fit_prompt(from, 28);
        return *out ? 0 : fail(error, "Out of memory");
    }
    {
        const char *destination = NULL;
        char described_buf[1024];
        char *described = NULL;
        char prompt[1200];
        if (voice_menu_busy(status) && status->recording_label && status->recording_label[0]) destination = status->recording_label;
        else {
            size_t i;
            for (i = 0; i < status->session_count; i++) {
                if (!status->sessions[i].selected) continue;
                if (voice_session_description(&status->sessions[i], &described, NULL) == 0 && described && described[0]) {
                    snprintf(described_buf, sizeof described_buf, "%s", described);
                    destination = described_buf;
                }
                break;
            }
        }
        if (!destination || !destination[0]) destination = status->session_label;
        if (!destination || !destination[0]) {
            free(described);
            *out = xstrdup("Voice");
            return *out ? 0 : fail(error, "Out of memory");
        }
        {
            char clean[1024];
            if (voice_public_label(destination, VOICE_LABEL_UNLIMITED, clean, sizeof clean)) {
                free(described);
                return fail(error, "Destination is too long");
            }
            snprintf(prompt, sizeof prompt, "Voice | %s", clean);
        }
        free(described);
        *out = fit_prompt(prompt, 28);
        return *out ? 0 : fail(error, "Out of memory");
    }
}

static int auto_same(const VoiceMenuStatus *left, const VoiceMenuStatus *right) {
    if (left->auto_missing != right->auto_missing) return 0;
    if (left->auto_missing) return 1;
    return left->auto_value == right->auto_value;
}

static int identities_same(const VoicePresentation *left, const VoicePresentation *right, const char *action) {
    const VoiceIdentity *a = identity_for(left, action);
    const VoiceIdentity *b = identity_for(right, action);
    if (!a && !b) return 1;
    if (!a || !b) return 0;
    return strcmp(a->token, b->token) == 0 && strcmp(a->id, b->id) == 0;
}

int voice_menu_run(const char *section, voice_request_fn request, void *request_user,
    voice_pick_fn pick, void *pick_user, char **error) {
    VoiceMenuStatus *status = NULL;
    const char *current = section && section[0] ? section : "menu";
    if (!request || !pick) return fail(error, "Voice menu needs a controller and picker");
    if (request(request_user, "status", &status, error)) return -1;
    for (;;) {
        VoicePresentation *view = NULL;
        VoiceMenuRow *rows = NULL;
        size_t count = 0;
        char *prompt = NULL;
        char *action = NULL;
        int picked;
        if (voice_presentation(status, &view, error)) { voice_menu_status_free(status); return -1; }
        if (voice_menu_rows(status, current, &rows, &count, error)) {
            voice_presentation_free(view);
            voice_menu_status_free(status);
            return -1;
        }
        if (!count) {
            fail(error, voice_menu_empty_reason(status, current));
            voice_menu_rows_free(rows, count);
            voice_presentation_free(view);
            voice_menu_status_free(status);
            return -1;
        }
        if (voice_menu_prompt(status, current, &prompt, error)) {
            voice_menu_rows_free(rows, count);
            voice_presentation_free(view);
            voice_menu_status_free(status);
            return -1;
        }
        picked = pick(pick_user, prompt, rows, count, &action, error);
        free(prompt);
        if (picked < 0) {
            voice_menu_rows_free(rows, count);
            voice_presentation_free(view);
            voice_menu_status_free(status);
            return -1;
        }
        if (picked > 0 || !action) {
            voice_menu_rows_free(rows, count);
            voice_presentation_free(view);
            voice_menu_status_free(status);
            return 0;
        }
        if (!row_has(rows, count, action)) {
            free(action);
            voice_menu_rows_free(rows, count);
            voice_presentation_free(view);
            voice_menu_status_free(status);
            return fail(error, "Invalid voice menu action");
        }
        if (strncmp(action, "menu:", 5) == 0) {
            current = strcmp(action, "menu:voices") == 0 ? "voices"
                : strcmp(action, "menu:dictation") == 0 ? "dictation"
                : strcmp(action, "menu:speech") == 0 ? "speech"
                : strcmp(action, "menu:sessions") == 0 ? "sessions" : "menu";
            free(action);
            voice_menu_rows_free(rows, count);
            voice_presentation_free(view);
            voice_menu_status_free(status);
            status = NULL;
            if (request(request_user, "status", &status, error)) return -1;
            continue;
        }
        if (strcmp(action, "stop") == 0) {
            VoiceMenuStatus *ignored = NULL;
            int rc = request(request_user, action, &ignored, error);
            voice_menu_status_free(ignored);
            free(action);
            voice_menu_rows_free(rows, count);
            voice_presentation_free(view);
            voice_menu_status_free(status);
            return rc;
        }
        {
            VoiceMenuStatus *fresh = NULL;
            VoicePresentation *fresh_view = NULL;
            const VoiceAction *enabled;
            int guarded = 0;
            if (request(request_user, "status", &fresh, error)) {
                free(action);
                voice_menu_rows_free(rows, count);
                voice_presentation_free(view);
                voice_menu_status_free(status);
                return -1;
            }
            if (voice_presentation(fresh, &fresh_view, error)) {
                free(action);
                voice_menu_status_free(fresh);
                voice_menu_rows_free(rows, count);
                voice_presentation_free(view);
                voice_menu_status_free(status);
                return -1;
            }
            enabled = voice_presentation_action(fresh_view, action);
            if (strncmp(action, "select:", 7) == 0 && fresh_view->selected_session
                && strcmp(action, fresh_view->selected_session) == 0) {
                if (!identities_same(view, fresh_view, action)) {
                    fail(error, "That session changed; reopen the menu");
                    free(action);
                    voice_presentation_free(fresh_view);
                    voice_menu_status_free(fresh);
                    voice_menu_rows_free(rows, count);
                    voice_presentation_free(view);
                    voice_menu_status_free(status);
                    return -1;
                }
                if (!enabled || !enabled->enabled) {
                    free(action);
                    voice_presentation_free(fresh_view);
                    voice_menu_status_free(fresh);
                    voice_menu_rows_free(rows, count);
                    voice_presentation_free(view);
                    voice_menu_status_free(status);
                    return 0;
                }
            }
            if (!enabled || !enabled->enabled) {
                fail(error, "That action is no longer available; reopen the menu");
                free(action);
                voice_presentation_free(fresh_view);
                voice_menu_status_free(fresh);
                voice_menu_rows_free(rows, count);
                voice_presentation_free(view);
                voice_menu_status_free(status);
                return -1;
            }
            if (strncmp(action, "select:", 7) == 0 && !identities_same(view, fresh_view, action)) {
                fail(error, "That session changed; reopen the menu");
                free(action);
                voice_presentation_free(fresh_view);
                voice_menu_status_free(fresh);
                voice_menu_rows_free(rows, count);
                voice_presentation_free(view);
                voice_menu_status_free(status);
                return -1;
            }
            guarded = strcmp(action, "read") == 0 || strcmp(action, "append") == 0 || strcmp(action, "retry") == 0
                || strcmp(action, "discard") == 0 || strcmp(action, "rebind") == 0
                || strcmp(action, "recover-stage") == 0 || strcmp(action, "auto-toggle") == 0;
            if (guarded && voice_selected_same(fresh, status) != 0) {
                fail(error, "The selected session changed; reopen the menu");
                free(action);
                voice_presentation_free(fresh_view);
                voice_menu_status_free(fresh);
                voice_menu_rows_free(rows, count);
                voice_presentation_free(view);
                voice_menu_status_free(status);
                return -1;
            }
            if (strcmp(action, "auto-toggle") == 0 && !auto_same(fresh, status)) {
                fail(error, "The automatic playback setting changed; reopen the menu");
                free(action);
                voice_presentation_free(fresh_view);
                voice_menu_status_free(fresh);
                voice_menu_rows_free(rows, count);
                voice_presentation_free(view);
                voice_menu_status_free(status);
                return -1;
            }
            if (strcmp(action, "team-toggle") == 0 && status->show_team != fresh->show_team) {
                fail(error, "The team visibility setting changed; reopen the menu");
                free(action);
                voice_presentation_free(fresh_view);
                voice_menu_status_free(fresh);
                voice_menu_rows_free(rows, count);
                voice_presentation_free(view);
                voice_menu_status_free(status);
                return -1;
            }
            voice_presentation_free(fresh_view);
            voice_menu_status_free(fresh);
            {
                VoiceMenuStatus *done = NULL;
                int rc = request(request_user, action, &done, error);
                voice_menu_status_free(done);
                free(action);
                voice_menu_rows_free(rows, count);
                voice_presentation_free(view);
                voice_menu_status_free(status);
                return rc;
            }
        }
    }
}

static void argv_free(char **argv) {
    size_t i;
    if (!argv) return;
    for (i = 0; argv[i]; i++) free(argv[i]);
    free(argv);
}

int voice_menu_fuzzel_plan(const char *prompt, const VoiceMenuRow *rows, size_t count,
    const char *config, char ***argv_out, char **input_out, char **error) {
    /* 4 bytes per code point: public_label limits are code points, not bytes. */
    char shown[320 * 4 + 1];
    char prompt_line[320 * 4 + 8];
    char **argv;
    char *input;
    size_t input_len = 1, used = 0, i;
    if (!config || !config[0]) return fail(error, "Fuzzel needs a configuration");
    if (voice_public_label(prompt ? prompt : "", 320, shown, sizeof shown)) return fail(error, "Menu prompt is too long");
    snprintf(prompt_line, sizeof prompt_line, "%s> ", shown);
    argv = calloc(13, sizeof *argv);
    if (!argv) return fail(error, "Out of memory");
    argv[0] = xstrdup("fuzzel");
    argv[1] = xstrdup("--config");
    argv[2] = xstrdup(config);
    argv[3] = xstrdup("--dmenu");
    argv[4] = xstrdup("--index");
    argv[5] = xstrdup("--only-match");
    argv[6] = xstrdup("--match-mode=fzf");
    argv[7] = xstrdup("--no-mouse");
    argv[8] = xstrdup("--log-level=error");
    argv[9] = xstrdup("--log-no-syslog");
    argv[10] = xstrdup("--prompt");
    argv[11] = xstrdup(prompt_line);
    if (!argv[0] || !argv[2] || !argv[11]) {
        argv_free(argv);
        return fail(error, "Out of memory");
    }
    for (i = 0; i < count; i++) input_len += 120 * 4 + 2;
    input = calloc(input_len + 8, 1);
    if (!input) {
        argv_free(argv);
        return fail(error, "Out of memory");
    }
    for (i = 0; i < count; i++) {
        char label[120 * 4 + 1];
        int wrote;
        if (voice_public_label(rows[i].label, 120, label, sizeof label)) {
            free(input);
            argv_free(argv);
            return fail(error, "Menu label is too long");
        }
        wrote = snprintf(input + used, input_len + 8 - used, "%s\n", label);
        if (wrote < 0 || (size_t)wrote >= input_len + 8 - used) {
            free(input);
            argv_free(argv);
            return fail(error, "Menu label is too long");
        }
        used += (size_t)wrote;
    }
    if (argv_out) *argv_out = argv;
    else argv_free(argv);
    if (input_out) *input_out = input;
    else free(input);
    return 0;
}

static void trim_ws(char *text) {
    char *start;
    size_t n;
    if (!text) return;
    start = text;
    while (*start && isspace((unsigned char)*start)) start++;
    n = strlen(start);
    while (n && isspace((unsigned char)start[n - 1])) start[--n] = 0;
    if (start != text) memmove(text, start, n + 1);
}

int voice_menu_interpret_fuzzel(int exit_code, const char *stdout_text, const char *stderr_text,
    size_t row_count, char **action, const VoiceMenuRow *rows, char **error) {
    char errbuf[1024];
    char out[128];
    char *end = NULL;
    long index;
    size_t i;
    if (!action) return fail(error, "Fuzzel selection needs an output");
    *action = NULL;
    if (exit_code) {
        if (stderr_text && voice_public_label(stderr_text, 200, errbuf, sizeof errbuf) == 0 && errbuf[0]) {
            char message[1200];
            snprintf(message, sizeof message, "Fuzzel could not open the voice menu: %s", errbuf);
            return fail(error, message);
        }
        return 1;
    }
    snprintf(out, sizeof out, "%s", stdout_text ? stdout_text : "");
    trim_ws(out);
    if (!out[0]) return fail(error, "Fuzzel returned an invalid selection");
    for (i = 0; out[i]; i++) if (out[i] < '0' || out[i] > '9') return fail(error, "Fuzzel returned an invalid selection");
    index = strtol(out, &end, 10);
    if (!end || *end || index < 0 || (size_t)index >= row_count || !rows) return fail(error, "Fuzzel returned an invalid selection");
    *action = xstrdup(rows[index].action);
    return *action ? 0 : fail(error, "Out of memory");
}

int voice_menu_pick_fuzzel(const char *prompt, const VoiceMenuRow *rows, size_t count,
    const char *config, voice_spawn_fn spawn, void *user, char **action, char **error) {
    char **argv = NULL;
    char *input = NULL;
    char *stdout_text = NULL;
    char *stderr_text = NULL;
    int exit_code = 1;
    int rc;
    if (!spawn) return fail(error, "Fuzzel runner is unavailable");
    if (voice_menu_fuzzel_plan(prompt, rows, count, config, &argv, &input, error)) return -1;
    rc = spawn(user, argv, input, &stdout_text, &stderr_text, &exit_code, error);
    if (rc == 0) rc = voice_menu_interpret_fuzzel(exit_code, stdout_text, stderr_text, count, action, rows, error);
    argv_free(argv);
    free(input);
    free(stdout_text);
    free(stderr_text);
    return rc;
}

#ifndef VOICE_MENU_NO_MAIN
#include "ipc.h"
#include "runtime_adapters.h"

#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <yyjson.h>

static int spawn_ipc(void *user, char *const *argv, const char *input, char **stdout_text,
    char **stderr_text, int *exit_code, char **error) {
    int argc = 0;
    ipc_process_request request;
    ipc_process_result result;
    char err[256];
    int rc;
    (void)user;
    while (argv && argv[argc]) argc++;
    memset(&request, 0, sizeof request);
    /* ipc_process_run does not mutate argv. */
    request.argv = (const char *const *)argv;
    request.argc = argc;
    request.stdin_bytes = (const unsigned char *)(input ? input : "");
    request.stdin_len = input ? strlen(input) : 0;
    request.capture_stdout = 1;
    request.capture_stderr = 1;
    request.deadline_ms = 120000;
    rc = ipc_process_run(&request, &result, err, sizeof err);
    if (rc != IPC_OK) {
        ipc_process_result_free(&result);
        return fail(error, err[0] ? err : "Fuzzel could not open the voice menu");
    }
    if (stdout_text) {
        *stdout_text = calloc(result.stdout_len + 1, 1);
        if (*stdout_text && result.stdout_bytes) memcpy(*stdout_text, result.stdout_bytes, result.stdout_len);
    }
    if (stderr_text) {
        *stderr_text = calloc(result.stderr_len + 1, 1);
        if (*stderr_text && result.stderr_bytes) memcpy(*stderr_text, result.stderr_bytes, result.stderr_len);
    }
    if (exit_code) *exit_code = result.exit_code;
    ipc_process_result_free(&result);
    return 0;
}

static int control_exchange(const char *action, const char *message, const char *tone, int start, yyjson_doc **response, char **error) {
    char runtime[512];
    char socket_path[576];
    char err[256];
    yyjson_mut_doc *mut;
    yyjson_doc *request = NULL;
    int attempt;
    if (voice_runtime_dir_resolve(runtime, sizeof runtime, err, sizeof err)) return fail(error, err[0] ? err : "Pi voice runtime is unavailable");
    snprintf(socket_path, sizeof socket_path, "%s/control.sock", runtime);
    if (start) {
        const char *argv[] = {"systemctl", "--user", "start", "pi-voice.service", NULL};
        ipc_process_request proc = {0};
        ipc_process_result result;
        proc.argv = (const char *const *)argv;
        proc.argc = 4;
        proc.deadline_ms = 15000;
        if (ipc_process_run(&proc, &result, err, sizeof err) != IPC_OK || result.exit_code != 0) {
            ipc_process_result_free(&result);
            return fail(error, "Pi voice service is unavailable");
        }
        ipc_process_result_free(&result);
    }
    mut = yyjson_mut_doc_new(NULL);
    if (!mut) return fail(error, "Out of memory");
    yyjson_mut_val *root = yyjson_mut_obj(mut);
    yyjson_mut_doc_set_root(mut, root);
    yyjson_mut_obj_add_strcpy(mut, root, "action", action ? action : "");
    if (message) yyjson_mut_obj_add_strcpy(mut, root, "message", message);
    if (tone) yyjson_mut_obj_add_strcpy(mut, root, "tone", tone);
    request = yyjson_mut_doc_imut_copy(mut, NULL);
    yyjson_mut_doc_free(mut);
    if (!request) return fail(error, "Out of memory");
    for (attempt = 0; attempt < (start ? 40 : 1); attempt++) {
        ipc_unix_request call = {0};
        int sent = 0;
        int rc;
        call.socket_path = socket_path;
        call.request = request;
        call.deadline_ms = 15000;
        call.max_response = 1024 * 1024;
        rc = ipc_unix_json(&call, &sent, response, err, sizeof err);
        if (rc == IPC_OK) {
            yyjson_doc_free(request);
            return 0;
        }
        if (sent || !start || attempt == 39) {
            yyjson_doc_free(request);
            return fail(error, err[0] ? err : "Pi voice service is unavailable");
        }
        usleep(50000);
    }
    yyjson_doc_free(request);
    return fail(error, "Pi voice service is unavailable");
}

static void write_notice(const char *runtime, const char *text, const char *tone) {
    char path[640];
    char tmp[656];
    FILE *file;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    char *json;
    if (!doc) return;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "message", text ? text : "");
    yyjson_mut_obj_add_strcpy(doc, root, "tone", tone ? tone : "red");
    yyjson_mut_obj_add_real(doc, root, "until", (double)time(NULL) + 8.0);
    json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    if (!json) return;
    snprintf(path, sizeof path, "%s/osd-notice.json", runtime);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    file = fopen(tmp, "w");
    if (file) {
        fputs(json, file);
        fclose(file);
        chmod(tmp, 0600);
        rename(tmp, path);
    }
    free(json);
}

static void desktop_notice(const char *title, const char *detail) {
    char combined[512];
    char text[256];
    char runtime[512];
    char err[256];
    yyjson_doc *response = NULL;
    char *error = NULL;
    snprintf(combined, sizeof combined, "%s%s%.400s", title ? title : "Voice", detail && detail[0] ? ": " : "", detail ? detail : "");
    if (voice_public_label(combined, 160, text, sizeof text)) snprintf(text, sizeof text, "Voice menu unavailable");
    fprintf(stderr, "Pi voice: %s\n", text);
    fflush(stderr);
    if (control_exchange("notice", text, "red", 0, &response, &error)) {
        if (voice_runtime_dir_resolve(runtime, sizeof runtime, err, sizeof err) == 0) write_notice(runtime, text, "red");
    }
    yyjson_doc_free(response);
    free(error);
}

static int menu_request(void *user, const char *action, VoiceMenuStatus **out, char **error) {
    yyjson_doc *response = NULL;
    char *json = NULL;
    yyjson_val *root;
    yyjson_val *ok;
    const char *failure;
    (void)user;
    if (control_exchange(action, NULL, NULL, 1, &response, error)) return -1;
    root = yyjson_doc_get_root(response);
    ok = yyjson_obj_get(root, "ok");
    if (!yyjson_is_true(ok)) {
        failure = yyjson_get_str(yyjson_obj_get(root, "error"));
        fail(error, failure && failure[0] ? failure : "Voice command failed");
        yyjson_doc_free(response);
        return -1;
    }
    json = yyjson_write(response, 0, NULL);
    yyjson_doc_free(response);
    if (!json) return fail(error, "Voice status was empty");
    if (voice_menu_status_from_json(json, out, error)) {
        free(json);
        return -1;
    }
    free(json);
    return 0;
}

static int menu_pick(void *user, const char *prompt, const VoiceMenuRow *rows, size_t count, char **action, char **error) {
    return voice_menu_pick_fuzzel(prompt, rows, count, user, spawn_ipc, NULL, action, error);
}

int main(int argc, char **argv) {
    const char *section = "menu";
    const char *config = NULL;
    int i;
    char *error = NULL;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) config = argv[++i];
        else if (strncmp(argv[i], "--config=", 9) == 0) config = argv[i] + 9;
        else if (argv[i][0] != '-') section = argv[i];
        else {
            fprintf(stderr, "voice-menu: unknown argument\n");
            return 2;
        }
    }
    if (!config || !config[0]) {
        fprintf(stderr, "voice-menu: --config is required\n");
        return 2;
    }
    if (strcmp(section, "menu") && strcmp(section, "voices") && strcmp(section, "dictation")
        && strcmp(section, "speech") && strcmp(section, "sessions")) {
        fprintf(stderr, "voice-menu: unknown section\n");
        return 2;
    }
    if (voice_menu_run(section, menu_request, NULL, menu_pick, (void *)config, &error)) {
        char detail[512];
        if (voice_public_label(error ? error : "Voice menu failed", 200, detail, sizeof detail))
            snprintf(detail, sizeof detail, "Voice menu failed");
        fprintf(stderr, "voice-menu: %s\n", detail);
        desktop_notice("Voice menu unavailable", detail);
        free(error);
        return 1;
    }
    return 0;
}
#endif
