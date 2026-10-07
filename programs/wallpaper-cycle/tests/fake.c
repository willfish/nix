#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <yyjson.h>

static char *read_all(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END) < 0) {
    fclose(f);
    return NULL;
  }
  long n = ftell(f);
  if (n < 0) {
    fclose(f);
    return NULL;
  }
  rewind(f);
  char *buf = malloc((size_t)n + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  if (n && fread(buf, 1, (size_t)n, f) != (size_t)n) {
    free(buf);
    fclose(f);
    return NULL;
  }
  buf[n] = 0;
  fclose(f);
  *len = (size_t)n;
  return buf;
}
static int write_file(const char *path, const void *data, size_t n) {
  FILE *f = fopen(path, "wb");
  if (!f)
    return -1;
  if (n && fwrite(data, 1, n, f) != n) {
    fclose(f);
    return -1;
  }
  return fclose(f) == 0 ? 0 : -1;
}
int main(int argc, char **argv) {
  const char *db_path = getenv("CYCLE_FAKE_DB");
  if (!db_path)
    return 2;
  size_t db_len = 0;
  char *db_text = read_all(db_path, &db_len);
  if (!db_text)
    return 2;
  yyjson_doc *db = yyjson_read(db_text, db_len, 0);
  free(db_text);
  if (!db)
    return 2;
  yyjson_val *root = yyjson_doc_get_root(db);
  const char *log = yyjson_get_str(yyjson_obj_get(root, "log"));
  const char *theme = yyjson_get_str(yyjson_obj_get(root, "theme"));
  const char *state = yyjson_get_str(yyjson_obj_get(root, "state"));
  const char *fail = yyjson_get_str(yyjson_obj_get(root, "fail"));
  const char *race = yyjson_get_str(yyjson_obj_get(root, "race"));
  if (!log || !theme || !state || !fail || !race) {
    yyjson_doc_free(db);
    return 2;
  }
  char *copy = strdup(argv[0]);
  if (!copy) {
    yyjson_doc_free(db);
    return 1;
  }
  const char *name = basename(copy);
  yyjson_mut_doc *line = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *arr = yyjson_mut_arr(line);
  yyjson_mut_doc_set_root(line, arr);
  yyjson_mut_arr_add_strcpy(line, arr, name);
  for (int i = 1; i < argc; i++)
    yyjson_mut_arr_add_strcpy(line, arr, argv[i]);
  size_t line_len = 0;
  char *serialized = yyjson_mut_write(line, 0, &line_len);
  yyjson_mut_doc_free(line);
  if (!serialized) {
    free(copy);
    yyjson_doc_free(db);
    return 1;
  }
  FILE *out = fopen(log, "ab");
  if (!out || fwrite(serialized, 1, line_len, out) != line_len ||
      fputc('\n', out) == EOF || fclose(out) != 0) {
    free(serialized);
    free(copy);
    yyjson_doc_free(db);
    return 1;
  }
  free(serialized);
  if (!strcmp(fail, name)) {
    free(copy);
    yyjson_doc_free(db);
    return 17;
  }
  int status = 0;
  if (!strcmp(name, "nix")) {
    if (argc < 4) {
      status = 2;
    } else if (unlink(argv[3]) < 0 && errno != ENOENT) {
      status = 1;
    } else if (symlink(theme, argv[3]) < 0) {
      status = 1;
    }
  } else {
    if (argc < 3 || strlen(argv[2]) < 4) {
      status = 2;
    } else {
      size_t n = 0;
      char *body = read_all(argv[1], &n);
      if (!body) {
        status = 1;
      } else {
        char *png = malloc(n + 5);
        if (!png) {
          free(body);
          status = 1;
        } else {
          memcpy(png, "png:", 4);
          memcpy(png + 4, body, n);
          if (write_file(argv[2] + 4, png, n + 4) < 0)
            status = 1;
          free(png);
          free(body);
        }
      }
      if (!status && !strcmp(race, "link")) {
        char *link = NULL;
        size_t ns = strlen(state), extra = strlen("/wallpaper-source");
        link = malloc(ns + extra + 1);
        if (!link)
          status = 1;
        else {
          memcpy(link, state, ns);
          memcpy(link + ns, "/wallpaper-source", extra + 1);
          if (unlink(link) < 0 && errno != ENOENT)
            status = 1;
          else if (symlink(state, link) < 0)
            status = 1;
          free(link);
        }
      } else if (!status && !strcmp(race, "current")) {
        char *path = NULL;
        size_t ns = strlen(state), extra = strlen("/wallpaper-current");
        path = malloc(ns + extra + 1);
        if (!path)
          status = 1;
        else {
          memcpy(path, state, ns);
          memcpy(path + ns, "/wallpaper-current", extra + 1);
          if (write_file(path, "other.png\n", 10) < 0)
            status = 1;
          free(path);
        }
      }
    }
  }
  free(copy);
  yyjson_doc_free(db);
  return status;
}
