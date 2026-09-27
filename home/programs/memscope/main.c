#define _POSIX_C_SOURCE 200809L
#include "memscope.h"
#include <errno.h>
#include <getopt.h>
#include <langinfo.h>
#include <limits.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void usage(FILE *out)
{
    fputs("Usage: memscope [options]\n"
          "A one-shot Linux memory overview and named-mapping PSS ranking.\n\n"
          "  --limit N              Show N mappings plus Other (default 5)\n"
          "  --full-paths           Show full mapping paths\n"
          "  --color auto|always|never\n"
          "  --ascii                Use ASCII bars\n"
          "  --width N              Output width, 8..240 columns\n"
          "  --proc-root PATH       Read an alternate proc tree (offline fixtures)\n"
          "  --help                 Show this help\n"
          "  --version              Show version\n\n"
          "Used = MemTotal - MemAvailable (kernel estimate). Mapping PSS includes\n"
          "private copies and excludes unmapped cache. File percentages divide by\n"
          "ALL observed eligible mapping PSS, not only the displayed rows.\n"
          "Process PSS overlaps mappings; sections are not additive.\n"
          "Nonempty NO_COLOR overrides --color. Pipes and TERM=dumb default to\n"
          "unstyled ASCII. ANSI colours follow the terminal/Omarchy palette.\n", out);
}

static unsigned number(const char *s, unsigned min, unsigned max)
{
    char *end;
    errno = 0;
    unsigned long n = strtoul(s, &end, 10);
    if (errno || !*s || *end || *s == '-' || n < min || n > max) {
        fprintf(stderr, "memscope: invalid numeric option\n");
        exit(2);
    }
    return (unsigned)n;
}

int main(int argc, char **argv)
{
    setlocale(LC_CTYPE, "");
    bool tty = isatty(STDOUT_FILENO), ascii = false;
    const char *term = getenv("TERM"), *root = "/proc", *color = "auto";
    bool dumb = term && !strcmp(term, "dumb");
    View v = {.width = 80, .limit = 5};
    struct winsize ws;
    if (tty && ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col)
        v.width = ws.ws_col > 240 ? 240 : ws.ws_col;
    if (v.width < 8) v.width = 8;
    static const struct option options[] = {
        {"limit", required_argument, NULL, 'l'},
        {"full-paths", no_argument, NULL, 'p'},
        {"color", required_argument, NULL, 'c'},
        {"ascii", no_argument, NULL, 'a'},
        {"width", required_argument, NULL, 'w'},
        {"proc-root", required_argument, NULL, 'r'},
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'v'},
        {NULL, 0, NULL, 0}
    };
    int option;
    while ((option = getopt_long(argc, argv, "", options, NULL)) != -1) {
        switch (option) {
        case 'l': v.limit = number(optarg, 1, 1000); break;
        case 'p': v.full_paths = true; break;
        case 'c': color = optarg; break;
        case 'a': ascii = true; break;
        case 'w': v.width = number(optarg, 8, 240); break;
        case 'r': root = optarg; break;
        case 'h': usage(stdout); return 0;
        case 'v': puts("memscope 0.1.0"); return 0;
        default: usage(stderr); return 2;
        }
    }
    if (optind != argc || (strcmp(color, "auto") && strcmp(color, "always") && strcmp(color, "never"))) {
        fputs("memscope: invalid option; see --help\n", stderr);
        return 2;
    }
    const char *no_color = getenv("NO_COLOR");
    v.color = (!strcmp(color, "always") || (!strcmp(color, "auto") && tty && !dumb))
              && !(no_color && *no_color);
    v.unicode = tty && !dumb && !ascii && !(no_color && *no_color)
                && !strcmp(nl_langinfo(CODESET), "UTF-8");
    Sample sample = {0};
    char error[256] = {0};
    if (collect(root, &sample, error, sizeof(error)) != 0) {
        fprintf(stderr, "memscope: %s\n", error);
        sample_free(&sample);
        return 1;
    }
    render(stdout, &sample, &v);
    sample_free(&sample);
    return fflush(stdout) == 0 && !ferror(stdout) ? 0 : 1;
}
