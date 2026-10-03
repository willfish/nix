#define _POSIX_C_SOURCE 200809L
#include "attachments.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char ADMITTED[] = "admitted";
static const char EXISTING[] = "existing";
static const char SUPERSEDED[] = "superseded";
static const char RETRY[] = "retry";

static int uid_override_enabled;
static unsigned uid_override;

void attachments_set_uid_override(int enabled, unsigned uid) {
    uid_override_enabled = enabled;
    uid_override = uid;
}

static unsigned current_uid(void) {
    if (uid_override_enabled) return uid_override;
    return (unsigned)getuid();
}

static int py_ne(const char *a, const char *b) {
    if (!a && !b) return 0;
    if (!a || !b) return 1;
    return strcmp(a, b) != 0;
}

static char *dup_str(const char *s) {
    if (!s) return NULL;
    return strdup(s);
}

int can_auto_select(int selection_initialized, const char *selected_token, int ready, int team_child) {
    return !selection_initialized && selected_token == NULL && ready && !team_child;
}

PaneKey pane_key(const AttachmentTarget *target) {
    PaneKey key = {0};
    if (!target) return key;
    key.socket_path = target->socket;
    key.socket_device = target->socket_device;
    key.socket_inode = target->socket_inode;
    key.pane_id = target->pane;
    return key;
}

BridgeIdentity bridge_identity(const AttachmentTarget *target) {
    BridgeIdentity id = {0};
    if (!target) return id;
    id.pane = pane_key(target);
    id.pid = target->pid;
    id.process_start = target->start;
    id.bridge_id = target->bridge_id;
    return id;
}

int identity_equal(const BridgeIdentity *a, const BridgeIdentity *b) {
    if (!a || !b) return 0;
    return a->pid == b->pid
        && !py_ne(a->process_start, b->process_start)
        && !py_ne(a->bridge_id, b->bridge_id)
        && !py_ne(a->pane.socket_path, b->pane.socket_path)
        && a->pane.socket_device == b->pane.socket_device
        && a->pane.socket_inode == b->pane.socket_inode
        && !py_ne(a->pane.pane_id, b->pane.pane_id);
}

static int canonical_uuid(const char *s) {
    if (!s || strlen(s) != 36) return 0;
    for (size_t i = 0; i < 36; i++) {
        int hyphen = i == 8 || i == 13 || i == 18 || i == 23;
        if (hyphen) {
            if (s[i] != '-') return 0;
        } else if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

static int pathlib_absolute(const char *path, char *out, size_t cap) {
    char raw[PATH_MAX];
    if (!path || !out || cap == 0) return -1;
    if (path[0] == '/') {
        if (snprintf(raw, sizeof raw, "%s", path) >= (int)sizeof raw) return -1;
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof cwd)) return -1;
        if (snprintf(raw, sizeof raw, "%s/%s", cwd, path) >= (int)sizeof raw) return -1;
    }
    char *parts[512];
    int n = 0;
    char *cursor = raw;
    if (*cursor == '/') cursor++;
    while (*cursor) {
        char *start = cursor;
        while (*cursor && *cursor != '/') cursor++;
        if (*cursor == '/') {
            *cursor++ = 0;
        }
        if (start[0] == 0 || strcmp(start, ".") == 0) continue;
        if (n >= 512) return -1;
        parts[n++] = start;
    }
    if (n == 0) {
        if (cap < 2) return -1;
        memcpy(out, "/", 2);
        return 0;
    }
    size_t used = 0;
    for (int i = 0; i < n; i++) {
        int w = snprintf(out + used, cap - used, "/%s", parts[i]);
        if (w < 0 || (size_t)w >= cap - used) return -1;
        used += (size_t)w;
    }
    return 0;
}

static int resolve_matches_absolute(const char *runtime) {
    char *resolved = realpath(runtime, NULL);
    if (!resolved) return 0;
    char absolute[PATH_MAX];
    int ok = pathlib_absolute(runtime, absolute, sizeof absolute) == 0 && strcmp(resolved, absolute) == 0;
    free(resolved);
    return ok;
}

