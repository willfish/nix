#include "tools.h"
#include <stdlib.h>
#include <string.h>
#include <yyjson.h>
static size_t count(const char *text, const char *needle) {
  size_t n = 0;
  while ((text = strstr(text, needle))) {
    n++;
    text += strlen(needle);
  }
  return n;
}
char *pp_patch(const char *source, const char *guard, char **error) {
  const char *old = "    async def handle_chat(self, request):\n        ws = "
                    "web.WebSocketResponse()";
  const char *replacement =
#include "server-guard.inc"
      ;
  const char *old_root =
      "return web.FileResponse(os.path.join(static_path, \"index.html\"))";
  if (count(source, old) != 1) {
    pp_error(error, "Pinned PersonaPlex server changed; review origin guards");
    return NULL;
  }
  if (guard && count(source, old_root) != 1) {
    pp_error(error,
             "Pinned PersonaPlex root changed; review microphone cleanup");
    return NULL;
  }
  GString *patched = g_string_new(source);
  g_string_replace(patched, old, replacement, 0);
  if (guard) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *value = yyjson_mut_str(doc, guard);
    char *quoted =
        yyjson_mut_val_write(value, YYJSON_WRITE_ESCAPE_UNICODE, NULL);
    yyjson_mut_doc_free(doc);
    if (!quoted) {
      g_string_free(patched, true);
      pp_error(error, "Could not quote browser guard path");
      return NULL;
    }
    char *new_root = g_strconcat(
        "return web.Response(text=Path(os.path.join(static_path, "
        "\"index.html\")).read_text().replace(\"<head>\", \"<head><script>\" + "
        "Path(",
        quoted,
        ").read_text() + \"</script>\", 1), content_type=\"text/html\")", NULL);
    g_string_replace(patched, old_root, new_root, 0);
    g_free(new_root);
    free(quoted);
  }
  g_string_replace(patched, "int(request[\"seed\"])",
                   "int(request.query[\"seed\"])", 0);
  return g_string_free(patched, false);
}
