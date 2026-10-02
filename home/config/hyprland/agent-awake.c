#define _XOPEN_SOURCE 700

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef SYSTEMD_INHIBIT
#define SYSTEMD_INHIBIT "systemd-inhibit"
#endif
#ifndef SLEEP_BIN
#define SLEEP_BIN "sleep"
#endif

#define REFRESH_MS 30000
#define LIST_TIMEOUT_MS 5000
#define RETRY_MS 2000
#define STOP_WAIT_MS 2000
#define LINE_MAX (1024 * 1024)

static volatile sig_atomic_t stopping = 0;

static void on_stop(int sig)
{
    (void)sig;
    stopping = 1;
}

static void log_msg(const char *text)
{
    fprintf(stderr, "herdr-agent-awake: %s\n", text);
    fflush(stderr);
}

static void log_unavailable(const char *reason)
{
    fprintf(stderr, "herdr-agent-awake: herdr unavailable (%s); sleep allowed\n",
            reason && reason[0] ? reason : "unknown error");
    fflush(stderr);
}

static void sleep_ms(int ms)
{
    while (ms > 0 && !stopping) {
        struct timespec ts;
        int step = ms > 100 ? 100 : ms;
        ts.tv_sec = step / 1000;
        ts.tv_nsec = (long)(step % 1000) * 1000000L;
        nanosleep(&ts, NULL);
        ms -= step;
    }
}

/* --- JSON ---------------------------------------------------------------- */

typedef enum {
    J_BAD,
    J_NULL,
    J_BOOL,
    J_NUM,
    J_STR,
    J_ARR,
    J_OBJ
} JType;

typedef struct JVal JVal;
typedef struct JMember JMember;

struct JMember {
    char *key;
    JVal *val;
    JMember *next;
};

struct JVal {
    JType type;
    char *str;
    int boolean;
    JVal *item;
    JVal *next;
    JMember *member;
};

static void j_free(JVal *v);

static void j_free_member(JMember *m)
{
    while (m) {
        JMember *next = m->next;
        free(m->key);
        j_free(m->val);
        free(m);
        m = next;
    }
}

static void j_free(JVal *v)
{
    if (!v)
        return;
    free(v->str);
    j_free(v->item);
    j_free(v->next);
    j_free_member(v->member);
    free(v);
}

static JVal *j_new(JType type)
{
    JVal *v = calloc(1, sizeof(*v));
    if (v)
        v->type = type;
    return v;
}

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int utf8_put(unsigned cp, char *out)
{
    if (cp <= 0x7F) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp <= 0xFFFF) {
        if (cp >= 0xD800 && cp <= 0xDFFF)
            return -1;
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp > 0x10FFFF)
        return -1;
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int parse_string(const char **pp, const char *end, char **out)
{
    const char *p = *pp;
    char *buf;
    size_t cap = 32, len = 0;
    if (p >= end || *p != '"')
        return -1;
    p++;
    buf = malloc(cap);
    if (!buf)
        return -1;
    while (p < end && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        char tmp[4];
        int n = 1;
        if (c == '\\') {
            if (p >= end) {
                free(buf);
                return -1;
            }
            c = (unsigned char)*p++;
            switch (c) {
            case '"':
            case '\\':
            case '/':
                tmp[0] = (char)c;
                break;
            case 'b':
                tmp[0] = '\b';
                break;
            case 'f':
                tmp[0] = '\f';
                break;
            case 'n':
                tmp[0] = '\n';
                break;
            case 'r':
                tmp[0] = '\r';
                break;
            case 't':
                tmp[0] = '\t';
                break;
            case 'u': {
                unsigned cp = 0;
                int i;
                for (i = 0; i < 4; i++) {
                    int h;
                    if (p >= end || (h = hex_value(*p++)) < 0) {
                        free(buf);
                        return -1;
                    }
                    cp = (cp << 4) | (unsigned)h;
                }
                n = utf8_put(cp, tmp);
                if (n < 0) {
                    free(buf);
                    return -1;
                }
                break;
            }
            default:
                free(buf);
                return -1;
            }
        } else if (c < 0x20) {
            free(buf);
            return -1;
        } else {
            tmp[0] = (char)c;
        }
        if (len + (size_t)n + 1 > cap) {
            char *grown;
            while (len + (size_t)n + 1 > cap)
                cap *= 2;
            grown = realloc(buf, cap);
            if (!grown) {
                free(buf);
                return -1;
            }
            buf = grown;
        }
        memcpy(buf + len, tmp, (size_t)n);
        len += (size_t)n;
    }
    if (p >= end || *p != '"') {
        free(buf);
        return -1;
    }
    buf[len] = '\0';
    *out = buf;
    *pp = p + 1;
    return 0;
}