static int split_absolute(const char *path, char *parent, size_t parent_cap, char *name, size_t name_cap) {
    char abs[PATH_MAX];
    if (pathlib_absolute(path, abs, sizeof abs) != 0) return -1;
    char *slash = strrchr(abs, '/');
    if (!slash) return -1;
    if (strlen(slash + 1) >= name_cap) return -1;
    memcpy(name, slash + 1, strlen(slash + 1) + 1);
    if (slash == abs) {
        if (parent_cap < 2) return -1;
        memcpy(parent, "/", 2);
    } else {
        *slash = 0;
        if (strlen(abs) >= parent_cap) return -1;
        memcpy(parent, abs, strlen(abs) + 1);
    }
    return 0;
}

int socket_instance(const char *path, char *canonical, size_t cap, uint64_t *dev, uint64_t *ino) {
    if (!path || !canonical || cap == 0 || !dev || !ino) return -1;
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if (!S_ISSOCK(st.st_mode) || st.st_uid != current_uid()) return -1;
    char *copy = strdup(path);
    if (!copy) return -1;
    char *slash = strrchr(copy, '/');
    const char *parent;
    const char *name;
    if (!slash) {
        parent = ".";
        name = copy;
    } else if (slash == copy) {
        parent = "/";
        name = slash + 1;
    } else {
        *slash = 0;
        parent = copy;
        name = slash + 1;
    }
    if (!name[0]) {
        free(copy);
        return -1;
    }
    char *parent_real = realpath(parent, NULL);
    if (!parent_real) {
        free(copy);
        return -1;
    }
    int n = strcmp(parent_real, "/") == 0
        ? snprintf(canonical, cap, "/%s", name)
        : snprintf(canonical, cap, "%s/%s", parent_real, name);
    free(parent_real);
    free(copy);
    if (n < 0 || (size_t)n >= cap) return -1;
    *dev = (uint64_t)st.st_dev;
    *ino = (uint64_t)st.st_ino;
    return 0;
}

int validate_endpoint(const char *runtime, const char *path, int pid, const char *bridge_id,
    uint64_t *dev, uint64_t *ino) {
    if (!dev || !ino || !runtime || !path) return -1;
    if (!canonical_uuid(bridge_id)) return -1;
    struct stat parent;
    if (lstat(runtime, &parent) != 0) return -1;
    if (!S_ISDIR(parent.st_mode) || parent.st_uid != current_uid() || (parent.st_mode & 077)
        || !resolve_matches_absolute(runtime)) {
        return -1;
    }
    if (path[0] != '/') return -1;
    char parent_path[PATH_MAX];
    char name[256];
    char runtime_abs[PATH_MAX];
    char expect[256];
    if (split_absolute(path, parent_path, sizeof parent_path, name, sizeof name) != 0) return -1;
    if (pathlib_absolute(runtime, runtime_abs, sizeof runtime_abs) != 0) return -1;
    if (snprintf(expect, sizeof expect, "pi-%d-%s.sock", pid, bridge_id) >= (int)sizeof expect) return -1;
    if (strcmp(parent_path, runtime_abs) != 0 || strcmp(name, expect) != 0) return -1;
    struct stat info;
    if (lstat(path, &info) != 0) return -1;
    if (!S_ISSOCK(info.st_mode) || info.st_uid != current_uid() || (info.st_mode & 077)) return -1;
    *dev = (uint64_t)info.st_dev;
    *ino = (uint64_t)info.st_ino;
    return 0;
}

static int opt_str_eq(const char *a, const char *b) {
    return !py_ne(a, b);
}

static int opt_int_eq(int has_a, int64_t a, int has_b, int64_t b) {
    if (!has_a && !has_b) return 1;
    if (has_a != has_b) return 0;
    return a == b;
}

