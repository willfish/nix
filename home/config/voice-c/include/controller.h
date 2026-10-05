#ifndef PI_VOICE_CONTROLLER_H
#define PI_VOICE_CONTROLLER_H

/* Native voice controller. dispatch returns an owned JSON envelope.
 * Target snapshots are immutable. Completion checks operation generation
 * and target identity. State and delivery locks are separate. Socket and
 * process I/O stay outside the state lock.
 *
 * Expected adapter symbols, published by include/runtime_adapters.h or
 * audio.h. Do not duplicate them here. Production calls them when present:
 *
 *   int audio_transcribe_owned(audio *audio, const char *path,
 *       atomic_int *cancelled, audio_drained_fn on_drained, void *drain_user,
 *       void (*destroy_ctx)(void *ctx), char *out, size_t out_cap,
 *       char *err, size_t err_cap);
 *
 * destroy_ctx runs exactly once: before return on the waiting path, or from
 * the HTTP worker after an abandoned call returns. Production passes it.
 *
 * harness.h is expected to provide the Pi/Herdr client used by main when the
 * header is present. ipc.h is expected to provide the private control server
 * when present. Missing headers use the injected deps and the private server
 * in main.c; they are not a second public adapter API.
 */

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define CTRL_OK 0
#define CTRL_ERR (-1)
#define CTRL_UNCERTAIN (-2)
#define CTRL_CANCELLED (-3)
#define CTRL_BUSY (-4)

typedef struct voice_controller voice_controller;

typedef struct controller_target_view {
    const char *pane;
    const char *socket_path;
    int has_socket_instance;
    uint64_t socket_device;
    uint64_t socket_inode;
    int pid;
    const char *start;
    const char *harness;
    const char *session;
    int has_session;
    const char *bridge_id;
    int has_activation;
    int64_t activation;
    int team_child;
    int has_team_child;
    const char *model;
    const char *thinking;
    const char *adapter_socket;
    int has_adapter;
    uint64_t adapter_device;
    uint64_t adapter_inode;
    int managed;
    const char *token;
} controller_target_view;

typedef struct controller_capture controller_capture;
struct controller_capture {
    void *user;
    int (*wait_ready)(controller_capture *cap, double timeout);
    /* 0 and *code set, -1 timeout or failure. */
    int (*wait)(controller_capture *cap, double timeout, int *code);
    /* 0 still running, 1 exited and *code set, -1 error. */
    int (*poll)(controller_capture *cap, int *code);
    void (*close)(controller_capture *cap);
    /* Optional. 0 and one chunk, 1 if no chunk ready, -1 error. */
    int (*drain_chunk)(controller_capture *cap, int16_t **samples, size_t *count);
    int (*wait_progress)(controller_capture *cap, double timeout);
    const char *(*error)(controller_capture *cap);
    double (*level)(controller_capture *cap);
    int (*clipping)(controller_capture *cap);
    void (*destroy)(controller_capture *cap);
};

typedef struct controller_terminal {
    void *user;
    int (*validate_target)(void *user, const controller_target_view *target, char *err, size_t err_cap);
    int (*validate)(void *user, const controller_target_view *target, char *err, size_t err_cap);
    /* 0 and writes status/draft. 1 if no snapshot method. -1 error. */
    int (*activity_snapshot)(void *user, const controller_target_view *target,
        char *agent_status, size_t status_cap, char *draft_state, size_t draft_cap,
        char *err, size_t err_cap);
    int (*activity)(void *user, const controller_target_view *target, char *state, size_t state_cap, char *err, size_t err_cap);
    int (*insert_guarded)(void *user, const controller_target_view *target, const char *text,
        atomic_int *cancelled, char *err, size_t err_cap);
    int (*submit_guarded)(void *user, const controller_target_view *target, atomic_int *cancelled,
        int allow_edited, char *err, size_t err_cap);
} controller_terminal;

