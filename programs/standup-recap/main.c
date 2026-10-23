/*
 * standup-recap: fill "What I did yesterday" in today's daily note.
 *
 * A Pi print-mode run gathers the previous working day's GitHub, Slack,
 * Outlook mail and meetings and prints bullet lines between markers. This
 * program owns the dates, the run markers, the log, validation and the note
 * write; Pi never edits the note itself.
 *
 * All paths are user-owned configuration or the user's own notes, so lexical
 * path handling is intended.
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PROGRAM "standup-recap"
#define KEEP_RUNS 5
#define MAX_RUNS 64
#define MAX_OUTPUT (256 * 1024)
#define MAX_FILE (1024 * 1024)
#define MAX_BULLETS 25
#define MAX_BULLET_LEN 400
#define BEGIN_MARK "<<<STANDUP-RECAP"
#define END_MARK "STANDUP-RECAP>>>"
#define SECTION "What I did yesterday"

struct buf {
    char *p;
    size_t len, cap;
};

struct config {
    char provider[64];
    char model[128];
    char thinking[16];
    char pi_command[PATH_MAX];
    char notes_dir[PATH_MAX];
    char prompt_file[PATH_MAX];
    long timeout;
};

static int log_fd = -1;
static char run_dir[PATH_MAX];

static void logmsg(const char *fmt, ...)
{
    char msg[1536];
    char line[2048];
    char stamp[32];
    time_t now = time(NULL);
    struct tm tm;
    va_list ap;
    int n;

    if (localtime_r(&now, &tm) == NULL || strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm) == 0)
        snprintf(stamp, sizeof stamp, "?");
    va_start(ap, fmt);
    if (vsnprintf(msg, sizeof msg, fmt, ap) < 0)
        msg[0] = '\0';
    va_end(ap);
    n = snprintf(line, sizeof line, "%s %s\n", stamp, msg);
    if (n > 0 && log_fd >= 0 && write(log_fd, line, (size_t)n) < 0) {
        /* Logging is best effort; stderr still reaches the journal. */
    }
    fprintf(stderr, "%s: %s\n", PROGRAM, msg);
}

static bool touch(const char *name)
{
    char path[PATH_MAX];
    int fd;
    bool ok;

    if (snprintf(path, sizeof path, "%s/%s", run_dir, name) >= (int)sizeof path)
        return false;
    fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    ok = futimens(fd, NULL) == 0;
    return close(fd) == 0 && ok;
}

static void unlink_marker(const char *name)
{
    char path[PATH_MAX];

    if (snprintf(path, sizeof path, "%s/%s", run_dir, name) < (int)sizeof path && unlink(path) != 0 && errno != ENOENT)
        logmsg("could not remove stale %s marker: %s", name, strerror(errno));
}

static _Noreturn void fail(const char *fmt, ...)
{
    char msg[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    logmsg("FAILED: %s", msg);
    if (run_dir[0] != '\0' && !touch("failed"))
        logmsg("could not write failed marker");
    exit(1);
}

static void buf_add(struct buf *b, const char *s, size_t n)
{
    if (n > SIZE_MAX - b->len - 1)
        fail("buffer size overflow");
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        char *p;

        while (cap < b->len + n + 1) {
            if (cap > SIZE_MAX / 2)
                fail("buffer size overflow");
            cap *= 2;
        }
        p = realloc(b->p, cap);
        if (p == NULL)
            fail("out of memory");
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void buf_str(struct buf *b, const char *s)
{
    buf_add(b, s, strlen(s));
}

static bool read_file(const char *path, struct buf *out)
{
    char chunk[8192];
    ssize_t n;
    int fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return false;
    out->len = 0;
    buf_add(out, "", 0);
    while ((n = read(fd, chunk, sizeof chunk)) != 0) {
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return false;
        }
        if (out->len + (size_t)n > MAX_FILE) {
            close(fd);
            errno = EFBIG;
            return false;
        }
        buf_add(out, chunk, (size_t)n);
    }
    close(fd);
    return true;
}

static bool write_all(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);

        if (w < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        p += w;
        n -= (size_t)w;
    }
    return true;
}