static JVal *parse_value(const char **pp, const char *end);

static JVal *parse_object(const char **pp, const char *end)
{
    const char *p = *pp;
    JVal *obj = j_new(J_OBJ);
    JMember **tail;
    if (!obj)
        return NULL;
    if (p >= end || *p != '{') {
        j_free(obj);
        return NULL;
    }
    p = skip_ws(p + 1, end);
    tail = &obj->member;
    if (p < end && *p == '}') {
        *pp = p + 1;
        return obj;
    }
    for (;;) {
        char *key = NULL;
        JVal *val;
        JMember *member;
        p = skip_ws(p, end);
        if (parse_string(&p, end, &key) != 0) {
            j_free(obj);
            return NULL;
        }
        p = skip_ws(p, end);
        if (p >= end || *p != ':') {
            free(key);
            j_free(obj);
            return NULL;
        }
        p = skip_ws(p + 1, end);
        val = parse_value(&p, end);
        if (!val) {
            free(key);
            j_free(obj);
            return NULL;
        }
        member = calloc(1, sizeof(*member));
        if (!member) {
            free(key);
            j_free(val);
            j_free(obj);
            return NULL;
        }
        member->key = key;
        member->val = val;
        *tail = member;
        tail = &member->next;
        p = skip_ws(p, end);
        if (p < end && *p == ',') {
            p++;
            continue;
        }
        if (p < end && *p == '}') {
            *pp = p + 1;
            return obj;
        }
        j_free(obj);
        return NULL;
    }
}

static JVal *parse_array(const char **pp, const char *end)
{
    const char *p = *pp;
    JVal *arr = j_new(J_ARR);
    JVal **tail;
    if (!arr)
        return NULL;
    if (p >= end || *p != '[') {
        j_free(arr);
        return NULL;
    }
    p = skip_ws(p + 1, end);
    tail = &arr->item;
    if (p < end && *p == ']') {
        *pp = p + 1;
        return arr;
    }
    for (;;) {
        JVal *item = parse_value(&p, end);
        if (!item) {
            j_free(arr);
            return NULL;
        }
        *tail = item;
        tail = &item->next;
        p = skip_ws(p, end);
        if (p < end && *p == ',') {
            p = skip_ws(p + 1, end);
            continue;
        }
        if (p < end && *p == ']') {
            *pp = p + 1;
            return arr;
        }
        j_free(arr);
        return NULL;
    }
}

static JVal *parse_number(const char **pp, const char *end)
{
    const char *p = *pp;
    const char *start = p;
    JVal *v;
    if (p < end && *p == '-')
        p++;
    if (p >= end || !isdigit((unsigned char)*p))
        return NULL;
    if (*p == '0') {
        p++;
    } else {
        while (p < end && isdigit((unsigned char)*p))
            p++;
    }
    if (p < end && *p == '.') {
        p++;
        if (p >= end || !isdigit((unsigned char)*p))
            return NULL;
        while (p < end && isdigit((unsigned char)*p))
            p++;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < end && (*p == '+' || *p == '-'))
            p++;
        if (p >= end || !isdigit((unsigned char)*p))
            return NULL;
        while (p < end && isdigit((unsigned char)*p))
            p++;
    }
    v = j_new(J_NUM);
    if (!v)
        return NULL;
    v->str = strndup(start, (size_t)(p - start));
    if (!v->str) {
        j_free(v);
        return NULL;
    }
    *pp = p;
    return v;
}

static JVal *parse_literal(const char **pp, const char *end, const char *lit, JType type, int boolean)
{
    size_t n = strlen(lit);
    JVal *v;
    if ((size_t)(end - *pp) < n || memcmp(*pp, lit, n) != 0)
        return NULL;
    v = j_new(type);
    if (!v)
        return NULL;
    v->boolean = boolean;
    *pp += n;
    return v;
}

static JVal *parse_value(const char **pp, const char *end)
{
    const char *p = skip_ws(*pp, end);
    JVal *v = NULL;
    if (p >= end)
        return NULL;
    if (*p == '"') {
        char *s = NULL;
        if (parse_string(&p, end, &s) != 0)
            return NULL;
        v = j_new(J_STR);
        if (!v) {
            free(s);
            return NULL;
        }
        v->str = s;
    } else if (*p == '{') {
        v = parse_object(&p, end);
    } else if (*p == '[') {
        v = parse_array(&p, end);
    } else if (*p == 't') {
        v = parse_literal(&p, end, "true", J_BOOL, 1);
    } else if (*p == 'f') {
        v = parse_literal(&p, end, "false", J_BOOL, 0);
    } else if (*p == 'n') {
        v = parse_literal(&p, end, "null", J_NULL, 0);
    } else if (*p == '-' || isdigit((unsigned char)*p)) {
        v = parse_number(&p, end);
    }
    if (!v)
        return NULL;
    *pp = p;
    return v;
}

