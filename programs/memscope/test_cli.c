#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;
static char bin[PATH_MAX];

static void sleep_ms(int ms)
{
    struct timespec delay = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    nanosleep(&delay, NULL);
}
static char helper[PATH_MAX];
static char **roots;
static size_t root_n, root_cap;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", msg);
        failures++;
    }
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fputs("out of memory\n", stderr);
        exit(1);
    }
    return p;
}

static char *xdup(const char *s)
{
    size_t n = strlen(s);
    char *p = xmalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

static int write_file(const char *path, const char *text)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    if (fputs(text, fp) < 0) {
        fclose(fp);
        return -1;
    }
    return fclose(fp);
}

static void remove_tree(const char *path)
{
    DIR *dir;
    struct dirent *ent;
    char **names = NULL;
    size_t n = 0, cap = 0, i;
    if (!path) return;
    dir = opendir(path);
    if (!dir) {
        unlink(path);
        return;
    }
    while ((ent = readdir(dir)) != NULL) {
        char *copy;
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 8;
            char **next = realloc(names, ncap * sizeof *names);
            if (!next) break;
            names = next;
            cap = ncap;
        }
        copy = xmalloc(strlen(ent->d_name) + 1);
        memcpy(copy, ent->d_name, strlen(ent->d_name) + 1);
        names[n++] = copy;
    }
    closedir(dir);
    for (i = 0; i < n; i++) {
        size_t len = strlen(path) + 1 + strlen(names[i]);
        char *child = xmalloc(len + 1);
        snprintf(child, len + 1, "%s/%s", path, names[i]);
        remove_tree(child);
        free(child);
        free(names[i]);
    }
    free(names);
    rmdir(path);
}

static void cleanup_roots(void)
{
    size_t i;
    for (i = 0; i < root_n; i++) {
        remove_tree(roots[i]);
        free(roots[i]);
    }
    free(roots);
    roots = NULL;
    root_n = root_cap = 0;
}

static char *directory(void)
{
    char tmpl[] = "/tmp/memscope-XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) return NULL;
    if (root_n == root_cap) {
        size_t ncap = root_cap ? root_cap * 2 : 8;
        char **next = realloc(roots, ncap * sizeof *roots);
        if (!next) return NULL;
        roots = next;
        root_cap = ncap;
    }
    roots[root_n++] = xdup(dir);
    return roots[root_n - 1];
}

static char *mapping(unsigned long long inode, const char *path, const char *pss,
                     const char *flags, unsigned long long anon)
{
    char *out;
    int n;
    if (!path) path = "/models/weights.gguf";
    if (!pss) pss = "1048576";
    if (!flags) flags = "rd mr mw me";
    n = snprintf(NULL, 0,
                 "10000000-90000000 r--p 00000000 08:01 %llu %s\n"
                 "Size: 2097152 kB\nRss: %s kB\nPss: %s kB\nAnonymous: %llu kB\n"
                 "Shared_Hugetlb: 0 kB\nPrivate_Hugetlb: 0 kB\nVmFlags: %s\n",
                 inode, path, pss, pss, anon, flags);
    if (n < 0) return NULL;
    out = xmalloc((size_t)n + 1);
    snprintf(out, (size_t)n + 1,
             "10000000-90000000 r--p 00000000 08:01 %llu %s\n"
             "Size: 2097152 kB\nRss: %s kB\nPss: %s kB\nAnonymous: %llu kB\n"
             "Shared_Hugetlb: 0 kB\nPrivate_Hugetlb: 0 kB\nVmFlags: %s\n",
             inode, path, pss, pss, anon, flags);
    return out;
}

static int add_proc(const char *root, int pid, const char *name, const char *smaps)
{
    char path[PATH_MAX];
    if (snprintf(path, sizeof path, "%s/%d", root, pid) >= (int)sizeof path) return -1;
    if (mkdir(path, 0700) != 0 && errno != EEXIST) return -1;
    if (snprintf(path, sizeof path, "%s/%d/comm", root, pid) >= (int)sizeof path) return -1;
    {
        size_t n = strlen(name);
        char *text = xmalloc(n + 2);
        memcpy(text, name, n);
        text[n] = '\n';
        text[n + 1] = '\0';
        if (write_file(path, text) != 0) {
            free(text);
            return -1;
        }
        free(text);
    }
    if (snprintf(path, sizeof path, "%s/%d/smaps", root, pid) >= (int)sizeof path) return -1;
    return write_file(path, smaps);
}

typedef struct {
    char *root;
} fixture;

static fixture make_fixture(void)
{
    fixture f = {0};
    char *def;
    char path[PATH_MAX];
    f.root = directory();
    if (!f.root) return f;
    if (snprintf(path, sizeof path, "%s/meminfo", f.root) >= (int)sizeof path) return f;
    write_file(path, "MemTotal: 33554432 kB\nMemAvailable: 12582912 kB\nAnonPages: 16777216 kB\n");
    def = mapping(1, NULL, NULL, NULL, 0);
    add_proc(f.root, 10, "llama-server", def);
    free(def);
    def = mapping(2, "/cache/media-cache.db", "524288", NULL, 0);
    add_proc(f.root, 11, "browser", def);
    free(def);
    def = mapping(3, "/lib/libcuda.so.1", "262144", NULL, 0);
    add_proc(f.root, 12, "audio-server", def);
    free(def);
    def = mapping(4, "/lib/other.so", "262144", NULL, 0);
    add_proc(f.root, 13, "other", def);
    free(def);
    return f;
}

