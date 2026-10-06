#pragma once
#include <glib.h>
#include <stdbool.h>
#include <stddef.h>
typedef struct {
  const char *data;
  size_t length;
} Text;
GString *adapt_markdown(Text text, bool agent);
int adapt_tree(const char *source, const char *destination, bool agent);
int adapt_cli(int argc, char **argv);
