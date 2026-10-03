#include <stdio.h>

int test_spoken(void);
int test_chunks(void);
int test_attachments(void);
int test_engines(void);

int main(void) {
    fprintf(stderr, "voice-c test driver ready\n");
    int failed = test_spoken() + test_chunks() + test_attachments() + test_engines();
    if (failed) fprintf(stderr, "%d spoken tests failed\n", failed);
    return failed ? 1 : 0;
}
