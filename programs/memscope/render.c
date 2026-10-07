#define _POSIX_C_SOURCE 200809L
#include "memscope.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static void *allocate(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { fputs("memscope: out of memory\n", stderr); exit(1); }
    return p;
}

/* Escape bytes outside printable ASCII so labels cannot inject terminal controls.
 * This also makes layout independent of ambiguous-width filename characters. */
static char *safe(const char *s)
{
    size_t n = strlen(s);
    if (n > (SIZE_MAX - 1) / 4) exit(1);
    char *out = allocate(n * 4 + 1), *p = out;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 32 && c <= 126) *p++ = (char)c;
        else { snprintf(p, 5, "\\x%02x", c); p += 4; }
    }
    *p = '\0';
    return out;
}

static void text(FILE *out, unsigned width, const char *s)
{
    while (*s) {
        size_t n = strlen(s);
        if (n <= width) { fprintf(out, "%s\n", s); return; }
        size_t cut = width;
        while (cut > 0 && s[cut] != ' ') --cut;
        if (!cut) cut = width;
        fprintf(out, "%.*s\n", (int)cut, s);
        s += cut;
        while (*s == ' ') ++s;
    }
}

/* Semantic roles inherit the terminal palette; no fixed RGB or dim text. */
static void style(FILE *out, const View *v, const char *code)
{
    if (v->color) fprintf(out, "\033[%sm", code);
}

static void styled_text(FILE *out, const View *v, const char *code, const char *s)
{
    style(out, v, code);
    text(out, v->width, s);
    style(out, v, "0");
}

typedef struct { const char *text, *style; } Span;

static void spans(FILE *out, const View *v, const Span *parts, size_t count)
{
    unsigned col = 0;
    for (size_t i = 0; i < count; ++i) {
        style(out, v, parts[i].style);
        const char *p = parts[i].text;
        while (*p) {
            const char *end = strchr(p, ' ');
            size_t n = end ? (size_t)(end - p) : strlen(p);
            if (col && n <= v->width && col + n > v->width) {
                fputc('\n', out); col = 0;
            }
            for (size_t j = 0; j < n; ++j) {
                if (col == v->width) { fputc('\n', out); col = 0; }
                fputc(p[j], out); ++col;
            }
            p += n;
            if (*p == ' ') {
                if (col < v->width) { fputc(' ', out); ++col; }
                else { fputc('\n', out); col = 0; }
                ++p;
            }
        }
    }
    style(out, v, "0");
    fputc('\n', out);
}

static void size_string(uint64_t kib, char out[32])
{
    if (kib >= UINT64_C(1048576)) snprintf(out, 32, "%.1f GiB", (double)kib / 1048576.0);
    else if (kib >= 1024) snprintf(out, 32, "%.0f MiB", (double)kib / 1024.0);
    else snprintf(out, 32, "%" PRIu64 " KiB", kib);
}

static double percent(uint64_t value, uint64_t total)
{
    return total ? 100.0 * (double)value / (double)total : 0.0;
}

static void bar(FILE *out, const View *v, unsigned cells, double pct, bool track)
{
    unsigned filled = (unsigned)(pct * cells / 100.0 + 0.5);
    if (filled > cells) filled = cells;
    if (v->color) fputs("\033[34m", out);
    for (unsigned i = 0; i < filled; ++i) fputs(v->unicode ? "█" : "#", out);
    if (v->color) fputs("\033[39m", out);
    for (unsigned i = filled; i < cells; ++i)
        fputs(track ? (v->unicode ? "░" : "-") : " ", out);
}

static int map_compare(const void *a, const void *b)
{
    const Mapping *x = *(const Mapping *const *)a, *y = *(const Mapping *const *)b;
    if (x->pss != y->pss) return x->pss > y->pss ? -1 : 1;
    if (x->major != y->major) return x->major < y->major ? -1 : 1;
    if (x->minor != y->minor) return x->minor < y->minor ? -1 : 1;
    return (x->inode > y->inode) - (x->inode < y->inode);
}

static int process_compare(const void *a, const void *b)
{
    const Process *x = *(const Process *const *)a, *y = *(const Process *const *)b;
    if (x->pss != y->pss) return x->pss > y->pss ? -1 : 1;
    return (x->pid > y->pid) - (x->pid < y->pid);
}