typedef struct {
    int status;
    char *out;
    char *err;
} result;

static void result_free(result *r)
{
    free(r->out);
    free(r->err);
    r->out = r->err = NULL;
}

static char *read_all(int fd, size_t *len)
{
    char *buf = NULL;
    size_t n = 0, cap = 0;
    char tmp[4096];
    ssize_t got;
    while ((got = read(fd, tmp, sizeof tmp)) > 0) {
        if (n + (size_t)got + 1 > cap) {
            size_t ncap = cap ? cap * 2 : 8192;
            char *next;
            while (ncap < n + (size_t)got + 1) ncap *= 2;
            next = realloc(buf, ncap);
            if (!next) {
                free(buf);
                return NULL;
            }
            buf = next;
            cap = ncap;
        }
        memcpy(buf + n, tmp, (size_t)got);
        n += (size_t)got;
    }
    if (got < 0) {
        free(buf);
        return NULL;
    }
    if (!buf) buf = xmalloc(1);
    buf[n] = '\0';
    if (len) *len = n;
    return buf;
}

static int line_has_span(const char *s, const char *left, const char *right)
{
    const char *p = s;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        char *line = xmalloc(n + 1);
        char *hit;
        int ok;
        memcpy(line, p, n);
        line[n] = '\0';
        hit = strstr(line, left);
        ok = hit && strstr(hit, right);
        free(line);
        if (ok) return 1;
        if (!nl) break;
        p = nl + 1;
    }
    return 0;
}

static result run_bin(const char *root, char *const extra[], const char *no_color, const char *term)
{
    char *argv[16];
    int n = 0, i;
    pid_t pid;
    int outp[2], errp[2];
    result r = {.status = -1};
    argv[n++] = bin;
    argv[n++] = "--proc-root";
    argv[n++] = (char *)root;
    if (extra) for (i = 0; extra[i]; i++) argv[n++] = extra[i];
    argv[n] = NULL;
    if (pipe(outp) != 0 || pipe(errp) != 0) return r;
    pid = fork();
    if (pid < 0) return r;
    if (!pid) {
        dup2(outp[1], STDOUT_FILENO);
        dup2(errp[1], STDERR_FILENO);
        close(outp[0]);
        close(outp[1]);
        close(errp[0]);
        close(errp[1]);
        setenv("NO_COLOR", no_color ? no_color : "", 1);
        setenv("TERM", term ? term : "xterm-256color", 1);
        execv(bin, argv);
        _exit(127);
    }
    close(outp[1]);
    close(errp[1]);
    {
        time_t deadline = time(NULL) + 10;
        int status = 0, reaped = 0;
        while (!reaped) {
            pid_t got = waitpid(pid, &status, WNOHANG);
            if (got == pid) {
                reaped = 1;
                r.status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            } else if (time(NULL) > deadline) {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                reaped = 1;
            } else sleep_ms(10);
        }
    }
    r.out = read_all(outp[0], NULL);
    r.err = read_all(errp[0], NULL);
    close(outp[0]);
    close(errp[0]);
    if (!r.out) r.out = xdup("");
    if (!r.err) r.err = xdup("");
    return r;
}

static char *output_env(const char *root, char *const extra[], const char *no_color, const char *term)
{
    result r = run_bin(root, extra, no_color, term);
    if (r.status != 0) {
        fprintf(stderr, "FAIL command status %d: %s\n", r.status, r.err);
        failures++;
    }
    free(r.err);
    return r.out ? r.out : xdup("");
}

static char *output(const char *root, char *const extra[])
{
    return output_env(root, extra, "", "xterm-256color");
}

static char *strip_ansi(const char *s)
{
    char *out = xmalloc(strlen(s) + 1);
    const char *p = s;
    char *w = out;
    while (*p) {
        if (p[0] == '\033' && p[1] == '[') {
            const char *q = p + 2;
            while (*q && ((*q >= '0' && *q <= '9') || *q == ';')) q++;
            if (*q == 'm') {
                p = q + 1;
                continue;
            }
        }
        *w++ = *p++;
    }
    *w = '\0';
    return out;
}

static char *replace_time(const char *s)
{
    char *out = xmalloc(strlen(s) + 1);
    const char *p = s;
    char *w = out;
    while (*p) {
        const char *q = p;
        while (*q >= '0' && *q <= '9') q++;
        if (q > p && strncmp(q, " ms sample", 10) == 0) {
            memcpy(w, "TIME", 4);
            w += 4;
            p = q + 10;
            continue;
        }
        *w++ = *p++;
    }
    *w = '\0';
    return out;
}

static size_t codepoints(const char *s, size_t n)
{
    size_t i = 0, count = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        size_t len = 1;
        if ((c & 0xe0) == 0xc0) len = 2;
        else if ((c & 0xf0) == 0xe0) len = 3;
        else if ((c & 0xf8) == 0xf0) len = 4;
        if (i + len > n) break;
        i += len;
        count++;
    }
    return count;
}

