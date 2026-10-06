#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int terminal(int argc, char **argv)
{
    if (argc != 5) return 2;
    int master, slave;
    struct winsize size = {.ws_row = 40, .ws_col = (unsigned short)atoi(argv[2])};
    if (openpty(&master, &slave, NULL, NULL, &size) != 0) return 1;
    pid_t child = fork();
    if (child < 0) return 1;
    if (!child) {
        close(master);
        if (dup2(slave, STDOUT_FILENO) < 0) _exit(126);
        close(slave);
        execl(argv[3], argv[3], "--proc-root", argv[4], (char *)NULL);
        _exit(127);
    }
    close(slave);
    char buffer[65536];
    ssize_t n;
    while ((n = read(master, buffer, sizeof(buffer))) > 0)
        if (fwrite(buffer, 1, (size_t)n, stdout) != (size_t)n) return 1;
    close(master);
    int status;
    if (waitpid(child, &status, 0) < 0) return 1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static unsigned long long pss(const char *path)
{
    FILE *smaps = fopen("/proc/self/smaps", "r");
    assert(smaps);
    char *line = NULL;
    size_t capacity = 0;
    int selected = 0;
    unsigned long long total = 0, value;
    while (getline(&line, &capacity, smaps) >= 0) {
        unsigned long start, end;
        if (sscanf(line, "%lx-%lx", &start, &end) == 2)
            selected = strstr(line, path) != NULL;
        else if (selected && sscanf(line, "Pss: %llu", &value) == 1)
            total += value;
    }
    free(line);
    fclose(smaps);
    return total;
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    alarm(30);
    if (!strcmp(argv[1], "pty")) return terminal(argc, argv);
    if (argc != 3) return 2;
    int cow = !strcmp(argv[1], "cow"), memfd = !strcmp(argv[1], "memfd");
    if (!cow && !memfd && strcmp(argv[1], "shared")) return 2;
    size_t size = (memfd ? 4u : 8u) * 1024 * 1024;
    int fd = memfd ? memfd_create("memscope-memfd", 0) : open(argv[2], O_RDWR | O_CREAT, 0600);
    if (fd < 0 || ftruncate(fd, (off_t)size) != 0) return 1;
    unsigned char *shared = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shared == MAP_FAILED) return 1;
    memset(shared, 'x', size);
    unsigned char *private = NULL;
    if (cow) {
        private = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
        if (private == MAP_FAILED) return 1;
        for (size_t i = 0; i < size; i += 4096) assert(((volatile unsigned char *)private)[i] == 'x');
        (void)madvise(private, size, MADV_NOHUGEPAGE);
    }
    puts("ready");
    fflush(stdout);
    char command[32];
    while (fgets(command, sizeof(command), stdin)) {
        if (!strcmp(command, "pss\n")) printf("%llu\n", pss(argv[2]));
        else if (!strcmp(command, "copy\n") && private) { memset(private, 'y', size); puts("copied"); }
        else if (!strcmp(command, "unlink\n") && !memfd) { assert(unlink(argv[2]) == 0); puts("unlinked"); }
        else break;
        fflush(stdout);
    }
    if (private) munmap(private, size);
    munmap(shared, size);
    close(fd);
    return 0;
}
