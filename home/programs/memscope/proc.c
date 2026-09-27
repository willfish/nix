#define _POSIX_C_SOURCE 200809L
#include "memscope.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * Required meminfo: MemTotal in kB and non-zero. MemAvailable, AnonPages and
 * Hugetlb are optional kB fields. MemFree is never a used-memory fallback.
 * A modern smaps VMA is accepted only when it has a header, Pss in kB, and
 * VmFlags. Any other field is ignored so newer kernels can add lines.
 * Rss, Size and Anonymous are not accounting inputs. Identity is dev+inode.
 * An empty smaps is an empty address space. ESRCH on smaps is vanished;
 * kernel threads normally present an empty smaps rather than ESRCH.
 */

enum {
    SMAPS_LINE_MAX = 65536,
    USER_LIMIT = 8,
    NAME_LIMIT = 64,
    ALIAS_LIMIT = 8
};

static void set_error(char *error, size_t n, const char *msg)
{
    if (!error || n == 0 || error[0]) return;
    snprintf(error, n, "%s", msg);
}

static int add_u64(uint64_t *dst, uint64_t v)
{
    if (v > UINT64_MAX - *dst) return -1;
    *dst += v;
    return 0;
}

static int add_size(size_t *dst, size_t v)
{
    if (v > SIZE_MAX - *dst) return -1;
    *dst += v;
    return 0;
}

static int grow(void **ptr, size_t *cap, size_t need, size_t elem)
{
    size_t ncap, bytes;
    void *next;
    if (need <= *cap) return 0;
    ncap = *cap ? *cap : 8;
    while (ncap < need) {
        if (ncap > SIZE_MAX / 2) return -1;
        ncap *= 2;
    }
    if (elem != 0 && ncap > SIZE_MAX / elem) return -1;
    bytes = ncap * elem;
    next = realloc(*ptr, bytes ? bytes : 1);
    if (!next) return -1;
    *ptr = next;
    *cap = ncap;
    return 0;
}

