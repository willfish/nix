#pragma once
#include "../repo-data/common.h"
#include <gio/gio.h>
#include <math.h>
char *decimal_text(const char *s);
bool number_arg(const char *s, double *out);
char *replace_all(const char *s, const char *from, const char *to);
int youtube_main(int argc, char **argv);