int event_matches(const EventView *target, const EventView *event, int session) {
    if (!target || !event) return 0;
    if (!opt_str_eq(target->bridge_id, event->bridge_id)) return 0;
    if (!opt_int_eq(target->has_activation, target->activation, event->has_activation, event->activation))
        return 0;
    if (!opt_int_eq(target->has_pid, target->pid, event->has_pid, event->pid)) return 0;
    if (!session) return 1;
    return opt_str_eq(target->session, event->session) && opt_str_eq(target->harness, event->harness);
}

static int read_stat_text(int pid, char *buf, size_t cap) {
    char path[64];
    if (pid <= 0 || cap < 2) return -1;
    if (snprintf(path, sizeof path, "/proc/%d/stat", pid) >= (int)sizeof path) return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t n = 0;
    while (n + 1 < cap) {
        ssize_t got = read(fd, buf + n, cap - 1 - n);
        if (got < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (got == 0) break;
        n += (size_t)got;
    }
    close(fd);
    if (n == 0) return -1;
    buf[n] = 0;
    return 0;
}

int process_start_ticks(int pid, char *out, size_t cap) {
    char buf[4096];
    if (!out || cap == 0) return -1;
    if (read_stat_text(pid, buf, sizeof buf) != 0) return -1;
    char *rp = strrchr(buf, ')');
    if (!rp || !rp[1]) return -1;
    char *save = NULL;
    char *tok = strtok_r(rp + 1, " \t\n", &save);
    for (int i = 0; tok && i < 19; i++) tok = strtok_r(NULL, " \t\n", &save);
    if (!tok || strlen(tok) >= cap) return -1;
    memcpy(out, tok, strlen(tok) + 1);
    return 0;
}

int process_uid_matches(int pid) {
    char path[64];
    struct stat st;
    if (pid <= 0) return 0;
    if (snprintf(path, sizeof path, "/proc/%d", pid) >= (int)sizeof path) return 0;
    if (stat(path, &st) != 0) return 0;
    return st.st_uid == current_uid();
}

int process_still_same(int pid, const char *start_ticks) {
    char ticks[64];
    if (!start_ticks) return 0;
    if (process_start_ticks(pid, ticks, sizeof ticks) != 0) return 0;
    if (!process_uid_matches(pid)) return 0;
    return strcmp(ticks, start_ticks) == 0;
}

static int random_bytes(unsigned char *buf, size_t n) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < n) {
        ssize_t got = read(fd, buf + off, n - off);
        if (got < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (got == 0) {
            close(fd);
            return -1;
        }
        off += (size_t)got;
    }
    close(fd);
    return 0;
}

static int uuid_v4(char *out, size_t cap) {
    unsigned char b[16];
    if (cap < 37 || random_bytes(b, sizeof b) != 0) return -1;
    b[6] = (unsigned char)((b[6] & 0x0f) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);
    snprintf(out, cap,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11],
        b[12], b[13], b[14], b[15]);
    return 0;
}

typedef struct {
    char *path;
    uint64_t device;
    uint64_t inode;
    char *pane;
    int pid;
    char *start;
    char *bridge;
    char token[128];
} IdentitySlot;

typedef struct {
    char *path;
    char *pane;
    char *token;
    int64_t revision;
} PaneSlot;

typedef struct {
    int pid;
    char *start;
    int64_t ordinal;
    char *bridge;
} Highwater;

typedef struct {
    int pid;
    char *start;
    char *bridge;
} Retired;

struct AttachmentRegistry {
    IdentitySlot *ids;
    size_t id_count;
    size_t id_cap;
    PaneSlot *panes;
    size_t pane_count;
    size_t pane_cap;
    Highwater *high;
    size_t high_count;
    size_t high_cap;
    Retired *retired;
    size_t retired_count;
    size_t retired_cap;
};

static int grow(void **ptr, size_t count, size_t *cap, size_t elem) {
    if (count < *cap) return 0;
    size_t ncap = *cap ? *cap * 2 : 8;
    void *next = realloc(*ptr, ncap * elem);
    if (!next) return -1;
    *ptr = next;
    *cap = ncap;
    return 0;
}

