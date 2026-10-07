#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <yyjson.h>

static int append_actions(const char *home, int argc, char **argv) {
  char *path = NULL;
  int n = asprintf(&path, "%s/actions", home);
  if (n < 0)
    return 1;
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *arr = yyjson_mut_arr(doc);
  yyjson_mut_doc_set_root(doc, arr);
  for (int i = 1; i < argc; i++)
    yyjson_mut_arr_add_strcpy(doc, arr, argv[i]);
  size_t len = 0;
  char *text = yyjson_mut_write(doc, 0, &len);
  yyjson_mut_doc_free(doc);
  if (!text) {
    free(path);
    return 1;
  }
  int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  free(path);
  if (fd < 0) {
    free(text);
    return 1;
  }
  int rc = 0;
  if (write(fd, text, len) != (ssize_t)len || write(fd, "\n", 1) != 1)
    rc = 1;
  free(text);
  if (close(fd))
    rc = 1;
  return rc;
}
static int write_clients(const char *home, const char *mode) {
  if (!strcmp(mode, "bad"))
    return fputs("bad", stdout) < 0;
  if (!strcmp(mode, "invalid")) {
    unsigned char b = 255;
    return fwrite(&b, 1, 1, stdout) != 1;
  }
  char *path = NULL;
  if (asprintf(&path, "%s/clients.json", home) < 0)
    return 1;
  FILE *in = fopen(path, "rb");
  free(path);
  if (!in)
    return 1;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, in)))
    if (fwrite(buf, 1, n, stdout) != n) {
      fclose(in);
      return 1;
    }
  fclose(in);
  if (!strcmp(mode, "nul")) {
    unsigned char tail[] = {0, 't', 'a', 'i', 'l'};
    if (fwrite(tail, 1, sizeof tail, stdout) != sizeof tail)
      return 1;
  }
  return 0;
}
int main(int argc, char **argv) {
  const char *home = getenv("HOME");
  const char *mode = getenv("HYPR_FAKE_MODE");
  if (!home)
    return 2;
  if (!mode)
    mode = "normal";
  if (append_actions(home, argc, argv))
    return 1;
  if (argc > 1 && !strcmp(argv[1], "clients")) {
    if (!strcmp(mode, "stall"))
      sleep(12);
    else if (!strcmp(mode, "exit"))
      return 4;
    else if (write_clients(home, mode))
      return 1;
  }
  if (argc > 1 && !strcmp(argv[1], "dispatch") && !strcmp(mode, "dispatch-stall"))
    sleep(12);
  return 0;
}
