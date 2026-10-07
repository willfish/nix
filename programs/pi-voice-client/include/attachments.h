#ifndef PI_VOICE_ATTACHMENTS_H
#define PI_VOICE_ATTACHMENTS_H

#include <stddef.h>
#include <stdint.h>

#define HEARTBEAT_SECONDS 5
#define LEASE_SECONDS 15
/* Reserve room for JSON escaping (up to six bytes per character) and identity. */
#define MAX_RETAINED_BYTES ((256 * 1024 - 8192) / 6)

typedef struct {
    const char *socket;
    uint64_t socket_device;
    uint64_t socket_inode;
    const char *pane;
    int pid;
    const char *start;
    const char *bridge_id;
} AttachmentTarget;

typedef struct {
    const char *socket_path;
    uint64_t socket_device;
    uint64_t socket_inode;
    const char *pane_id;
} PaneKey;

typedef struct {
    PaneKey pane;
    int pid;
    const char *process_start;
    const char *bridge_id;
} BridgeIdentity;

typedef struct {
    const char *state;
    char token[128];
    int has_token;
    char replaced[128];
    int has_replaced;
} Admission;

typedef struct {
    const char *bridge_id;
    int has_activation;
    int64_t activation;
    int has_pid;
    int64_t pid;
    const char *session;
    const char *harness;
} EventView;

typedef struct {
    int pid;
    const char *process_start;
    int64_t ordinal;
    const char *bridge_id;
} FenceHighwater;

typedef struct {
    int pid;
    const char *process_start;
    const char *bridge_id;
} FenceRetired;

typedef struct {
    FenceHighwater *highwater;
    size_t highwater_count;
    FenceRetired *retired;
    size_t retired_count;
} Fences;

typedef struct {
    int pid;
    const char *start; /* NULL means the incarnation is gone. */
} ObservedStart;

typedef struct {
    int pid;
    const char *start;
} CapturedIncarnation;

typedef struct AttachmentRegistry AttachmentRegistry;

int can_auto_select(int selection_initialized, const char *selected_token, int ready, int team_child);
PaneKey pane_key(const AttachmentTarget *target);
BridgeIdentity bridge_identity(const AttachmentTarget *target);
int identity_equal(const BridgeIdentity *a, const BridgeIdentity *b);

/* lstat identity. Does not follow a final symlink. Returns 0 on success. */
int socket_instance(const char *path, char *canonical, size_t cap, uint64_t *dev, uint64_t *ino);

/* Returns 0 and writes lstat device/inode, or -1 if the endpoint is invalid. */
int validate_endpoint(const char *runtime, const char *path, int pid, const char *bridge_id,
    uint64_t *dev, uint64_t *ino);

int event_matches(const EventView *target, const EventView *event, int session);

/* /proc/$pid/stat start ticks. 0 on success. Does not check uid. */
int process_start_ticks(int pid, char *out, size_t cap);
/* 1 when /proc/$pid is owned by the caller uid. */
int process_uid_matches(int pid);
/* 1 only when start ticks and caller uid still match this incarnation. */
int process_still_same(int pid, const char *start_ticks);

/* Test seam. Production leaves this disabled and uses getuid(). */
void attachments_set_uid_override(int enabled, unsigned uid);

AttachmentRegistry *registry_new(void);
void registry_free(AttachmentRegistry *registry);
int64_t registry_revision(const AttachmentRegistry *registry, const PaneKey *pane);
const char *registry_lookup(const AttachmentRegistry *registry, const BridgeIdentity *identity);
/* 0 and fills out, or -1 if the activation ordinal is already owned. */
int registry_admit(AttachmentRegistry *registry, const BridgeIdentity *identity,
    int64_t activation, int64_t revision, const char *token, Admission *out);
void registry_remove(AttachmentRegistry *registry, const char *token, int retire);
int registry_fences(const AttachmentRegistry *registry, Fences *out);
void fences_free(Fences *fences);
int registry_restore_fences(AttachmentRegistry *registry, const Fences *saved);
/* has_captured == 0 snapshots current fence keys before pruning. */
void registry_prune(AttachmentRegistry *registry, const ObservedStart *starts, size_t nstarts,
    const CapturedIncarnation *captured, size_t ncaptured, int has_captured);

#endif