/* Replace path with content via a same-directory temporary and rename. */
static bool write_atomic(const char *path, const char *content, size_t len, mode_t mode)
{
    char tmp[PATH_MAX];
    int fd;

    if (snprintf(tmp, sizeof tmp, "%s.XXXXXX", path) >= (int)sizeof tmp)
        return false;
    fd = mkstemp(tmp);
    if (fd < 0)
        return false;
    if (!write_all(fd, content, len) || fchmod(fd, mode) != 0 || fsync(fd) != 0) {
        close(fd);
        unlink(tmp);
        return false;
    }
    if (close(fd) != 0 || rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

static bool mkdirs(const char *path)
{
    char tmp[PATH_MAX];
    size_t n = strlen(path);

    if (n == 0 || n >= sizeof tmp)
        return false;
    memcpy(tmp, path, n + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
            return false;
        *p = '/';
    }
    return mkdir(tmp, 0700) == 0 || errno == EEXIST;
}

static bool expand_home(char *dst, size_t size, const char *src, const char *home)
{
    int n;

    if (src[0] == '~' && (src[1] == '/' || src[1] == '\0'))
        n = snprintf(dst, size, "%s%s", home, src + 1);
    else
        n = snprintf(dst, size, "%s", src);
    return n >= 0 && (size_t)n < size;
}

static char *trim(char *s)
{
    char *e;

    while (isspace((unsigned char)*s))
        s++;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = '\0';
    return s;
}

static void load_config(struct config *c, const char *config_dir, const char *home)
{
    char path[PATH_MAX];
    char line[PATH_MAX + 64];
    FILE *f;
    int lineno = 0;

    snprintf(c->pi_command, sizeof c->pi_command, "pi");
    if (!expand_home(c->notes_dir, sizeof c->notes_dir, "~/Notes", home))
        fail("home path is too long");
    if (snprintf(c->prompt_file, sizeof c->prompt_file, "%s/prompt.md", config_dir) >= (int)sizeof c->prompt_file)
        fail("config path is too long");
    c->timeout = 900;

    if (snprintf(path, sizeof path, "%s/config", config_dir) >= (int)sizeof path)
        fail("config path is too long");
    f = fopen(path, "re");
    if (f == NULL)
        fail("cannot open %s: %s", path, strerror(errno));
    while (fgets(line, sizeof line, f) != NULL) {
        char *key, *value, *eq;
        char *dst = NULL;
        size_t dst_size = 0;

        lineno++;
        key = trim(line);
        if (*key == '\0' || *key == '#')
            continue;
        eq = strchr(key, '=');
        if (eq == NULL)
            fail("config line %d has no '='", lineno);
        *eq = '\0';
        key = trim(key);
        value = trim(eq + 1);
        if (strcmp(key, "timeout_seconds") == 0) {
            char *end;
            long t;

            errno = 0;
            t = strtol(value, &end, 10);
            if (errno != 0 || *end != '\0' || t < 30 || t > 7200)
                fail("config timeout_seconds must be 30-7200");
            c->timeout = t;
            continue;
        }
        if (strcmp(key, "provider") == 0)
            dst = c->provider, dst_size = sizeof c->provider;
        else if (strcmp(key, "model") == 0)
            dst = c->model, dst_size = sizeof c->model;
        else if (strcmp(key, "thinking") == 0)
            dst = c->thinking, dst_size = sizeof c->thinking;
        else if (strcmp(key, "pi_command") == 0)
            dst = c->pi_command, dst_size = sizeof c->pi_command;
        else if (strcmp(key, "notes_dir") == 0)
            dst = c->notes_dir, dst_size = sizeof c->notes_dir;
        else if (strcmp(key, "prompt_file") == 0)
            dst = c->prompt_file, dst_size = sizeof c->prompt_file;
        else
            fail("unknown config key '%s' on line %d", key, lineno);
        if (!expand_home(dst, dst_size, value, home))
            fail("config value for '%s' is too long", key);
    }
    if (ferror(f))
        fail("cannot read %s", path);
    fclose(f);
}

/* Today at local noon, offset by days; noon avoids DST edge cases. */
static struct tm day_offset(const struct tm *base, int days)
{
    struct tm t = *base;

    t.tm_mday += days;
    t.tm_hour = 12;
    t.tm_min = t.tm_sec = 0;
    t.tm_isdst = -1;
    if (mktime(&t) == (time_t)-1)
        fail("cannot calculate date");
    return t;
}

static void fmt_date(char *dst, size_t size, const char *fmt, const struct tm *t)
{
    if (strftime(dst, size, fmt, t) == 0)
        fail("cannot format date");
}

static void ensure_note(const char *note, const char *notes_dir, const char *date)
{
    char dir[PATH_MAX];
    char template_path[PATH_MAX];
    struct buf tpl = {0}, out = {0};
    struct stat st;
    char *mark;

    if (stat(note, &st) == 0)
        return;
    if (errno != ENOENT)
        fail("cannot stat note: %s", strerror(errno));
    if (snprintf(dir, sizeof dir, "%s/%s", notes_dir, date) >= (int)sizeof dir)
        fail("note path is too long");
    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
        fail("cannot create %s: %s", dir, strerror(errno));
    if (snprintf(template_path, sizeof template_path, "%s/templates/today.md", notes_dir) >= (int)sizeof template_path)
        fail("template path is too long");
    if (!read_file(template_path, &tpl)) {
        logmsg("template unavailable (%s); using the built-in layout", strerror(errno));
        tpl.len = 0;
        buf_str(&tpl, "# TodaysDate\n\nWhat I did yesterday\n\n-\n-\n-\n\nWhat I plan to do today\n\n-\n-\n-\n\n"
                      "Blocked by\n\n-\n\nTeam Blockages and follow up\n");
    }
    mark = strstr(tpl.p, "TodaysDate");
    if (mark != NULL) {
        buf_add(&out, tpl.p, (size_t)(mark - tpl.p));
        buf_str(&out, date);
        buf_str(&out, mark + strlen("TodaysDate"));
    } else {
        buf_add(&out, tpl.p, tpl.len);
    }
    if (!write_atomic(note, out.p, out.len, 0644))
        fail("cannot create note: %s", strerror(errno));
    logmsg("created %s from the template", note);
    free(tpl.p);
    free(out.p);
}

static bool line_is(const char *line, size_t len, const char *text)
{
    while (len > 0 && isspace((unsigned char)line[len - 1]))
        len--;
    return len == strlen(text) && memcmp(line, text, len) == 0;
}

static bool line_ends_section(const char *line, size_t len)
{
    return line_is(line, len, "What I plan to do today") || line_is(line, len, "Blocked by") ||
           line_is(line, len, "Team Blockages and follow up") || (len > 0 && line[0] == '#');
}

/*
 * Find the body of the "What I did yesterday" section. Returns false when the
 * heading is missing. *filled reports whether it holds more than the template
 * placeholders (blank lines and bare "-").
 */
static bool find_section(const char *text, size_t *body_start, size_t *body_end, bool *filled)
{
    const char *p = text;
    bool in = false;

    *filled = false;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        const char *next = nl ? nl + 1 : p + len;

        if (!in) {
            if (line_is(p, len, SECTION)) {
                in = true;
                *body_start = (size_t)(next - text);
            }
        } else {
            size_t i = 0;

            if (line_ends_section(p, len)) {
                *body_end = (size_t)(p - text);
                return true;
            }
            while (i < len && isspace((unsigned char)p[i]))
                i++;
            if (i < len && !line_is(p + i, len - i, "-"))
                *filled = true;
        }
        p = next;
    }
    if (in)
        *body_end = (size_t)(p - text);
    return in;
}

static void kill_group(pid_t pid)
{
    struct timespec pause = {0, 200 * 1000 * 1000};

    kill(-pid, SIGTERM);
    for (int i = 0; i < 50; i++) {
        if (waitpid(pid, NULL, WNOHANG) == pid)
            return;
        nanosleep(&pause, NULL);
    }
    kill(-pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

static void run_pi(const struct config *c, const char *prompt, struct buf *out)
{
    char *argv[16];
    int argc = 0;
    int fds[2];
    pid_t pid;
    struct timespec start, now;
    int status;

    argv[argc++] = (char *)c->pi_command;
    argv[argc++] = "--print";
    argv[argc++] = "--no-session";
    if (c->provider[0]) {
        argv[argc++] = "--provider";
        argv[argc++] = (char *)c->provider;
    }
    if (c->model[0]) {
        argv[argc++] = "--model";
        argv[argc++] = (char *)c->model;
    }
    if (c->thinking[0]) {
        argv[argc++] = "--thinking";
        argv[argc++] = (char *)c->thinking;
    }
    argv[argc++] = (char *)prompt;
    argv[argc] = NULL;

    if (pipe(fds) != 0)
        fail("pipe: %s", strerror(errno));
    logmsg("starting %s (model %s, thinking %s, timeout %lds)", c->pi_command, c->model[0] ? c->model : "default",
           c->thinking[0] ? c->thinking : "default", c->timeout);
    pid = fork();
    if (pid < 0)
        fail("fork: %s", strerror(errno));
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDONLY);

        setpgid(0, 0);
        if (devnull < 0 || dup2(devnull, STDIN_FILENO) < 0 || dup2(fds[1], STDOUT_FILENO) < 0 ||
            (log_fd >= 0 && dup2(log_fd, STDERR_FILENO) < 0) || chdir(c->notes_dir) != 0)
            _exit(126);
        close(fds[0]);
        close(fds[1]);
        execvp(argv[0], argv);
        dprintf(STDERR_FILENO, "%s: cannot run %s: %s\n", PROGRAM, argv[0], strerror(errno));
        _exit(127);
    }
    setpgid(pid, pid);
    close(fds[1]);
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        struct pollfd pfd = {fds[0], POLLIN, 0};
        char chunk[8192];
        long elapsed_ms, left_ms;
        int r;
        ssize_t n;

        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed_ms = (long)(now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
        left_ms = c->timeout * 1000 - elapsed_ms;
        if (left_ms <= 0) {
            kill_group(pid);
            close(fds[0]);
            fail("pi timed out after %ld seconds", c->timeout);
        }
        r = poll(&pfd, 1, left_ms > 60000 ? 60000 : (int)left_ms);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0) {
            kill_group(pid);
            fail("poll: %s", strerror(errno));
        }
        if (r == 0)
            continue;
        n = read(fds[0], chunk, sizeof chunk);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        if (out->len + (size_t)n > MAX_OUTPUT) {
            kill_group(pid);
            close(fds[0]);
            fail("pi output exceeded %d bytes", MAX_OUTPUT);
        }
        buf_add(out, chunk, (size_t)n);
    }
    close(fds[0]);
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            fail("waitpid: %s", strerror(errno));
    }
    /* Pi has closed stdout; reap any lingering children of its group. */
    kill(-pid, SIGTERM);
    if (out->len > 0 && log_fd >= 0) {
        logmsg("pi output follows");
        if (!write_all(log_fd, out->p, out->len) || !write_all(log_fd, "\n", 1))
            logmsg("could not copy pi output to the log");
    }
    if (WIFSIGNALED(status))
        fail("pi was killed by signal %d", WTERMSIG(status));
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        fail("pi exited with status %d", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
}

