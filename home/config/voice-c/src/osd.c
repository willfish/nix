#include "osd.h"

#include <ctype.h>
#include <glib.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static const VoiceColours DEFAULT_COLOURS = {
    .background = "#222222",
    .text = "#c2c2b0",
    .border = "#78824b",
    .accent = "#c9a554",
    .muted = "#8a8a78",
    .red = "#e0303e",
    .yellow = "#c9a554",
    .green = "#2aa45f",
    .orange = "#e2681e",
    .teal = "#2f9e9a",
};

static const char *COLOUR_NAMES[] = {
    "background", "text", "border", "accent", "muted",
    "red", "yellow", "green", "orange", "teal",
};

static void copy_text(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (!dst || cap == 0) return;
    if (!src) src = "";
    for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = 0;
}

static char *colour_slot(VoiceColours *colours, int index) {
    switch (index) {
    case 0: return colours->background;
    case 1: return colours->text;
    case 2: return colours->border;
    case 3: return colours->accent;
    case 4: return colours->muted;
    case 5: return colours->red;
    case 6: return colours->yellow;
    case 7: return colours->green;
    case 8: return colours->orange;
    case 9: return colours->teal;
    default: return NULL;
    }
}

static const char *colour_slot_const(const VoiceColours *colours, int index) {
    return colour_slot((VoiceColours *)colours, index);
}

static int colour_index(const char *key) {
    for (int i = 0; i < 10; i++) {
        if (strcmp(key, COLOUR_NAMES[i]) == 0) return i;
    }
    return -1;
}

static int hex_colour(const char *value) {
    size_t n;
    if (!value || value[0] != '#') return 0;
    n = strlen(value);
    return n == 4 || n == 7 || n == 9;
}

static void strip_inplace(char *s) {
    char *start = s;
    char *end;
    while (*start && isspace((unsigned char)*start)) start++;
    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) end--;
    *end = 0;
    if (start != s) memmove(s, start, (size_t)(end - start) + 1);
}

static void strip_quotes(char *s) {
    size_t n = strlen(s);
    size_t start = 0;
    while (start < n && (s[start] == '"' || s[start] == '\'')) start++;
    while (n > start && (s[n - 1] == '"' || s[n - 1] == '\'')) n--;
    s[n] = 0;
    if (start) memmove(s, s + start, n - start + 1);
}

static int next_line(const char **cursor, char *buf, size_t cap) {
    const char *s;
    size_t i = 0;
    if (!cursor || !*cursor || !**cursor) return 0;
    s = *cursor;
    while (*s && *s != '\n' && i + 1 < cap) buf[i++] = *s++;
    buf[i] = 0;
    while (*s && *s != '\n') s++;
    if (*s == '\n') s++;
    *cursor = s;
    i = strlen(buf);
    if (i > 0 && buf[i - 1] == '\r') buf[i - 1] = 0;
    return 1;
}

static int utf8_next(const char *s, unsigned int *cp, size_t *len) {
    const unsigned char *u = (const unsigned char *)s;
    if (!u || !*u) return 0;
    if (u[0] < 0x80) {
        *cp = u[0];
        *len = 1;
        return 1;
    }
    if ((u[0] & 0xE0) == 0xC0 && (u[1] & 0xC0) == 0x80) {
        *cp = ((unsigned int)(u[0] & 0x1F) << 6) | (u[1] & 0x3F);
        *len = 2;
        return 1;
    }
    if ((u[0] & 0xF0) == 0xE0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80) {
        *cp = ((unsigned int)(u[0] & 0x0F) << 12) | ((unsigned int)(u[1] & 0x3F) << 6) | (u[2] & 0x3F);
        *len = 3;
        return 1;
    }
    if ((u[0] & 0xF8) == 0xF0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80 && (u[3] & 0xC0) == 0x80) {
        *cp = ((unsigned int)(u[0] & 0x07) << 18) | ((unsigned int)(u[1] & 0x3F) << 12)
            | ((unsigned int)(u[2] & 0x3F) << 6) | (u[3] & 0x3F);
        *len = 4;
        return 1;
    }
    *cp = u[0];
    *len = 1;
    return 1;
}

