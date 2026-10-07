#define _POSIX_C_SOURCE 200809L
#include "memscope.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

static void expect(int cond, const char *msg)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", msg);
        failures++;
    }
}

static char *make_root(void)
{
    char tmpl[] = "/tmp/memscope-proc-XXXXXX";
    char *dir = mkdtemp(tmpl);
    char *copy;
    if (!dir) return NULL;
    copy = malloc(strlen(dir) + 1);
    if (!copy) return NULL;
    memcpy(copy, dir, strlen(dir) + 1);
    return copy;
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

static int add_proc(const char *root, const char *pid, const char *comm, const char *smaps)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", root, pid);
    if (mkdir(path, 0700) != 0 && errno != EEXIST) return -1;
    snprintf(path, sizeof path, "%s/%s/comm", root, pid);
    if (write_file(path, comm) != 0) return -1;
    snprintf(path, sizeof path, "%s/%s/smaps", root, pid);
    return write_file(path, smaps);
}

static char *vma(const char *header, const char *pss, const char *flags)
{
    char *out = malloc(1024);
    if (!out) return NULL;
    snprintf(out, 1024,
             "%s\nSize: 100 kB\nRss: 80 kB\nPss: %s kB\nAnonymous: 7 kB\n"
             "AnonHugePages: 0 kB\nFilePmdMapped: 0 kB\n"
             "Shared_Hugetlb: 2048 kB\nPrivate_Hugetlb: 0 kB\nFutureField: weird\n"
             "VmFlags: %s\n",
             header, pss, flags);
    return out;
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
    /* Snapshot names first. Unlinking the entry just returned by readdir can skip the next one. */
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
        copy = malloc(strlen(ent->d_name) + 1);
        if (!copy) break;
        memcpy(copy, ent->d_name, strlen(ent->d_name) + 1);
        names[n++] = copy;
    }
    closedir(dir);
    for (i = 0; i < n; i++) {
        size_t len = strlen(path) + 1 + strlen(names[i]);
        char *child = malloc(len + 1);
        if (child) {
            snprintf(child, len + 1, "%s/%s", path, names[i]);
            remove_tree(child);
            free(child);
        }
        free(names[i]);
    }
    free(names);
    rmdir(path);
}

static Mapping *find_inode(Sample *s, uint64_t inode)
{
    size_t i;
    for (i = 0; i < s->map_count; i++)
        if (s->maps[i].inode == inode) return &s->maps[i];
    return NULL;
}

static void test_aggregate(void)
{
    char *root = make_root();
    char err[128] = {0};
    Sample s = {0};
    char *a, *b, *joined;
    Mapping *m;
    expect(root != NULL, "temp root");
    a = vma("10000000-20000000 r--p 00000000 08:01 7 /models/my weights.gguf", "10", "rd mr");
    b = vma("20000000-30000000 r--p 00001000 08:01 7 /models/my weights.gguf", "6", "rd ex");
    joined = malloc(strlen(a) + strlen(b) + 1);
    expect(a && b && joined, "fixture alloc");
    memcpy(joined, a, strlen(a));
    memcpy(joined + strlen(a), b, strlen(b) + 1);
    {
        char path[512];
        snprintf(path, sizeof path, "%s/meminfo", root);
        expect(write_file(path,
                          "MemTotal: 33554432 kB\nMemFree: 1 kB\n"
                          "MemAvailable: 12582912 kB\nAnonPages: 42 kB\nHugetlb: 4096 kB\n") == 0,
               "meminfo");
    }
    expect(add_proc(root, "20", "lat\ner\n", a) == 0, "proc 20");
    expect(add_proc(root, "10", "early\n", joined) == 0, "proc 10");
    expect(collect(root, &s, err, sizeof err) == 0, err[0] ? err : "collect");
    expect(s.total == 33554432 && s.has_available && s.available == 12582912, "meminfo totals");
    expect(s.has_anonymous && s.anonymous == 42 && s.huge_kib == 4096, "anon and huge");
    expect(s.read == 2 && s.map_count == 1, "counts");
    m = find_inode(&s, 7);
    expect(m != NULL, "mapping present");
    if (m) {
        expect(m->pss == 26, "pss sums vmas and processes, ignores rss and hugetlb fields");
        expect(m->major == 0x8 && m->minor == 1, "device identity");
        expect(strcmp(m->path, "/models/my weights.gguf") == 0, "spaces preserved");
        expect(m->users && strstr(m->users, "early") && strstr(m->users, "lat\ner"), "users");
        expect(!m->deleted && !m->shmem, "ordinary file tags");
    }
    expect(s.mapping_pss == 26 && s.special_pss == 0, "mapping total");
    expect(s.invalid == 0, "A-F field names are not headers");
    expect(s.process_count == 2, "both processes");
    sample_free(&s);
    sample_free(&s);
    free(a);
    free(b);
    free(joined);
    remove_tree(root);
    free(root);
}