static void expect_width(const char *s, unsigned n, const char *msg)
{
    const char *p = s;
    int ok = 1;
    while (*p) {
        const char *nl = strpbrk(p, "\r\n");
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len && codepoints(p, len) > n) ok = 0;
        if (!nl) break;
        p = nl + 1;
        if (*nl == '\r' && *p == '\n') p++;
    }
    if (!ok) fprintf(stderr, "WIDTH %s\n%s\n", msg, s);
    expect(ok, msg);
}

static int digits_only(const char *s)
{
    if (!s || !*s) return 0;
    for (; *s; s++) if (!isdigit((unsigned char)*s)) return 0;
    return 1;
}

static void each_pid(const char *root, void (*fn)(const char *, const char *, void *), void *user)
{
    DIR *dir = opendir(root);
    struct dirent *ent;
    if (!dir) return;
    while ((ent = readdir(dir)) != NULL) {
        if (!digits_only(ent->d_name)) continue;
        fn(root, ent->d_name, user);
    }
    closedir(dir);
}

static void remove_pid(const char *root, const char *pid, void *user)
{
    char path[PATH_MAX];
    (void)user;
    if (snprintf(path, sizeof path, "%s/%s", root, pid) >= (int)sizeof path) return;
    remove_tree(path);
}

static void write_smaps(const char *root, const char *pid, void *user)
{
    char path[PATH_MAX];
    const char *text = user;
    if (snprintf(path, sizeof path, "%s/%s/smaps", root, pid) >= (int)sizeof path) return;
    write_file(path, text);
}

static void test_headline(void)
{
    fixture f = make_fixture();
    char *limit3[] = {"--limit", "3", NULL};
    char *limit1[] = {"--limit", "1", NULL};
    char *out = output(f.root, limit3);
    char *one = output(f.root, limit1);
    expect(strstr(out, "20.0 GiB / 32.0 GiB used   62.5%") != NULL, "headline used");
    expect(strstr(out, "12.0 GiB available") != NULL, "available");
    expect(strstr(out, "2.0 GiB PSS | 6.25% of total RAM") != NULL, "pss headline");
    expect(line_has_span(out, "weights.gguf", "50.0%"), "weights share");
    expect(line_has_span(out, "Other observed mappings", "12.5%"), "other share");
    expect(line_has_span(one, "weights.gguf", "50.0%"), "limit one weights");
    expect(line_has_span(one, "Other observed mappings", "50.0%"), "limit one other");
    expect(strchr(out, '\033') == NULL, "default unstyled");
    free(out);
    free(one);
}

static void test_identity(void)
{
    fixture f = make_fixture();
    char *alias = mapping(1, "/alias/model (deleted)", NULL, NULL, 0);
    char *other = mapping(99, "/another/weights.gguf", "200000", NULL, 0);
    char *out;
    add_proc(f.root, 14, "second", alias);
    out = output(f.root, NULL);
    expect(strstr(out, "3.0 GiB PSS") != NULL, "aliased pss");
    expect(strstr(out, "66.7%") != NULL, "alias percent");
    expect(strstr(out, "[deleted]") != NULL, "deleted marker");
    expect(strstr(out, "second") != NULL, "second process");
    free(out);
    add_proc(f.root, 15, "different", other);
    out = output(f.root, NULL);
    expect(strstr(out, "identity") != NULL, "distinct identity");
    free(out);
    free(alias);
    free(other);
}

static void test_private_copies(void)
{
    fixture f = make_fixture();
    char *text = mapping(1, NULL, NULL, NULL, 524288);
    char *out;
    add_proc(f.root, 10, "llama-server", text);
    out = output(f.root, NULL);
    expect(strstr(out, "2.0 GiB PSS") != NULL, "anon not subtracted");
    free(out);
    free(text);
}

static void test_special(void)
{
    fixture f = make_fixture();
    char *text = mapping(200, "/dev/example", NULL, "rd io pf", 0);
    char *out;
    add_proc(f.root, 20, "device", text);
    out = output(f.root, NULL);
    expect(strstr(out, "2.0 GiB PSS") != NULL, "special excluded from pss");
    expect(strstr(out, "1 special mappings excluded") != NULL, "special count");
    free(out);
    free(text);
}

static void test_shmem(void)
{
    fixture f = make_fixture();
    char *text = mapping(1, "/memfd:example (deleted)", NULL, NULL, 0);
    char *out;
    add_proc(f.root, 10, "shared", text);
    out = output(f.root, NULL);
    expect(strstr(out, "[shmem]") != NULL, "shmem label");
    free(out);
    free(text);
}

static void test_future_fields(void)
{
    fixture f = make_fixture();
    char *base = mapping(1, NULL, NULL, NULL, 0);
    char *fields = "AnonHugePages: 0 kB\nFilePmdMapped: 0 kB\nFuture_Field: 123 kB\n";
    char *flag = strstr(base, "VmFlags:");
    char *rewritten;
    char vanished[PATH_MAX];
    char *out;
    size_t head = (size_t)(flag - base);
    rewritten = xmalloc(head + strlen(fields) + strlen(flag) + 1);
    memcpy(rewritten, base, head);
    memcpy(rewritten + head, fields, strlen(fields));
    memcpy(rewritten + head + strlen(fields), flag, strlen(flag) + 1);
    add_proc(f.root, 10, "llama-server", rewritten);
    add_proc(f.root, 90, "kernel-thread", "");
    add_proc(f.root, 91, "exited", "");
    snprintf(vanished, sizeof vanished, "%s/91/smaps", f.root);
    unlink(vanished);
    out = output(f.root, NULL);
    expect(strstr(out, "2.0 GiB PSS") != NULL, "future fields kept");
    expect(strstr(out, "4 read") != NULL, "readable count");
    expect(strstr(out, "1 vanished") != NULL, "vanished count");
    expect(strstr(out, "invalid process") == NULL, "no invalid process");
    free(out);
    free(rewritten);
    free(base);
}

