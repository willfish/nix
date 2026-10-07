#include "adapt.h"
#include <stdint.h>
#include <string.h>

static bool space(gunichar c) {
  return (c >= 0x09 && c <= 0x0d) || (c >= 0x1c && c <= 0x20) || c == 0x85 ||
         c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) ||
         c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f ||
         c == 0x3000;
}
static bool newline(gunichar c) {
  return (c >= 0x0a && c <= 0x0d) || (c >= 0x1c && c <= 0x1e) || c == 0x85 ||
         c == 0x2028 || c == 0x2029;
}
static Text part(Text t, size_t start, size_t end) {
  return (Text){t.data + start, end - start};
}
static Text trim(Text t, bool start, bool end) {
  if (start)
    while (t.length && space(g_utf8_get_char(t.data))) {
      size_t n = (size_t)(g_utf8_next_char(t.data) - t.data);
      t.data += n;
      t.length -= n;
    }
  if (end)
    while (t.length) {
      const char *p = g_utf8_find_prev_char(t.data, t.data + t.length);
      if (!space(g_utf8_get_char(p)))
        break;
      t.length = (size_t)(p - t.data);
    }
  return t;
}
static bool equal(Text t, const char *s) {
  return t.length == strlen(s) && !memcmp(t.data, s, t.length);
}
static size_t find(Text t, Text wanted, size_t start) {
  if (wanted.length > t.length)
    return SIZE_MAX;
  for (size_t i = start; i <= t.length - wanted.length; i++)
    if (t.data[i] == wanted.data[0] &&
        !memcmp(t.data + i, wanted.data, wanted.length))
      return i;
  return SIZE_MAX;
}
static const char *const keys[] = {"description", "model",   "mode",
                                   "agent",       "subtask", "skills"};
static void field(Text line, GString **fields) {
  if (!line.length || *line.data == ' ')
    return;
  const char *colon = memchr(line.data, ':', line.length);
  if (!colon)
    return;
  size_t at = (size_t)(colon - line.data);
  Text key = trim(part(line, 0, at), true, true);
  Text value = trim(part(line, at + 1, line.length), true, true);
  for (size_t i = 0; i < G_N_ELEMENTS(keys); i++)
    if (equal(key, keys[i])) {
      if (fields[i])
        g_string_free(fields[i], true);
      fields[i] = g_string_new_len(value.data, (gssize)value.length);
      return;
    }
}
static void parse(Text text, GString **fields) {
  size_t start = 0, at = 0;
  while (at < text.length) {
    gunichar c = g_utf8_get_char(text.data + at);
    size_t n = (size_t)(g_utf8_next_char(text.data + at) - (text.data + at));
    if (newline(c)) {
      field(part(text, start, at), fields);
      at += n;
      if (c == '\r' && at < text.length && text.data[at] == '\n')
        at++;
      start = at;
    } else
      at += n;
  }
  if (start < text.length)
    field(part(text, start, text.length), fields);
}
static GString *replace_arguments(Text body) {
  GString *out = g_string_sized_new(body.length);
  size_t start = 0;
  for (size_t i = 0; i < body.length; i++)
    if (body.data[i] == '$' && i + 1 < body.length && body.data[i + 1] == '@') {
      g_string_append_len(out, body.data + start, (gssize)(i - start));
      g_string_append(out, "$ARGUMENTS");
      i++;
      start = i + 1;
    }
  g_string_append_len(out, body.data + start, (gssize)(body.length - start));
  return out;
}
GString *adapt_markdown(Text text, bool agent) {
  Text body = text;
  GString *fields[G_N_ELEMENTS(keys)] = {0};
  if (text.length >= 4 && !memcmp(text.data, "---\n", 4)) {
    size_t end = find(text, (Text){"\n---\n", 5}, 4);
    if (end != SIZE_MAX) {
      parse(part(text, 4, end), fields);
      body = part(text, end + 5, text.length);
    }
  }
  GString *owned = NULL;
  if (!agent) {
    owned = replace_arguments(body);
    body = (Text){owned->str, owned->len};
  } else {
    if (!fields[2])
      fields[2] = g_string_new("subagent");
    if (fields[5]) {
      Text skills = {fields[5]->str, fields[5]->len};
      while (skills.length && strchr("[] ", *skills.data) && *skills.data) {
        skills.data++;
        skills.length--;
      }
      while (skills.length && strchr("[] ", skills.data[skills.length - 1]) &&
             skills.data[skills.length - 1])
        skills.length--;
      if (skills.length) {
        GString *names = g_string_new(NULL);
        size_t at = 0;
        while (at <= skills.length) {
          const char *comma = memchr(skills.data + at, ',', skills.length - at);
          size_t end = comma ? (size_t)(comma - skills.data) : skills.length;
          Text name = trim(part(skills, at, end), true, true);
          if (name.length) {
            if (names->len)
              g_string_append(names, ", ");
            g_string_append_len(names, name.data, (gssize)name.length);
          }
          if (!comma)
            break;
          at = end + 1;
        }
        GString *note = g_string_new(
            "Load these skills when the task reaches their trigger: ");
        g_string_append_len(note, names->str, (gssize)names->len);
        g_string_append(note, ".\n");
        g_string_free(names, true);
        if (find(body, (Text){note->str, note->len - 1}, 0) == SIZE_MAX) {
          body = trim(body, false, true);
          owned = g_string_new_len(body.data, (gssize)body.length);
          g_string_append(owned, "\n\n");
          g_string_append_len(owned, note->str, (gssize)note->len);
          body = (Text){owned->str, owned->len};
        }
        g_string_free(note, true);
      }
    }
  }
  const size_t agent_order[] = {0, 2, 1}, command_order[] = {0, 3, 1, 4};
  const size_t *order = agent ? agent_order : command_order;
  size_t count =
      agent ? G_N_ELEMENTS(agent_order) : G_N_ELEMENTS(command_order);
  GString *out = g_string_new("---\n");
  for (size_t i = 0; i < count; i++) {
    size_t n = order[i];
    if (!fields[n] || !fields[n]->len)
      continue;
    g_string_append(out, keys[n]);
    g_string_append(out, ": ");
    g_string_append_len(out, fields[n]->str, (gssize)fields[n]->len);
    g_string_append_c(out, '\n');
  }
  g_string_append(out, "---\n");
  while (body.length && *body.data == '\n') {
    body.data++;
    body.length--;
  }
  g_string_append_len(out, body.data, (gssize)body.length);
  if (owned)
    g_string_free(owned, true);
  for (size_t i = 0; i < G_N_ELEMENTS(keys); i++)
    if (fields[i])
      g_string_free(fields[i], true);
  return out;
}