typedef struct controller_audio {
    void *user;
    /* Owned JSON object, or NULL fields when absent. */
    int (*status_json)(void *user, char **json);
    void (*stop)(void *user);
    int (*cue)(void *user, int frequency, char *err, size_t err_cap);
    controller_capture *(*start_capture)(void *user, const char *path, char *err, size_t err_cap);
    /* See audio_transcribe_owned contract above. */
    int (*transcribe_owned)(void *user, const char *path, atomic_int *cancelled,
        void (*on_drained)(const char *text, void *drain), void (*release)(void *drain), void *drain,
        char *out, size_t out_cap, char *err, size_t err_cap);
    int (*speak)(void *user, const char *text, atomic_int *cancelled, char *err, size_t err_cap);
    int (*set_voice)(void *user, const char *id, char *err, size_t err_cap);
    int (*set_speech_backend)(void *user, const char *id, char *err, size_t err_cap);
    int (*set_stt_backend)(void *user, const char *id, char *err, size_t err_cap);
    const char *(*speech_backend)(void *user);
    const char *(*stt_backend)(void *user);
    int (*playing)(void *user);
} controller_audio;

typedef struct controller_herdr {
    void *user;
    /* argv is NULL-terminated without the program name. *result_json is the
     * Herdr "result" object, owned by the caller. */
    int (*request)(void *user, const controller_target_view *target, const char *const *argv,
        char **result_json, char *err, size_t err_cap);
} controller_herdr;

typedef struct controller_engines {
    void *user;
    void (*set_population)(void *user, int sessions, int pending);
    int (*acquire)(void *user, const char *engine, void **lease, char *err, size_t err_cap);
    void (*release)(void *lease);
    /* 0 ready, 1 timeout, 2 cancelled, -1 error. */
    int (*wait)(void *lease, int timeout_ms, char *err, size_t err_cap);
    int (*failed)(void *lease);
    void (*ensure_resident)(void *user, const char *engine);
    void (*retire)(void *user, const char *engine);
    void (*warm)(void *user);
    void (*sweep)(void *user);
    int (*has_tts)(void *user);
    void (*close)(void *user);
} controller_engines;

typedef struct controller_catalogue {
    void *user;
    int max_keys;
    int (*set_active)(void *user, const char *const *paths, const uint64_t *devices,
        const uint64_t *inodes, size_t count);
    int (*refresh)(void *user, const char *path, uint64_t device, uint64_t inode);
    /* 1 authoritative. *pane_ids owned, count entries, each malloc'd. */
    int (*read_panes)(void *user, const char *path, uint64_t device, uint64_t inode,
        int *authoritative, char ***pane_ids, size_t *count);
    void (*close)(void *user);
} controller_catalogue;

typedef struct controller_deps {
    controller_terminal terminal;
    controller_audio audio;
    controller_herdr herdr;
    controller_engines engines;
    controller_catalogue catalogue;
    int has_engines;
    int has_catalogue;
    int speech_available;
    int auto_speak;
    double (*clock)(void *clock_user);
    void *clock_user;
    int (*copy_text)(void *copy_user, const char *text, char *err, size_t err_cap);
    void *copy_user;
} controller_deps;

voice_controller *controller_create(const char *runtime_dir, const controller_deps *deps);
int controller_restore(voice_controller *app);
/* Owned full response envelope. Never NULL except allocation failure. */
char *controller_dispatch(voice_controller *app, const char *request_json);
int controller_monitor(voice_controller *app);
void controller_shutdown(voice_controller *app);
void controller_free(voice_controller *app);
void controller_mark_audible(voice_controller *app);

/* 0 bound runtime/control.sock mode 0600. Rejects symlinks and foreign paths. */
int voice_bind_control(const char *runtime, int *listen_fd, char *err, size_t err_cap);

int voice_main(int argc, char **argv);
/* Launcher argv checks. Does not spawn. 0 and writes command pieces. */
int voice_launcher_command(const char *harness, int argc, char **argv, char *err, size_t err_cap);

#ifdef VOICE_CONTROLLER_INTERNAL

#include <pthread.h>
#include <yyjson.h>
#include "attachments.h"
#include "labels.h"

#define CTRL_MAX_SESSIONS 64
#define CTRL_MAX_TURNS 64
#define CTRL_MAX_EXCLUDED 32
#define CTRL_TOKEN 128
#define CTRL_PANE 4096
#define CTRL_PATH 4096
#define CTRL_START 64
#define CTRL_HARNESS 32
#define CTRL_SESSION 4096
#define CTRL_BRIDGE 4096
#define CTRL_MODEL 4096
#define CTRL_THINKING 64
#define CTRL_ID 128
#define CTRL_MSG 512
#define CTRL_PHASE 32
#define CTRL_WORKERS 6
#define CTRL_QUEUE 32