static void test_overflow(void)
{
    fixture f = make_fixture();
    char *a = mapping(1, NULL, "18446744073709551615", NULL, 0);
    char *b = mapping(20, "/other", "1", NULL, 0);
    char *joined = xmalloc(strlen(a) + strlen(b) + 1);
    char *out;
    memcpy(joined, a, strlen(a));
    memcpy(joined + strlen(a), b, strlen(b) + 1);
    add_proc(f.root, 10, "bad", joined);
    out = output(f.root, NULL);
    expect(strstr(out, "1 invalid") != NULL, "overflow invalid");
    expect(strstr(out, "1.0 GiB PSS") != NULL, "overflow remainder");
    expect(strstr(out, "weights.gguf") == NULL, "overflow discarded mapping");
    free(out);
    free(joined);
    free(a);
    free(b);
}

static void test_clipped(void)
{
    fixture f = make_fixture();
    const char *suffix = "same-suffix-012345678901234567890";
    char path[256];
    char *a, *b, *out;
    char *width[] = {"--width", "80", NULL};
    snprintf(path, sizeof path, "/models/first-%s", suffix);
    a = mapping(1, path, NULL, NULL, 0);
    snprintf(path, sizeof path, "/models/second-%s", suffix);
    b = mapping(2, path, "524288", NULL, 0);
    add_proc(f.root, 10, "first", a);
    add_proc(f.root, 11, "second", b);
    out = output(f.root, width);
    expect(strstr(out, "identity 8:1:1") != NULL, "identity one");
    expect(strstr(out, "identity 8:1:2") != NULL, "identity two");
    free(out);
    free(a);
    free(b);
}

static void test_comm_newline(void)
{
    fixture f = make_fixture();
    char *text = mapping(1, NULL, NULL, NULL, 0);
    char *out;
    add_proc(f.root, 10, "worker\nA", text);
    out = output(f.root, NULL);
    expect(strstr(out, "worker\\x0aA") != NULL, "escaped comm newline");
    free(out);
    free(text);
}

static void test_full_paths(void)
{
    fixture f = make_fixture();
    char name[512];
    char *text, *out, *joined, *w;
    char *args[] = {"--full-paths", NULL};
    int i;
    memcpy(name, "/a/very/long/path/", 18);
    w = name + 18;
    for (i = 0; i < 10; i++) {
        memcpy(w, "component/", 10);
        w += 10;
    }
    memcpy(w, "weights.gguf", 13);
    text = mapping(1, name, NULL, NULL, 0);
    add_proc(f.root, 10, "llama-server", text);
    out = output(f.root, args);
    joined = xmalloc(strlen(out) + 1);
    w = joined;
    for (i = 0; out[i]; i++) if (out[i] != '\n' && out[i] != '\r') *w++ = out[i];
    *w = '\0';
    expect(strstr(joined, name) != NULL, "full path retained");
    free(joined);
    free(out);
    free(text);
}

static void test_missing_available(void)
{
    fixture f = make_fixture();
    char path[PATH_MAX];
    char *out;
    snprintf(path, sizeof path, "%s/meminfo", f.root);
    write_file(path, "MemTotal: 33554432 kB\nMemFree: 123 kB\n");
    out = output(f.root, NULL);
    expect(strstr(out, "used/available unavailable") != NULL, "no free fallback");
    free(out);
}

static void test_invalid_total(void)
{
    fixture f = make_fixture();
    const char *texts[] = {
        "MemTotal: 0 kB\n",
        "MemTotal: 18446744073709551616 kB\n",
        "MemTotal: -1 kB\n",
        "MemTotal: 32 MB\n",
    };
    size_t i;
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/meminfo", f.root);
    for (i = 0; i < sizeof texts / sizeof texts[0]; i++) {
        result r;
        write_file(path, texts[i]);
        r = run_bin(f.root, NULL, "", "xterm-256color");
        expect(r.status != 0, "invalid total fails");
        result_free(&r);
    }
}

static void test_malformed(void)
{
    fixture f = make_fixture();
    char *a = mapping(1, NULL, NULL, NULL, 0);
    char *b = mapping(19, "/partial", "9999", NULL, 0);
    char *flag = strstr(b, "VmFlags:");
    char *joined = xmalloc(strlen(a) + (size_t)(flag - b) + 1);
    char *out;
    memcpy(joined, a, strlen(a));
    memcpy(joined + strlen(a), b, (size_t)(flag - b));
    joined[strlen(a) + (size_t)(flag - b)] = '\0';
    add_proc(f.root, 10, "bad", joined);
    out = output(f.root, NULL);
    expect(strstr(out, "1 invalid") != NULL, "malformed invalid");
    expect(strstr(out, "1.0 GiB PSS") != NULL, "malformed remainder");
    expect(strstr(out, "weights.gguf") == NULL, "malformed discarded");
    free(out);
    free(joined);
    free(a);
    free(b);
}