/* Python str.isprintable: category C, line/paragraph separators, and
   non-ASCII spaces are not printable. ASCII space is. */
static int py_printable(gunichar c) {
    if (c == ' ') return 1;
    switch (g_unichar_type(c)) {
    case G_UNICODE_CONTROL:
    case G_UNICODE_FORMAT:
    case G_UNICODE_SURROGATE:
    case G_UNICODE_PRIVATE_USE:
    case G_UNICODE_UNASSIGNED:
    case G_UNICODE_LINE_SEPARATOR:
    case G_UNICODE_PARAGRAPH_SEPARATOR:
    case G_UNICODE_SPACE_SEPARATOR:
        return 0;
    default:
        return 1;
    }
}

static void append_bytes(char *out, size_t cap, size_t *used, const char *bytes, size_t n) {
    if (*used + n + 1 > cap) return;
    memcpy(out + *used, bytes, n);
    *used += n;
    out[*used] = 0;
}

static void public_label(const char *value, size_t limit, char *out, size_t cap) {
    size_t used = 0;
    size_t cps = 0;
    int started = 0;
    int pending_space = 0;
    const char *cursor;
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!value) value = "";
    cursor = value;
    while (*cursor && cps < limit) {
        unsigned int cp = 0;
        size_t len = 0;
        char encoded[6];
        int n;
        if (!utf8_next(cursor, &cp, &len)) break;
        if (g_unichar_isspace((gunichar)cp)) {
            if (started) pending_space = 1;
        } else if (py_printable((gunichar)cp)) {
            if (pending_space && cps < limit) {
                append_bytes(out, cap, &used, " ", 1);
                cps++;
                pending_space = 0;
            }
            if (cps < limit) {
                n = g_unichar_to_utf8((gunichar)cp, encoded);
                if (n > 0) append_bytes(out, cap, &used, encoded, (size_t)n);
                cps++;
            }
            started = 1;
        }
        cursor += len;
    }
}

static void fit_label(const char *text, size_t limit, char *out, size_t cap) {
    char full[512];
    size_t cps = 0;
    const char *p;
    if (!out || cap == 0) return;
    public_label(text, 200, full, sizeof full);
    for (p = full; *p; ) {
        unsigned int cp = 0;
        size_t len = 0;
        if (!utf8_next(p, &cp, &len)) break;
        cps++;
        p += len;
    }
    if (cps <= limit) {
        copy_text(out, cap, full);
        return;
    }
    {
        char prefix[512];
        size_t used = 0;
        size_t kept = 0;
        size_t cut = limit > 0 ? limit - 1 : 0;
        prefix[0] = 0;
        p = full;
        while (*p && kept < cut) {
            unsigned int cp = 0;
            size_t len = 0;
            if (!utf8_next(p, &cp, &len)) break;
            append_bytes(prefix, sizeof prefix, &used, p, len);
            kept++;
            p += len;
        }
        while (used > 0 && isspace((unsigned char)prefix[used - 1])) {
            prefix[--used] = 0;
        }
        append_bytes(prefix, sizeof prefix, &used, "\xe2\x80\xa6", 3);
        copy_text(out, cap, prefix);
    }
}

static int in_set(const char *lower, const char *const *set) {
    for (size_t i = 0; set[i]; i++) {
        if (strcmp(lower, set[i]) == 0) return 1;
    }
    return 0;
}

