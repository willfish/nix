#include <stdio.h>

int test_spoken(void);

int main(void) {
    fprintf(stderr, "voice-c test driver ready\n");
    int failed = test_spoken();
    if (failed) fprintf(stderr, "%d spoken tests failed\n", failed);
    return failed ? 1 : 0;
}
