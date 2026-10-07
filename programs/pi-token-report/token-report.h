#pragma once
#include "common.h"
#include <stdint.h>
#include <sys/types.h>
typedef yyjson_mut_doc Doc;
typedef yyjson_mut_val Mut;
typedef struct {
  int64_t value[5];
  unsigned present;
} Usage;
typedef struct {
  double input, output, cached, write;
} Rates;
typedef struct {
  unsigned index;
  char *ts, *url, *model;
  Val *status;
  int64_t chars, schema_chars, messages;
  GPtrArray *advertised, *calls, *results, *queries, *mcp;
  GHashTable *ids, *result_chars;
  Usage usage;
  double cost;
} Request;
typedef struct {
  char *name;
  int64_t unique, wire, results, chars, advertised;
  double cost;
} Tool;
typedef struct {
  GPtrArray *tools, *queries, *mcp;
  int64_t schema_chars;
  double schema_cost;
} Attribution;
typedef struct {
  char *run, *captured_at, *task_result;
  int exit_code;
  bool captured;
} Meta;
const char *token_text(Val *v);
bool token_truth(Val *v);
char *token_home(const char *suffix);
char *token_state(const char *suffix);
char *token_capture(Meta *meta);
bool token_private_file(const char *path, const char *data, size_t length,
                        mode_t mode);
char *token_html(Meta *meta, GPtrArray *requests, Attribution *attr,
                 Rates rates);
extern const char *token_turn_names[3];
extern const char *token_turns[3];