static void compact_destination(const char *text, char *out, size_t cap) {
    static const char *const thinking[] = {
        "off", "minimal", "low", "medium", "high", "xhigh", "max", NULL
    };
    static const char *const flags[] = {
        "selected", "team", "pi", "qwen-pi", "qwen pi", NULL
    };
    static const char sep[] = " \xc2\xb7 ";
    char joined[512];
    size_t used = 0;
    const char *cursor = text ? text : "";
    joined[0] = 0;
    while (*cursor) {
        const char *mark = strstr(cursor, sep);
        size_t raw_len = mark ? (size_t)(mark - cursor) : strlen(cursor);
        char piece[256];
        char lower[256];
        size_t n = raw_len < sizeof piece - 1 ? raw_len : sizeof piece - 1;
        int skip = 0;
        int digit = 0;
        int dash_or_dot = 0;
        memcpy(piece, cursor, n);
        piece[n] = 0;
        strip_inplace(piece);
        lower[0] = 0;
        for (size_t i = 0; piece[i] && i + 1 < sizeof lower; i++) {
            lower[i] = (char)tolower((unsigned char)piece[i]);
            lower[i + 1] = 0;
        }
        if (piece[0] == 0 || in_set(lower, thinking) || in_set(lower, flags)) skip = 1;
        if (piece[0] == '@' || strchr(piece, ':')) skip = 1;
        for (size_t i = 0; piece[i]; i++) {
            if (isdigit((unsigned char)piece[i])) digit = 1;
            if (piece[i] == '-' || piece[i] == '.') dash_or_dot = 1;
        }
        if (digit && dash_or_dot) skip = 1;
        if (!skip) {
            if (used) append_bytes(joined, sizeof joined, &used, sep, sizeof sep - 1);
            append_bytes(joined, sizeof joined, &used, piece, strlen(piece));
        }
        if (!mark) break;
        cursor = mark + (sizeof sep - 1);
    }
    fit_label(joined, 46, out, cap);
    if (!out[0]) copy_text(out, cap, "Selected Pi session");
}

static int phase_active(const char *phase) {
    return strcmp(phase, "starting") == 0 || strcmp(phase, "recording") == 0
        || strcmp(phase, "stopping") == 0 || strcmp(phase, "transcribing") == 0;
}

static int tone_known(const char *tone) {
    return strcmp(tone, "red") == 0 || strcmp(tone, "yellow") == 0 || strcmp(tone, "green") == 0
        || strcmp(tone, "orange") == 0 || strcmp(tone, "teal") == 0 || strcmp(tone, "accent") == 0
        || strcmp(tone, "muted") == 0;
}

static long long finite_seconds(const VoiceStatus *status) {
    double value;
    if (!status->has_recording_seconds || status->recording_seconds_invalid) return 0;
    value = status->recording_seconds;
    if (!isfinite(value) || value < 0) return 0;
    if (value >= (double)LLONG_MAX) return LLONG_MAX;
    return (long long)value;
}

static void recording_timer(long long seconds, char *out, size_t cap) {
    if (seconds < 0) seconds = 0;
    snprintf(out, cap, "%02lld:%02lld", seconds / 60, seconds % 60);
}

static void capitalize(const char *in, char *out, size_t cap) {
    size_t i = 0;
    if (!in) in = "";
    for (; in[i] && i + 1 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        out[i] = (char)(i == 0 ? toupper(c) : tolower(c));
    }
    out[i] = 0;
}