/* Extract and validate the bullet block printed between the markers. */
static void extract_bullets(const char *output, struct buf *bullets)
{
    const char *begin = strstr(output, BEGIN_MARK);
    const char *end;
    const char *p;
    int count = 0;

    if (begin == NULL)
        fail("pi output has no %s marker", BEGIN_MARK);
    begin = strchr(begin, '\n');
    if (begin == NULL)
        fail("pi output ends after the begin marker");
    begin++;
    end = strstr(begin, END_MARK);
    if (end == NULL)
        fail("pi output has no %s marker", END_MARK);
    for (p = begin; p < end;) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);

        while (len > 0 && isspace((unsigned char)p[len - 1]))
            len--;
        if (len > 0) {
            if (len < 3 || p[0] != '-' || p[1] != ' ')
                fail("pi output line %d is not a '- ' bullet", count + 1);
            if (len > MAX_BULLET_LEN)
                fail("pi output bullet %d is longer than %d bytes", count + 1, MAX_BULLET_LEN);
            for (size_t i = 0; i < len; i++) {
                if ((unsigned char)p[i] < 0x20 && p[i] != '\t')
                    fail("pi output bullet %d contains control characters", count + 1);
            }
            if (++count > MAX_BULLETS)
                fail("pi output has more than %d bullets", MAX_BULLETS);
            buf_add(bullets, p, len);
            buf_str(bullets, "\n");
        }
        p = nl ? nl + 1 : end;
    }
    if (count == 0)
        fail("pi output has no bullets");
    logmsg("received %d bullets", count);
}