static JVal *parse_json(const char *text)
{
    const char *p = text;
    const char *end = text + strlen(text);
    JVal *v = parse_value(&p, end);
    if (!v)
        return NULL;
    p = skip_ws(p, end);
    if (p != end) {
        j_free(v);
        return NULL;
    }
    return v;
}

static const JVal *object_get(const JVal *obj, const char *key)
{
    JMember *m;
    if (!obj || obj->type != J_OBJ)
        return NULL;
    for (m = obj->member; m; m = m->next) {
        if (strcmp(m->key, key) == 0)
            return m->val;
    }
    return NULL;
}

static int object_has(const JVal *obj, const char *key)
{
    JMember *m;
    if (!obj || obj->type != J_OBJ)
        return 0;
    for (m = obj->member; m; m = m->next) {
        if (strcmp(m->key, key) == 0)
            return 1;
    }
    return 0;
}

static const char *string_field(const JVal *obj, const char *key)
{
    const JVal *v = object_get(obj, key);
    if (!v || v->type != J_STR)
        return NULL;
    return v->str;
}

/* --- pane table ---------------------------------------------------------- */

typedef struct {
    char *id;
    char *status;
} Pane;

typedef struct {
    Pane *items;
    size_t len;
    size_t cap;
} Panes;

static void panes_clear(Panes *panes)
{
    size_t i;
    for (i = 0; i < panes->len; i++) {
        free(panes->items[i].id);
        free(panes->items[i].status);
    }
    panes->len = 0;
}

static void panes_free(Panes *panes)
{
    panes_clear(panes);
    free(panes->items);
    panes->items = NULL;
    panes->cap = 0;
}

static int panes_set(Panes *panes, const char *id, const char *status)
{
    size_t i;
    char *copy_id, *copy_status;
    for (i = 0; i < panes->len; i++) {
        if (strcmp(panes->items[i].id, id) == 0) {
            copy_status = strdup(status);
            if (!copy_status)
                return -1;
            free(panes->items[i].status);
            panes->items[i].status = copy_status;
            return 0;
        }
    }
    if (panes->len == panes->cap) {
        size_t cap = panes->cap ? panes->cap * 2 : 8;
        Pane *grown = realloc(panes->items, cap * sizeof(*grown));
        if (!grown)
            return -1;
        panes->items = grown;
        panes->cap = cap;
    }
    copy_id = strdup(id);
    copy_status = strdup(status);
    if (!copy_id || !copy_status) {
        free(copy_id);
        free(copy_status);
        return -1;
    }
    panes->items[panes->len].id = copy_id;
    panes->items[panes->len].status = copy_status;
    panes->len++;
    return 0;
}

static void panes_remove(Panes *panes, const char *id)
{
    size_t i;
    for (i = 0; i < panes->len; i++) {
        if (strcmp(panes->items[i].id, id) != 0)
            continue;
        free(panes->items[i].id);
        free(panes->items[i].status);
        memmove(&panes->items[i], &panes->items[i + 1],
                (panes->len - i - 1) * sizeof(panes->items[0]));
        panes->len--;
        return;
    }
}

static int panes_busy(const Panes *panes)
{
    size_t i;
    for (i = 0; i < panes->len; i++) {
        if (strcmp(panes->items[i].status, "working") == 0)
            return 1;
    }
    return 0;
}

static int nonempty(const char *s)
{
    return s && s[0];
}

static int panes_from_agents(Panes *panes, const JVal *agents)
{
    const JVal *agent;
    panes_clear(panes);
    if (!agents || agents->type != J_ARR)
        return -1;
    for (agent = agents->item; agent; agent = agent->next) {
        const char *id, *status;
        if (agent->type != J_OBJ)
            continue;
        id = string_field(agent, "pane_id");
        status = string_field(agent, "agent_status");
        if (!nonempty(id) || !nonempty(status))
            continue;
        if (panes_set(panes, id, status) != 0)
            return -1;
    }
    return 0;
}