static void test_alias_deleted_special_and_reject(void)
{
    char *root = make_root();
    char err[128] = {0};
    Sample s = {0};
    char path[512];
    char *good, *alias, *special, *truncated, *bad;
    Mapping *m;
    expect(root != NULL, "temp root");
    snprintf(path, sizeof path, "%s/meminfo", root);
    expect(write_file(path, "MemTotal: 1000 kB\nMemAvailable: 100 kB\n") == 0, "meminfo");
    good = vma("1000-2000 r--p 0 08:01 1 /models/weights.gguf", "10", "rd");
    alias = vma("1000-2000 r--p 0 08:01 1 /alias/model (deleted)", "5", "rd");
    special = vma("3000-4000 rw-s 0 00:05 200 /dev/example", "9", "rd io pf");
    truncated = malloc(strlen(good) + 128);
    expect(good && alias && special && truncated, "alloc");
    snprintf(truncated, strlen(good) + 128, "%s1000-2000 r--p 0 08:01 19 /tmp/partial\nPss: 99 kB\n", good);
    bad = "not a mapping\n";
    expect(add_proc(root, "1", "llama-server\n", good) == 0, "good proc");
    expect(add_proc(root, "2", "second\n", alias) == 0, "alias proc");
    expect(add_proc(root, "3", "device\n", special) == 0, "special proc");
    expect(add_proc(root, "4", "bad\n", truncated) == 0, "truncated proc");
    expect(add_proc(root, "5", "worse\n", bad) == 0, "malformed proc");
    snprintf(path, sizeof path, "%s/6", root);
    expect(mkdir(path, 0700) == 0, "vanished pid dir");
    snprintf(path, sizeof path, "%s/7", root);
    expect(mkdir(path, 0700) == 0, "empty pid dir");
    snprintf(path, sizeof path, "%s/7/smaps", root);
    expect(write_file(path, "") == 0, "empty smaps");
    expect(collect(root, &s, err, sizeof err) == 0, err[0] ? err : "collect aliases");
    expect(s.read == 3 && s.invalid == 2 && s.vanished == 1 && s.empty == 1, "status counts");
    expect(s.denied == 0, "missing file is not denied");
    m = find_inode(&s, 1);
    expect(m != NULL, "aliased mapping");
    if (m) {
        expect(m->pss == 15, "alias pss merged by inode");
        expect(m->deleted, "deleted tag");
        expect(strstr(m->path, "/models/weights.gguf") != NULL, "preferred path kept");
        expect(strstr(m->path, "/alias/model") != NULL, "alias path kept");
        expect(m->path[strlen(m->path) - strlen("/models/weights.gguf")] == '/'
               || strstr(m->path, "/models/weights.gguf") == m->path
                      + strlen(m->path) - strlen("/models/weights.gguf"),
               "preferred path is the basename suffix");
        expect(strcmp(m->path + strlen(m->path) - strlen("/models/weights.gguf"),
                      "/models/weights.gguf") == 0,
               "suffix is the non-deleted path");
        expect(m->users && strstr(m->users, "llama-server") && strstr(m->users, "second"),
               "both users");
        expect(m->major == 0x8 && m->minor == 1 && m->inode == 1, "identity not path");
    }
    expect(find_inode(&s, 200) == NULL, "special mapping excluded");
    expect(s.special_count == 1 && s.special_pss == 9, "special accounted aside");
    expect(s.mapping_pss == 15, "eligible total excludes special and rejected");
    expect(s.process_count == 3, "rejected process omitted");
    sample_free(&s);
    free(good);
    free(alias);
    free(truncated);
    free(special);
    remove_tree(root);
    free(root);
}