AttachmentRegistry *registry_new(void) {
    return calloc(1, sizeof(AttachmentRegistry));
}

static void free_identity(IdentitySlot *id) {
    free(id->path);
    free(id->pane);
    free(id->start);
    free(id->bridge);
    memset(id, 0, sizeof *id);
}

void registry_free(AttachmentRegistry *registry) {
    if (!registry) return;
    for (size_t i = 0; i < registry->id_count; i++) free_identity(&registry->ids[i]);
    free(registry->ids);
    for (size_t i = 0; i < registry->pane_count; i++) {
        free(registry->panes[i].path);
        free(registry->panes[i].pane);
        free(registry->panes[i].token);
    }
    free(registry->panes);
    for (size_t i = 0; i < registry->high_count; i++) {
        free(registry->high[i].start);
        free(registry->high[i].bridge);
    }
    free(registry->high);
    for (size_t i = 0; i < registry->retired_count; i++) {
        free(registry->retired[i].start);
        free(registry->retired[i].bridge);
    }
    free(registry->retired);
    free(registry);
}

static int same_identity(const IdentitySlot *s, const BridgeIdentity *id) {
    return s->pid == id->pid
        && !py_ne(s->start, id->process_start)
        && !py_ne(s->bridge, id->bridge_id)
        && s->device == id->pane.socket_device
        && s->inode == id->pane.socket_inode
        && !py_ne(s->path, id->pane.socket_path)
        && !py_ne(s->pane, id->pane.pane_id);
}

static PaneSlot *find_pane(AttachmentRegistry *registry, const char *path, const char *pane, int create) {
    for (size_t i = 0; i < registry->pane_count; i++) {
        if (!py_ne(registry->panes[i].path, path) && !py_ne(registry->panes[i].pane, pane))
            return &registry->panes[i];
    }
    if (!create) return NULL;
    if (grow((void **)&registry->panes, registry->pane_count, &registry->pane_cap, sizeof(PaneSlot)) != 0)
        return NULL;
    PaneSlot *slot = &registry->panes[registry->pane_count++];
    memset(slot, 0, sizeof *slot);
    slot->path = dup_str(path ? path : "");
    slot->pane = dup_str(pane ? pane : "");
    if (!slot->path || !slot->pane) {
        free(slot->path);
        free(slot->pane);
        registry->pane_count--;
        return NULL;
    }
    return slot;
}

int64_t registry_revision(const AttachmentRegistry *registry, const PaneKey *pane) {
    if (!registry || !pane) return 0;
    for (size_t i = 0; i < registry->pane_count; i++) {
        if (!py_ne(registry->panes[i].path, pane->socket_path)
            && !py_ne(registry->panes[i].pane, pane->pane_id)) {
            return registry->panes[i].revision;
        }
    }
    return 0;
}

const char *registry_lookup(const AttachmentRegistry *registry, const BridgeIdentity *identity) {
    if (!registry || !identity) return NULL;
    for (size_t i = 0; i < registry->id_count; i++) {
        if (same_identity(&registry->ids[i], identity)) return registry->ids[i].token;
    }
    return NULL;
}

static IdentitySlot *find_token(AttachmentRegistry *registry, const char *token) {
    if (!token) return NULL;
    for (size_t i = 0; i < registry->id_count; i++) {
        if (strcmp(registry->ids[i].token, token) == 0) return &registry->ids[i];
    }
    return NULL;
}

static int retired_has(const AttachmentRegistry *registry, int pid, const char *start, const char *bridge) {
    for (size_t i = 0; i < registry->retired_count; i++) {
        if (registry->retired[i].pid == pid && !py_ne(registry->retired[i].start, start)
            && !py_ne(registry->retired[i].bridge, bridge)) {
            return 1;
        }
    }
    return 0;
}

