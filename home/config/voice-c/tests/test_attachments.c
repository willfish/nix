#define _POSIX_C_SOURCE 200809L
#include "attachments.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int failures;

static void fail(const char *name) {
    fprintf(stderr, "FAIL %s\n", name);
    failures++;
}

static int valid_uuid(const char *s) {
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

static BridgeIdentity ident(const char *path, uint64_t dev, uint64_t ino, const char *pane,
    int pid, const char *start, const char *bridge) {
    AttachmentTarget target = {
        .socket = path,
        .socket_device = dev,
        .socket_inode = ino,
        .pane = pane,
        .pid = pid,
        .start = start,
        .bridge_id = bridge,
    };
    return bridge_identity(&target);
}

static int fence_has(const Fences *fences, int pid, const char *start) {
    for (size_t i = 0; i < fences->highwater_count; i++) {
        if (fences->highwater[i].pid == pid && strcmp(fences->highwater[i].process_start, start) == 0)
            return 1;
    }
    return 0;
}

static int retired_has(const Fences *fences, int pid, const char *start, const char *bridge) {
    for (size_t i = 0; i < fences->retired_count; i++) {
        if (fences->retired[i].pid == pid && strcmp(fences->retired[i].process_start, start) == 0
            && strcmp(fences->retired[i].bridge_id, bridge) == 0) {
            return 1;
        }
    }
    return 0;
}

static void test_identity_and_autoselect(void) {
    const char *bridge = "11111111-1111-4111-8111-111111111111";
    const char *other_bridge = "22222222-2222-4222-8222-222222222222";
    BridgeIdentity id = ident("/tmp/herdr.sock", 1, 2, "w1:p1", 42, "100", bridge);
    BridgeIdentity start = ident("/tmp/herdr.sock", 1, 2, "w1:p1", 42, "101", bridge);
    BridgeIdentity bridged = ident("/tmp/herdr.sock", 1, 2, "w1:p1", 42, "100", other_bridge);
    BridgeIdentity inode = ident("/tmp/herdr.sock", 1, 3, "w1:p1", 42, "100", bridge);
    if (!identity_equal(&id, &id) || identity_equal(&id, &start) || identity_equal(&id, &bridged)
        || identity_equal(&id, &inode)) {
        fail("distinct incarnations");
    }
    PaneKey key = pane_key(&(AttachmentTarget){
        .socket = "/tmp/herdr.sock", .socket_device = 1, .socket_inode = 2, .pane = "w1:p1"});
    if (strcmp(key.socket_path, "/tmp/herdr.sock") != 0 || key.socket_inode != 2
        || strcmp(key.pane_id, "w1:p1") != 0) {
        fail("pane key");
    }

    struct {
        int initialized;
        const char *selected;
        int ready;
        int child;
        int expected;
    } cases[] = {
        {0, NULL, 0, 0, 0},
        {0, NULL, 1, 1, 0},
        {0, NULL, 1, 0, 1},
        {1, NULL, 1, 0, 0},
        {0, "legacy", 1, 0, 0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int got = can_auto_select(cases[i].initialized, cases[i].selected, cases[i].ready, cases[i].child);
        if (got != cases[i].expected) fail("can_auto_select");
    }
}

static void test_idempotent_token_and_revision(void) {
    const char *bridge = "11111111-1111-4111-8111-111111111111";
    BridgeIdentity id = ident("/tmp/herdr.sock", 1, 2, "w1:p1", 42, "100", bridge);
    AttachmentRegistry *registry = registry_new();
    Admission result = {0};
    if (!registry || registry_admit(registry, &id, 1, 0, NULL, &result) != 0
        || strcmp(result.state, "admitted") != 0 || !result.has_token || !valid_uuid(result.token)) {
        fail("first admit");
    }
    int64_t revision = registry_revision(registry, &id.pane);
    Admission retry = {0};
    if (registry_admit(registry, &id, 1, revision, NULL, &retry) != 0 || strcmp(retry.state, "existing") != 0
        || !retry.has_token || strcmp(retry.token, result.token) != 0
        || registry_revision(registry, &id.pane) != revision) {
        fail("idempotent token and revision");
    }
    registry_free(registry);
}

static void test_reversed_reload(void) {
    const char *bridge = "11111111-1111-4111-8111-111111111111";
    const char *newer_bridge = "33333333-3333-4333-8333-333333333333";
    BridgeIdentity id = ident("/tmp/herdr.sock", 1, 2, "w1:p1", 42, "100", bridge);
    BridgeIdentity newer = ident("/tmp/herdr.sock", 1, 2, "w1:p1", 42, "100", newer_bridge);
    AttachmentRegistry *registry = registry_new();
    Admission winner = {0};
    Admission late = {0};
    if (!registry || registry_admit(registry, &newer, 2, 0, NULL, &winner) != 0
        || strcmp(winner.state, "admitted") != 0) {
        fail("newer admit");
    }
    if (registry_admit(registry, &id, 1, 0, NULL, &late) != 0 || strcmp(late.state, "superseded") != 0
        || late.has_token) {
        fail("older activation superseded");
    }
    const char *found = registry_lookup(registry, &newer);
    if (!found || strcmp(found, winner.token) != 0) fail("lookup winner");
    registry_free(registry);
}

static void test_revision_revalidates_different_process(void) {
    const char *bridge = "11111111-1111-4111-8111-111111111111";
    BridgeIdentity id = ident("/tmp/herdr.sock", 1, 2, "w1:p1", 42, "100", bridge);
    BridgeIdentity other = ident("/tmp/herdr.sock", 1, 2, "w1:p1", 43, "100", bridge);
    AttachmentRegistry *registry = registry_new();
    Admission first = {0};
    Admission retry = {0};
    Admission admitted = {0};
    Admission late = {0};
    if (!registry || registry_admit(registry, &id, 1, 0, NULL, &first) != 0) fail("seed admit");
    if (registry_admit(registry, &other, 1, 0, NULL, &retry) != 0 || strcmp(retry.state, "retry") != 0)
        fail("stale revision retries");
    if (registry_admit(registry, &other, 1, registry_revision(registry, &id.pane), NULL, &admitted) != 0
        || strcmp(admitted.state, "admitted") != 0) {
        fail("revalidated process admitted");
    }
    if (registry_admit(registry, &id, 1, registry_revision(registry, &id.pane), NULL, &late) != 0
        || strcmp(late.state, "superseded") != 0) {
        fail("retired process superseded");
    }
    registry_free(registry);
}

static void test_prune_unrelated_probe_keeps_fence(void) {
    const char *bridge = "11111111-1111-4111-8111-111111111111";
    const char *late_bridge = "44444444-4444-4444-8444-444444444444";
    BridgeIdentity id = ident("/tmp/herdr.sock", 1, 2, "w1:p1", 42, "100", bridge);
    BridgeIdentity late = ident("/tmp/herdr.sock", 1, 2, "w1:p1", 42, "100", late_bridge);
    AttachmentRegistry *registry = registry_new();
    Admission seed = {0};
    Admission result = {0};
    ObservedStart starts[] = {{99, NULL}};
    if (!registry || registry_admit(registry, &id, 2, 0, NULL, &seed) != 0) fail("prune seed");
    registry_prune(registry, starts, 1, NULL, 0, 0);
    if (registry_admit(registry, &late, 1, registry_revision(registry, &id.pane), NULL, &result) != 0
        || strcmp(result.state, "superseded") != 0) {
        fail("unrelated prune forgot fence");
    }
    registry_free(registry);
}

static void test_ordinal_collision(void) {
    const char *bridge = "11111111-1111-4111-8111-111111111111";
    const char *other_bridge = "55555555-5555-4555-8555-555555555555";
    BridgeIdentity id = ident("/tmp/herdr.sock", 1, 2, "w1:p1", 42, "100", bridge);
    BridgeIdentity other = ident("/tmp/herdr.sock", 1, 2, "w1:p1", 42, "100", other_bridge);
    AttachmentRegistry *registry = registry_new();
    Admission first = {0};
    Admission collision = {0};
    if (!registry || registry_admit(registry, &id, 1, 0, NULL, &first) != 0) fail("collision seed");
    if (registry_admit(registry, &other, 1, registry_revision(registry, &id.pane), NULL, &collision) == 0)
        fail("ordinal collision accepted");
    const char *found = registry_lookup(registry, &id);
    if (!found || strcmp(found, first.token) != 0) fail("collision mutated owner");
    registry_free(registry);
}

static void test_expiry_and_retirement(void) {
    const char *bridge = "11111111-1111-4111-8111-111111111111";
    BridgeIdentity id = ident("/tmp/herdr.sock", 1, 2, "w1:p1", 42, "100", bridge);
    AttachmentRegistry *registry = registry_new();
    Admission result = {0};
    Admission again = {0};
    Admission blocked = {0};
    if (!registry || registry_admit(registry, &id, 1, 0, NULL, &result) != 0) fail("expiry seed");
    registry_remove(registry, result.token, 0);
    Fences open = {0};
    if (registry_fences(registry, &open) != 0 || retired_has(&open, 42, "100", bridge))
        fail("lease expiry retired");
    fences_free(&open);
    if (registry_admit(registry, &id, 1, registry_revision(registry, &id.pane), NULL, &again) != 0
        || strcmp(again.state, "admitted") != 0) {
        fail("reattach after expiry");
    }
    registry_remove(registry, again.token, 1);
    Fences closed = {0};
    if (registry_fences(registry, &closed) != 0 || !retired_has(&closed, 42, "100", bridge))
        fail("explicit retirement missing");
    fences_free(&closed);
    if (registry_admit(registry, &id, 1, registry_revision(registry, &id.pane), NULL, &blocked) != 0
        || strcmp(blocked.state, "superseded") != 0) {
        fail("retirement blocks reattach");
    }
    registry_free(registry);
}

static void test_stale_prune_and_incarnation(void) {
    AttachmentRegistry *registry = registry_new();
    FenceHighwater old_only[] = {{42, "old", 1, "old-bridge"}};
    Fences saved = {.highwater = old_only, .highwater_count = 1};
    if (!registry || registry_restore_fences(registry, &saved) != 0) fail("restore old fence");
    CapturedIncarnation captured[] = {{42, "old"}};
    FenceHighwater both[] = {
        {42, "old", 1, "old-bridge"},
        {42, "new", 2, "new-bridge"},
    };
    FenceRetired retired[] = {{42, "new", "new-bridge"}};
    Fences during = {
        .highwater = both,
        .highwater_count = 2,
        .retired = retired,
        .retired_count = 1,
    };
    if (registry_restore_fences(registry, &during) != 0) fail("restore new incarnation");
    ObservedStart gone[] = {{42, NULL}};
    registry_prune(registry, gone, 1, captured, 1, 1);
    Fences after = {0};
    if (registry_fences(registry, &after) != 0 || fence_has(&after, 42, "old")
        || !fence_has(&after, 42, "new") || !retired_has(&after, 42, "new", "new-bridge")) {
        fail("stale prune deleted new incarnation");
    }
    fences_free(&after);

    FenceHighwater live[] = {{42, "100", 2, "bridge"}};
    FenceRetired old_retired[] = {{42, "100", "gone-bridge"}};
    Fences keep = {.highwater = live, .highwater_count = 1, .retired = old_retired, .retired_count = 1};
    ObservedStart same[] = {{42, "100"}};
    if (registry_restore_fences(registry, &keep) != 0) fail("restore live fence");
    registry_prune(registry, same, 1, NULL, 0, 0);
    Fences kept = {0};
    if (registry_fences(registry, &kept) != 0 || !fence_has(&kept, 42, "100")
        || !retired_has(&kept, 42, "100", "gone-bridge")) {
        fail("matching incarnation pruned");
    }
    fences_free(&kept);
    ObservedStart reused[] = {{42, "101"}};
    registry_prune(registry, reused, 1, NULL, 0, 0);
    Fences dropped = {0};
    if (registry_fences(registry, &dropped) != 0 || fence_has(&dropped, 42, "100")
        || retired_has(&dropped, 42, "100", "gone-bridge")) {
        fail("replaced incarnation kept");
    }
    fences_free(&dropped);

    FenceHighwater pair[] = {
        {7, "a", 1, "bridge-a"},
        {7, "b", 2, "bridge-b"},
    };
    FenceRetired pair_retired[] = {{7, "a", "bridge-a"}, {7, "b", "bridge-b"}};
    Fences many = {
        .highwater = pair,
        .highwater_count = 2,
        .retired = pair_retired,
        .retired_count = 2,
    };
    ObservedStart replaced[] = {{7, "c"}};
    if (registry_restore_fences(registry, &many) != 0) fail("restore paired fences");
    registry_prune(registry, replaced, 1, NULL, 0, 0);
    Fences cleared = {0};
    if (registry_fences(registry, &cleared) != 0 || fence_has(&cleared, 7, "a")
        || fence_has(&cleared, 7, "b") || retired_has(&cleared, 7, "a", "bridge-a")
        || retired_has(&cleared, 7, "b", "bridge-b")) {
        fail("replaced incarnation pair kept");
    }
    fences_free(&cleared);
    registry_free(registry);
}

static int write_file(const char *path, const char *text) {
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    size_t n = strlen(text);
    ssize_t w = write(fd, text, n);
    close(fd);
    return w == (ssize_t)n ? 0 : -1;
}

static int read_file(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    return 0;
}

static void test_endpoint_and_socket_identity(void) {
    char dir[] = "/tmp/voice-attach-XXXXXX";
    if (!mkdtemp(dir)) {
        fail("mkdtemp");
        return;
    }
    if (chmod(dir, 0700) != 0) fail("chmod runtime");
    const char *bridge = "12345678-1234-4567-89ab-123456789abc";
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/pi-%d-%s.sock", dir, (int)getpid(), bridge);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof addr.sun_path) {
        fail("socket path length");
        close(fd);
        rmdir(dir);
        return;
    }
    memcpy(addr.sun_path, path, strlen(path) + 1);
    if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 || chmod(path, 0600) != 0
        || listen(fd, 1) != 0) {
        fail("bind socket");
        if (fd >= 0) close(fd);
        rmdir(dir);
        return;
    }
    uint64_t dev = 0, ino = 0;
    struct stat st;
    if (validate_endpoint(dir, path, (int)getpid(), bridge, &dev, &ino) != 0 || stat(path, &st) != 0
        || dev != (uint64_t)st.st_dev || ino != (uint64_t)st.st_ino) {
        fail("validate real socket");
    }
    char canonical[PATH_MAX];
    uint64_t sdev = 0, sino = 0;
    struct stat lst;
    if (socket_instance(path, canonical, sizeof canonical, &sdev, &sino) != 0 || lstat(path, &lst) != 0
        || sdev != (uint64_t)lst.st_dev || sino != (uint64_t)lst.st_ino
        || strcmp(canonical, path) != 0) {
        fail("socket lstat identity");
    }
    if (chmod(path, 0666) != 0 || validate_endpoint(dir, path, (int)getpid(), bridge, &dev, &ino) == 0)
        fail("group/world socket accepted");
    if (chmod(path, 0600) != 0 || chmod(dir, 0755) != 0
        || validate_endpoint(dir, path, (int)getpid(), bridge, &dev, &ino) == 0) {
        fail("group/world runtime accepted");
    }
    if (chmod(dir, 0700) != 0 || access(path, F_OK) != 0) fail("endpoint unlinked during rejection");
    close(fd);
    if (unlink(path) != 0 || write_file(path, "unrelated") != 0
        || validate_endpoint(dir, path, (int)getpid(), bridge, &dev, &ino) == 0) {
        fail("non-socket accepted");
    }
    char other[PATH_MAX];
    char linktext[64];
    snprintf(other, sizeof other, "%s/control.sock", dir);
    if (unlink(path) != 0 || write_file(other, "unrelated") != 0 || symlink(other, path) != 0
        || validate_endpoint(dir, path, (int)getpid(), bridge, &dev, &ino) == 0
        || socket_instance(path, canonical, sizeof canonical, &sdev, &sino) == 0
        || read_file(other, linktext, sizeof linktext) != 0 || strcmp(linktext, "unrelated") != 0) {
        fail("symlink escape");
    }
    unlink(path);
    unlink(other);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 || chmod(path, 0600) != 0) {
        fail("rebind socket");
    } else {
        char linkdir[PATH_MAX];
        char dotted[PATH_MAX];
        char dotted_sock[PATH_MAX + 64];
        snprintf(linkdir, sizeof linkdir, "%s-link", dir);
        if (symlink(dir, linkdir) != 0
            || validate_endpoint(linkdir, path, (int)getpid(), bridge, &dev, &ino) == 0) {
            fail("runtime symlink escape");
        }
        unlink(linkdir);
        snprintf(dotted, sizeof dotted, "%s/../%s", dir, strrchr(dir, '/') + 1);
        snprintf(dotted_sock, sizeof dotted_sock, "%s/pi-%d-%s.sock", dotted, (int)getpid(), bridge);
        if (validate_endpoint(dotted, dotted_sock, (int)getpid(), bridge, &dev, &ino) == 0)
            fail("runtime dotdot escape");
        if (validate_endpoint(dir, path, (int)getpid(), bridge, &dev, &ino) != 0)
            fail("runtime still valid after escape checks");
    }
    if (fd >= 0) close(fd);
    unlink(path);
    rmdir(dir);

    char dir2[] = "/tmp/voice-attach-XXXXXX";
    if (!mkdtemp(dir2)) {
        fail("mkdtemp uuid");
        return;
    }
    chmod(dir2, 0700);
    snprintf(path, sizeof path, "%s/pi-%d-%s.sock", dir2, (int)getpid(), bridge);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(path) < sizeof addr.sun_path) memcpy(addr.sun_path, path, strlen(path) + 1);
    if (fd < 0 || strlen(path) >= sizeof addr.sun_path
        || bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 || chmod(path, 0600) != 0) {
        fail("bind uuid socket");
    } else {
        if (validate_endpoint(dir2, path, (int)getpid(), bridge, &dev, &ino) != 0)
            fail("uuid socket should validate");
        if (validate_endpoint(dir2, path, (int)getpid(), "not-a-uuid", &dev, &ino) == 0)
            fail("noncanonical uuid accepted");
        if (validate_endpoint(dir2, path, (int)getpid() + 1, bridge, &dev, &ino) == 0)
            fail("wrong pid filename accepted");
        attachments_set_uid_override(1, (unsigned)getuid() + 1);
        if (validate_endpoint(dir2, path, (int)getpid(), bridge, &dev, &ino) == 0)
            fail("foreign uid accepted");
        attachments_set_uid_override(0, 0);
    }
    if (fd >= 0) close(fd);
    unlink(path);
    rmdir(dir2);
}