static void test_shmem_and_meminfo_errors(void)
{
    char *root = make_root();
    char err[128];
    Sample s = {0};
    char path[512];
    char *shmem;
    Mapping *m;
    const char *bads[] = {
        "MemTotal: 0 kB\n",
        "MemTotal: 18446744073709551616 kB\n",
        "MemTotal: -1 kB\n",
        "MemTotal: 32 MB\n",
        "MemFree: 10 kB\n"
    };
    size_t i;
    expect(root != NULL, "temp root");
    snprintf(path, sizeof path, "%s/meminfo", root);
    expect(write_file(path, "MemTotal: 1000 kB\nMemFree: 5 kB\n") == 0, "no available");
    shmem = vma("1000-2000 rw-s 0 00:01 9 /memfd:example (deleted)", "4", "rd wr");
    expect(add_proc(root, "8", "shared\n", shmem) == 0, "shmem proc");
    err[0] = 0;
    expect(collect(root, &s, err, sizeof err) == 0, "collect without available");
    expect(!s.has_available && s.available == 0 && s.total == 1000, "no MemFree fallback");
    m = find_inode(&s, 9);
    expect(m && m->shmem && m->deleted && m->uncertain, "shmem tagged uncertain");
    expect(m && strcmp(m->path, "/memfd:example") == 0, "deleted suffix stripped");
    expect(s.mapping_pss == 4, "anonymous field not subtracted");
    sample_free(&s);
    for (i = 0; i < sizeof bads / sizeof bads[0]; i++) {
        err[0] = 0;
        expect(write_file(path, bads[i]) == 0, "bad meminfo write");
        expect(collect(root, &s, err, sizeof err) != 0, "bad meminfo rejected");
        expect(err[0] != 0, "error message set");
        expect(s.maps == NULL && s.map_count == 0, "failed collect leaves no maps");
        sample_free(&s);
    }
    expect(collect(NULL, &s, err, sizeof err) != 0, "null root");
    expect(collect(root, NULL, err, sizeof err) != 0, "null sample");
    free(shmem);
    remove_tree(root);
    free(root);
}

static void test_denied_and_padded_header(void)
{
    char *root = make_root();
    char err[64] = {0};
    Sample s = {0};
    char path[512];
    const char *smaps =
        "5c10df675000-5c10df676000 r--p 00000000 fe:00 46276095"
        "                   /nix/store/foo bar\n"
        "Rss: 999999 kB\nPss: 3 kB\nVmFlags: rd mr mw me sd\n";
    if (geteuid() == 0) {
        remove_tree(root);
        free(root);
        return;
    }
    expect(root != NULL, "temp root");
    snprintf(path, sizeof path, "%s/meminfo", root);
    expect(write_file(path, "MemTotal: 100 kB\n") == 0, "meminfo");
    expect(add_proc(root, "2", "kthreadd\n", "") == 0, "empty kernel thread file");
    expect(add_proc(root, "4", "app\x1b[2Jname\n", smaps) == 0, "padded proc");
    snprintf(path, sizeof path, "%s/4/smaps", root);
    expect(chmod(path, 0) == 0, "chmod");
    expect(collect(root, &s, err, sizeof err) == 0, "collect denied");
    expect(s.denied == 1 && s.empty == 1 && s.read == 0, "denied distinct from empty");
    expect(s.mapping_pss == 0, "denied process not mixed in");
    chmod(path, 0600);
    expect(collect(root, &s, err, sizeof err) == 0, "collect padded");
    expect(s.read == 1 && s.map_count == 1, "padded header accepted");
    if (s.map_count == 1) {
        expect(s.maps[0].major == 0xfe && s.maps[0].minor == 0, "wide device");
        expect(s.maps[0].inode == 46276095 && s.maps[0].pss == 3, "padded pss not rss");
        expect(strcmp(s.maps[0].path, "/nix/store/foo bar") == 0, "padded path spaces");
        expect(s.maps[0].users && strchr(s.maps[0].users, '\x1b') != NULL, "control char kept");
    }
    sample_free(&s);
    remove_tree(root);
    free(root);
}

int main(void)
{
    test_aggregate();
    test_alias_deleted_special_and_reject();
    test_shmem_and_meminfo_errors();
    test_denied_and_padded_header();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("ok");
    return 0;
}