static void card_state(const VoiceStatus *status, const char *phase, const char *message,
    char *title, size_t title_cap, char *tone, size_t tone_cap) {
    const char *connection = status->connection_state ? status->connection_state : "";
    if (phase_active(phase)) {
        if (strcmp(phase, "recording") == 0) {
            char timer[32];
            recording_timer(finite_seconds(status), timer, sizeof timer);
            snprintf(title, title_cap, "Listening %s", timer);
            copy_text(tone, tone_cap, "red");
            return;
        }
        if (strcmp(phase, "starting") == 0) copy_text(title, title_cap, "Starting microphone");
        else if (strcmp(phase, "stopping") == 0) copy_text(title, title_cap, "Finishing");
        else if (strcmp(phase, "transcribing") == 0) copy_text(title, title_cap, "Transcribing");
        else copy_text(title, title_cap, "Listening");
        copy_text(tone, tone_cap, "yellow");
        return;
    }
    if (status->osd && message[0]) {
        copy_text(title, title_cap, message);
        if (status->osd_tone && tone_known(status->osd_tone)) copy_text(tone, tone_cap, status->osd_tone);
        else copy_text(tone, tone_cap, "orange");
        return;
    }
    if (strcmp(phase, "error") == 0) {
        const char *error = (status->error && status->error[0]) ? status->error : "Voice unavailable";
        public_label(error, 120, title, title_cap);
        copy_text(tone, tone_cap, "red");
        return;
    }
    if (status->queued) {
        copy_text(title, title_cap, "Will send when idle");
        copy_text(tone, tone_cap, "green");
        return;
    }
    if (status->pending || (status->draft && !status->draft_edited)) {
        copy_text(title, title_cap, "Ready to send");
        copy_text(tone, tone_cap, "green");
        return;
    }
    if (status->retained) {
        copy_text(title, title_cap, "Dictation retained");
        copy_text(tone, tone_cap, "orange");
        return;
    }
    if (status->speaking) {
        if (status->audible) {
            copy_text(title, title_cap, "Speaking");
            copy_text(tone, tone_cap, "teal");
        } else {
            copy_text(title, title_cap, "About to speak");
            copy_text(tone, tone_cap, "accent");
        }
        return;
    }
    if (status->agent_state && strcmp(status->agent_state, "blocked") == 0) {
        copy_text(title, title_cap, "Needs attention");
        copy_text(tone, tone_cap, "orange");
        return;
    }
    if (strcmp(connection, "connecting") == 0 || strcmp(connection, "reconnecting") == 0) {
        capitalize(connection, title, title_cap);
        copy_text(tone, tone_cap, "yellow");
        return;
    }
    if (status->rebind_needed) {
        copy_text(title, title_cap, "Conversation changed");
        copy_text(tone, tone_cap, "yellow");
        return;
    }
    if (status->models && strcmp(status->models, "unavailable") == 0) {
        copy_text(title, title_cap, "Speech models unavailable");
        copy_text(tone, tone_cap, "orange");
        return;
    }
    copy_text(title, title_cap, "Voice");
    copy_text(tone, tone_cap, status->osd ? "accent" : "muted");
}

static void destination(const VoiceStatus *status, const char *phase, char *out, size_t cap) {
    const char *label = NULL;
    const VoiceSession *selected = NULL;
    if (phase_active(phase) && status->recording_label && status->recording_label[0]) {
        label = status->recording_label;
    }
    if (status->sessions && status->session_count) {
        for (size_t i = 0; i < status->session_count; i++) {
            if (status->sessions[i].selected) {
                selected = &status->sessions[i];
                break;
            }
        }
    }
    if (!label && selected) {
        if (selected->full_label && selected->full_label[0]) label = selected->full_label;
        else if (selected->label && selected->label[0]) label = selected->label;
    }
    if (!label && status->session_label && status->session_label[0]) label = status->session_label;
    if (!label && !(status->pane && status->pane[0])) {
        copy_text(out, cap, "No Pi session selected");
        return;
    }
    compact_destination(
        label ? label : (status->pane && status->pane[0] ? status->pane : "Selected Pi session"),
        out, cap);
}

static void apply_colour_line(VoiceColours *colours, const char *key, const char *value) {
    int index;
    if (!hex_colour(value)) return;
    index = colour_index(key);
    if (index < 0) return;
    copy_text(colour_slot(colours, index), 16, value);
}

static void parse_popup(const char *text, VoiceColours *colours, int apply_values) {
    const char *cursor = text;
    char line[512];
    char section[64];
    section[0] = 0;
    if (!text) return;
    while (next_line(&cursor, line, sizeof line)) {
        char *eq;
        strip_inplace(line);
        if (!line[0] || line[0] == '#') continue;
        if (line[0] == '[' && line[strlen(line) - 1] == ']') {
            line[strlen(line) - 1] = 0;
            copy_text(section, sizeof section, line + 1);
            strip_inplace(section);
            continue;
        }
        if (strcmp(section, "popups") != 0) continue;
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        strip_inplace(line);
        strip_quotes(line);
        strip_inplace(eq + 1);
        strip_quotes(eq + 1);
        if (apply_values) apply_colour_line(colours, line, eq + 1);
    }
}

