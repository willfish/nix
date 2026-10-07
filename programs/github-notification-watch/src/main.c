#include "watch.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
  GMainLoop *loop;
  size_t pending;
} Poll;
typedef struct {
  Poll *poll;
  char *url;
} Notice;
static void finished(Capture *capture, void *raw) {
  Notice *notice = raw;
  if (!capture->timed_out)
    open_chosen(capture->output, notice->url);
  capture_free(capture);
  g_free(notice->url);
  if (!--notice->poll->pending)
    g_main_loop_quit(notice->poll->loop);
  g_free(notice);
}
int main(void) {
  const char *home = g_getenv("HOME");
  if (!home)
    home = g_get_home_dir();
  char *path = g_build_filename(
      home, ".local/state/github-notifications/seen.json", NULL);
  yyjson_doc *state =
      yyjson_read_file(path, YYJSON_READ_ALLOW_INF_AND_NAN, NULL, NULL);
  if (state && !yyjson_is_obj(yyjson_doc_get_root(state))) {
    yyjson_doc_free(state);
    g_free(path);
    return 1;
  }
  const char *argv[] = {"gh", "api", "--paginate", "notifications", NULL};
  Capture *fetch = capture_wait(argv, 30000);
  if (!fetch || !fetch->ok || !fetch->output) {
    capture_free(fetch);
    yyjson_doc_free(state);
    g_free(path);
    return 0;
  }
  yyjson_doc *response = yyjson_read(fetch->output, strlen(fetch->output),
                                     YYJSON_READ_ALLOW_INF_AND_NAN);
  capture_free(fetch);
  if (!response) {
    yyjson_doc_free(state);
    g_free(path);
    return 0;
  }
  yyjson_val *items = yyjson_doc_get_root(response);
  yyjson_doc *empty_read = NULL;
  if (yyjson_is_obj(items)) {
    items = field(items, "items");
    if (!truth(items)) {
      empty_read = yyjson_read("[]", 2, 0);
      items = yyjson_doc_get_root(empty_read);
    }
  }
  yyjson_mut_doc *planned = plan(items, yyjson_doc_get_root(state));
  yyjson_doc_free(response);
  yyjson_doc_free(state);
  yyjson_doc_free(empty_read);
  if (!planned) {
    g_free(path);
    return 1;
  }
  char *serialized =
      yyjson_mut_write(planned, YYJSON_WRITE_ALLOW_INF_AND_NAN, NULL);
  yyjson_doc *readable = serialized
                             ? yyjson_read(serialized, strlen(serialized),
                                           YYJSON_READ_ALLOW_INF_AND_NAN)
                             : NULL;
  free(serialized);
  Poll poll = {.loop = g_main_loop_new(NULL, false)};
  int status = readable ? 0 : 1;
  yyjson_val *item;
  size_t i, n;
  yyjson_arr_foreach(field(yyjson_doc_get_root(readable), "announcements"), i,
                     n, item) {
    char **command = notify_command(item);
    if (!command) {
      status = 1;
      break;
    }
    Notice *notice = g_new0(Notice, 1);
    notice->poll = &poll;
    notice->url = g_strdup(yyjson_get_str(field(item, "url")));
    Capture *capture =
        capture_start((const char *const *)command, 60000, finished, notice);
    g_strfreev(command);
    if (!capture) {
      g_free(notice->url);
      g_free(notice);
      status = 1;
      break;
    }
    poll.pending++;
  }
  if (!status &&
      !save_state(path,
                  yyjson_mut_obj_get(yyjson_mut_doc_get_root(planned), "seen")))
    status = 1;
  if (poll.pending)
    g_main_loop_run(poll.loop);
  g_main_loop_unref(poll.loop);
  yyjson_doc_free(readable);
  yyjson_mut_doc_free(planned);
  g_free(path);
  return status;
}