struct run {
    int y, m, d;
    char path[PATH_MAX];
};

static int parse_num(const char *s, int digits)
{
    int v = 0;

    if ((int)strlen(s) != digits)
        return -1;
    for (int i = 0; i < digits; i++) {
        if (!isdigit((unsigned char)s[i]))
            return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

static int cmp_run_desc(const void *a, const void *b)
{
    const struct run *x = a, *y = b;
    long kx = x->y * 10000L + x->m * 100L + x->d, ky = y->y * 10000L + y->m * 100L + y->d;

    return (kx < ky) - (kx > ky);
}

static void remove_run(const char *path)
{
    DIR *dir = opendir(path);
    struct dirent *e;
    char parent[PATH_MAX];
    char *slash;

    if (dir != NULL) {
        while ((e = readdir(dir)) != NULL) {
            if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0 && unlinkat(dirfd(dir), e->d_name, 0) != 0)
                logmsg("cannot remove %s/%s: %s", path, e->d_name, strerror(errno));
        }
        closedir(dir);
    }
    if (rmdir(path) != 0) {
        logmsg("cannot remove %s: %s", path, strerror(errno));
        return;
    }
    /* Drop the month and day directories once they are empty. */
    snprintf(parent, sizeof parent, "%s", path);
    for (int i = 0; i < 2 && (slash = strrchr(parent, '/')) != NULL; i++) {
        *slash = '\0';
        if (rmdir(parent) != 0)
            break;
    }
}

/* Keep the newest KEEP_RUNS cache/DD/MM/YYYY run directories. */
static void rotate(const char *cache)
{
    static struct run runs[MAX_RUNS];
    size_t n = 0;
    DIR *dd = opendir(cache);
    struct dirent *e1;

    if (dd == NULL)
        return;
    while ((e1 = readdir(dd)) != NULL) {
        char p1[PATH_MAX];
        DIR *mm;
        struct dirent *e2;
        int d = parse_num(e1->d_name, 2);

        if (d < 1 || snprintf(p1, sizeof p1, "%s/%s", cache, e1->d_name) >= (int)sizeof p1 || (mm = opendir(p1)) == NULL)
            continue;
        while ((e2 = readdir(mm)) != NULL) {
            char p2[PATH_MAX];
            DIR *yy;
            struct dirent *e3;
            int m = parse_num(e2->d_name, 2);

            if (m < 1 || snprintf(p2, sizeof p2, "%s/%s", p1, e2->d_name) >= (int)sizeof p2 ||
                (yy = opendir(p2)) == NULL)
                continue;
            while ((e3 = readdir(yy)) != NULL && n < MAX_RUNS) {
                int y = parse_num(e3->d_name, 4);

                if (y < 0)
                    continue;
                if (snprintf(runs[n].path, sizeof runs[n].path, "%s/%s", p2, e3->d_name) >= (int)sizeof runs[n].path)
                    continue;
                runs[n].y = y;
                runs[n].m = m;
                runs[n].d = d;
                n++;
            }
            closedir(yy);
        }
        closedir(mm);
    }
    closedir(dd);
    qsort(runs, n, sizeof runs[0], cmp_run_desc);
    for (size_t i = KEEP_RUNS; i < n; i++)
        remove_run(runs[i].path);
}

static void substitute(struct buf *out, const char *tpl, const char *const *pairs, size_t npairs)
{
    const char *p = tpl;

    while (*p) {
        const char *open = strstr(p, "{{");
        bool matched = false;

        if (open == NULL) {
            buf_str(out, p);
            return;
        }
        buf_add(out, p, (size_t)(open - p));
        for (size_t i = 0; i < npairs; i += 2) {
            size_t klen = strlen(pairs[i]);

            if (strncmp(open + 2, pairs[i], klen) == 0 && strncmp(open + 2 + klen, "}}", 2) == 0) {
                buf_str(out, pairs[i + 1]);
                p = open + 4 + klen;
                matched = true;
                break;
            }
        }
        if (!matched) {
            buf_add(out, "{{", 2);
            p = open + 2;
        }
    }
}

static void usage(FILE *f)
{
    fprintf(f, "usage: %s [--force]\n"
               "Fill \"What I did yesterday\" in today's note from the previous working day.\n"
               "  --force  run again even if today already completed, or on a weekend\n",
            PROGRAM);
}

int main(int argc, char **argv)
{
    bool force = false;
    const char *home = getenv("HOME");
    const char *xdg;
    char config_dir[PATH_MAX], cache[PATH_MAX], log_path[PATH_MAX], note[PATH_MAX], completed[PATH_MAX];
    char today_iso[16], prev_iso[16], prev_name[16], dd[4], mm[4], yyyy[8];
    struct config cfg = {0};
    struct tm now_tm, prev_tm;
    time_t now = time(NULL);
    struct buf text = {0}, prompt_tpl = {0}, prompt = {0}, output = {0}, bullets = {0}, updated = {0};
    size_t body_start = 0, body_end = 0;
    bool filled;
    struct stat st;
    int back;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--force") == 0) {
            force = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        } else {
            usage(stderr);
            return 64;
        }
    }
    if (home == NULL || home[0] != '/')
        fail("HOME is not set to an absolute path");
    if (localtime_r(&now, &now_tm) == NULL)
        fail("cannot read local time");
    if (!force && (now_tm.tm_wday == 0 || now_tm.tm_wday == 6)) {
        fprintf(stderr, "%s: weekend, nothing to do\n", PROGRAM);
        return 0;
    }
    /* Monday looks back to Friday; a forced weekend run also uses Friday. */
    back = now_tm.tm_wday == 1 ? -3 : now_tm.tm_wday == 0 ? -2 : -1;
    now_tm = day_offset(&now_tm, 0);
    prev_tm = day_offset(&now_tm, back);
    fmt_date(today_iso, sizeof today_iso, "%Y-%m-%d", &now_tm);
    fmt_date(prev_iso, sizeof prev_iso, "%Y-%m-%d", &prev_tm);
    fmt_date(prev_name, sizeof prev_name, "%A", &prev_tm);
    fmt_date(dd, sizeof dd, "%d", &now_tm);
    fmt_date(mm, sizeof mm, "%m", &now_tm);
    fmt_date(yyyy, sizeof yyyy, "%Y", &now_tm);

    xdg = getenv("XDG_DATA_HOME");
    if (xdg != NULL && xdg[0] == '/')
        snprintf(cache, sizeof cache, "%s/%s/cache", xdg, PROGRAM);
    else
        snprintf(cache, sizeof cache, "%s/.local/share/%s/cache", home, PROGRAM);
    if (snprintf(run_dir, sizeof run_dir, "%s/%s/%s/%s", cache, dd, mm, yyyy) >= (int)sizeof run_dir)
        fail("run directory path is too long");
    if (!mkdirs(run_dir)) {
        int err = errno;

        run_dir[0] = '\0';
        fail("cannot create run directory: %s", strerror(err));
    }
    snprintf(completed, sizeof completed, "%s/completed", run_dir);
    if (!force && stat(completed, &st) == 0) {
        fprintf(stderr, "%s: already completed for %s\n", PROGRAM, today_iso);
        return 0;
    }
    snprintf(log_path, sizeof log_path, "%s/today.log", run_dir);
    log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (log_fd < 0)
        fail("cannot open log: %s", strerror(errno));
    unlink_marker("failed");
    unlink_marker("completed");
    if (!touch("started"))
        fail("cannot write started marker: %s", strerror(errno));
    logmsg("started: today %s, previous working day %s %s", today_iso, prev_name, prev_iso);

    xdg = getenv("XDG_CONFIG_HOME");
    if (xdg != NULL && xdg[0] == '/')
        snprintf(config_dir, sizeof config_dir, "%s/%s", xdg, PROGRAM);
    else
        snprintf(config_dir, sizeof config_dir, "%s/.config/%s", home, PROGRAM);
    load_config(&cfg, config_dir, home);

    if (snprintf(note, sizeof note, "%s/%s/today.md", cfg.notes_dir, today_iso) >= (int)sizeof note)
        fail("note path is too long");
    ensure_note(note, cfg.notes_dir, today_iso);
    if (!read_file(note, &text))
        fail("cannot read note: %s", strerror(errno));
    if (!find_section(text.p, &body_start, &body_end, &filled))
        fail("note has no \"%s\" heading", SECTION);
    if (filled) {
        logmsg("\"%s\" already has entries; leaving the note unchanged", SECTION);
        goto done;
    }

    if (!read_file(cfg.prompt_file, &prompt_tpl))
        fail("cannot read prompt %s: %s", cfg.prompt_file, strerror(errno));
    {
        const char *pairs[] = {"TODAY", today_iso, "PREVIOUS_DAY", prev_iso, "PREVIOUS_DAY_NAME", prev_name,
                               "BEGIN", BEGIN_MARK, "END", END_MARK};

        substitute(&prompt, prompt_tpl.p, pairs, sizeof pairs / sizeof pairs[0]);
    }
    run_pi(&cfg, prompt.p, &output);
    if (output.p == NULL)
        fail("pi produced no output");
    extract_bullets(output.p, &bullets);

    /* Re-read so edits made while Pi ran are not overwritten. */
    if (!read_file(note, &text))
        fail("cannot re-read note: %s", strerror(errno));
    if (!find_section(text.p, &body_start, &body_end, &filled))
        fail("note lost its \"%s\" heading during the run", SECTION);
    if (filled) {
        logmsg("\"%s\" was filled during the run; leaving the note unchanged", SECTION);
        goto done;
    }
    buf_add(&updated, text.p, body_start);
    buf_str(&updated, "\n");
    buf_add(&updated, bullets.p, bullets.len);
    buf_str(&updated, "\n");
    buf_str(&updated, text.p + body_end);
    if (stat(note, &st) != 0)
        fail("cannot stat note: %s", strerror(errno));
    if (!write_atomic(note, updated.p, updated.len, st.st_mode & 07777))
        fail("cannot write note: %s", strerror(errno));
    logmsg("updated %s", note);

done:
    if (!touch("completed"))
        fail("cannot write completed marker: %s", strerror(errno));
    logmsg("completed");
    rotate(cache);
    free(text.p);
    free(prompt_tpl.p);
    free(prompt.p);
    free(output.p);
    free(bullets.p);
    free(updated.p);
    close(log_fd);
    return 0;
}
