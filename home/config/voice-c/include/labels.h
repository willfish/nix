#ifndef PI_VOICE_LABELS_H
#define PI_VOICE_LABELS_H

#include <stddef.h>
#include <stdint.h>

/* Presentation labels for Pi panes. Rendering must not refresh Herdr. */

#define LABELS_MAX_KEYS 64
#define LABELS_MAX_WORKERS 8
#define LABELS_REFRESH_SEC 3.0
#define LABELS_AUTHORITY_SEC 3.0
#define LABELS_SNAPSHOT_TIMEOUT 1.0
#define LABELS_SHORT_BUDGET 55

enum {
    LABELS_OK = 0,
    LABELS_ERR = -1,
    LABELS_TOO_MANY = 1,
    LABELS_CLOSED = 2,
    LABELS_INVALID = 3
};

enum {
    LABELS_OUTCOME_UNKNOWN = 0,
    LABELS_OUTCOME_OK = 1,
    LABELS_OUTCOME_UNAVAILABLE = 2
};

typedef struct {
    char *path;
    uint64_t device;
    uint64_t inode;
} SocketKey;

typedef struct LabelsCache LabelsCache;
typedef struct SnapshotState SnapshotState;

typedef int (*labels_runner_fn)(const SocketKey *key, double timeout, char **snapshot_json, void *user);
typedef double (*labels_clock_fn)(void *user);
typedef int (*labels_submit_fn)(void (*fn)(void *arg), void *arg, void *sched);
typedef int (*labels_stat_fn)(const char *path, uint64_t *device, uint64_t *inode, void *user);
typedef int (*labels_exec_fn)(const char *socket_path, double timeout, char **stdout_text, void *user);

typedef struct {
    labels_runner_fn runner;
    void *runner_user;
    labels_clock_fn clock;
    void *clock_user;
    labels_submit_fn submit;
    void *sched;
    int max_keys;
} LabelsCacheConfig;

typedef struct {
    char *harness;
    char *pane;
    char *token;
    char *id;
    char *label;
    char *full_label;
    char *thinking;
    char *model;
    char *model_id;
    char *model_name;
    int has_model;
    int model_is_dict;
    int selected;
    int team_child;
    int has_socket_key;
    int label_present;
    int full_label_present;
    SocketKey socket_key;
} LabelEntry;

int socket_key_init(SocketKey *key, const char *path, uint64_t device, uint64_t inode);
void socket_key_clear(SocketKey *key);
int socket_key_equal(const SocketKey *a, const SocketKey *b);

LabelsCache *labels_cache_new(const LabelsCacheConfig *cfg);
void labels_cache_close(LabelsCache *cache);
/* Destroys the cache. The caller must keep an injected submit scheduler
   quiescent: no refresh callback may run once free begins. A callback after
   that is a use-after-free. The owned pool, used when submit is NULL, is
   joined before the cache is destroyed. */
void labels_cache_free(LabelsCache *cache);
int labels_cache_set_active(LabelsCache *cache, const SocketKey *keys, size_t count);
SnapshotState *labels_cache_read(LabelsCache *cache, const SocketKey *key);
int labels_cache_refresh(LabelsCache *cache, const SocketKey *key);

SnapshotState *labels_parse_snapshot(const char *json);
void labels_state_free(SnapshotState *state);
int labels_state_outcome(const SnapshotState *state);
int labels_state_inflight(const SnapshotState *state);
double labels_state_fetched_at(const SnapshotState *state);
double labels_state_attempted_at(const SnapshotState *state);
int labels_state_authoritative(const SnapshotState *state, double now);
size_t labels_state_pane_count(const SnapshotState *state);
const char *labels_state_pane_id(const SnapshotState *state, size_t index);
const char *labels_state_field(const SnapshotState *state, const char *pane_id, const char *field);
int labels_state_set_field(SnapshotState *state, const char *pane_id, const char *field, const char *value);

int labels_run_snapshot(const SocketKey *key, double timeout, char **snapshot_json);
void labels_set_stat_fn(labels_stat_fn fn, void *user);
void labels_set_exec_fn(labels_exec_fn fn, void *user);
const char *labels_last_error(void);

int labels_display_width(const char *text);
int labels_build(const LabelEntry *in, size_t count,
    const SocketKey *snap_keys, SnapshotState *const *snap_states, size_t snap_count,
    LabelEntry **out, size_t *out_count);
void labels_entry_clear(LabelEntry *entry);
void labels_entries_free(LabelEntry *entries, size_t count);

int test_labels(void);

#endif
