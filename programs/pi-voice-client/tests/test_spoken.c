#include "spoken.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void expect(const char *name, const char *input, const char *want) {
    char got[2048];
    if (spoken_text(input, got, sizeof got) != 0 || strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL %s\n  got:  [%s]\n  want: [%s]\n", name, got, want);
        failures++;
    }
}

int test_spoken(void) {
    failures = 0;
    expect("null", NULL, "");
    expect("empty", "", "");
    expect("prose", "Just read the source.", "");
    expect("not a label", "Spoken summary is a feature, not a label.", "");
    expect("summary of", "Summary of the work is not a label.", "");
    expect("fenced backtick", "```md\n## Spoken summary\nExample only.\n```", "");
    expect("fenced tilde", "~~~~md\n## Spoken summary\nExample only.\n~~~~", "");
    expect("quoted", "> ## Spoken summary\n> A quoted example.", "");
    expect("empty section", "## Spoken summary\n", "");
    expect("only fence", "## Spoken summary\n```sh\nexit\n```", "");
    expect("later heading", "## Spoken summary\nShort.\n## Details\nLong answer.", "");
    expect("setext", "## Spoken summary\nShort.\nDetails\n-------\nDetails.", "");

    char words[1024];
    size_t n = 0;
    strcpy(words, "## Spoken summary\n");
    n = strlen(words);
    for (int i = 0; i < 121; i++) n += (size_t)snprintf(words + n, sizeof words - n, "word ");
    expect("too many words", words, "");

    char long_text[1600];
    strcpy(long_text, "## Spoken summary\n");
    memset(long_text + strlen(long_text), 'a', 1501);
    long_text[strlen("## Spoken summary\n") + 1501] = 0;
    expect("too long", long_text, "");

    expect("code and link",
        "Screen-only details.\n\n## Spoken summary\n"
        "**Fixed** the [login](https://example.test).\n"
        "```sh\nrm -rf example\n```\nTests passed.",
        "Fixed the login. Tests passed.");
    expect("lists",
        "## Spoken summary\n- **Fixed** the issue.\n2. Tests passed.",
        "Fixed the issue. Tests passed.");
    expect("final wins",
        "## TL;DR\nEarlier overview.\n## Details\nScreen only.\n"
        "````md\n```\n## Spoken summary\nFake.\n```\n````\n"
        "## Spoken summary\nThe final result.",
        "The final result.");

    const char *labels[] = {
        "## Spoken summary", "Spoken summary:", "**Spoken summary:**",
        "## Summary", "Summary:", "**Summary:**", "### TL;DR",
        "**TL;DR**:", "TLDR:", "tl;dr:",
    };
    for (size_t i = 0; i < sizeof labels / sizeof labels[0]; i++) {
        char input[256];
        const char *sep = labels[i][strlen(labels[i]) - 1] == ':' ? " " : "\n";
        snprintf(input, sizeof input,
            "# Full response\nDo not speak these details.\n\n%s%sIt worked. Next, review it.",
            labels[i], sep);
        expect(labels[i], input, "It worked. Next, review it.");
        if (sep[0] == ' ') {
            snprintf(input, sizeof input,
                "# Full response\nDo not speak these details.\n\n%s\nIt worked. Next, review it.",
                labels[i]);
            expect(labels[i], input, "It worked. Next, review it.");
        }
    }
    expect("fence closing whitespace", "## Summary\n```\nexample\n```  \nSpeak this.", "Speak this.");
    expect("symbols only", "## Summary\n\U0001f600\u2026", "");
    expect("nested list prefixes", "## Summary\n> > hello\n- - world", "> hello - world");
    expect("Unicode whitespace", "## Summary\none\u00a0two\u2003three", "one two three");
    expect("invalid UTF-8", "## Summary\n\xff", "");
    char tiny[32];
    if (spoken_text(long_text, tiny, sizeof tiny) != 0 || tiny[0] ||
        spoken_text("## Summary\nA valid summary too large for this buffer.", tiny, sizeof tiny) != -1 || tiny[0]) {
        fprintf(stderr, "FAIL truncated summary accepted\n");
        failures++;
    }
    char unicode[1801] = "", unicode_input[1900];
    for (int i = 0; i < 600; i++) strcat(unicode, "\u754c");
    snprintf(unicode_input, sizeof unicode_input, "## Summary\n%s", unicode);
    expect("character not byte bound", unicode_input, unicode);
    char *many_lines = malloc(20000);
    if (!many_lines) return failures + 1;
    strcpy(many_lines, "## Summary\nEarlier.\n");
    for (int i = 0; i < 4100; i++) strcat(many_lines, "\n");
    strcat(many_lines, "## Details\nNot a final summary.");
    expect("final section beyond line limit", many_lines, "");
    strcat(many_lines, "\n## Summary\nFinal result.");
    expect("final label beyond line limit", many_lines, "Final result.");
    free(many_lines);
    return failures;
}