static int add_retired(AttachmentRegistry *registry, int pid, const char *start, const char *bridge) {
    if (retired_has(registry, pid, start, bridge)) return 0;
    if (grow((void **)&registry->retired, registry->retired_count, &registry->retired_cap, sizeof(Retired)) != 0)
        return -1;
    Retired *item = &registry->retired[registry->retired_count];
    item->pid = pid;
    item->start = dup_str(start ? start : "");
    item->bridge = dup_str(bridge ? bridge : "");
    if (!item->start || !item->bridge) return -1;
    registry->retired_count++;
    return 0;
}

static Highwater *find_high(AttachmentRegistry *registry, int pid, const char *start) {
    for (size_t i = 0; i < registry->high_count; i++) {
        if (registry->high[i].pid == pid && !py_ne(registry->high[i].start, start)) return &registry->high[i];
    }
    return NULL;
}

static int set_highwater(AttachmentRegistry *registry, int pid, const char *start, int64_t ordinal, const char *bridge) {
    Highwater *existing = find_high(registry, pid, start);
    char *copy = dup_str(bridge ? bridge : "");
    if (!copy) return -1;
    if (existing) {
        free(existing->bridge);
        existing->bridge = copy;
        existing->ordinal = ordinal;
        return 0;
    }
    if (grow((void **)&registry->high, registry->high_count, &registry->high_cap, sizeof(Highwater)) != 0) {
        free(copy);
        return -1;
    }
    Highwater *item = &registry->high[registry->high_count++];
    item->pid = pid;
    item->start = dup_str(start ? start : "");
    item->ordinal = ordinal;
    item->bridge = copy;
    if (!item->start) return -1;
    return 0;
}

static void fill_token(Admission *out, const char *token) {
    out->has_token = 0;
    out->token[0] = 0;
    if (!token) return;
    snprintf(out->token, sizeof out->token, "%s", token);
    out->has_token = 1;
}

static void clear_admission(Admission *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
}

void registry_remove(AttachmentRegistry *registry, const char *token, int retire) {
    if (!registry || !token) return;
    IdentitySlot *found = find_token(registry, token);
    if (!found) return;
    if (retire) add_retired(registry, found->pid, found->start, found->bridge);
    char *path = dup_str(found->path);
    char *pane = dup_str(found->pane);
    size_t index = (size_t)(found - registry->ids);
    free_identity(found);
    if (index + 1 < registry->id_count) {
        memmove(&registry->ids[index], &registry->ids[index + 1],
            (registry->id_count - index - 1) * sizeof(IdentitySlot));
    }
    registry->id_count--;
    if (path && pane) {
        PaneSlot *slot = find_pane(registry, path, pane, 1);
        if (slot) {
            if (slot->token && strcmp(slot->token, token) == 0) {
                free(slot->token);
                slot->token = NULL;
            }
            slot->revision += 1;
        }
    }
    free(path);
    free(pane);
}