typedef struct target_data {
    char pane[CTRL_PANE];
    char socket[CTRL_PATH];
    int has_socket_instance;
    uint64_t socket_device;
    uint64_t socket_inode;
    int pid;
    int has_start;
    char start[CTRL_START];
    char harness[CTRL_HARNESS];
    int has_session;
    int session_null;
    char session[CTRL_SESSION];
    int has_bridge;
    char bridge_id[CTRL_BRIDGE];
    int has_activation;
    int64_t activation;
    int has_team_child;
    int team_child;
    int has_model;
    int model_is_dict;
    char model[CTRL_MODEL];
    char model_id[CTRL_MODEL];
    char model_name[CTRL_MODEL];
    int has_thinking;
    char thinking[CTRL_THINKING];
    int has_adapter;
    char adapter_socket[CTRL_PATH];
    uint64_t adapter_device;
    uint64_t adapter_inode;
    int managed;
    char token[CTRL_TOKEN];
} target_data;

typedef struct target_snap {
    atomic_int refs;
    target_data data;
} target_snap;

typedef struct op_state {
    atomic_int refs;
    uint64_t generation;
    atomic_int cancelled;
    int voice_target_lost;
    int recovery_revision;
    char delivery_outcome[16];
    void *engine_lease;
} op_state;

typedef struct retained_dictation {
    char *text;
    char source_token[CTRL_TOKEN];
    char source_session[CTRL_SESSION];
    char source_label[512];
} retained_dictation;

typedef struct session_entry {
    int used;
    char token[CTRL_TOKEN];
    target_snap *target;
    int has_thread;
    char thread[CTRL_SESSION];
    int has_candidate;
    char candidate[CTRL_SESSION];
    char **turns;
    size_t turn_count;
    size_t turn_cap;
    char excluded[CTRL_MAX_EXCLUDED][CTRL_ID];
    size_t excluded_count;
    char *pending;
    int draft;
    char *reply;
    int has_connection;
    char connection_state[CTRL_PHASE];
    double heartbeat_at;
    char agent_state[CTRL_PHASE];
    int activity_revision;
    int draft_revision;
    int has_active_turn;
    char active_turn[CTRL_ID];
    int has_activity_checked;
    double activity_checked;
    char draft_state[CTRL_PHASE];
    int has_socket_key;
    SocketKey socket_key;
    int activity_inflight;
} session_entry;

typedef struct ctrl_job {
    void (*fn)(void *);
    void *arg;
} ctrl_job;

typedef struct drain_hold {
    atomic_int refs;
    atomic_int released;
    void (*on_drained)(const char *text, void *user);
    void *user;
    struct drain_hold *next;
} drain_hold;

struct voice_controller {
    char *runtime;
    controller_deps deps;
    pthread_mutex_t state;
    pthread_mutex_t delivery;
    int delivery_held;
    target_snap *target;
    char token[CTRL_TOKEN];
    int has_token;
    session_entry sessions[CTRL_MAX_SESSIONS];
    int draft;
    op_state *input_op;
    op_state *record_op;
    op_state *speak_op;
    char *reply;
    char *pending;
    int auto_read;
    int speech_available;
    int speaking;
    int audible;
    controller_capture *capture;
    void *capture_owner;
    char phase[CTRL_PHASE];
    char *error;
    double record_started;
    int has_record_started;
    void *retry_lease;
    char *retry_path;
    target_snap *retry_target;
    char retry_token[CTRL_TOKEN];
    double retry_deadline;
    int has_retry;
    char *retry_previous;
    char retry_mode[16];
    char *active_retry_path;
    op_state *active_retry_op;
    double active_retry_deadline;
    int has_active_retry;
    int send_when_idle;
    AttachmentRegistry *attachments;
    int selection_initialized;
    int selection_explicit;
    int has_reconnect;
    PaneKey reconnect_pane;
    char reconnect_path[CTRL_PATH];
    char reconnect_pane_id[CTRL_PANE];
    int reconnect_explicit;
    retained_dictation *retained;
    int recovery_revision;
    int recovery_inflight;
    int recovery_uncertain;
    int pending_admissions;
    atomic_int attachments_closed;
    int show_team;
    char *recording_label;
    double osd_until;
    char *osd_message;
    char osd_tone[16];
    int has_osd_tone;
    uint64_t next_generation;
    char **turns;
    size_t turn_count;
    size_t turn_cap;
    int has_thread;
    char thread[CTRL_SESSION];
    pthread_t workers[CTRL_WORKERS];
    ctrl_job queue[CTRL_QUEUE];
    int q_head, q_tail, q_count, pool_stop, pool_started;
    pthread_mutex_t pool_mu;
    pthread_cond_t pool_cv;
    drain_hold *drains;
    pthread_mutex_t drain_mu;
};