static char *dup_n(const char *s, size_t n)
{
    char *p = malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static char *dup_str(const char *s)
{
    return dup_n(s, strlen(s));
}

static int parse_u64(const char *s, char **end, int base, uint64_t *out)
{
    unsigned long long v;
    unsigned char c;
    if (!s || !end || !out) return -1;
    c = (unsigned char)*s;
    if (base == 16) {
        if (!isxdigit(c)) return -1;
    } else if (!isdigit(c)) {
        return -1;
    }
    errno = 0;
    v = strtoull(s, end, base);
    if (errno == ERANGE || *end == s) return -1;
    *out = (uint64_t)v;
    return 0;
}

static int parse_kb(const char *v, uint64_t *out)
{
    char *end;
    while (*v == ' ' || *v == '\t') v++;
    if (parse_u64(v, &end, 10, out) != 0) return -1;
    while (*end == ' ' || *end == '\t') end++;
    if (strncmp(end, "kB", 2) != 0) return -1;
    end += 2;
    while (*end == ' ' || *end == '\t') end++;
    return *end ? -1 : 0;
}

/* 1 line, 0 clean EOF, -1 I/O or OOM (errno), -2 line longer than SMAPS_LINE_MAX. */
static int read_line(FILE *fp, char **buf, size_t *cap)
{
    size_t n = 0;
    for (;;) {
        int c = fgetc(fp);
        if (c == EOF) {
            if (ferror(fp)) return -1;
            if (n == 0) return 0;
            break;
        }
        if (n + 1 >= *cap) {
            size_t ncap = *cap ? *cap * 2 : 128;
            char *next;
            if (*cap >= SMAPS_LINE_MAX) return -2;
            if (ncap > SMAPS_LINE_MAX) ncap = SMAPS_LINE_MAX;
            next = realloc(*buf, ncap);
            if (!next) {
                errno = ENOMEM;
                return -1;
            }
            *buf = next;
            *cap = ncap;
        }
        if (c == '\n') break;
        (*buf)[n++] = (char)c;
    }
    (*buf)[n] = '\0';
    if (n && (*buf)[n - 1] == '\r') (*buf)[n - 1] = '\0';
    return 1;
}

static bool has_prefix(const char *s, const char *prefix)
{
    size_t n = strlen(prefix);
    return strncmp(s, prefix, n) == 0;
}

/* Path heuristics only. uncertain means the tag is not a proof of backing type. */
static void classify_path(const char *path, bool *shmem, bool *uncertain)
{
    bool shm = strcmp(path, "/dev/shm") == 0 || has_prefix(path, "/dev/shm/")
        || strcmp(path, "/run/shm") == 0 || has_prefix(path, "/run/shm/")
        || has_prefix(path, "memfd:") || has_prefix(path, "/memfd:")
        || strcmp(path, "[shmem]") == 0 || has_prefix(path, "/SYSV");
    *shmem = shm;
    *uncertain = shm || path[0] != '/' || has_prefix(path, "/dev/");
}

static char *clean_path(char *raw, bool *deleted, bool *shmem, bool *uncertain)
{
    size_t n;
    char *path;
    const char *suffix = " (deleted)";
    size_t sn = strlen(suffix);
    while (*raw == ' ' || *raw == '\t') raw++;
    n = strlen(raw);
    while (n && (raw[n - 1] == ' ' || raw[n - 1] == '\t')) raw[--n] = '\0';
    *deleted = n >= sn && memcmp(raw + n - sn, suffix, sn) == 0;
    if (*deleted) raw[n - sn] = '\0';
    if (!raw[0]) return NULL;
    path = dup_str(raw);
    if (!path) return NULL;
    classify_path(path, shmem, uncertain);
    return path;
}

static int field_key(const char *line, char *key, size_t key_cap, const char **val)
{
    size_t i = 0;
    const char *p;
    if (!isalpha((unsigned char)line[0])) return -1;
    while (isalnum((unsigned char)line[i]) || line[i] == '_') {
        if (i + 1 >= key_cap) return -1;
        key[i] = line[i];
        i++;
    }
    key[i] = '\0';
    p = line + i;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return -1;
    *val = p + 1;
    return 0;
}

static bool flags_special(const char *v)
{
    while (*v) {
        const char *start;
        size_t n;
        while (*v == ' ' || *v == '\t') v++;
        if (!*v) break;
        start = v;
        while (*v && *v != ' ' && *v != '\t') v++;
        n = (size_t)(v - start);
        if (n == 2 && ((start[0] == 'i' && start[1] == 'o')
                       || (start[0] == 'p' && start[1] == 'f')))
            return true;
    }
    return false;
}

static int parse_header(char *line, uint64_t *major, uint64_t *minor, uint64_t *inode,
                        char **path, bool *deleted, bool *shmem, bool *uncertain)
{
    char *p = line;
    uint64_t start, end, offset;
    if (parse_u64(p, &p, 16, &start) != 0 || *p++ != '-') return -1;
    if (parse_u64(p, &p, 16, &end) != 0 || *p++ != ' ') return -1;
    if (!p[0] || !p[1] || !p[2] || !p[3] || p[4] != ' ') return -1;
    p += 5;
    if (parse_u64(p, &p, 16, &offset) != 0 || *p++ != ' ') return -1;
    if (parse_u64(p, &p, 16, major) != 0 || *p++ != ':') return -1;
    if (parse_u64(p, &p, 16, minor) != 0 || (*p != ' ' && *p != '\t' && *p != '\0'))
        return -1;
    while (*p == ' ' || *p == '\t') p++;
    if (parse_u64(p, &p, 10, inode) != 0) return -1;
    if (*p && *p != ' ' && *p != '\t') return -1;
    while (*p == ' ' || *p == '\t') p++;
    *path = clean_path(p, deleted, shmem, uncertain);
    if (*p && !*path) return -2;
    (void)start;
    (void)end;
    (void)offset;
    return 0;
}

typedef struct {
    uint64_t major, minor, inode, pss;
    char *path;
    bool deleted, shmem, uncertain, special;
} Obs;

typedef struct {
    Obs *obs;
    size_t n, cap, vmas, special_n;
    uint64_t pss, map_pss, special_pss;
} Local;

static void local_free(Local *local)
{
    size_t i;
    if (!local) return;
    for (i = 0; i < local->n; i++) free(local->obs[i].path);
    free(local->obs);
    memset(local, 0, sizeof *local);
}

typedef struct {
    char *path;
    bool live;
} Alias;

typedef struct {
    uint64_t major, minor, inode, pss;
    Alias *aliases;
    size_t alias_n, alias_cap;
    char **users;
    size_t user_n, user_cap;
    bool users_more, deleted, shmem, uncertain;
} Acc;

typedef struct {
    Acc *acc;
    size_t acc_n, acc_cap;
    size_t *slots;
    size_t slot_n;
    Process *procs;
    size_t proc_n, proc_cap;
    uint64_t mapping_pss, special_pss;
    size_t special_n;
    char *error;
    size_t error_size;
} Collector;

static void collector_free(Collector *c)
{
    size_t i, j;
    if (!c) return;
    for (i = 0; i < c->acc_n; i++) {
        for (j = 0; j < c->acc[i].alias_n; j++) free(c->acc[i].aliases[j].path);
        free(c->acc[i].aliases);
        for (j = 0; j < c->acc[i].user_n; j++) free(c->acc[i].users[j]);
        free(c->acc[i].users);
    }
    free(c->acc);
    free(c->slots);
    for (i = 0; i < c->proc_n; i++) free(c->procs[i].name);
    free(c->procs);
    memset(c, 0, sizeof *c);
}

static size_t hash_key(uint64_t major, uint64_t minor, uint64_t inode)
{
    uint64_t x = inode;
    x ^= major + UINT64_C(0x9E3779B97F4A7C15) + (x << 6) + (x >> 2);
    x ^= minor + UINT64_C(0x9E3779B97F4A7C15) + (x << 6) + (x >> 2);
    return (size_t)x;
}

static int rehash(Collector *c, size_t ncap)
{
    size_t *slots = calloc(ncap, sizeof *slots);
    size_t i;
    if (!slots) return -1;
    for (i = 0; i < c->acc_n; i++) {
        size_t mask = ncap - 1;
        size_t j = hash_key(c->acc[i].major, c->acc[i].minor, c->acc[i].inode) & mask;
        while (slots[j]) j = (j + 1) & mask;
        slots[j] = i + 1;
    }
    free(c->slots);
    c->slots = slots;
    c->slot_n = ncap;
    return 0;
}

static Acc *acc_touch(Collector *c, uint64_t major, uint64_t minor, uint64_t inode)
{
    size_t mask, j;
    if (c->slot_n == 0 || c->acc_n + 1 > c->slot_n / 2) {
        size_t ncap = c->slot_n ? c->slot_n * 2 : 16;
        if (rehash(c, ncap) != 0) return NULL;
    }
    mask = c->slot_n - 1;
    j = hash_key(major, minor, inode) & mask;
    for (;;) {
        size_t slot = c->slots[j];
        Acc *a;
        if (slot == 0) break;
        a = &c->acc[slot - 1];
        if (a->major == major && a->minor == minor && a->inode == inode) return a;
        j = (j + 1) & mask;
    }
    if (grow((void **)&c->acc, &c->acc_cap, c->acc_n + 1, sizeof *c->acc) != 0)
        return NULL;
    memset(&c->acc[c->acc_n], 0, sizeof *c->acc);
    c->acc[c->acc_n].major = major;
    c->acc[c->acc_n].minor = minor;
    c->acc[c->acc_n].inode = inode;
    c->slots[j] = c->acc_n + 1;
    return &c->acc[c->acc_n++];
}

static int alias_add(Acc *a, const char *path, bool live)
{
    size_t i;
    Alias *item;
    for (i = 0; i < a->alias_n; i++) {
        if (strcmp(a->aliases[i].path, path) == 0) {
            if (live) a->aliases[i].live = true;
            return 0;
        }
    }
    if (a->alias_n >= ALIAS_LIMIT) {
        a->uncertain = true;
        return 0;
    }
    if (grow((void **)&a->aliases, &a->alias_cap, a->alias_n + 1, sizeof *a->aliases) != 0)
        return -1;
    item = &a->aliases[a->alias_n];
    item->path = dup_str(path);
    if (!item->path) return -1;
    item->live = live;
    a->alias_n++;
    return 0;
}

static int user_add(Acc *a, const char *name)
{
    size_t i;
    char *copy;
    if (!name || !name[0]) return 0;
    for (i = 0; i < a->user_n; i++)
        if (strcmp(a->users[i], name) == 0) return 0;
    if (a->user_n >= USER_LIMIT) {
        a->users_more = true;
        return 0;
    }
    if (grow((void **)&a->users, &a->user_cap, a->user_n + 1, sizeof *a->users) != 0)
        return -1;
    copy = dup_n(name, strnlen(name, NAME_LIMIT));
    if (!copy) return -1;
    a->users[a->user_n++] = copy;
    return 0;
}

static int preferred_alias(const Acc *a)
{
    int pref = -1;
    size_t i;
    for (i = 0; i < a->alias_n; i++) {
        if (!a->aliases[i].live) continue;
        if (pref < 0 || strcmp(a->aliases[i].path, a->aliases[pref].path) < 0)
            pref = (int)i;
    }
    if (pref >= 0) return pref;
    for (i = 0; i < a->alias_n; i++) {
        if (pref < 0 || strcmp(a->aliases[i].path, a->aliases[pref].path) < 0)
            pref = (int)i;
    }
    return pref;
}

/* Preferred path is the suffix so renderer's last-'/' basename stays stable. */
static char *format_path(Acc *a)
{
    int pref = preferred_alias(a);
    bool slash, hide_rest = false;
    size_t len = 0, i, pos = 0;
    char *out;
    if (pref < 0) return dup_str("");
    slash = strchr(a->aliases[pref].path, '/') != NULL;
    if (!slash) {
        for (i = 0; i < a->alias_n; i++) {
            if ((int)i != pref && strchr(a->aliases[i].path, '/')) hide_rest = true;
        }
    }
    if (hide_rest) a->uncertain = true;
    for (i = 0; i < a->alias_n; i++) {
        size_t n;
        if ((int)i == pref || hide_rest) continue;
        n = strlen(a->aliases[i].path);
        if (len > SIZE_MAX - n - 3) return NULL;
        len += n + 3;
    }
    {
        size_t n = strlen(a->aliases[pref].path);
        if (len > SIZE_MAX - n) return NULL;
        len += n;
    }
    out = malloc(len + 1);
    if (!out) return NULL;
    for (i = 0; i < a->alias_n; i++) {
        size_t n;
        if ((int)i == pref || hide_rest) continue;
        n = strlen(a->aliases[i].path);
        memcpy(out + pos, a->aliases[i].path, n);
        pos += n;
        memcpy(out + pos, " | ", 3);
        pos += 3;
    }
    memcpy(out + pos, a->aliases[pref].path, strlen(a->aliases[pref].path) + 1);
    return out;
}

static char *format_users(const Acc *a)
{
    size_t len = 0, i, pos = 0;
    char *out;
    if (a->user_n == 0) return NULL;
    for (i = 0; i < a->user_n; i++) {
        size_t n = strlen(a->users[i]);
        if (len > SIZE_MAX - n - 2) return NULL;
        len += n + 2;
    }
    if (a->users_more) {
        if (len > SIZE_MAX - 5) return NULL;
        len += 5;
    }
    out = malloc(len + 1);
    if (!out) return NULL;
    for (i = 0; i < a->user_n; i++) {
        size_t n = strlen(a->users[i]);
        if (i) {
            memcpy(out + pos, ", ", 2);
            pos += 2;
        }
        memcpy(out + pos, a->users[i], n);
        pos += n;
    }
    if (a->users_more) {
        memcpy(out + pos, ", ...", 5);
        pos += 5;
    }
    out[pos] = '\0';
    return out;
}

static int commit_local(Collector *c, Local *local, unsigned pid, const char *name)
{
    size_t i;
    char pid_name[32];
    const char *shown = name;
    if (local->map_pss > UINT64_MAX - c->mapping_pss
        || local->special_pss > UINT64_MAX - c->special_pss
        || local->special_n > SIZE_MAX - c->special_n)
        return 1;
    if (!shown || !shown[0]) {
        snprintf(pid_name, sizeof pid_name, "%u", pid);
        shown = pid_name;
    }
    for (i = 0; i < local->n; i++) {
        Obs *o = &local->obs[i];
        Acc *a;
        if (o->special || !o->path) continue;
        a = acc_touch(c, o->major, o->minor, o->inode);
        if (!a) return -1;
        if (add_u64(&a->pss, o->pss) != 0) return -1;
        if (alias_add(a, o->path, !o->deleted) != 0) return -1;
        if (user_add(a, shown) != 0) return -1;
        a->deleted = a->deleted || o->deleted;
        a->shmem = a->shmem || o->shmem;
        a->uncertain = a->uncertain || o->uncertain;
    }
    if (add_u64(&c->mapping_pss, local->map_pss) != 0) return -1;
    if (add_u64(&c->special_pss, local->special_pss) != 0) return -1;
    if (add_size(&c->special_n, local->special_n) != 0) return -1;
    if (grow((void **)&c->procs, &c->proc_cap, c->proc_n + 1, sizeof *c->procs) != 0)
        return -1;
    c->procs[c->proc_n].pid = pid;
    c->procs[c->proc_n].pss = local->pss;
    c->procs[c->proc_n].name = dup_n(shown, strnlen(shown, NAME_LIMIT));
    if (!c->procs[c->proc_n].name) return -1;
    c->proc_n++;
    return 0;
}

/* Consumes path. -1 OOM, -2 accounting overflow, 0 stored or intentionally dropped. */
static int take_obs(Local *local, uint64_t major, uint64_t minor, uint64_t inode,
                    uint64_t pss, char *path, bool deleted, bool shmem, bool uncertain,
                    bool special)
{
    Obs *o;
    int rc = 0;
    if (add_u64(&local->pss, pss) != 0) rc = -2;
    if (rc == 0) local->vmas++;
    if (rc == 0 && special) {
        if (add_u64(&local->special_pss, pss) != 0 || add_size(&local->special_n, 1) != 0)
            rc = -2;
    } else if (rc == 0 && path && inode != 0) {
        if (add_u64(&local->map_pss, pss) != 0) {
            rc = -2;
        } else if (grow((void **)&local->obs, &local->cap, local->n + 1, sizeof *local->obs) != 0) {
            errno = ENOMEM;
            rc = -1;
        } else {
            o = &local->obs[local->n++];
            o->major = major;
            o->minor = minor;
            o->inode = inode;
            o->pss = pss;
            o->path = path;
            o->deleted = deleted;
            o->shmem = shmem;
            o->uncertain = uncertain;
            o->special = false;
            path = NULL;
        }
    }
    free(path);
    return rc;
}

/* Field names such as Anonymous and FilePmdMapped start with A-F. A header is
 * a hex address, then '-', then another hex address. */
static bool header_line(const char *line)
{
    const char *p = line;
    if (!isxdigit((unsigned char)*p)) return false;
    while (isxdigit((unsigned char)*p)) p++;
    return *p == '-';
}

static int parse_smaps(FILE *fp, Local *local)
{
    char *line = NULL;
    size_t cap = 0;
    bool in = false, saw_pss = false, saw_flags = false;
    bool deleted = false, shmem = false, uncertain = false, special = false;
    uint64_t major = 0, minor = 0, inode = 0, pss = 0;
    char *path = NULL;
    int rc;

    memset(local, 0, sizeof *local);
    for (;;) {
        rc = read_line(fp, &line, &cap);
        if (rc == 0) break;
        if (rc == -2) {
            rc = -3;
            goto out;
        }
        if (rc < 0) {
            rc = errno == ENOMEM ? -1 : -2;
            goto out;
        }
        if (!line[0]) continue;
        if (header_line(line)) {
            if (in) {
                int took;
                if (!saw_pss || !saw_flags) {
                    rc = -3;
                    goto out;
                }
                took = take_obs(local, major, minor, inode, pss, path, deleted, shmem,
                                uncertain, special);
                path = NULL;
                if (took == -1) {
                    rc = -1;
                    goto out;
                }
                if (took != 0) {
                    rc = -3;
                    goto out;
                }
            }
            saw_pss = saw_flags = special = false;
            pss = 0;
            {
                int hdr = parse_header(line, &major, &minor, &inode, &path, &deleted,
                                       &shmem, &uncertain);
                if (hdr != 0) {
                    rc = hdr == -2 ? -1 : -3;
                    goto out;
                }
            }
            in = true;
            continue;
        }
        if (!in) {
            rc = -3;
            goto out;
        }
        {
            char key[64];
            const char *val = NULL;
            if (field_key(line, key, sizeof key, &val) != 0) {
                rc = -3;
                goto out;
            }
            if (strcmp(key, "Pss") == 0) {
                if (saw_pss || parse_kb(val, &pss) != 0) {
                    rc = -3;
                    goto out;
                }
                saw_pss = true;
            } else if (strcmp(key, "VmFlags") == 0) {
                if (saw_flags) {
                    rc = -3;
                    goto out;
                }
                special = flags_special(val);
                saw_flags = true;
            }
        }
    }
    if (in) {
        int took;
        if (!saw_pss || !saw_flags) {
            rc = -3;
            goto out;
        }
        took = take_obs(local, major, minor, inode, pss, path, deleted, shmem, uncertain,
                        special);
        path = NULL;
        if (took == -1) {
            rc = -1;
            goto out;
        }
        if (took != 0) {
            rc = -3;
            goto out;
        }
    }
    rc = 0;
out:
    free(path);
    free(line);
    if (rc != 0) local_free(local);
    return rc;
}

static int read_meminfo(const char *path, Sample *sample, char *error, size_t error_size)
{
    FILE *fp = fopen(path, "r");
    char *line = NULL;
    size_t cap = 0;
    bool saw_total = false, saw_huge = false;
    int rc = 0;
    if (!fp) {
        set_error(error, error_size, "cannot read meminfo");
        return -1;
    }
    for (;;) {
        char key[64];
        const char *val = NULL;
        uint64_t n = 0;
        int kind, line_rc = read_line(fp, &line, &cap);
        if (line_rc == 0) break;
        if (line_rc < 0) {
            set_error(error, error_size, line_rc == -2 ? "meminfo line is too long"
                                                       : "cannot read meminfo");
            rc = -1;
            break;
        }
        if (!line[0] || !strchr(line, ':')) continue;
        if (field_key(line, key, sizeof key, &val) != 0) continue;
        kind = !strcmp(key, "MemTotal") ? 1
            : !strcmp(key, "MemAvailable") ? 2
            : !strcmp(key, "AnonPages") ? 3
            : !strcmp(key, "Hugetlb") ? 4 : 0;
        if (!kind) continue;
        if ((kind == 1 && saw_total) || (kind == 2 && sample->has_available)
            || (kind == 3 && sample->has_anonymous) || (kind == 4 && saw_huge)) {
            set_error(error, error_size, "meminfo field is duplicated");
            rc = -1;
            break;
        }
        if (parse_kb(val, &n) != 0 || (kind == 1 && n == 0)) {
            set_error(error, error_size, "meminfo field is invalid");
            rc = -1;
            break;
        }
        if (kind == 1) {
            sample->total = n;
            saw_total = true;
        } else if (kind == 2) {
            sample->available = n;
            sample->has_available = true;
        } else if (kind == 3) {
            sample->anonymous = n;
            sample->has_anonymous = true;
        } else {
            sample->huge_kib = n;
            saw_huge = true;
        }
    }
    free(line);
    fclose(fp);
    if (rc == 0 && !saw_total) {
        set_error(error, error_size, "meminfo is missing MemTotal");
        rc = -1;
    }
    return rc;
}

static char *join_path(const char *root, const char *pid, const char *leaf)
{
    size_t rl = strlen(root), pl = strlen(pid), ll = strlen(leaf);
    int slash = !(rl && root[rl - 1] == '/');
    size_t n = rl + (size_t)slash + pl + 1 + ll;
    char *s;
    if (n < rl || n < pl) return NULL;
    s = malloc(n + 1);
    if (!s) return NULL;
    memcpy(s, root, rl);
    if (slash) s[rl] = '/';
    memcpy(s + rl + (size_t)slash, pid, pl);
    s[rl + (size_t)slash + pl] = '/';
    memcpy(s + rl + (size_t)slash + pl + 1, leaf, ll + 1);
    return s;
}

static char *read_comm(const char *path)
{
    FILE *fp = fopen(path, "r");
    char buf[256];
    size_t n;
    if (!fp) return NULL;
    n = fread(buf, 1, sizeof buf - 1, fp);
    if (n == 0 && ferror(fp)) {
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    if (n && buf[n - 1] == '\n') n--;
    buf[n] = '\0';
    if (!buf[0]) return NULL;
    return dup_n(buf, strnlen(buf, NAME_LIMIT));
}

static int pid_cmp(const void *x, const void *y)
{
    const char *const *a = x, *const *b = y;
    unsigned long pa = strtoul(*a, NULL, 10), pb = strtoul(*b, NULL, 10);
    if (pa != pb) return pa < pb ? -1 : 1;
    return strcmp(*a, *b);
}

static int count_open_errno(Sample *sample, int err)
{
    if (err == EACCES || err == EPERM) {
        if (add_size(&sample->denied, 1) != 0) return -1;
    } else if (err == ENOENT || err == ESRCH) {
        if (add_size(&sample->vanished, 1) != 0) return -1;
    } else if (add_size(&sample->invalid, 1) != 0) {
        return -1;
    }
    return 0;
}

static int scan_pid(Collector *c, Sample *sample, const char *root, const char *pid_name)
{
    char *smaps_path = NULL, *comm_path = NULL, *name = NULL;
    FILE *fp = NULL;
    Local local;
    unsigned long pid_ul;
    unsigned pid;
    char *end = NULL;
    int rc, parsed;

    memset(&local, 0, sizeof local);
    pid_ul = strtoul(pid_name, &end, 10);
    if (!pid_name[0] || !end || *end || pid_ul > (unsigned long)UINT_MAX) {
        if (add_size(&sample->invalid, 1) != 0) return -1;
        return 0;
    }
    pid = (unsigned)pid_ul;
    smaps_path = join_path(root, pid_name, "smaps");
    if (!smaps_path) {
        set_error(c->error, c->error_size, "out of memory");
        return -1;
    }
    fp = fopen(smaps_path, "r");
    if (!fp) {
        rc = count_open_errno(sample, errno);
        free(smaps_path);
        return rc;
    }
    parsed = parse_smaps(fp, &local);
    {
        int err = errno;
        fclose(fp);
        free(smaps_path);
        smaps_path = NULL;
        if (parsed == -1) {
            set_error(c->error, c->error_size, "out of memory");
            return -1;
        }
        if (parsed == -2) return count_open_errno(sample, err ? err : EIO);
    }
    if (parsed != 0) {
        if (add_size(&sample->invalid, 1) != 0) return -1;
        return 0;
    }
    if (local.vmas == 0) {
        local_free(&local);
        if (add_size(&sample->empty, 1) != 0) return -1;
        return 0;
    }
    comm_path = join_path(root, pid_name, "comm");
    if (!comm_path) {
        local_free(&local);
        set_error(c->error, c->error_size, "out of memory");
        return -1;
    }
    name = read_comm(comm_path);
    free(comm_path);
    rc = commit_local(c, &local, pid, name);
    free(name);
    local_free(&local);
    if (rc < 0) {
        set_error(c->error, c->error_size, "out of memory");
        return -1;
    }
    if (rc > 0) {
        if (add_size(&sample->invalid, 1) != 0) return -1;
        return 0;
    }
    if (add_size(&sample->read, 1) != 0) return -1;
    return 0;
}

static int publish(Collector *c, Sample *sample)
{
    Mapping *maps = NULL;
    size_t i;
    if (c->acc_n) {
        maps = calloc(c->acc_n, sizeof *maps);
        if (!maps) return -1;
        for (i = 0; i < c->acc_n; i++) {
            maps[i].major = c->acc[i].major;
            maps[i].minor = c->acc[i].minor;
            maps[i].inode = c->acc[i].inode;
            maps[i].pss = c->acc[i].pss;
            maps[i].deleted = c->acc[i].deleted;
            maps[i].shmem = c->acc[i].shmem;
            maps[i].uncertain = c->acc[i].uncertain;
            maps[i].path = format_path(&c->acc[i]);
            maps[i].users = format_users(&c->acc[i]);
            if (!maps[i].path || (c->acc[i].user_n && !maps[i].users)) {
                size_t j;
                for (j = 0; j <= i; j++) {
                    free(maps[j].path);
                    free(maps[j].users);
                }
                free(maps);
                return -1;
            }
        }
    }
    sample->maps = maps;
    sample->map_count = c->acc_n;
    sample->mapping_pss = c->mapping_pss;
    sample->special_pss = c->special_pss;
    sample->special_count = c->special_n;
    sample->processes = c->procs;
    sample->process_count = c->proc_n;
    c->procs = NULL;
    c->proc_n = 0;
    c->proc_cap = 0;
    return 0;
}

static bool digits_only(const char *s)
{
    if (!s || !s[0]) return false;
    for (; *s; s++)
        if (*s < '0' || *s > '9') return false;
    return true;
}

int collect(const char *root, Sample *sample, char *error, size_t error_size)
{
    Collector collector;
    DIR *dir;
    struct dirent *ent;
    char **pids = NULL;
    size_t pid_n = 0, pid_cap = 0, i;
    char *meminfo = NULL;
    struct timespec t0, t1;
    int rc = 0;

    if (error && error_size) error[0] = '\0';
    if (!sample) {
        set_error(error, error_size, "sample is missing");
        return -1;
    }
    sample_free(sample);
    if (!root || !root[0]) {
        set_error(error, error_size, "proc root is missing");
        return -1;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &t0) != 0) {
        t0.tv_sec = 0;
        t0.tv_nsec = 0;
    }
    memset(&collector, 0, sizeof collector);
    collector.error = error;
    collector.error_size = error_size;

    {
        size_t rl = strlen(root);
        int slash = !(rl && root[rl - 1] == '/');
        meminfo = malloc(rl + (size_t)slash + strlen("meminfo") + 1);
        if (!meminfo) {
            set_error(error, error_size, "out of memory");
            return -1;
        }
        memcpy(meminfo, root, rl);
        if (slash) meminfo[rl] = '/';
        memcpy(meminfo + rl + (size_t)slash, "meminfo", strlen("meminfo") + 1);
    }
    if (read_meminfo(meminfo, sample, error, error_size) != 0) {
        free(meminfo);
        sample_free(sample);
        return -1;
    }
    free(meminfo);

    dir = opendir(root);
    if (!dir) {
        set_error(error, error_size, "cannot open proc root");
        sample_free(sample);
        return -1;
    }
    errno = 0;
    while ((ent = readdir(dir)) != NULL) {
        char *copy;
        if (!digits_only(ent->d_name)) continue;
        if (grow((void **)&pids, &pid_cap, pid_n + 1, sizeof *pids) != 0) {
            rc = -1;
            set_error(error, error_size, "out of memory");
            break;
        }
        copy = dup_str(ent->d_name);
        if (!copy) {
            rc = -1;
            set_error(error, error_size, "out of memory");
            break;
        }
        pids[pid_n++] = copy;
        errno = 0;
    }
    if (!rc && errno) {
        set_error(error, error_size, "cannot read proc root");
        rc = -1;
    }
    closedir(dir);
    if (rc) goto fail;

    if (pid_n > 1) qsort(pids, pid_n, sizeof *pids, pid_cmp);
    for (i = 0; i < pid_n; i++) {
        if (scan_pid(&collector, sample, root, pids[i]) != 0) {
            rc = -1;
            break;
        }
    }
    if (rc) goto fail;
    if (publish(&collector, sample) != 0) {
        set_error(error, error_size, "out of memory");
        rc = -1;
        goto fail;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &t1) == 0) {
        sample->elapsed = (double)(t1.tv_sec - t0.tv_sec)
            + (double)(t1.tv_nsec - t0.tv_nsec) / 1000000000.0;
        if (sample->elapsed < 0) sample->elapsed = 0;
    }
    collector_free(&collector);
    for (i = 0; i < pid_n; i++) free(pids[i]);
    free(pids);
    return 0;

fail:
    collector_free(&collector);
    for (i = 0; i < pid_n; i++) free(pids[i]);
    free(pids);
    sample_free(sample);
    if (error && error_size && !error[0])
        set_error(error, error_size, "collect failed");
    return -1;
}

void sample_free(Sample *sample)
{
    size_t i;
    if (!sample) return;
    for (i = 0; i < sample->map_count; i++) {
        free(sample->maps[i].path);
        free(sample->maps[i].users);
    }
    free(sample->maps);
    for (i = 0; i < sample->process_count; i++) free(sample->processes[i].name);
    free(sample->processes);
    memset(sample, 0, sizeof *sample);
}
