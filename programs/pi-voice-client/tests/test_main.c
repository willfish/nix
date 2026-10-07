#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "spoken.h"
#include "chunks.h"
#include "text.h"

int test_spoken(void);
int test_chunks(void);
int test_attachments(void);
int test_capture(void);
int test_devices(void);
int test_audio(void);
int test_labels(void);
int test_osd(void);
int test_pill(void);
int test_ipc(void);
int test_runtime_adapters(void);
int test_harness(void);
int test_controller(void);
int test_presentation(void);
int test_menu(void);
int test_conversation(void);
int test_osd_main(void);

static void *concurrent_text(void *arg) {
    int *failed = arg;
    for (int i = 0; i < 100; i++) {
        char text[128];
        char *chunks[8] = {0};
        size_t count = 0;
        if (spoken_text("## Summary\nIt worked.", text, sizeof text) || strcmp(text, "It worked.")) (*failed)++;
        if (dictation_text("Hello [music] there.", text, sizeof text) || strcmp(text, "Hello there.")) (*failed)++;
        if (speech_chunks("Hello there.", chunks, &count, 8) || count != 1 || strcmp(chunks[0], "Hello there.")) (*failed)++;
        for (size_t j = 0; j < count; j++) free(chunks[j]);
    }
    return NULL;
}

static int test_concurrent_initialization(void) {
    pthread_t threads[16];
    int results[16] = {0};
    size_t started = 0;
    int failures = 0;
    for (; started < 16; started++) {
        if (pthread_create(&threads[started], NULL, concurrent_text, &results[started])) {
            failures++;
            break;
        }
    }
    for (size_t i = 0; i < started; i++) {
        pthread_join(threads[i], NULL);
        failures += results[i];
    }
    return failures;
}

int main(int argc, char **argv) {
    const struct { const char *name; int (*run)(void); } groups[] = {
        {"concurrent-initialization", test_concurrent_initialization},
        {"spoken", test_spoken}, {"chunks", test_chunks},
        {"attachments", test_attachments},
        {"capture", test_capture}, {"devices", test_devices},
        {"audio", test_audio}, {"labels", test_labels},
        {"osd", test_osd}, {"pill", test_pill}, {"ipc", test_ipc},
        {"runtime-adapters", test_runtime_adapters}, {"harness", test_harness},
        {"controller", test_controller},
        {"presentation", test_presentation},
        {"menu", test_menu}, {"conversation", test_conversation},
        {"osd-main", test_osd_main},
    };
    fprintf(stderr, "voice-c test driver ready\n");
    int failed = 0, ran = 0;
    for (size_t i = 0; i < sizeof groups / sizeof groups[0]; i++) {
        if (argc > 1 && strcmp(argv[1], groups[i].name)) continue;
        fprintf(stderr, "RUN %s\n", groups[i].name);
        int result = groups[i].run();
        fprintf(stderr, "END %s: %d failures\n", groups[i].name, result);
        failed += result;
        ran++;
    }
    if (!ran) {
        fprintf(stderr, "Unknown voice test group\n");
        return 2;
    }
    fprintf(stderr, "%d voice groups, %d checks failed\n", ran, failed);
    return failed ? 1 : 0;
}