double controller_now(voice_controller *app);
void controller_notice(voice_controller *app, const char *title, const char *detail, const char *tone);
void controller_report_error(voice_controller *app, const char *error);
void controller_request_osd(voice_controller *app, const char *message, double seconds, const char *tone);
int controller_recording_active(const voice_controller *app);
void controller_stop(voice_controller *app, int discard, int target_lost);
void controller_save_selection(voice_controller *app);
session_entry *controller_find(voice_controller *app, const char *token);
session_entry *session_slot_pub(voice_controller *app);
int controller_has_turn(char **turns, size_t count, const char *id);
int controller_add_turn(char ***turns, size_t *count, size_t *cap, const char *id);
void controller_clear_turns(char ***turns, size_t *count, size_t *cap);
int controller_copy_turns(char **src, size_t n, char ***dst, size_t *count, size_t *cap);
void controller_refresh_activity(voice_controller *app);
void *controller_capture_track(controller_capture *capture);
void controller_capture_publish(voice_controller *app, void *owner);
void *controller_capture_unpublish(voice_controller *app);
void controller_capture_release(void *owner);
target_snap *controller_snap(const target_data *data);
target_snap *controller_snap_retain(target_snap *snap);
void controller_snap_release(target_snap *snap);
void controller_view(const target_data *data, controller_target_view *out);
int controller_same_target(const target_data *a, const target_data *b);
int controller_same_attachment(const target_data *a, const target_data *b);
op_state *controller_op_new(voice_controller *app);
op_state *controller_op_retain(op_state *op);
void controller_op_release(op_state *op);
int controller_submit(voice_controller *app, void (*fn)(void *), void *arg);
void controller_population(voice_controller *app);
void controller_require_ready(voice_controller *app, const char *token, char *err, size_t cap);
int controller_remove_session(voice_controller *app, const char *token, int retire);
int controller_stage(voice_controller *app, const char *text, const char *token, op_state *cancelled, int finish, char *err, size_t cap);
void controller_delivery_outcome(voice_controller *app, const char *token, const char *text, op_state *operation, op_state *cancelled, const char *outcome);
int controller_retain(voice_controller *app, const char *text, const char *token, const target_data *target, op_state *cancelled);
char *controller_status_json(voice_controller *app);
int controller_parse_target(yyjson_val *obj, target_data *out, char *err, size_t cap);
char *controller_selection_json(voice_controller *app);
void controller_load_selected(voice_controller *app, const char *token, int explicit_sel);
void controller_remember(voice_controller *app);
int controller_validate_attachment(voice_controller *app, const target_data *incoming, target_data *out, char *err, size_t cap);
void controller_expire_retry(voice_controller *app, int force);
void controller_refresh_labels(voice_controller *app);
void controller_refresh_reconnect(voice_controller *app);
char *controller_attach_json(voice_controller *app, yyjson_val *incoming);
int controller_detach(voice_controller *app, const char *token, yyjson_val *identity);
int controller_harness_event(voice_controller *app, const char *token, yyjson_val *event);
int controller_notify(voice_controller *app, const char *token, yyjson_val *event);
int controller_register(voice_controller *app, const char *token, yyjson_val *target, char *err, size_t cap);
int controller_select(voice_controller *app, const char *token, char *err, size_t cap);
int controller_record(voice_controller *app, const char *mode, char *err, size_t cap);
int controller_send(voice_controller *app, const char *expected_token, uint64_t expected_gen, int allow_edited, char *err, size_t cap);
int controller_read(voice_controller *app, int replace, char *err, size_t cap);
int controller_retry(voice_controller *app, char *err, size_t cap);
int controller_rebind(voice_controller *app, char *err, size_t cap);
int controller_interact(voice_controller *app, char *err, size_t cap);
int controller_recover_copy(voice_controller *app, char *err, size_t cap);
int controller_recover_stage(voice_controller *app, char *err, size_t cap);
int controller_recover_discard(voice_controller *app, char *err, size_t cap);
void controller_set_activity(session_entry *entry, const char *state);

#endif

#endif