/* status, refresh, or ignore. */
static const char *apply_message(Panes *panes, const JVal *msg)
{
    const JVal *data, *payload;
    const char *event, *pane_id, *status;
    if (!msg || msg->type != J_OBJ)
        return "ignore";
    if (object_has(msg, "error"))
        return "refresh";
    event = string_field(msg, "event");
    data = object_get(msg, "data");
    payload = (data && data->type == J_OBJ) ? data : NULL;
    pane_id = payload ? string_field(payload, "pane_id") : NULL;
    status = payload ? string_field(payload, "agent_status") : NULL;
    if (event && (strcmp(event, "pane.agent_status_changed") == 0 ||
                  strcmp(event, "pane_agent_status_changed") == 0)) {
        if (nonempty(pane_id) && nonempty(status)) {
            if (panes_set(panes, pane_id, status) != 0)
                return "refresh";
            return "status";
        }
        return "refresh";
    }
    if (event && (strcmp(event, "pane_closed") == 0 || strcmp(event, "pane_exited") == 0)) {
        if (nonempty(pane_id))
            panes_remove(panes, pane_id);
        return "refresh";
    }
    if (event && (strcmp(event, "pane_created") == 0 || strcmp(event, "pane_agent_detected") == 0))
        return "refresh";
    return "ignore";
}

static int append_text(char **buf, size_t *len, size_t *cap, const char *text)
{
    size_t n = strlen(text);
    if (*len + n + 1 > *cap) {
        size_t next = *cap ? *cap : 128;
        char *grown;
        while (*len + n + 1 > next)
            next *= 2;
        grown = realloc(*buf, next);
        if (!grown)
            return -1;
        *buf = grown;
        *cap = next;
    }
    memcpy(*buf + *len, text, n + 1);
    *len += n;
    return 0;
}

static int append_json_string(char **buf, size_t *len, size_t *cap, const char *text)
{
    const unsigned char *p;
    if (append_text(buf, len, cap, "\"") != 0)
        return -1;
    for (p = (const unsigned char *)text; *p; p++) {
        char tmp[8];
        if (*p == '"' || *p == '\\')
            snprintf(tmp, sizeof tmp, "\\%c", *p);
        else if (*p < 0x20)
            snprintf(tmp, sizeof tmp, "\\u%04x", *p);
        else
            snprintf(tmp, sizeof tmp, "%c", *p);
        if (append_text(buf, len, cap, tmp) != 0)
            return -1;
    }
    return append_text(buf, len, cap, "\"");
}

static char *subscription_json(const Panes *panes)
{
    static const char *lifecycle[] = {
        "pane.created",
        "pane.closed",
        "pane.exited",
        "pane.agent_detected",
    };
    char *buf = NULL;
    size_t len = 0, cap = 0;
    size_t i;
    if (append_text(&buf, &len, &cap,
                    "{\"id\":\"awake-subscribe\",\"method\":\"events.subscribe\",\"params\":{\"subscriptions\":[") != 0)
        return NULL;
    for (i = 0; i < 4; i++) {
        if (i && append_text(&buf, &len, &cap, ",") != 0) {
            free(buf);
            return NULL;
        }
        if (append_text(&buf, &len, &cap, "{\"type\":") != 0 ||
            append_json_string(&buf, &len, &cap, lifecycle[i]) != 0 ||
            append_text(&buf, &len, &cap, "}") != 0) {
            free(buf);
            return NULL;
        }
    }
    for (i = 0; i < panes->len; i++) {
        if (append_text(&buf, &len, &cap, ",{\"type\":\"pane.agent_status_changed\",\"pane_id\":") != 0 ||
            append_json_string(&buf, &len, &cap, panes->items[i].id) != 0 ||
            append_text(&buf, &len, &cap, "}") != 0) {
            free(buf);
            return NULL;
        }
    }
    if (append_text(&buf, &len, &cap, "]}}\n") != 0) {
        free(buf);
        return NULL;
    }
    return buf;
}

/* --- inhibitor ----------------------------------------------------------- */

static pid_t inhibit_pid = -1;

static int child_alive(pid_t pid)
{
    int status;
    pid_t got;
    if (pid <= 0)
        return 0;
    do {
        got = waitpid(pid, &status, WNOHANG);
    } while (got < 0 && errno == EINTR && !stopping);
    return got == 0;
}

static void wait_child(pid_t pid, int timeout_ms)
{
    int waited = 0;
    while (child_alive(pid) && waited < timeout_ms && !stopping) {
        sleep_ms(50);
        waited += 50;
    }
}

static void stop_group(pid_t pid)
{
    if (!child_alive(pid))
        return;
    if (killpg(pid, SIGTERM) != 0)
        kill(pid, SIGTERM);
    wait_child(pid, STOP_WAIT_MS);
    if (!child_alive(pid))
        return;
    if (killpg(pid, SIGKILL) != 0)
        kill(pid, SIGKILL);
    wait_child(pid, STOP_WAIT_MS);
    if (child_alive(pid))
        waitpid(pid, NULL, WNOHANG);
}