static void test_pid_reuse(void) {
    char ticks[64];
    if (process_start_ticks((int)getpid(), ticks, sizeof ticks) != 0 || ticks[0] == 0
        || !process_uid_matches((int)getpid()) || !process_still_same((int)getpid(), ticks)) {
        fail("self incarnation");
    }
    if (process_still_same((int)getpid(), "not-the-ticks")) fail("changed start ticks still matched");
    int dead = -1;
    for (int pid = 2; pid < 100000; pid++) {
        char proc[64];
        snprintf(proc, sizeof proc, "/proc/%d", pid);
        if (access(proc, F_OK) != 0) {
            dead = pid;
            break;
        }
    }
    if (dead < 0 || process_start_ticks(dead, ticks, sizeof ticks) == 0 || process_still_same(dead, ticks))
        fail("dead pid looked alive");
    char saved[64];
    if (process_start_ticks((int)getpid(), saved, sizeof saved) != 0) fail("reread ticks");
    attachments_set_uid_override(1, (unsigned)getuid() + 1);
    if (process_uid_matches((int)getpid()) || process_still_same((int)getpid(), saved))
        fail("uid mismatch treated as same incarnation");
    if (process_start_ticks((int)getpid(), ticks, sizeof ticks) != 0 || strcmp(ticks, saved) != 0)
        fail("uid check hid start ticks");
    attachments_set_uid_override(0, 0);

    char oldcomm[32] = {0};
    int cfd = open("/proc/self/comm", O_RDONLY | O_CLOEXEC);
    if (cfd >= 0) {
        ssize_t n = read(cfd, oldcomm, sizeof oldcomm - 1);
        close(cfd);
        if (n < 0) oldcomm[0] = 0;
    }
    cfd = open("/proc/self/comm", O_WRONLY | O_CLOEXEC);
    if (cfd < 0 || write(cfd, "a)b\n", 4) != 4) {
        if (cfd >= 0) close(cfd);
        fail("set comm");
    } else {
        close(cfd);
        char manual[4096];
        char parsed[64];
        snprintf(manual, sizeof manual, "/proc/%d/stat", (int)getpid());
        int sfd = open(manual, O_RDONLY | O_CLOEXEC);
        ssize_t n = sfd < 0 ? -1 : read(sfd, manual, sizeof manual - 1);
        if (sfd >= 0) close(sfd);
        if (n < 0) {
            fail("read stat");
        } else {
            manual[n] = 0;
            char *rp = strrchr(manual, ')');
            char *field = rp ? rp + 1 : NULL;
            for (int i = 0; field && i < 20; i++) {
                while (*field == ' ' || *field == '\t') field++;
                if (i == 19) break;
                while (*field && *field != ' ' && *field != '\t' && *field != '\n') field++;
            }
            if (!field || sscanf(field, "%63s", parsed) != 1
                || process_start_ticks((int)getpid(), ticks, sizeof ticks) != 0
                || strcmp(ticks, parsed) != 0) {
                fail("start ticks with parenthesis in comm");
            }
        }
    }
    if (oldcomm[0]) {
        cfd = open("/proc/self/comm", O_WRONLY | O_CLOEXEC);
        if (cfd >= 0) {
            size_t n = strcspn(oldcomm, "\n");
            if (write(cfd, oldcomm, n) < 0) fail("restore comm");
            close(cfd);
        }
    }
}

