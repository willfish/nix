#include "common.h"
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static bool luminance(const char *s, double *out) {
  if (strlen(s) != 7 || s[0] != '#')
    return false;
  const double weights[] = {0.2126, 0.7152, 0.0722};
  *out = 0;
  for (size_t i = 0; i < 3; i++) {
    int a = g_ascii_xdigit_value(s[1 + i * 2]),
        b = g_ascii_xdigit_value(s[2 + i * 2]);
    if (a < 0 || b < 0)
      return false;
    double v = (a * 16 + b) / 255.0;
    *out +=
        (v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4)) * weights[i];
  }
  return true;
}
static int invalid(const char *message) {
  fprintf(stderr, "contrast: %s\n", message);
  return 2;
}
int main(int argc, char **argv) {
  const struct option options[] = {{"minimum", required_argument, NULL, 'm'},
                                   {"help", no_argument, NULL, 'h'},
                                   {NULL, 0, NULL, 0}};
  double minimum = 4.5;
  int opt;
  opterr = 0;
  while ((opt = getopt_long(argc, argv, "h", options, NULL)) != -1) {
    if (opt == 'h') {
      puts("usage: contrast foreground background [--minimum 4.5]\nCheck one "
           "opaque sRGB pair, not accessibility or rendered composition.");
      return 0;
    }
    if (opt != 'm' || !number_arg(optarg, &minimum))
      return invalid("invalid minimum or arguments");
  }
  if (argc - optind != 2)
    return invalid("foreground and background required");
  if (!isfinite(minimum) || minimum < 1 || minimum > 21)
    return invalid("minimum must be finite and between 1 and 21");
  double a, b;
  if (!luminance(argv[optind], &a) || !luminance(argv[optind + 1], &b))
    return invalid("use an opaque six-digit sRGB colour such as #18212b");
  double value = (fmax(a, b) + 0.05) / (fmin(a, b) + 0.05);
  bool passed = value >= minimum;
  printf("%s %.6f:1; minimum %g:1 (opaque sRGB pair only)\n",
         passed ? "PASS" : "FAIL", value, minimum);
  return passed ? 0 : 1;
}
