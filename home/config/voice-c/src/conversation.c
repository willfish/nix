#ifdef VOICE_CONVERSATION_MAIN
#define voice_conversation_new voice_conversation_new_exe
#define voice_conversation_free voice_conversation_free_exe
#define voice_conversation_active voice_conversation_active_exe
#define voice_conversation_start voice_conversation_start_exe
#define voice_conversation_open voice_conversation_open_exe
#define voice_conversation_stop voice_conversation_stop_exe
#define voice_conversation_menu voice_conversation_menu_exe
#define voice_can_switch voice_can_switch_exe
#define voice_open_when_ready voice_open_when_ready_exe
#define voice_personaplex_probe voice_personaplex_probe_exe
#endif
#include "conversation.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ipc.h"

#ifdef VOICE_CONVERSATION_MAIN
#include "presentation.h"
#include "runtime_adapters.h"
#include <yyjson.h>
#endif

struct VoiceConversation {
    voice_proc_fn run;
    void *user;
};

static char *xstrdup(const char *text) {
    size_t n = strlen(text ? text : "") + 1;
    char *copy = malloc(n);
    if (!copy) return NULL;
    memcpy(copy, text ? text : "", n);
    return copy;
}

static int fail(char **error, const char *message) {
    if (error && !*error) *error = xstrdup(message ? message : "PersonaPlex failed");
    return -1;
}

static void trim(char *text) {
    char *start;
    size_t n;
    if (!text) return;
    start = text;
    while (*start && isspace((unsigned char)*start)) start++;
    n = strlen(start);
    while (n && isspace((unsigned char)start[n - 1])) n--;
    start[n] = 0;
    if (start != text) memmove(text, start, n + 1);
}

static int default_proc(void *user, const char *const *argv, int timeout_sec, char **stdout_text, char **error) {
    int argc = 0;
    ipc_process_request request;
    ipc_process_result result;
    char err[256];
    int rc;
    (void)user;
    while (argv && argv[argc]) argc++;
    memset(&request, 0, sizeof request);
    request.argv = argv;
    request.argc = argc;
    request.capture_stdout = 1;
    request.capture_stderr = 1;
    request.deadline_ms = timeout_sec > 0 ? timeout_sec * 1000 : 1000;
    if (request.deadline_ms > 120000) request.deadline_ms = 120000;
    rc = ipc_process_run(&request, &result, err, sizeof err);
    if (rc != IPC_OK || result.exit_code != 0) {
        char detail[512];
        if (result.stderr_bytes && result.stderr_len) {
            size_t n = result.stderr_len < 200 ? result.stderr_len : 200;
            memcpy(detail, result.stderr_bytes, n);
            detail[n] = 0;
            trim(detail);
        } else snprintf(detail, sizeof detail, "%s", err[0] ? err : "systemctl failed");
        ipc_process_result_free(&result);
        return fail(error, detail[0] ? detail : "systemctl failed");
    }
    if (stdout_text) {
        *stdout_text = calloc(result.stdout_len + 1, 1);
        if (!*stdout_text) {
            ipc_process_result_free(&result);
            return fail(error, "Out of memory");
        }
        if (result.stdout_bytes) memcpy(*stdout_text, result.stdout_bytes, result.stdout_len);
    }
    ipc_process_result_free(&result);
    return 0;
}

VoiceConversation *voice_conversation_new(voice_proc_fn run, void *user) {
    VoiceConversation *conversation = calloc(1, sizeof *conversation);
    if (!conversation) return NULL;
    conversation->run = run ? run : default_proc;
    conversation->user = user;
    return conversation;
}

void voice_conversation_free(VoiceConversation *conversation) {
    free(conversation);
}

static int run_proc(VoiceConversation *conversation, const char *const *argv, int timeout, char **stdout_text, char **error) {
    if (!conversation || !conversation->run) return fail(error, "PersonaPlex runner is unavailable");
    return conversation->run(conversation->user, argv, timeout, stdout_text, error);
}

