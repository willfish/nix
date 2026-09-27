#ifndef MEMSCOPE_H
#define MEMSCOPE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* All memory quantities are KiB, as reported by procfs. */
typedef struct {
    uint64_t major, minor, inode, pss;
    char *path;
    char *users; /* bounded display summary of process names, never interpreted */
    bool deleted, shmem, uncertain;
} Mapping;
typedef struct {
    unsigned pid;
    char *name;
    uint64_t pss;
} Process;
typedef struct {
    uint64_t total, available, anonymous;
    bool has_available, has_anonymous;
    Mapping *maps;
    size_t map_count;
    Process *processes;
    size_t process_count;
    uint64_t mapping_pss, special_pss, huge_kib;
    size_t read, denied, vanished, invalid, empty, special_count;
    double elapsed;
} Sample;
typedef struct {
    unsigned width, limit;
    bool color, unicode, full_paths;
} View;
/* A custom proc root supports offline fixtures, without privilege escalation. */
int collect(const char *root, Sample *sample, char *error, size_t error_size);
void sample_free(Sample *sample);
void render(FILE *out, const Sample *sample, const View *view);
#endif