static void test_empty_tree(void)
{
    fixture f = make_fixture();
    char *out;
    each_pid(f.root, remove_pid, NULL);
    out = output(f.root, NULL);
    expect(strstr(out, "Mapping PSS unavailable") != NULL, "empty tree");
    free(out);
}

static void test_ellipsis(void)
{
    fixture f = make_fixture();
    char name[64];
    char *text = mapping(1, NULL, "1024", NULL, 0);
    char *compact_args[] = {"--width", "40", NULL};
    char *full_args[] = {"--width", "40", "--full-paths", NULL};
    char *compact, *plain, *full;
    int pid;
    for (pid = 20; pid < 28; pid++) {
        snprintf(name, sizeof name, "process-%d-with-a-long-name", pid);
        add_proc(f.root, pid, name, text);
    }
    compact = output(f.root, compact_args);
    expect(strstr(compact, "...") != NULL, "attachment ellipsis");
    plain = strip_ansi(compact);
    expect_width(plain, 40, "width 40");
    full = output(f.root, full_args);
    expect(strstr(full, "process-20-with-a-long-name") != NULL, "full attachment");
    free(full);
    free(plain);
    free(compact);
    free(text);
}

static void test_zero_distinct(void)
{
    fixture f = make_fixture();
    char *zero = mapping(1, NULL, "0", NULL, 0);
    char *out;
    each_pid(f.root, write_smaps, zero);
    out = output(f.root, NULL);
    expect(strstr(out, "No resident PSS") != NULL, "zero pss");
    {
        char *lower = xdup(out);
        char *p;
        for (p = lower; *p; p++) *p = (char)tolower((unsigned char)*p);
        expect(strstr(lower, "nan") == NULL, "no nan");
        free(lower);
    }
    free(out);
    each_pid(f.root, write_smaps, "bad\n");
    out = output(f.root, NULL);
    expect(strstr(out, "Mapping PSS unavailable") != NULL, "bad smaps unavailable");
    free(out);
    free(zero);
}

static void test_permission(void)
{
    fixture f = make_fixture();
    char path[PATH_MAX];
    char *out;
    if (geteuid() == 0) {
        fputs("SKIP permission failures remain partial\n", stderr);
        return;
    }
    snprintf(path, sizeof path, "%s/10/smaps", f.root);
    if (chmod(path, 0) != 0) {
        expect(0, "chmod fixture");
        return;
    }
    out = output(f.root, NULL);
    expect(strstr(out, "1 denied") != NULL, "denied count");
    expect(strstr(out, "PARTIAL") != NULL, "partial marker");
    expect(strstr(out, "weights.gguf") == NULL, "denied mapping hidden");
    chmod(path, 0600);
    free(out);
}

static void test_injection(void)
{
    fixture f = make_fixture();
    char *text = mapping(1, "/evil/\033[31mweights.gguf", NULL, NULL, 0);
    unsigned widths[] = {8, 20, 40, 60, 80, 120};
    size_t i;
    char *out;
    add_proc(f.root, 10, "bad\033[2Jname", text);
    for (i = 0; i < sizeof widths / sizeof widths[0]; i++) {
        char num[16];
        char *args[3];
        char *plain;
        snprintf(num, sizeof num, "%u", widths[i]);
        args[0] = "--width";
        args[1] = num;
        args[2] = NULL;
        out = output(f.root, args);
        expect(strchr(out, '\033') == NULL, "no raw escape");
        plain = strip_ansi(out);
        expect_width(plain, widths[i], "injection width");
        free(plain);
        free(out);
    }
    out = output(f.root, NULL);
    expect(strstr(out, "\\x1b") != NULL, "escaped escape");
    free(out);
    free(text);
}

static void test_color_cli(void)
{
    fixture f = make_fixture();
    char *always[] = {"--color", "always", NULL};
    char *never[] = {"--color", "never", NULL};
    char *styled = output(f.root, always);
    char *plain_styled, *timed_styled, *plain_never, *timed_never, *none, *dumb;
    const char *codes[] = {"34", "0;35", "36", "32", "1", "1;39"};
    size_t i;
    const char *bad[][3] = {
        {"--limit", "0", NULL},
        {"--limit", "-1", NULL},
        {"--width", "999", NULL},
        {"--color", "invalid", NULL},
        {"unexpected", NULL, NULL},
    };
    for (i = 0; i < sizeof codes / sizeof codes[0]; i++) {
        char needle[32];
        snprintf(needle, sizeof needle, "\033[%sm", codes[i]);
        expect(strstr(styled, needle) != NULL, "sgr code");
    }
    plain_styled = strip_ansi(styled);
    timed_styled = replace_time(plain_styled);
    plain_never = output(f.root, never);
    timed_never = replace_time(plain_never);
    expect(strcmp(timed_styled, timed_never) == 0, "color does not change text");
    none = output_env(f.root, always, "1", "xterm-256color");
    expect(strchr(none, '\033') == NULL, "NO_COLOR wins");
    dumb = output_env(f.root, NULL, "", "dumb");
    expect(strchr(dumb, '\033') == NULL, "dumb terminal");
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char *args[3] = {(char *)bad[i][0], (char *)bad[i][1], NULL};
        result r = run_bin(f.root, args, "", "xterm-256color");
        expect(r.status == 2, "cli rejection");
        result_free(&r);
    }
    {
        char *version[] = {"--version", NULL};
        char *help[] = {"--help", NULL};
        char *ver = output(f.root, version);
        char *usage = output(f.root, help);
        expect(strstr(ver, "memscope 0.1.0") != NULL, "version");
        expect(strstr(usage, "MemTotal - MemAvailable") != NULL, "help formula");
        free(ver);
        free(usage);
    }
    free(dumb);
    free(none);
    free(timed_never);
    free(plain_never);
    free(timed_styled);
    free(plain_styled);
    free(styled);
}