int voice_conversation_active(VoiceConversation *conversation, char **error) {
    char *stdout_text = NULL;
    const char *argv[] = {
        "systemctl", "--user", "show", "personaplex.service", "--property=ActiveState", "--value", NULL
    };
    int rc = run_proc(conversation, argv, 20, &stdout_text, error);
    int active = 0;
    if (rc) return -1;
    trim(stdout_text);
    active = stdout_text && (strcmp(stdout_text, "active") == 0 || strcmp(stdout_text, "activating") == 0
        || strcmp(stdout_text, "deactivating") == 0);
    free(stdout_text);
    return active;
}

int voice_conversation_start(VoiceConversation *conversation, char **error) {
    const char *argv[] = {"systemctl", "--user", "start", "--no-block", "personaplex.service", NULL};
    if (run_proc(conversation, argv, 20, NULL, error)) return -1;
    return voice_conversation_open(conversation, error);
}

int voice_conversation_open(VoiceConversation *conversation, char **error) {
    const char *argv[] = {"systemctl", "--user", "restart", "--no-block", "personaplex-open.service", NULL};
    return run_proc(conversation, argv, 20, NULL, error);
}

int voice_conversation_stop(VoiceConversation *conversation, char **error) {
    const char *stop[] = {
        "systemctl", "--user", "stop", "personaplex-open.service", "personaplex.service", NULL
    };
    const char *start[] = {
        "systemctl", "--user", "start", "pi-voice.service", "pi-voice-osd.service", NULL
    };
    if (run_proc(conversation, stop, 20, NULL, error)) return -1;
    return run_proc(conversation, start, 20, NULL, error);
}

int voice_conversation_menu(VoiceConversation *conversation, voice_pick_fn pick, void *pick_user, char **error) {
    VoiceMenuRow rows[2];
    char *action = NULL;
    int picked;
    if (!pick) return fail(error, "PersonaPlex menu needs a picker");
    rows[0].action = (char *)"open";
    rows[0].label = (char *)"Open PersonaPlex conversation (experimental)";
    rows[1].action = (char *)"pi";
    rows[1].label = (char *)"Switch back to Pi dictation and playback";
    picked = pick(pick_user, "Voice mode: PersonaPlex | separate from Pi", rows, 2, &action, error);
    if (picked < 0) return -1;
    if (picked > 0 || !action) return 0;
    if (strcmp(action, "pi") == 0) {
        free(action);
        return voice_conversation_stop(conversation, error);
    }
    if (strcmp(action, "open") == 0) {
        free(action);
        return voice_conversation_open(conversation, error);
    }
    free(action);
    return fail(error, "Invalid conversation action");
}

int voice_can_switch(const VoiceMenuStatus *status) {
    const char *phase = status && status->phase && status->phase[0] ? status->phase : "idle";
    if (status && status->phase && !status->phase[0]) return 0;
    return strcmp(phase, "idle") == 0
        && !(status && status->retained)
        && !(status && status->preparing)
        && !(status && status->retry)
        && !(status && status->pending)
        && !(status && status->draft);
}

int voice_open_when_ready(const VoiceReadyHooks *hooks, void *user, double timeout_sec, char **error) {
    double deadline;
    if (!hooks || !hooks->monotonic || !hooks->active || !hooks->probe || !hooks->open_url)
        return fail(error, "PersonaPlex readiness hooks are incomplete");
    deadline = hooks->monotonic(user) + timeout_sec;
    while (hooks->monotonic(user) < deadline) {
        int active = hooks->active(user, error);
        int probe;
        if (active < 0) return -1;
        if (!active) return fail(error, "PersonaPlex stopped or failed; check its service journal");
        probe = hooks->probe(user, error);
        if (probe < 0) return -1;
        if (probe) return hooks->open_url(user, VOICE_PERSONAPLEX_URL, error);
        if (hooks->sleep_sec) hooks->sleep_sec(user, 1);
    }
    return fail(error, "PersonaPlex did not become ready within four minutes");
}

