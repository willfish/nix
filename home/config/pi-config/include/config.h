#pragma once
#include "json.h"
char *config_path(const char *path);
char *config_sibling(const char *path, const char *name);
int config_exists(const char *path);
GString *config_read(const char *path);
GString *config_strip(GString *text);
Json *config_load(const char *path, bool optional);
bool config_save(const char *path, Json *value);
bool config_marker(const char *path);
int config_settings(const char *defaults, const char *settings);
int config_auth(const char *path, GPtrArray *drop, GString *provider,
                const char *refresh, const char *account);
GString *json_argument(const char *text);