static void test_pty(void)
{
    unsigned widths[] = {40, 60, 80, 120};
    size_t i;
    fixture f = make_fixture();
    for (i = 0; i < sizeof widths / sizeof widths[0]; i++) {
        char num[16];
        char *argv[6];
        int outp[2], errp[2];
        pid_t pid;
        result r = {.status = -1};
        char *plain;
        snprintf(num, sizeof num, "%u", widths[i]);
        argv[0] = helper;
        argv[1] = "pty";
        argv[2] = num;
        argv[3] = bin;
        argv[4] = f.root;
        argv[5] = NULL;
        if (pipe(outp) != 0 || pipe(errp) != 0) {
            expect(0, "pty pipes");
            return;
        }
        pid = fork();
        if (pid < 0) {
            expect(0, "pty fork");
            return;
        }
        if (!pid) {
            dup2(outp[1], STDOUT_FILENO);
            dup2(errp[1], STDERR_FILENO);
            close(outp[0]);
            close(outp[1]);
            close(errp[0]);
            close(errp[1]);
            setenv("TERM", "xterm-256color", 1);
            setenv("LC_ALL", "C.UTF-8", 1);
            setenv("NO_COLOR", "", 1);
            execv(helper, argv);
            _exit(127);
        }
        close(outp[1]);
        close(errp[1]);
        {
            time_t deadline = time(NULL) + 10;
            int status = 0, reaped = 0;
            while (!reaped) {
                pid_t got = waitpid(pid, &status, WNOHANG);
                if (got == pid) {
                    reaped = 1;
                    r.status = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
                } else if (time(NULL) > deadline) {
                    kill(pid, SIGKILL);
                    waitpid(pid, &status, 0);
                    reaped = 1;
                } else sleep_ms(10);
            }
        }
        r.out = read_all(outp[0], NULL);
        r.err = read_all(errp[0], NULL);
        close(outp[0]);
        close(errp[0]);
        expect(r.status == 0, r.err ? r.err : "pty status");
        expect(r.out && strstr(r.out, "\033[34m") != NULL, "pty color");
        expect(r.out && strstr(r.out, "█") != NULL, "unicode bar");
        plain = strip_ansi(r.out ? r.out : "");
        expect_width(plain, widths[i], "pty width");
        free(plain);
        result_free(&r);
    }
}

typedef struct {
    pid_t pid;
    int in;
    int out;
    int err;
} mapped;

static int read_line(int fd, char *buf, size_t cap, int timeout_ms)
{
    size_t n = 0;
    int64_t end = (int64_t)time(NULL) * 1000 + timeout_ms;
    int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    while (n + 1 < cap && (int64_t)time(NULL) * 1000 < end) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        char c;
        ssize_t got;
        int pr = poll(&pfd, 1, 50);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (pr == 0) continue;
        got = read(fd, &c, 1);
        if (got == 1) {
            if (c == '\n') {
                buf[n] = '\0';
                return 0;
            }
            buf[n++] = c;
            continue;
        }
        if (got == 0) return -1;
        if (errno != EAGAIN && errno != EINTR) return -1;
    }
    return -1;
}