static const char *basename_of(const char *path)
{
    const char *last = strrchr(path, '/');
    return last ? last + 1 : path;
}

static void clipped(FILE *out, const char *s, unsigned n)
{
    size_t len = strlen(s);
    if (len <= n) fprintf(out, "%-*s", (int)n, s);
    else if (n >= 4) fprintf(out, "...%s", s + len - n + 3);
    else fprintf(out, "%.*s", (int)n, s);
}

static void mapping_row(FILE *out, const View *v, const char *label,
                        uint64_t pss, uint64_t total)
{
    char amount[32], line[256];
    size_string(pss, amount);
    double pct = percent(pss, total);
    if (v->width >= 40) {
        unsigned label_width = v->width >= 76 ? 26 : v->width / 3;
        unsigned cells = v->width - label_width - 22;
        if (cells > 26) cells = 26;
        style(out, v, "34");
        clipped(out, label, label_width);
        style(out, v, "0");
        fputs("  ", out);
        bar(out, v, cells, pct, false);
        style(out, v, "1;39");
        fprintf(out, "  %5.1f", pct);
        style(out, v, "0;35");
        fputc('%', out);
        style(out, v, "1;39");
        fprintf(out, "  %10s", amount);
        style(out, v, "0");
        fputc('\n', out);
    } else {
        styled_text(out, v, "34", label);
        if (v->width >= 32) {
            fputs("  ", out);
            bar(out, v, v->width - 26, pct, false);
            style(out, v, "1;39");
            fprintf(out, "  %5.1f", pct);
            style(out, v, "0;35");
            fputc('%', out);
            style(out, v, "1;39");
            fprintf(out, "  %10s", amount);
            style(out, v, "0");
            fputc('\n', out);
        } else {
            snprintf(line, sizeof(line), "  %5.1f", pct);
            const Span values[] = {{line, "1;39"}, {"%", "0;35"},
                {"  ", "0"}, {amount, "1;39"}};
            spans(out, v, values, 4);
        }
    }
}

