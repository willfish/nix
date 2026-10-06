/* Manual regression for the speech parser's partially grown object arrays.
 * Include the production parser to inject allocation failures without adding
 * allocator hooks to the installed library. */
#define _GNU_SOURCE
#include <stdlib.h>
#include <errno.h>

static size_t allocations, fail_at;
static void *checked_realloc(void *ptr, size_t size) {
    if (++allocations == fail_at) { errno = ENOMEM; return NULL; }
    return realloc(ptr, size);
}
#define realloc checked_realloc
#include "../src/audio.c"
#undef realloc

int main(void) {
    const char source[] = "{\"first\":1,\"second\":2,\"third\":3}";
    const char *cursor = source;
    json_value *value = parse_value(&cursor, source + strlen(source), 0);
    if (!value) return 1;
    json_free(value);
    size_t count = allocations;
    for (size_t i = 1; i <= count; i++) {
        fail_at = i;
        allocations = 0;
        cursor = source;
        value = parse_value(&cursor, source + strlen(source), 0);
        if (value) { json_free(value); return 1; }
    }
    printf("Speech JSON allocation failures: %zu checked\n", count);
    return 0;
}
