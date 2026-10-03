#include "spoken.h"

#include <stdio.h>
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
    return failures;
}
