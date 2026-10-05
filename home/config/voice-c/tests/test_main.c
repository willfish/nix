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
int test_engines(void);
int test_capture(void);
int test_devices(void);
int test_audio(void);
int test_labels(void);
int test_osd(void);
int test_pill(void);

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

int main(void) {
    fprintf(stderr, "voice-c test driver ready\n");
    int failed = test_concurrent_initialization();
    failed += test_spoken() + test_chunks() + test_attachments() + test_engines()
        + test_capture() + test_devices() + test_audio() + test_labels()
        + test_osd() + test_pill();
    if (failed) fprintf(stderr, "%d voice checks failed\n", failed);
    return failed ? 1 : 0;
}