static pid_t spawn_group(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        sigset_t set;
        sigemptyset(&set);
        sigprocmask(SIG_SETMASK, &set, NULL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        if (setsid() < 0)
            _exit(127);
        execvp(argv[0], argv);
        _exit(127);
    }
    return pid;
}

static int start_inhibitor(void)
{
    char *argv[] = {
        (char *)SYSTEMD_INHIBIT,
        "--what=idle:sleep",
        "--mode=block",
        "--who=herdr-agents",
        "--why=Herdr agent working",
        (char *)SLEEP_BIN,
        "infinity",
        NULL,
    };
    pid_t pid;
    if (child_alive(inhibit_pid))
        return 0;
    inhibit_pid = -1;
    pid = spawn_group(argv);
    if (pid < 0)
        return -1;
    inhibit_pid = pid;
    return 0;
}

static void set_busy(int active)
{
    if (active) {
        if (start_inhibitor() != 0)
            log_msg("could not start systemd-inhibit");
        return;
    }
    stop_group(inhibit_pid);
    inhibit_pid = -1;
}

/* --- socket -------------------------------------------------------------- */

static int set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static int connect_unix(const char *path, int timeout_ms)
{
    struct sockaddr_un addr;
    int fd, flags, rc;
    if (strlen(path) >= sizeof addr.sun_path) {
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (set_cloexec(fd) != 0) {
        close(fd);
        return -1;
    }
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, strlen(path) + 1);
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return -1;
    }
    rc = connect(fd, (struct sockaddr *)&addr, sizeof addr);
    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    if (rc < 0) {
        struct pollfd pfd = {.fd = fd, .events = POLLOUT};
        int pr;
        do {
            pr = poll(&pfd, 1, timeout_ms);
        } while (pr < 0 && errno == EINTR && !stopping);
        if (pr <= 0) {
            close(fd);
            errno = pr == 0 ? ETIMEDOUT : errno;
            return -1;
        }
        {
            int err = 0;
            socklen_t len = sizeof err;
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err) {
                close(fd);
                errno = err ? err : errno;
                return -1;
            }
        }
    }
    if (fcntl(fd, F_SETFL, flags) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int write_all(int fd, const char *buf, size_t len, int timeout_ms)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n;
        if (stopping)
            return -1;
        n = write(fd, buf + off, len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd = {.fd = fd, .events = POLLOUT};
            int pr = poll(&pfd, 1, timeout_ms);
            if (pr == 0) {
                errno = ETIMEDOUT;
                return -1;
            }
            if (pr < 0 && errno == EINTR)
                continue;
            if (pr < 0)
                return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

typedef struct {
    int fd;
    char *buf;
    size_t len;
    size_t cap;
} Reader;

static void reader_close(Reader *r)
{
    if (r->fd >= 0)
        close(r->fd);
    free(r->buf);
    r->fd = -1;
    r->buf = NULL;
    r->len = 0;
    r->cap = 0;
}

/* 0 = line, 1 = timeout, -1 = error. Timeout discards a partial line. */
static int reader_line(Reader *r, char **out, int timeout_ms)
{
    for (;;) {
        char *nl = memchr(r->buf, '\n', r->len);
        if (nl) {
            size_t n = (size_t)(nl - r->buf);
            size_t rest = r->len - n - 1;
            char *line = malloc(n + 1);
            if (!line)
                return -1;
            memcpy(line, r->buf, n);
            line[n] = '\0';
            memmove(r->buf, nl + 1, rest);
            r->len = rest;
            *out = line;
            return 0;
        }
        {
            struct pollfd pfd = {.fd = r->fd, .events = POLLIN};
            int pr;
            char tmp[4096];
            ssize_t n;
            if (stopping)
                return -1;
            do {
                pr = poll(&pfd, 1, timeout_ms);
            } while (pr < 0 && errno == EINTR && !stopping);
            if (pr == 0) {
                r->len = 0;
                return 1;
            }
            if (pr < 0)
                return -1;
            n = read(r->fd, tmp, sizeof tmp);
            if (n == 0) {
                errno = ECONNRESET;
                return -1;
            }
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            if (r->len + (size_t)n + 1 > LINE_MAX) {
                errno = EMSGSIZE;
                return -1;
            }
            if (r->len + (size_t)n + 1 > r->cap) {
                size_t next = r->cap ? r->cap : 256;
                char *grown;
                while (r->len + (size_t)n + 1 > next)
                    next *= 2;
                grown = realloc(r->buf, next);
                if (!grown)
                    return -1;
                r->buf = grown;
                r->cap = next;
            }
            memcpy(r->buf + r->len, tmp, (size_t)n);
            r->len += (size_t)n;
        }
    }
}

static int reason_from_errno(char *reason, size_t reason_len)
{
    if (errno == ENOENT)
        snprintf(reason, reason_len, "no such socket");
    else if (errno == ETIMEDOUT)
        snprintf(reason, reason_len, "timed out");
    else if (errno == ECONNRESET)
        snprintf(reason, reason_len, "herdr closed the socket");
    else
        snprintf(reason, reason_len, "%s", strerror(errno));
    return -1;
}

static int take_object(Reader *r, int timeout_ms, JVal **out, char *reason, size_t reason_len)
{
    char *line = NULL;
    int rc = reader_line(r, &line, timeout_ms);
    if (rc == 1) {
        snprintf(reason, reason_len, "timed out");
        return -1;
    }
    if (rc != 0)
        return reason_from_errno(reason, reason_len);
    *out = parse_json(line);
    free(line);
    if (!*out) {
        snprintf(reason, reason_len, "invalid JSON");
        return -1;
    }
    if ((*out)->type != J_OBJ) {
        j_free(*out);
        *out = NULL;
        snprintf(reason, reason_len, "herdr returned a non-object response");
        return -1;
    }
    if (object_has(*out, "error")) {
        const JVal *err = object_get(*out, "error");
        const char *detail = NULL;
        if (err && err->type == J_OBJ)
            detail = string_field(err, "message");
        else if (err && err->type == J_STR)
            detail = err->str;
        snprintf(reason, reason_len, "herdr request failed: %s", detail ? detail : "error");
        j_free(*out);
        *out = NULL;
        return -1;
    }
    return 0;
}

static int request_json(const char *path, const char *payload, int timeout_ms, JVal **out, char *reason, size_t reason_len)
{
    Reader reader = {.fd = connect_unix(path, timeout_ms)};
    int rc;
    if (reader.fd < 0)
        return reason_from_errno(reason, reason_len);
    if (write_all(reader.fd, payload, strlen(payload), timeout_ms) != 0) {
        reason_from_errno(reason, reason_len);
        reader_close(&reader);
        return -1;
    }
    rc = take_object(&reader, timeout_ms, out, reason, reason_len);
    reader_close(&reader);
    return rc;
}

static int list_agents(const char *path, Panes *panes, char *reason, size_t reason_len)
{
    JVal *msg = NULL;
    const JVal *result, *agents;
    int rc = request_json(path,
                          "{\"id\":\"awake-list\",\"method\":\"agent.list\",\"params\":{}}\n",
                          LIST_TIMEOUT_MS, &msg, reason, reason_len);
    if (rc != 0)
        return -1;
    result = object_get(msg, "result");
    if (!result || result->type != J_OBJ) {
        j_free(msg);
        snprintf(reason, reason_len, "herdr agent.list returned no result");
        return -1;
    }
    agents = object_get(result, "agents");
    if (!agents || agents->type != J_ARR) {
        j_free(msg);
        snprintf(reason, reason_len, "herdr agent.list returned no agents");
        return -1;
    }
    rc = panes_from_agents(panes, agents);
    j_free(msg);
    if (rc != 0) {
        snprintf(reason, reason_len, "out of memory");
        return -1;
    }
    return 0;
}

/* 0 = cycle finished, -1 = fail open. */
static int watch_subscription(const char *path, Panes *panes, char *reason, size_t reason_len)
{
    char *payload = subscription_json(panes);
    Reader reader = {.fd = -1};
    JVal *started = NULL;
    int rc;
    if (!payload) {
        snprintf(reason, reason_len, "out of memory");
        return -1;
    }
    reader.fd = connect_unix(path, REFRESH_MS);
    if (reader.fd < 0) {
        free(payload);
        return reason_from_errno(reason, reason_len);
    }
    if (write_all(reader.fd, payload, strlen(payload), REFRESH_MS) != 0) {
        reason_from_errno(reason, reason_len);
        free(payload);
        reader_close(&reader);
        return -1;
    }
    free(payload);
    rc = take_object(&reader, REFRESH_MS, &started, reason, reason_len);
    if (rc != 0) {
        reader_close(&reader);
        if (strstr(reason, "herdr request failed") == reason)
            snprintf(reason, reason_len, "herdr subscription failed");
        return -1;
    }
    j_free(started);
    for (;;) {
        char *line = NULL;
        JVal *msg;
        const char *action;
        rc = reader_line(&reader, &line, REFRESH_MS);
        if (rc == 1) {
            reader_close(&reader);
            return 0;
        }
        if (rc != 0) {
            reason_from_errno(reason, reason_len);
            reader_close(&reader);
            return -1;
        }
        msg = parse_json(line);
        free(line);
        if (!msg) {
            reader_close(&reader);
            snprintf(reason, reason_len, "invalid JSON");
            return -1;
        }
        if (msg->type != J_OBJ) {
            j_free(msg);
            continue;
        }
        action = apply_message(panes, msg);
        j_free(msg);
        if (strcmp(action, "status") == 0) {
            set_busy(panes_busy(panes));
            continue;
        }
        if (strcmp(action, "refresh") == 0) {
            set_busy(panes_busy(panes));
            reader_close(&reader);
            return 0;
        }
    }
}

static int watch_once(const char *path, Panes *panes, char *reason, size_t reason_len)
{
    panes_clear(panes);
    if (list_agents(path, panes, reason, reason_len) != 0)
        return -1;
    set_busy(panes_busy(panes));
    return watch_subscription(path, panes, reason, reason_len);
}

static int socket_path(char *out, size_t out_len)
{
    const char *override = getenv("HERDR_SOCKET_PATH");
    const char *config_home = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    int n;
    if (override && override[0]) {
        n = snprintf(out, out_len, "%s", override);
    } else if (config_home && config_home[0]) {
        n = snprintf(out, out_len, "%s/herdr/herdr.sock", config_home);
    } else if (home && home[0]) {
        n = snprintf(out, out_len, "%s/.config/herdr/herdr.sock", home);
    } else {
        return -1;
    }
    return n > 0 && (size_t)n < out_len ? 0 : -1;
}

static int install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGTERM, &sa, NULL) != 0)
        return -1;
    if (sigaction(SIGINT, &sa, NULL) != 0)
        return -1;
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

