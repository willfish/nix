#include "chunks.h"
#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void expect_chunks(const char *name, const char *text, const char **want, size_t n) {
    char *got[8] = {0};
    size_t count = 0;
    if (speech_chunks(text, got, &count, 8) != 0 || count != n) {
        fprintf(stderr, "FAIL %s count %zu\n", name, count);
        failures++;
    } else {
        for (size_t i = 0; i < n; i++) {
            if (strcmp(got[i], want[i]) != 0) {
                fprintf(stderr, "FAIL %s chunk %zu\n  got:  [%s]\n  want: [%s]\n",
                    name, i, got[i], want[i]);
                failures++;
            }
        }
    }
    for (size_t i = 0; i < count; i++) free(got[i]);
}

static void expect_text(const char *name, const char *input, const char *want) {
    char got[512];
    if (dictation_text(input, got, sizeof got) != 0 || strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL %s\n  got: [%s]\n  want: [%s]\n", name, got, want);
        failures++;
    }
}

int test_chunks(void) {
    failures = 0;
    const char *ready[] = {
        "The voice is now ready to use. The recorder waits for your microphone before it starts.",
        "Dictation stays in your selected terminal until you send it. You can cancel recording or playback from the tray.",
    };
    expect_chunks("sentences",
        "The voice is now ready to use. The recorder waits for your microphone before it starts. "
        "Dictation stays in your selected terminal until you send it. You can cancel recording or playback from the tray.",
        ready, 2);

    char sentence[256] = "The recorder ";
    for (int i = 0; i < 8; i++) strcat(sentence, "captures your voice ");
    strcat(sentence, "reliably.");
    const char *one[] = {sentence};
    expect_chunks("uncut sentence", sentence, one, 1);

    char clause[128] = "These changes keep ";
    for (int i = 0; i < 4; i++) strcat(clause, "all of your words ");
    strcat(clause, "safe;");
    char long_clause[1024];
    snprintf(long_clause, sizeof long_clause, "%s ", clause);
    for (int i = 0; i < 30; i++) strcat(long_clause, "recognition ");
    char *got[8] = {0};
    size_t count = 0;
    speech_chunks(long_clause, got, &count, 8);
    if (!count || strcmp(got[0], clause) != 0) {
        fprintf(stderr, "FAIL clause\n  got: [%s]\n", count ? got[0] : "");
        failures++;
    }
    for (size_t i = 0; i < count; i++) free(got[i]);

    char word[812];
    memset(word, 'x', 811);
    word[811] = 0;
    char *parts[8] = {0};
    count = 0;
    speech_chunks(word, parts, &count, 8);
    char joined[812] = "";
    int bounded = count > 0 && strlen(parts[0]) == 120;
    for (size_t i = 0; i < count; i++) {
        if (strlen(parts[i]) == 0 || strlen(parts[i]) > 260) bounded = 0;
        strcat(joined, parts[i]);
        free(parts[i]);
    }
    if (!bounded || strcmp(joined, word) != 0) {
        fprintf(stderr, "FAIL oversized joined %zu\n", strlen(joined));
        failures++;
    }

    expect_text("marker", "Please [MUSIC] continue (silence) now.", "Please continue now.");
    expect_text("silence", "[Silence]", "");
    expect_text("zwsp", "Hi\u200b there", "Hi there");
    if (!has_control_characters("\x1b[31m")) {
        fprintf(stderr, "FAIL escape not rejected\n");
        failures++;
    }
    if (has_control_characters("line\n\tmore\r")) {
        fprintf(stderr, "FAIL legal controls rejected\n");
        failures++;
    }
    return failures;
}