int registry_admit(AttachmentRegistry *registry, const BridgeIdentity *identity,
    int64_t activation, int64_t revision, const char *token, Admission *out) {
    if (!out) return -1;
    clear_admission(out);
    if (!registry || !identity || !identity->pane.socket_path || !identity->pane.pane_id
        || !identity->process_start || !identity->bridge_id) {
        return -1;
    }
    Highwater *mark = find_high(registry, identity->pid, identity->process_start);
    int64_t highest = mark ? mark->ordinal : 0;
    const char *owner = mark ? mark->bridge : NULL;
    if (retired_has(registry, identity->pid, identity->process_start, identity->bridge_id)
        || activation < highest) {
        out->state = SUPERSEDED;
        return 0;
    }
    if (activation == highest && py_ne(owner, identity->bridge_id)) return -1;
    if (registry_revision(registry, &identity->pane) != revision) {
        out->state = RETRY;
        return 0;
    }
    const char *existing = registry_lookup(registry, identity);
    if (existing) {
        out->state = EXISTING;
        fill_token(out, existing);
        return 0;
    }
    PaneSlot *slot = find_pane(registry, identity->pane.socket_path, identity->pane.pane_id, 0);
    char replaced[128] = {0};
    int has_replaced = 0;
    if (slot && slot->token) {
        snprintf(replaced, sizeof replaced, "%s", slot->token);
        has_replaced = 1;
    }
    char issued[128];
    if (token && token[0]) {
        if (strlen(token) >= sizeof issued) return -1;
        memcpy(issued, token, strlen(token) + 1);
    } else if (uuid_v4(issued, sizeof issued) != 0) {
        return -1;
    }
    IdentitySlot fresh = {0};
    fresh.path = dup_str(identity->pane.socket_path);
    fresh.pane = dup_str(identity->pane.pane_id);
    fresh.start = dup_str(identity->process_start);
    fresh.bridge = dup_str(identity->bridge_id);
    fresh.device = identity->pane.socket_device;
    fresh.inode = identity->pane.socket_inode;
    fresh.pid = identity->pid;
    snprintf(fresh.token, sizeof fresh.token, "%s", issued);
    if (!fresh.path || !fresh.pane || !fresh.start || !fresh.bridge
        || grow((void **)&registry->ids, registry->id_count, &registry->id_cap, sizeof(IdentitySlot)) != 0) {
        free_identity(&fresh);
        return -1;
    }
    if (has_replaced) registry_remove(registry, replaced, 1);
    if (set_highwater(registry, identity->pid, identity->process_start, activation, identity->bridge_id) != 0) {
        free_identity(&fresh);
        return -1;
    }
    registry->ids[registry->id_count++] = fresh;
    PaneSlot *dest = find_pane(registry, identity->pane.socket_path, identity->pane.pane_id, 1);
    if (!dest) return -1;
    free(dest->token);
    dest->token = dup_str(issued);
    if (!dest->token) return -1;
    dest->revision += 1;
    out->state = ADMITTED;
    fill_token(out, issued);
    if (has_replaced) {
        snprintf(out->replaced, sizeof out->replaced, "%s", replaced);
        out->has_replaced = 1;
    }
    return 0;
}

static char *dup_or_empty(const char *s) {
    return dup_str(s ? s : "");
}

int registry_fences(const AttachmentRegistry *registry, Fences *out) {
    if (!registry || !out) return -1;
    memset(out, 0, sizeof *out);
    if (registry->high_count) {
        out->highwater = calloc(registry->high_count, sizeof(FenceHighwater));
        if (!out->highwater) return -1;
        for (size_t i = 0; i < registry->high_count; i++) {
            out->highwater[i].pid = registry->high[i].pid;
            out->highwater[i].ordinal = registry->high[i].ordinal;
            out->highwater[i].process_start = dup_or_empty(registry->high[i].start);
            out->highwater[i].bridge_id = dup_or_empty(registry->high[i].bridge);
            if (!out->highwater[i].process_start || !out->highwater[i].bridge_id) {
                fences_free(out);
                return -1;
            }
        }
        out->highwater_count = registry->high_count;
    }
    if (registry->retired_count) {
        out->retired = calloc(registry->retired_count, sizeof(FenceRetired));
        if (!out->retired) {
            fences_free(out);
            return -1;
        }
        for (size_t i = 0; i < registry->retired_count; i++) {
            out->retired[i].pid = registry->retired[i].pid;
            out->retired[i].process_start = dup_or_empty(registry->retired[i].start);
            out->retired[i].bridge_id = dup_or_empty(registry->retired[i].bridge);
            if (!out->retired[i].process_start || !out->retired[i].bridge_id) {
                fences_free(out);
                return -1;
            }
        }
        out->retired_count = registry->retired_count;
    }
    return 0;
}

void fences_free(Fences *fences) {
    if (!fences) return;
    for (size_t i = 0; i < fences->highwater_count; i++) {
        free((void *)fences->highwater[i].process_start);
        free((void *)fences->highwater[i].bridge_id);
    }
    free(fences->highwater);
    for (size_t i = 0; i < fences->retired_count; i++) {
        free((void *)fences->retired[i].process_start);
        free((void *)fences->retired[i].bridge_id);
    }
    free(fences->retired);
    memset(fences, 0, sizeof *fences);
}

