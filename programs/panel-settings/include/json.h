#pragma once
#include <glib.h>
#include <stdbool.h>
typedef struct Json Json;
char *settings_input(const char *arg, bool *too_long);
Json *settings_parse(const char *input);
void settings_free(Json *value);
bool settings_object(Json *value);
char *settings_encode(Json *value);