static void finish_popup_accent(VoiceColours *colours) {
    if (strcmp(colours->accent, DEFAULT_COLOURS.accent) == 0 && colours->border[0] == '#') {
        copy_text(colours->accent, sizeof colours->accent, colours->border);
    }
}

const char *focused_connector(const VoiceMonitor *monitors, size_t count) {
    const char *name = NULL;
    size_t focused = 0;
    if (!monitors) return NULL;
    for (size_t i = 0; i < count; i++) {
        if (monitors[i].focused != 1) continue;
        focused++;
        name = monitors[i].name;
    }
    if (focused != 1 || !name || !name[0]) return NULL;
    return name;
}

void popup_colours(const char *text, VoiceColours *out) {
    if (!out) return;
    *out = DEFAULT_COLOURS;
    if (!text) return;
    parse_popup(text, out, 1);
    finish_popup_accent(out);
}

static void apply_waybar(VoiceColours *colours, const char *text) {
    const char *cursor = text;
    char line[512];
    if (!text) return;
    while (next_line(&cursor, line, sizeof line)) {
        char *parts[8];
        int count = 0;
        char *token;
        strip_inplace(line);
        while (line[0] && line[strlen(line) - 1] == ';') line[strlen(line) - 1] = 0;
        strip_inplace(line);
        if (strncmp(line, "@define-color ", 14) != 0) continue;
        token = strtok(line, " \t");
        while (token && count < 8) {
            parts[count++] = token;
            token = strtok(NULL, " \t");
        }
        if (count < 3) continue;
        if (colour_index(parts[1]) < 0 || !hex_colour(parts[2])) continue;
        apply_colour_line(colours, parts[1], parts[2]);
    }
}

void resolved_colours(const char *shell_text, const char *waybar_text, VoiceColours *out) {
    VoiceColours parsed;
    char keys[16][64];
    int key_count = 0;
    int border_explicit = 0;
    int accent_explicit = 0;
    const char *cursor = shell_text;
    char line[512];
    char section[64];
    if (!out) return;
    *out = DEFAULT_COLOURS;
    apply_waybar(out, waybar_text);
    section[0] = 0;
    if (shell_text) {
        while (next_line(&cursor, line, sizeof line)) {
            char *eq;
            strip_inplace(line);
            if (line[0] == '[' && line[strlen(line) - 1] == ']') {
                line[strlen(line) - 1] = 0;
                copy_text(section, sizeof section, line + 1);
                strip_inplace(section);
                continue;
            }
            if (strcmp(section, "popups") != 0 || line[0] == '#' || !strchr(line, '=')) continue;
            eq = strchr(line, '=');
            *eq = 0;
            strip_inplace(line);
            if (key_count < 16) copy_text(keys[key_count++], sizeof keys[0], line);
            if (strcmp(line, "border") == 0) border_explicit = 1;
            if (strcmp(line, "accent") == 0) accent_explicit = 1;
        }
    }
    popup_colours(shell_text, &parsed);
    for (int i = 0; i < key_count; i++) {
        int index = colour_index(keys[i]);
        if (index < 0) continue;
        copy_text(colour_slot(out, index), 16, colour_slot_const(&parsed, index));
    }
    if (border_explicit && !accent_explicit) {
        copy_text(out->accent, sizeof out->accent, parsed.border);
    }
}

const char *level_to_block(double level) {
    static const char *blocks[] = {
        "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
        "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88",
    };
    double db;
    double scale;
    int index;
    if (!isfinite(level) || !(level > 0)) return blocks[0];
    if (level > 1.0) level = 1.0;
    db = 20.0 * log10(level);
    scale = (db - (-50.0)) / 40.0;
    if (scale < 0) scale = 0;
    if (scale > 1) scale = 1;
    index = (int)(scale * 7.0);
    if (index > 7) index = 7;
    if (index < 0) index = 0;
    return blocks[index];
}

