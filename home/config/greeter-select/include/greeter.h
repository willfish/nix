#pragma once
#include <glib.h>
#include <stdbool.h>
#include <stddef.h>
#include <yyjson.h>
bool greeter_id(const unsigned char *data, size_t length);
bool greeter_store(const char *path, size_t length);
int greeter_read(const char *directory, const char *name, char id[65]);
int greeter_launch(const char *path, const char **error);
int greeter_cli(int argc, char **argv);