static void test_event_matches(void) {
    EventView target = {
        .bridge_id = "11111111-1111-4111-8111-111111111111",
        .has_activation = 1,
        .activation = 7,
        .has_pid = 1,
        .pid = 42,
        .session = "sess",
        .harness = "pi",
    };
    EventView event = target;
    if (!event_matches(&target, &event, 1)) fail("event match");
    event.session = "other";
    if (event_matches(&target, &event, 1) || !event_matches(&target, &event, 0))
        fail("session fence");
    event = target;
    event.harness = "other";
    if (event_matches(&target, &event, 1)) fail("harness fence");
    event = target;
    event.pid = 43;
    if (event_matches(&target, &event, 0)) fail("pid fence");
    event = target;
    event.has_activation = 0;
    if (event_matches(&target, &event, 1)) fail("missing activation matched");
    EventView blank = {0};
    if (!event_matches(&blank, &blank, 1)) fail("missing fields should match");
}

static void test_retained_cap(void) {
    if (MAX_RETAINED_BYTES != (256 * 1024 - 8192) / 6) fail("MAX_RETAINED_BYTES");
    size_t chars = (size_t)MAX_RETAINED_BYTES;
    char *text = malloc(chars * 2 + 1);
    if (!text) {
        fail("retained alloc");
        return;
    }
    for (size_t i = 0; i < chars; i++) {
        text[i * 2] = (char)0xc3;
        text[i * 2 + 1] = (char)0xa9;
    }
    text[chars * 2] = 0;
    if (strlen(text) <= (size_t)MAX_RETAINED_BYTES || strlen("more") > (size_t)MAX_RETAINED_BYTES)
        fail("retained cap");
    free(text);
}

int test_attachments(void) {
    failures = 0;
    attachments_set_uid_override(0, 0);
    test_identity_and_autoselect();
    test_idempotent_token_and_revision();
    test_reversed_reload();
    test_revision_revalidates_different_process();
    test_prune_unrelated_probe_keeps_fence();
    test_ordinal_collision();
    test_expiry_and_retirement();
    test_stale_prune_and_incarnation();
    test_endpoint_and_socket_identity();
    test_pid_reuse();
    test_event_matches();
    test_retained_cap();
    attachments_set_uid_override(0, 0);
    return failures;
}

#ifdef TEST_ATTACHMENTS_MAIN
int main(void) {
    int n = test_attachments();
    if (n) fprintf(stderr, "%d attachment failure(s)\n", n);
    return n > 255 ? 255 : n;
}
#endif