static mapped start_mapped(const char *mode, const char *path)
{
    int in[2], out[2], err[2];
    mapped m = {.pid = -1, .in = -1, .out = -1, .err = -1};
    char ready[64];
    char *argv[4];
    if (pipe(in) != 0 || pipe(out) != 0 || pipe(err) != 0) return m;
    m.pid = fork();
    if (m.pid < 0) return m;
    if (!m.pid) {
        dup2(in[0], STDIN_FILENO);
        dup2(out[1], STDOUT_FILENO);
        dup2(err[1], STDERR_FILENO);
        close(in[1]);
        close(out[0]);
        close(err[0]);
        argv[0] = helper;
        argv[1] = (char *)mode;
        argv[2] = (char *)path;
        argv[3] = NULL;
        execv(helper, argv);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    close(err[1]);
    m.in = in[1];
    m.out = out[0];
    m.err = err[0];
    if (read_line(m.out, ready, sizeof ready, 5000) != 0 || strcmp(ready, "ready") != 0) {
        kill(m.pid, SIGKILL);
        waitpid(m.pid, NULL, 0);
        m.pid = -1;
    }
    return m;
}

static int mapped_command(mapped *m, const char *text, char *reply, size_t cap)
{
    char line[64];
    size_t n = strlen(text);
    if (write(m->in, text, n) != (ssize_t)n || write(m->in, "\n", 1) != 1) return -1;
    if (read_line(m->out, line, sizeof line, 5000) != 0) return -1;
    if (strlen(line) + 1 > cap) return -1;
    memcpy(reply, line, strlen(line) + 1);
    return 0;
}

static void mapped_stop(mapped *m)
{
    int status;
    if (m->pid < 0) return;
    if (write(m->in, "quit\n", 5) != 5) kill(m->pid, SIGKILL);
    close(m->in);
    close(m->out);
    close(m->err);
    m->in = m->out = m->err = -1;
    if (waitpid(m->pid, &status, 0) < 0) expect(0, "mapped wait");
    else expect(WIFEXITED(status) && WEXITSTATUS(status) == 0, "mapped exit");
    m->pid = -1;
}

static char *live_root(pid_t *pids, size_t count)
{
    char *root = directory();
    char path[PATH_MAX];
    FILE *src, *dst;
    char buf[4096];
    size_t i, n;
    if (!root) return NULL;
    if (snprintf(path, sizeof path, "%s/meminfo", root) >= (int)sizeof path) return NULL;
    src = fopen("/proc/meminfo", "r");
    dst = fopen(path, "w");
    if (!src || !dst) {
        if (src) fclose(src);
        if (dst) fclose(dst);
        return NULL;
    }
    while ((n = fread(buf, 1, sizeof buf, src)) > 0) fwrite(buf, 1, n, dst);
    fclose(src);
    fclose(dst);
    for (i = 0; i < count; i++) {
        char target[64];
        snprintf(target, sizeof target, "/proc/%d", (int)pids[i]);
        snprintf(path, sizeof path, "%s/%d", root, (int)pids[i]);
        if (symlink(target, path) != 0) return NULL;
    }
    return root;
}

static void test_vanish(void)
{
    char *root = directory();
    char path[PATH_MAX];
    char fifo[PATH_MAX];
    char link[PATH_MAX];
    char proc[64];
    pid_t sleeper = -1, collector = -1;
    int writer = -1, outp[2] = {-1, -1}, errp[2] = {-1, -1};
    time_t deadline;
    char *out = NULL, *err = NULL;
    char *text;
    int status = 0;
    if (!root) {
        expect(0, "vanish root");
        return;
    }
    snprintf(path, sizeof path, "%s/meminfo", root);
    write_file(path, "MemTotal: 33554432 kB\n");
    snprintf(path, sizeof path, "%s/1", root);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/1/comm", root);
    write_file(path, "barrier\n");
    snprintf(fifo, sizeof fifo, "%s/1/smaps", root);
    expect(mkfifo(fifo, 0600) == 0, "mkfifo");
    sleeper = fork();
    if (!sleeper) {
        pause();
        _exit(0);
    }
    expect(sleeper > 0, "sleeper");
    snprintf(proc, sizeof proc, "/proc/%d", (int)sleeper);
    snprintf(link, sizeof link, "%s/%d", root, (int)sleeper);
    expect(symlink(proc, link) == 0, "proc symlink");
    if (pipe(outp) != 0 || pipe(errp) != 0) {
        expect(0, "collector pipes");
        goto done;
    }
    collector = fork();
    if (!collector) {
        char *argv[4];
        dup2(outp[1], STDOUT_FILENO);
        dup2(errp[1], STDERR_FILENO);
        close(outp[0]);
        close(outp[1]);
        close(errp[0]);
        close(errp[1]);
        setenv("NO_COLOR", "1", 1);
        argv[0] = bin;
        argv[1] = "--proc-root";
        argv[2] = root;
        argv[3] = NULL;
        execv(bin, argv);
        _exit(127);
    }
    close(outp[1]);
    close(errp[1]);
    deadline = time(NULL) + 5;
    while (writer < 0) {
        writer = open(fifo, O_WRONLY | O_NONBLOCK);
        if (writer >= 0) break;
        if (errno != ENXIO || time(NULL) > deadline) {
            expect(0, "fifo writer");
            kill(collector, SIGKILL);
            goto done;
        }
        sleep_ms(10);
    }
    kill(sleeper, SIGTERM);
    waitpid(sleeper, NULL, 0);
    sleeper = -1;
    text = mapping(1, NULL, NULL, NULL, 0);
    if (write(writer, text, strlen(text)) != (ssize_t)strlen(text)) expect(0, "fifo write");
    free(text);
    close(writer);
    writer = -1;
    {
        time_t end = time(NULL) + 5;
        int reaped = 0;
        while (!reaped) {
            pid_t got = waitpid(collector, &status, WNOHANG);
            if (got == collector) reaped = 1;
            else if (time(NULL) > end) {
                kill(collector, SIGKILL);
                waitpid(collector, &status, 0);
                reaped = 1;
                expect(0, "collector timeout");
            } else sleep_ms(10);
        }
    }
    out = read_all(outp[0], NULL);
    err = read_all(errp[0], NULL);
    expect(WIFEXITED(status) && WEXITSTATUS(status) == 0, err ? err : "collector status");
    expect(out && strstr(out, "1 read") != NULL, "vanish read");
    expect(out && strstr(out, "1 vanished") != NULL, "vanish vanished");
    expect(out && strstr(out, "PARTIAL") != NULL, "vanish partial");
done:
    if (writer >= 0) close(writer);
    if (sleeper > 0) {
        kill(sleeper, SIGKILL);
        waitpid(sleeper, NULL, 0);
    }
    if (outp[0] >= 0) close(outp[0]);
    if (errp[0] >= 0) close(errp[0]);
    free(out);
    free(err);
}

static int line_has(const char *out, const char *needle, const char *value)
{
    const char *p = out;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        char *line = xmalloc(n + 1);
        int hit;
        memcpy(line, p, n);
        line[n] = '\0';
        hit = strstr(line, needle) && strstr(line, value);
        free(line);
        if (hit) return 1;
        if (!nl) break;
        p = nl + 1;
    }
    return 0;
}