int voice_personaplex_probe(char **error) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in addr;
    struct timeval tv;
    char request[] = "GET / HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    char buf[20000];
    size_t used = 0;
    char *body;
    int status = 0;
    (void)error;
    if (fd < 0) return 0;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8998);
    addr.sin_addr.s_addr = htonl(0x7f000001);
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return 0;
    }
    if (send(fd, request, sizeof request - 1, 0) < 0) {
        close(fd);
        return 0;
    }
    while (used + 1 < sizeof buf) {
        ssize_t n = recv(fd, buf + used, sizeof buf - 1 - used, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
    }
    close(fd);
    buf[used] = 0;
    if (sscanf(buf, "HTTP/%*s %d", &status) != 1 || status != 200) return 0;
    body = strstr(buf, "\r\n\r\n");
    if (!body) return 0;
    body += 4;
    if ((size_t)(buf + used - body) > 16384) body[16384] = 0;
    return strstr(body, "<title>PersonaPlex</title>") != NULL;
}

#ifdef VOICE_CONVERSATION_MAIN
static double real_now(void *user) {
    struct timespec ts;
    (void)user;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void real_sleep(void *user, double seconds) {
    struct timespec ts;
    (void)user;
    if (seconds < 0) seconds = 0;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1000000000.0);
    nanosleep(&ts, NULL);
}

static int real_active(void *user, char **error) { return voice_conversation_active(user, error); }
static int real_probe(void *user, char **error) { (void)user; return voice_personaplex_probe(error); }

static int real_open(void *user, const char *url, char **error) {
    const char *argv[] = {"xdg-open", url, NULL};
    return run_proc(user, argv, 15, NULL, error);
}

static void desktop_notice(const char *title, const char *detail) {
    char combined[512];
    char text[256];
    char runtime[512];
    char err[256];
    snprintf(combined, sizeof combined, "%s%s%s", title ? title : "PersonaPlex",
        detail && detail[0] ? ": " : "", detail ? detail : "");
    if (voice_public_label(combined, 160, text, sizeof text)) snprintf(text, sizeof text, "PersonaPlex unavailable");
    fprintf(stderr, "Pi voice: %s\n", text);
    fflush(stderr);
    if (voice_runtime_dir_resolve(runtime, sizeof runtime, err, sizeof err) == 0) {
        char path[640], tmp[656];
        FILE *file;
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        char *json;
        snprintf(path, sizeof path, "%s/osd-notice.json", runtime);
        snprintf(tmp, sizeof tmp, "%s/osd-notice.json.tmp", runtime);
        if (doc) {
            yyjson_mut_val *root = yyjson_mut_obj(doc);
            yyjson_mut_doc_set_root(doc, root);
            yyjson_mut_obj_add_strcpy(doc, root, "message", text);
            yyjson_mut_obj_add_strcpy(doc, root, "tone", "red");
            yyjson_mut_obj_add_real(doc, root, "until", (double)time(NULL) + 8.0);
            json = yyjson_mut_write(doc, 0, NULL);
            yyjson_mut_doc_free(doc);
            file = json ? fopen(tmp, "w") : NULL;
            if (file) {
                fputs(json, file);
                fclose(file);
                chmod(tmp, 0600);
                rename(tmp, path);
            }
            free(json);
        }
    }
}

int main(void) {
    VoiceConversation *conversation = voice_conversation_new(NULL, NULL);
    VoiceReadyHooks hooks = {
        .active = real_active,
        .probe = real_probe,
        .open_url = real_open,
        .sleep_sec = real_sleep,
        .monotonic = real_now,
    };
    char *error = NULL;
    if (voice_open_when_ready(&hooks, conversation, 240, &error)) {
        desktop_notice("PersonaPlex unavailable", error ? error : "");
        free(error);
        voice_conversation_free(conversation);
        return 1;
    }
    voice_conversation_free(conversation);
    return 0;
}
#endif