void meter_blocks(const double *levels, size_t count, char *out, size_t out_cap) {
    double cells[8];
    size_t start;
    size_t used = 0;
    if (!out || out_cap == 0) return;
    out[0] = 0;
    for (int i = 0; i < 8; i++) cells[i] = 0;
    start = count > 8 ? count - 8 : 0;
    if (levels) {
        for (size_t i = start; i < count && i - start < 8; i++) {
            cells[8 - (count - start) + (i - start)] = levels[i];
        }
    }
    for (int i = 0; i < 8; i++) {
        const char *block = level_to_block(cells[i]);
        append_bytes(out, out_cap, &used, block, strlen(block));
    }
}

void osd_view(const VoiceStatus *status, OsdView *out) {
    VoiceStatus empty;
    const char *phase;
    char message[160];
    char detail[240];
    long long seconds;
    double level = 0;
    if (!out) return;
    memset(out, 0, sizeof *out);
    memset(&empty, 0, sizeof empty);
    if (!status) status = &empty;
    phase = status->phase ? status->phase : "idle";
    if (status->osd_message) public_label(status->osd_message, 120, message, sizeof message);
    else message[0] = 0;
    card_state(status, phase, message, out->title, sizeof out->title, out->tone, sizeof out->tone);
    if (!tone_known(out->tone)) copy_text(out->tone, sizeof out->tone, "muted");
    out->visible = phase_active(phase) || status->osd || strcmp(out->title, "Voice") != 0;
    if (status->draft_edited && strcmp(out->title, "Voice") == 0 && message[0] == 0) out->visible = 0;
    destination(status, phase, detail, sizeof detail);
    if (status->retained && !phase_active(phase)) {
        char source[160];
        public_label(status->retained_source ? status->retained_source : "", 120, source, sizeof source);
        if (source[0]) {
            char from[200];
            snprintf(from, sizeof from, "From %s", source);
            fit_label(from, 46, detail, sizeof detail);
        }
    }
    if (strcmp(phase, "recording") == 0 && status->has_microphone
        && (status->mic_muted || status->mic_clipping)) {
        char combined[320];
        const char *warnings = status->mic_muted && status->mic_clipping
            ? "muted, clipping"
            : (status->mic_muted ? "muted" : "clipping");
        snprintf(combined, sizeof combined, "%s \xc2\xb7 %s", detail, warnings);
        fit_label(combined, 46, detail, sizeof detail);
    }
    if (status->models && strcmp(status->models, "unavailable") == 0
        && strcmp(out->title, "Speech models unavailable") == 0
        && status->model_error && status->model_error[0]) {
        char model_error[160];
        public_label(status->model_error, 80, model_error, sizeof model_error);
        if (model_error[0]) copy_text(detail, sizeof detail, model_error);
    }
    copy_text(out->detail, sizeof out->detail, detail);
    if (strcmp(phase, "recording") == 0 && status->has_input_level && !status->input_level_invalid) {
        level = status->input_level;
        if (!isfinite(level)) level = (isinf(level) && level > 0) ? 1.0 : 0.0;
        else {
            if (level < 0) level = 0;
            if (level > 1) level = 1;
        }
    }
    out->level = level;
    if (strcmp(phase, "recording") == 0) meter_blocks(&level, 1, out->meter, sizeof out->meter);
    else meter_blocks(NULL, 0, out->meter, sizeof out->meter);
    out->recording = strcmp(phase, "recording") == 0;
    out->transcribing = strcmp(phase, "transcribing") == 0;
    copy_text(out->phase, sizeof out->phase, phase);
    seconds = finite_seconds(status);
    out->seconds = (double)seconds;
    recording_timer(seconds, out->timer, sizeof out->timer);
    out->muted = status->has_microphone && status->mic_muted;
    out->clipping = status->has_microphone && status->mic_clipping;
}

int status_from_response(const VoiceStatusPayload *payload, VoiceStatus *out) {
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!payload || !payload->ok_is_true) return 0;
    out->phase = payload->phase;
    (void)payload->reply;
    return 1;
}