static void clear_fences(AttachmentRegistry *registry) {
    for (size_t i = 0; i < registry->high_count; i++) {
        free(registry->high[i].start);
        free(registry->high[i].bridge);
    }
    registry->high_count = 0;
    for (size_t i = 0; i < registry->retired_count; i++) {
        free(registry->retired[i].start);
        free(registry->retired[i].bridge);
    }
    registry->retired_count = 0;
}

int registry_restore_fences(AttachmentRegistry *registry, const Fences *saved) {
    if (!registry) return -1;
    clear_fences(registry);
    if (!saved) return 0;
    for (size_t i = 0; i < saved->highwater_count; i++) {
        const FenceHighwater *item = &saved->highwater[i];
        if (set_highwater(registry, item->pid, item->process_start ? item->process_start : "",
                item->ordinal, item->bridge_id ? item->bridge_id : "") != 0) {
            return -1;
        }
    }
    for (size_t i = 0; i < saved->retired_count; i++) {
        const FenceRetired *item = &saved->retired[i];
        if (add_retired(registry, item->pid, item->process_start, item->bridge_id) != 0) return -1;
    }
    return 0;
}

static int captured_has(const CapturedIncarnation *captured, size_t n, int pid, const char *start) {
    for (size_t i = 0; i < n; i++) {
        if (captured[i].pid == pid && !py_ne(captured[i].start, start)) return 1;
    }
    return 0;
}

static int starts_equal(const ObservedStart *starts, size_t n, int pid, const char *start, int *found) {
    const char *observed = NULL;
    *found = 0;
    for (size_t i = 0; i < n; i++) {
        if (starts[i].pid == pid) {
            *found = 1;
            observed = starts[i].start;
        }
    }
    if (!*found) return 0;
    return !py_ne(observed, start);
}

static int keep_fence(int pid, const char *start, const ObservedStart *starts, size_t nstarts,
    const CapturedIncarnation *captured, size_t ncaptured) {
    if (!captured_has(captured, ncaptured, pid, start)) return 1;
    int found = 0;
    if (!starts_equal(starts, nstarts, pid, start, &found)) {
        if (!found) return 1;
        return 0;
    }
    return 1;
}

void registry_prune(AttachmentRegistry *registry, const ObservedStart *starts, size_t nstarts,
    const CapturedIncarnation *captured, size_t ncaptured, int has_captured) {
    if (!registry) return;
    CapturedIncarnation *owned = NULL;
    const CapturedIncarnation *use = captured;
    size_t use_n = has_captured ? ncaptured : 0;
    if (!has_captured) {
        use_n = registry->high_count + registry->retired_count;
        if (use_n) {
            owned = calloc(use_n, sizeof *owned);
            if (!owned) return;
            size_t n = 0;
            for (size_t i = 0; i < registry->high_count; i++) {
                owned[n].pid = registry->high[i].pid;
                owned[n].start = registry->high[i].start;
                n++;
            }
            for (size_t i = 0; i < registry->retired_count; i++) {
                owned[n].pid = registry->retired[i].pid;
                owned[n].start = registry->retired[i].start;
                n++;
            }
            use = owned;
            use_n = n;
        }
    }
    size_t w = 0;
    for (size_t i = 0; i < registry->high_count; i++) {
        if (keep_fence(registry->high[i].pid, registry->high[i].start, starts, nstarts, use, use_n)) {
            if (w != i) registry->high[w] = registry->high[i];
            w++;
        } else {
            free(registry->high[i].start);
            free(registry->high[i].bridge);
        }
    }
    registry->high_count = w;
    w = 0;
    for (size_t i = 0; i < registry->retired_count; i++) {
        if (keep_fence(registry->retired[i].pid, registry->retired[i].start, starts, nstarts, use, use_n)) {
            if (w != i) registry->retired[w] = registry->retired[i];
            w++;
        } else {
            free(registry->retired[i].start);
            free(registry->retired[i].bridge);
        }
    }
    registry->retired_count = w;
    free(owned);
}
