#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int write_all(FILE *file, const void *data, size_t len) {
  return fwrite(data, 1, len, file) == len;
}
static int json_string(FILE *out, const char *text, size_t len) {
  if (fputc('"', out) == EOF)
    return -1;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)text[i];
    char escaped[8];
    const char *piece = (const char *)&c;
    size_t n = 1;
    if (c == '"' || c == '\\') {
      escaped[0] = '\\';
      escaped[1] = (char)c;
      piece = escaped;
      n = 2;
    } else if (c < 0x20) {
      int wrote = snprintf(escaped, sizeof escaped, "\\u%04x", c);
      if (wrote < 0)
        return -1;
      piece = escaped;
      n = (size_t)wrote;
    }
    if (!write_all(out, piece, n))
      return -1;
  }
  return fputc('"', out) == EOF ? -1 : 0;
}
int main(int argc, char **argv) {
  const char *receipt = getenv("RECEIPT");
  if (!receipt || argc < 4)
    return 2;
  const char *runner = argv[3];
  FILE *source = fopen(runner, "rb");
  if (!source)
    return 2;
  if (fseek(source, 0, SEEK_END))
    return 2;
  long length = ftell(source);
  if (length < 0 || fseek(source, 0, SEEK_SET))
    return 2;
  char *text = calloc((size_t)length + 1, 1);
  if (!text)
    return 2;
  if (length && fread(text, 1, (size_t)length, source) != (size_t)length)
    return 2;
  fclose(source);
  const char *keys[] = {"PI_SESSION_FILE",    "PI_SESSION_ID",
                        "CAPTURE_PROMPTS",    "HTTP_PROXY",
                        "HTTPS_PROXY",        "http_proxy",
                        "https_proxy",        "SSL_CERT_FILE",
                        "REQUESTS_CA_BUNDLE", "NODE_EXTRA_CA_CERTS"};
  int unset = 1;
  for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
    if (getenv(keys[i]))
      unset = 0;
  FILE *out = fopen(receipt, "wb");
  if (!out)
    return 2;
  fputs("{\"args\":[", out);
  for (int i = 1; i < argc; i++) {
    if (i > 1)
      fputc(',', out);
    if (json_string(out, argv[i], strlen(argv[i])))
      return 2;
  }
  fputs("],\"source\":", out);
  if (json_string(out, text, (size_t)length))
    return 2;
  fprintf(out, ",\"unset\":%s,\"telemetry\":", unset ? "true" : "false");
  const char *telemetry = getenv("PI_TELEMETRY");
  if (!telemetry)
    fputs("null", out);
  else if (json_string(out, telemetry, strlen(telemetry)))
    return 2;
  if (fputc('}', out) == EOF || fclose(out))
    return 2;
  char *dir = strdup(runner);
  if (!dir)
    return 2;
  char *slash = strrchr(dir, '/');
  if (slash)
    *slash = 0;
  size_t dir_len = strlen(dir);
  char *probe = malloc(dir_len + 8);
  int rc = !probe;
  if (!rc) {
    snprintf(probe, dir_len + 8, "%s/add.py", dir);
    FILE *add = fopen(probe, "wb");
    rc = !add || !write_all(add, "SENSITIVE_PROBE_VALUE", 20) || fclose(add);
  }
  free(dir);
  free(probe);
  free(text);
  if (rc)
    return 2;
  fputs("run 20260912T104853-77612\n", stdout);
  return 17;
}