void render(FILE *out, const Sample *s, const View *v)
{
    char total[32], used[32], available[32], amount[32], line[512];
    size_string(s->total, total);
    if (s->has_available && s->available <= s->total) {
        uint64_t used_kib = s->total - s->available;
        size_string(used_kib, used);
        size_string(s->available, available);
        snprintf(line, sizeof(line), "   %.1f", percent(used_kib, s->total));
        const Span headline[] = {{"RAM   ", "1"}, {used, "1;34"}, {" / ", "0"},
            {total, "1"}, {" used", "0"}, {line, "1;39"}, {"%", "0;35"}};
        spans(out, v, headline, sizeof(headline) / sizeof(headline[0]));
        bar(out, v, v->width > 64 ? 64 : v->width, percent(used_kib, s->total), true);
        fputc('\n', out);
        const Span headroom[] = {{available, "32"}, {" available", "0"}};
        spans(out, v, headroom, 2);
    } else {
        snprintf(line, sizeof(line), "RAM   %s total; used/available unavailable", total);
        text(out, v->width, line);
    }
    if (s->has_anonymous) {
        size_string(s->anonymous, amount);
        const Span anon[] = {{"Anon  ", "0"}, {amount, "1"},
            {"  (overlapping)", "0"}};
        spans(out, v, anon, 3);
    }
    fputc('\n', out);
    bool partial = s->denied || s->vanished || s->invalid;
    const Span title[] = {{"OBSERVED FILE MAPPINGS", "1"},
        {partial ? "  PARTIAL" : "", "0;33"}};
    spans(out, v, title, 2);
    if (s->read == 0) {
        text(out, v->width, "Mapping PSS unavailable: no readable process samples.");
    } else {
        size_string(s->mapping_pss, amount);
        snprintf(line, sizeof(line), "%s PSS | %.2f%% of total RAM", amount,
                 percent(s->mapping_pss, s->total));
        styled_text(out, v, "1", line);
        text(out, v->width, "Bars: % all observed mapping PSS");
        fputc('\n', out);
        Mapping const **sorted = allocate(s->map_count * sizeof(*sorted));
        for (size_t i = 0; i < s->map_count; ++i) sorted[i] = &s->maps[i];
        qsort(sorted, s->map_count, sizeof(*sorted), map_compare);
        uint64_t shown = 0;
        size_t count = s->map_count < v->limit ? s->map_count : v->limit;
        for (size_t i = 0; i < count && sorted[i]->pss; ++i) {
            const Mapping *m = sorted[i];
            char *label = safe(v->full_paths ? m->path : basename_of(m->path));
            unsigned label_width = v->width >= 76 ? 26 : v->width / 3;
            bool shortened = !v->full_paths && v->width >= 40 && strlen(label) > label_width;
            if (v->full_paths) styled_text(out, v, "34", label);
            mapping_row(out, v, v->full_paths ? "" : label, m->pss, s->mapping_pss);
            free(label);
            bool collision = shortened;
            for (size_t j = 0; j < s->map_count; ++j)
                if (sorted[j] != m && !strcmp(basename_of(m->path), basename_of(sorted[j]->path)))
                    collision = true;
            if (collision) {
                snprintf(line, sizeof(line), "  identity %" PRIx64 ":%" PRIx64 ":%" PRIu64,
                         m->major, m->minor, m->inode);
                text(out, v->width, line);
            }
            char *users = safe(m->users ? m->users : "unknown process");
            /* Keep rows compact; explicit ellipsis marks omitted attachments.
             * Full-path output also expands the attachment list. */
            if (!v->full_paths && strlen(users) > v->width - 2) {
                size_t keep = v->width - 5;
                memcpy(users + keep, "...", 4);
            }
            const Span attachments[] = {{"  ", "0"}, {users, "36"}};
            spans(out, v, attachments, 2);
            free(users);
            if (m->deleted || m->shmem || m->uncertain) {
                snprintf(line, sizeof(line), "  %s%s%s", m->deleted ? "[deleted] " : "",
                         m->shmem ? "[shmem] " : "", m->uncertain ? "[backing unverified]" : "");
                text(out, v->width, line);
            }
            shown += m->pss;
        }
        if (s->mapping_pss > shown)
            mapping_row(out, v, v->width < 76 ? "Other" : "Other observed mappings",
                        s->mapping_pss - shown, s->mapping_pss);
        if (!s->mapping_pss) text(out, v->width, "No resident PSS in eligible observed mappings.");
        free(sorted);
    }
    fputc('\n', out);
    styled_text(out, v, "1", "TOP OBSERVED PROCESSES | overlapping PSS");
    Process const **processes = allocate(s->process_count * sizeof(*processes));
    for (size_t i = 0; i < s->process_count; ++i) processes[i] = &s->processes[i];
    qsort(processes, s->process_count, sizeof(*processes), process_compare);
    for (size_t i = 0; i < s->process_count && i < 3; ++i) {
        const Process *p = processes[i];
        char *name = safe(p->name);
        size_string(p->pss, amount);
        snprintf(line, sizeof(line), "  PID %u", p->pid);
        const Span process[] = {{amount, "1"}, {"  ", "0"}, {name, "36"}, {line, "0"}};
        spans(out, v, process, 4);
        free(name);
    }
    if (!s->process_count) text(out, v->width, "Process PSS unavailable.");
    free(processes);
    fputc('\n', out);
    snprintf(line, sizeof(line), " | %zu read | %zu denied | %zu vanished",
             s->read, s->denied, s->vanished);
    const Span coverage[] = {{partial ? "PARTIAL" : "VISIBLE PIDS", partial ? "33" : "0"},
        {line, "0"}};
    spans(out, v, coverage, 2);
    if (s->invalid) {
        snprintf(line, sizeof(line), "%zu invalid process samples omitted.", s->invalid);
        text(out, v->width, line);
    }
    snprintf(line, sizeof(line), "%.0f ms sample | visible PIDs | accounting: --help", s->elapsed * 1000);
    text(out, v->width, line);
    if (s->special_count || s->huge_kib) {
        size_string(s->special_pss, amount);
        snprintf(line, sizeof(line), "%zu special mappings excluded (%s PSS).", s->special_count, amount);
        text(out, v->width, line);
        if (s->huge_kib) text(out, v->width, "Explicit huge pages present: not covered by normal PSS.");
    }

}