static int run(const char *path)
{
    Panes panes = {0};
    char reason[256];
    while (!stopping) {
        reason[0] = '\0';
        if (watch_once(path, &panes, reason, sizeof reason) != 0) {
            set_busy(0);
            if (stopping)
                break;
            log_unavailable(reason);
            sleep_ms(RETRY_MS);
        }
    }
    set_busy(0);
    panes_free(&panes);
    return 0;
}

/* --- self-test ----------------------------------------------------------- */

static int g_failures = 0;

static void expect_true(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "self-test: %s\n", msg);
        g_failures++;
    }
}

static void test_logic(void)
{
    Panes panes = {0};
    JVal *msg;
    const char *action;
    char *sub;
    JVal *parsed, *subs;
    const JVal *item;
    int i;

    msg = parse_json("{\"result\":{\"agents\":["
                     "{\"pane_id\":\"w1:p1\",\"agent\":\"pi\",\"agent_status\":\"blocked\"},"
                     "{\"pane_id\":\"w2:p1\",\"agent\":\"codex\",\"agent_status\":\"done\"},"
                     "{\"agent_status\":\"working\"},"
                     "{\"pane_id\":\"w3:p1\"}"
                     "]}}");
    expect_true(msg != NULL, "parse agent list");
    expect_true(panes_from_agents(&panes, object_get(object_get(msg, "result"), "agents")) == 0,
                "load agents");
    j_free(msg);
    expect_true(panes.len == 2, "skipped incomplete agents");
    expect_true(!panes_busy(&panes), "blocked and done are not busy");

    panes_clear(&panes);
    expect_true(panes_set(&panes, "blocked", "blocked") == 0, "set blocked");
    expect_true(panes_set(&panes, "idle", "idle") == 0, "set idle");
    expect_true(panes_set(&panes, "done", "done") == 0, "set done");
    expect_true(panes_set(&panes, "unknown", "unknown") == 0, "set unknown");
    expect_true(!panes_busy(&panes), "only working holds the machine");
    expect_true(panes_set(&panes, "two", "working") == 0, "set working");
    expect_true(panes_busy(&panes), "one working pane holds the machine");

    panes_clear(&panes);
    expect_true(panes_set(&panes, "w1:p1", "idle") == 0, "seed idle");
    msg = parse_json("{\"event\":\"pane.agent_status_changed\",\"data\":{"
                     "\"pane_id\":\"w1:p1\",\"agent_status\":\"working\",\"agent\":\"pi\"}}");
    action = apply_message(&panes, msg);
    j_free(msg);
    expect_true(strcmp(action, "status") == 0, "status event does not refresh");
    expect_true(panes_busy(&panes), "status event can start the inhibitor");

    panes_clear(&panes);
    msg = parse_json("{\"event\":\"pane_agent_status_changed\",\"data\":{"
                     "\"type\":\"pane_agent_status_changed\",\"pane_id\":\"w9:p1\","
                     "\"agent_status\":\"working\"}}");
    action = apply_message(&panes, msg);
    j_free(msg);
    expect_true(strcmp(action, "status") == 0, "generic status event is accepted");
    expect_true(panes.len == 1 && strcmp(panes.items[0].status, "working") == 0,
                "generic status is stored");

    panes_clear(&panes);
    expect_true(panes_set(&panes, "w1:p1", "working") == 0, "seed working");
    msg = parse_json("{\"event\":\"pane_closed\",\"data\":{\"type\":\"pane_closed\",\"pane_id\":\"w1:p1\"}}");
    action = apply_message(&panes, msg);
    j_free(msg);
    expect_true(strcmp(action, "refresh") == 0, "close refreshes");
    expect_true(!panes_busy(&panes) && panes.len == 0, "closed pane is removed");

    msg = parse_json("{\"event\":\"pane_agent_detected\",\"data\":{}}");
    expect_true(strcmp(apply_message(&panes, msg), "refresh") == 0, "detected pane refreshes");
    j_free(msg);
    msg = parse_json("{\"event\":\"pane_created\",\"data\":{}}");
    expect_true(strcmp(apply_message(&panes, msg), "refresh") == 0, "created pane refreshes");
    j_free(msg);
    msg = parse_json("{\"event\":\"pane.agent_status_changed\",\"data\":{\"pane_id\":\"\",\"agent_status\":\"working\"}}");
    expect_true(strcmp(apply_message(&panes, msg), "refresh") == 0, "empty pane id refreshes");
    j_free(msg);
    msg = parse_json("{\"error\":{\"message\":\"nope\"}}");
    expect_true(strcmp(apply_message(&panes, msg), "refresh") == 0, "error field refreshes");
    j_free(msg);
    msg = parse_json("{\"event\":\"unrelated\"}");
    expect_true(strcmp(apply_message(&panes, msg), "ignore") == 0, "unknown event is ignored");
    j_free(msg);

    panes_clear(&panes);
    expect_true(panes_set(&panes, "w1:p1", "idle") == 0, "sub pane 1");
    expect_true(panes_set(&panes, "w2:p1", "done") == 0, "sub pane 2");
    sub = subscription_json(&panes);
    expect_true(sub != NULL, "subscription builds");
    parsed = sub ? parse_json(sub) : NULL;
    free(sub);
    subs = parsed ? (JVal *)object_get(object_get(parsed, "params"), "subscriptions") : NULL;
    expect_true(subs && subs->type == J_ARR, "subscriptions array");
    item = subs ? subs->item : NULL;
    for (i = 0; i < 4 && item; i++)
        item = item->next;
    expect_true(item && item->next && !item->next->next, "two pane subscriptions");
    if (subs && subs->item) {
        const char *types[] = {
            "pane.created", "pane.closed", "pane.exited", "pane.agent_detected",
            "pane.agent_status_changed", "pane.agent_status_changed",
        };
        const JVal *cur = subs->item;
        for (i = 0; i < 6; i++) {
            expect_true(cur && strcmp(string_field(cur, "type") ? string_field(cur, "type") : "", types[i]) == 0,
                        "subscription type order");
            if (cur)
                cur = cur->next;
        }
        cur = subs->item;
        for (i = 0; i < 5 && cur; i++)
            cur = cur->next;
        expect_true(cur && strcmp(string_field(cur, "pane_id") ? string_field(cur, "pane_id") : "", "w2:p1") == 0,
                    "last subscription is the second pane");
    }
    j_free(parsed);
    panes_free(&panes);
}

static void test_inhibitor(void)
{
    char *argv[] = {"sleep", "30", NULL};
    pid_t first = spawn_group(argv);
    pid_t second;
    expect_true(first > 0 && child_alive(first), "process group starts");
    second = spawn_group(argv);
    expect_true(second > 0 && second != first, "a second group is a new pid");
    stop_group(first);
    expect_true(!child_alive(first), "process group stops");
    stop_group(first);
    expect_true(!child_alive(first), "stopping twice is harmless");
    stop_group(second);
    expect_true(!child_alive(second), "second group stops");
}

static int self_test(void)
{
    test_logic();
    test_inhibitor();
    if (g_failures) {
        fprintf(stderr, "self-test: %d failure%s\n", g_failures, g_failures == 1 ? "" : "s");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    if (argc == 2 && strcmp(argv[1], "--self-test") == 0)
        return self_test();
    if (argc != 1) {
        fprintf(stderr, "usage: herdr-agent-awake [--self-test]\n");
        return 2;
    }
    if (install_signals() != 0) {
        log_msg("could not install signal handlers");
        return 1;
    }
    if (socket_path(path, sizeof path) != 0) {
        log_unavailable("socket path is too long or HOME is unset");
        return 1;
    }
    return run(path);
}
