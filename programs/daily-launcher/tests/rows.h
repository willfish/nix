#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
  char *name, *cwd, *input;
  char **args, **herdr;
  char *stdio[3];
  char *env_key[8], *env_val[8];
  int argc, herdr_n, env_n, pid, pgrp, sid;
  size_t input_len;
  bool inherited, has_input;
} Row;
typedef struct {
  Row *items;
  size_t len;
} Rows;
static int read_all(int fd, void *data, size_t n) {
  char *p = data;
  size_t off = 0;
  while (off < n) {
    ssize_t r = read(fd, p + off, n - off);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (!r)
      return -1;
    off += (size_t)r;
  }
  return 0;
}
static int read_u32(int fd, uint32_t *v) { return read_all(fd, v, sizeof *v); }
static int read_i32(int fd, int32_t *v) { return read_all(fd, v, sizeof *v); }
static char *read_blob(int fd, uint32_t *n_out) {
  uint32_t n = 0;
  if (read_u32(fd, &n) < 0)
    return NULL;
  char *buf = g_malloc(n + 1);
  if (n && read_all(fd, buf, n) < 0) {
    g_free(buf);
    return NULL;
  }
  buf[n] = 0;
  if (n_out)
    *n_out = n;
  return buf;
}
static void row_clear(Row *row) {
  g_free(row->name);
  g_free(row->cwd);
  g_free(row->input);
  g_strfreev(row->args);
  g_strfreev(row->herdr);
  for (int i = 0; i < 3; i++)
    g_free(row->stdio[i]);
  for (int i = 0; i < row->env_n; i++) {
    g_free(row->env_key[i]);
    g_free(row->env_val[i]);
  }
}
static Rows load_rows(const char *path) {
  Rows rows = {0};
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return rows;
  for (;;) {
    uint32_t magic = 0;
    if (read_u32(fd, &magic) < 0)
      break;
    if (magic != 0x31465744) {
      close(fd);
      return rows;
    }
    Row row = {0};
    uint32_t argc = 0, env_n = 0, herdr_n = 0, inherited = 0, has_input = 0;
    row.name = read_blob(fd, NULL);
    if (!row.name || read_u32(fd, &argc) < 0)
      break;
    row.args = g_new0(char *, argc + 1);
    row.argc = (int)argc;
    bool bad = false;
    for (uint32_t i = 0; i < argc; i++) {
      row.args[i] = read_blob(fd, NULL);
      bad = bad || !row.args[i];
    }
    row.cwd = read_blob(fd, NULL);
    int32_t pid = 0, pgrp = 0, sid = 0;
    bad = bad || !row.cwd || read_i32(fd, &pid) < 0 || read_i32(fd, &pgrp) < 0 ||
          read_i32(fd, &sid) < 0;
    row.pid = pid;
    row.pgrp = pgrp;
    row.sid = sid;
    for (int i = 0; i < 3; i++) {
      row.stdio[i] = read_blob(fd, NULL);
      bad = bad || !row.stdio[i];
    }
    bad = bad || read_u32(fd, &env_n) < 0 || env_n > 8;
    row.env_n = (int)env_n;
    for (uint32_t i = 0; i < env_n; i++) {
      row.env_key[i] = read_blob(fd, NULL);
      row.env_val[i] = read_blob(fd, NULL);
      bad = bad || !row.env_key[i] || !row.env_val[i];
    }
    bad = bad || read_u32(fd, &herdr_n) < 0;
    row.herdr = g_new0(char *, herdr_n + 1);
    row.herdr_n = (int)herdr_n;
    for (uint32_t i = 0; i < herdr_n; i++) {
      row.herdr[i] = read_blob(fd, NULL);
      bad = bad || !row.herdr[i];
    }
    uint32_t input_len = 0;
    bad = bad || read_u32(fd, &inherited) < 0 || read_u32(fd, &has_input) < 0;
    row.inherited = inherited != 0;
    row.has_input = has_input != 0;
    if (!bad && has_input) {
      row.input = read_blob(fd, &input_len);
      row.input_len = input_len;
      bad = !row.input && input_len;
    }
    if (bad) {
      row_clear(&row);
      break;
    }
    rows.items = g_realloc(rows.items, (rows.len + 1) * sizeof *rows.items);
    rows.items[rows.len++] = row;
  }
  close(fd);
  return rows;
}
static void rows_free(Rows rows) {
  for (size_t i = 0; i < rows.len; i++)
    row_clear(&rows.items[i]);
  g_free(rows.items);
}
static const char *env_get(const Row *row, const char *key) {
  for (int i = 0; i < row->env_n; i++)
    if (!strcmp(row->env_key[i], key))
      return row->env_val[i];
  return NULL;
}
static bool args_match(const Row *row, const char **expect, int n) {
  if (row->argc != n)
    return false;
  for (int i = 0; i < n; i++)
    if (strcmp(row->args[i], expect[i]))
      return false;
  return true;
}
static bool herdr_has(const Row *row, const char *key) {
  for (int i = 0; i < row->herdr_n; i++)
    if (!strcmp(row->herdr[i], key))
      return true;
  return false;
}
