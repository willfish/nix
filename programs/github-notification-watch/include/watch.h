#pragma once
#include <gio/gio.h>
#include <stdbool.h>
#include <yyjson.h>

typedef struct Capture Capture;
typedef void (*CaptureDone)(Capture *, void *);
struct Capture {
  GSubprocess *process;
  GCancellable *cancel;
  GMainLoop *loop;
  char *output;
  guint timer;
  bool timed_out, ok;
  CaptureDone done;
  void *data;
};
Capture *capture_start(const char *const *argv, guint timeout_ms,
                       CaptureDone done, void *data);
Capture *capture_wait(const char *const *argv, guint timeout_ms);
void capture_free(Capture *capture);
char *html_url(const char *url);
yyjson_val *field(yyjson_val *object, const char *key);
bool truth(yyjson_val *value);
yyjson_mut_doc *plan(yyjson_val *items, yyjson_val *state);
char **notify_command(yyjson_val *item);
bool open_chosen(const char *output, const char *url);
bool save_state(const char *path, yyjson_mut_val *seen);
