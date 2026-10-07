#ifndef PI_VOICE_RUNTIME_ADAPTERS_H
#define PI_VOICE_RUNTIME_ADAPTERS_H

#include "audio.h"
#include "devices.h"
#include "ipc.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <yyjson.h>

/* Outcomes match Python RuntimeError versus DeliveryUncertain.
 * UNCERTAIN means a mutation may have landed. Callers must not retry it.
 * Adapters never fall back to keystrokes and never replay an uncertain call.
 */

enum voice_outcome {
    VOICE_OK = 0,
    VOICE_REJECTED = 1,
    VOICE_UNCERTAIN = 2,
    VOICE_CANCELLED = 3
};

typedef struct voice_target voice_target;
typedef struct voice_pi voice_pi;
typedef struct voice_herdr voice_herdr;
typedef struct voice_terminal voice_terminal;
typedef struct voice_config voice_config;
typedef struct voice_audio_binding voice_audio_binding;

typedef struct voice_incarnation {
    int present;
    char start[64];
    int has_adapter;
    uint64_t adapter_device;
    uint64_t adapter_inode;
    int has_socket;
    uint64_t socket_device;
    uint64_t socket_inode;
} voice_incarnation;

typedef struct voice_target_fields {
    const char *socket;
    const char *pane;
    const char *session;
    const char *token;
    const char *adapter_socket;
    const char *bridge_id;
    const char *harness;
    const char *start;
    int pid;
    int managed;
    int has_activation;
    int64_t activation;
    int has_adapter_identity;
    uint64_t adapter_device;
    uint64_t adapter_inode;
    int has_socket_identity;
    uint64_t socket_device;
    uint64_t socket_inode;
} voice_target_fields;

typedef int (*voice_process_fn)(
    const ipc_process_request *request, ipc_process_result *result, void *user,
    char *err, size_t err_cap);

typedef struct voice_process_hook {
    voice_process_fn fn;
    void *user;
} voice_process_hook;

voice_target *voice_target_new(const voice_target_fields *fields);
/* Copies an object doc. Strings are bounded. NULL on invalid input. */
voice_target *voice_target_from_doc(const yyjson_doc *doc, char *err, size_t err_cap);
void voice_target_free(voice_target *target);
const voice_target_fields *voice_target_get_fields(const voice_target *target);

/* 0 and *out filled from the live process and socket identities. */
int voice_incarnation_observe(const voice_target *target, voice_incarnation *out, char *err, size_t err_cap);
int voice_incarnation_matches(const voice_incarnation *expected, const voice_incarnation *observed);

/* XDG_RUNTIME_DIR/pi-voice, or /run/user/<uid>/pi-voice. Creates mode 0700.
 * Rejects a symlink, foreign owner, or group/world-accessible directory.
 */
int voice_runtime_dir_resolve(char *out, size_t cap, char *err, size_t err_cap);

/* Owns every string and voice table referenced by voice_config_audio. */
voice_config *voice_config_parse(const yyjson_doc *doc, char *err, size_t err_cap);
void voice_config_free(voice_config *config);
const audio_config *voice_config_audio(const voice_config *config);
int voice_config_auto_speak(const voice_config *config);

/* Wires microphone status/resolve and capture. Unbind after audio_drain.
 * no_services uses an in-process microphone runner instead of pw-dump.
 */
voice_audio_binding *voice_audio_bind(
    audio *audio, int no_services, char *err, size_t err_cap);
/* runner NULL selects the bounded pw-dump runner, or an in-process empty dump when no_services. */
voice_audio_binding *voice_audio_bind_with_runner(
    audio *audio, int no_services,
    MicRunner runner, void *runner_user, char *err, size_t err_cap);
void voice_audio_unbind(voice_audio_binding *binding);

voice_pi *voice_pi_new(void);
void voice_pi_free(voice_pi *pi);
/* fields is an optional object merged into the request. Not consumed.
 * *result is an owned object doc on VOICE_OK. Mutation commands that lose an
 * acknowledgement return VOICE_UNCERTAIN and do not retry.
 */
int voice_pi_request(
    voice_pi *pi, const voice_target *target, const char *command, const yyjson_doc *fields,
    yyjson_doc **result, char *err, size_t err_cap);
int voice_pi_validate_target(
    voice_pi *pi, const voice_target *target, yyjson_doc **status, char *err, size_t err_cap);
int voice_pi_validate(voice_pi *pi, const voice_target *target, char *err, size_t err_cap);
int voice_pi_insert_guarded(
    voice_pi *pi, const voice_target *target, const char *text, atomic_int *cancelled,
    char *err, size_t err_cap);
int voice_pi_submit_guarded(
    voice_pi *pi, const voice_target *target, atomic_int *cancelled, int allow_edited,
    char *err, size_t err_cap);

voice_herdr *voice_herdr_new(const char *argv0);
void voice_herdr_free(voice_herdr *herdr);
void voice_herdr_set_process(voice_herdr *herdr, voice_process_fn fn, void *user);
/* args are the herdr CLI arguments, not including argv0. Read-only commands
 * time out as VOICE_REJECTED. pane.send_input is not used here.
 * *result is the owned inner result object on VOICE_OK.
 */
int voice_herdr_request(
    voice_herdr *herdr, const voice_target *target, const char *const *args, int argc,
    int mutation, yyjson_doc **result, char *err, size_t err_cap);
/* pane.send_input only. keys is always empty. Never send_text, never retry. */
int voice_herdr_input(
    voice_herdr *herdr, const voice_target *target, const char *text,
    char *err, size_t err_cap);
int voice_herdr_validate_target(
    voice_herdr *herdr, const voice_target *target, yyjson_doc **agent,
    char *err, size_t err_cap);
int voice_herdr_validate(voice_herdr *herdr, const voice_target *target, char *err, size_t err_cap);
int voice_herdr_insert_guarded(
    voice_herdr *herdr, const voice_target *target, const char *text, atomic_int *cancelled,
    char *err, size_t err_cap);
int voice_herdr_submit_guarded(
    voice_herdr *herdr, const voice_target *target, atomic_int *cancelled, int allow_edited,
    char *err, size_t err_cap);

/* Pi-only delivery, matching AgentTerminal. Herdr remains available separately. */
voice_terminal *voice_terminal_new(void);
void voice_terminal_free(voice_terminal *terminal);
voice_pi *voice_terminal_pi(voice_terminal *terminal);
voice_herdr *voice_terminal_herdr(voice_terminal *terminal);
void voice_terminal_set_herdr_argv0(voice_terminal *terminal, const char *argv0);
int voice_terminal_validate_target(
    voice_terminal *terminal, const voice_target *target, yyjson_doc **status,
    char *err, size_t err_cap);
int voice_terminal_validate(voice_terminal *terminal, const voice_target *target, char *err, size_t err_cap);
/* state is idle, working, blocked, or unknown. */
int voice_terminal_activity(
    voice_terminal *terminal, const voice_target *target, char *state, size_t state_cap,
    char *err, size_t err_cap);
int voice_terminal_insert_guarded(
    voice_terminal *terminal, const voice_target *target, const char *text, atomic_int *cancelled,
    char *err, size_t err_cap);
int voice_terminal_submit_guarded(
    voice_terminal *terminal, const voice_target *target, atomic_int *cancelled, int allow_edited,
    char *err, size_t err_cap);

int test_runtime_adapters(void);

#endif