static void test_live_cow(void)
{
    char *dir = directory();
    char path[PATH_MAX];
    mapped m;
    char reply[64];
    long before, after;
    char *root, *out;
    if (!dir) {
        expect(0, "cow dir");
        return;
    }
    snprintf(path, sizeof path, "%s/memscope-live-fixture", dir);
    m = start_mapped("cow", path);
    expect(m.pid > 0, "cow ready");
    if (m.pid < 0) return;
    root = live_root(&m.pid, 1);
    expect(mapped_command(&m, "pss", reply, sizeof reply) == 0, "pss before");
    before = strtol(reply, NULL, 10);
    expect(before >= 8000 && before <= 8300, "shared pss range");
    expect(mapped_command(&m, "copy", reply, sizeof reply) == 0 && !strcmp(reply, "copied"), "copy");
    expect(mapped_command(&m, "pss", reply, sizeof reply) == 0, "pss after");
    after = strtol(reply, NULL, 10);
    expect(after >= 16000 && after <= 16600, "copied pss range");
    expect(mapped_command(&m, "unlink", reply, sizeof reply) == 0 && !strcmp(reply, "unlinked"), "unlink");
    {
        char *args[] = {"--limit", "1000", NULL};
        out = output(root, args);
    }
    expect(strstr(out, "[deleted]") != NULL, "live deleted");
    expect(line_has(out, "memscope-live-fixture", "16 MiB"), "live private size");
    free(out);
    mapped_stop(&m);
}

static void test_live_shared(void)
{
    char *dir = directory();
    char path[PATH_MAX];
    mapped a, b;
    pid_t pids[2];
    char *root, *out;
    char *args[] = {"--limit", "1000", NULL};
    if (!dir) return;
    snprintf(path, sizeof path, "%s/shared-live-fixture", dir);
    a = start_mapped("shared", path);
    b = start_mapped("shared", path);
    expect(a.pid > 0 && b.pid > 0, "shared ready");
    if (a.pid < 0 || b.pid < 0) {
        if (a.pid > 0) mapped_stop(&a);
        if (b.pid > 0) mapped_stop(&b);
        return;
    }
    pids[0] = a.pid;
    pids[1] = b.pid;
    root = live_root(pids, 2);
    out = output(root, args);
    expect(line_has(out, "shared-live-fixture", "8 MiB"), "shared not doubled");
    expect(strstr(out, "2 read") != NULL, "two readers");
    free(out);
    mapped_stop(&a);
    mapped_stop(&b);
}

static void test_live_memfd(void)
{
    mapped m = start_mapped("memfd", "unused");
    char *root, *out;
    char *args[] = {"--limit", "1000", NULL};
    expect(m.pid > 0, "memfd ready");
    if (m.pid < 0) return;
    root = live_root(&m.pid, 1);
    out = output(root, args);
    expect(strstr(out, "memfd:memscope-memfd") != NULL, "memfd name");
    expect(strstr(out, "[shmem]") != NULL, "memfd shmem");
    free(out);
    mapped_stop(&m);
}

static int resolve_sibling(char *out, size_t cap, const char *name, const char *env_name)
{
    const char *env = getenv(env_name);
    char exe[PATH_MAX];
    ssize_t n;
    char *slash;
    if (env && *env) {
        if (strlen(env) >= cap) return -1;
        memcpy(out, env, strlen(env) + 1);
        return 0;
    }
    n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n < 0) return -1;
    exe[n] = '\0';
    slash = strrchr(exe, '/');
    if (!slash) return -1;
    slash[1] = '\0';
    if (strlen(exe) + strlen(name) >= cap) return -1;
    memcpy(out, exe, strlen(exe));
    memcpy(out + strlen(exe), name, strlen(name) + 1);
    return 0;
}

int main(void)
{
    if (resolve_sibling(bin, sizeof bin, "memscope", "MEMSCOPE_BIN") != 0
        || resolve_sibling(helper, sizeof helper, "test_support", "MEMSCOPE_FIXTURE") != 0) {
        fputs("could not locate memscope binaries\n", stderr);
        return 1;
    }
    test_headline();
    test_identity();
    test_private_copies();
    test_special();
    test_shmem();
    test_future_fields();
    test_overflow();
    test_clipped();
    test_comm_newline();
    test_full_paths();
    test_missing_available();
    test_invalid_total();
    test_malformed();
    test_empty_tree();
    test_ellipsis();
    test_zero_distinct();
    test_permission();
    test_injection();
    test_color_cli();
    test_pty();
    test_vanish();
    test_live_cow();
    test_live_shared();
    test_live_memfd();
    cleanup_roots();
    if (failures) {
        fprintf(stderr, "%d memscope checks failed\n", failures);
        return 1;
    }
    fputs("memscope behavior checks passed\n", stderr);
    return 0;
}
