#include <stdio.h>

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

int main(void) {
    fprintf(stderr, "voice-c test driver ready\n");
    int failed = test_spoken() + test_chunks() + test_attachments() + test_engines()
        + test_capture() + test_devices() + test_audio() + test_labels()
        + test_osd() + test_pill();
    if (failed) fprintf(stderr, "%d spoken tests failed\n", failed);
    return failed ? 1 : 0;
}
