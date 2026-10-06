#include "focus.h"
#include <string.h>
#define SEP " · "
static bool object(Val *v) {
  if (!yyjson_is_obj(v)) {
    focus_error = true;
    return false;
  }
  return true;
}
static bool array(Val *v) {
  if (!truth(v))
    return true;
  if (!yyjson_is_arr(v)) {
    focus_error = true;
    return false;
  }
  return true;
}
static char *lower(Val *v) {
  char *s = string(v), *t = strip(s), *l = g_utf8_strdown(t, -1);
  g_free(s);
  g_free(t);
  return l;
}
bool digit_text(const char *s) {
  // Python isdigit includes Numeric_Type=Digit, but int accepts only decimal
  // digits. Preserve that distinction instead of focusing a fallback window.
  static const gunichar ranges[][2] = {
      {0xb2, 0xb3},       {0xb9, 0xb9},       {0x1369, 0x1371},
      {0x19da, 0x19da},   {0x2070, 0x2070},   {0x2074, 0x2079},
      {0x2080, 0x2089},   {0x2460, 0x2468},   {0x2474, 0x247c},
      {0x2488, 0x2490},   {0x24ea, 0x24ea},   {0x24f5, 0x24fd},
      {0x24ff, 0x24ff},   {0x2776, 0x277e},   {0x2780, 0x2788},
      {0x278a, 0x2792},   {0x10a40, 0x10a43}, {0x10e60, 0x10e68},
      {0x11052, 0x1105a}, {0x1f100, 0x1f10a}};
  if (!*s)
    return false;
  for (; *s; s = g_utf8_next_char(s)) {
    gunichar c = g_utf8_get_char(s);
    bool digit = g_unichar_isdigit(c);
    for (size_t i = 0; !digit && i < G_N_ELEMENTS(ranges); i++)
      digit = c >= ranges[i][0] && c <= ranges[i][1];
    if (!digit)
      return false;
  }
  return true;
}
Doc *parse_toast(const char *summary, const char *body, const char *app) {
  char *s = strip(summary), *b = strip(body), *a = strip(app),
       *l = g_utf8_strdown(a, -1), *text = NULL;
  Doc *out = NULL;
  if (strcmp(s, "Ghostty") && !strstr(l, "ghostty"))
    goto done;
  if (!strcmp(s, "Ghostty"))
    text = g_strdup(b);
  else if (*b && !strchr(s, ':'))
    text = g_strconcat(s, ": ", b, NULL);
  else
    text = g_strdup(*b ? b : s);
  char *trim = strip(text);
  g_free(text);
  text = trim;
  const char *events[] = {"needs attention", "finished"};
  for (size_t i = 0; i < 2; i++) {
    char *marker = g_strconcat(" ", events[i], ":", NULL),
         *at = strstr(text, marker);
    if (!at || at == text) {
      g_free(marker);
      continue;
    }
    char *raw = g_strndup(text, (gsize)(at - text)), *agent = strip(raw),
         *context = strip(at + strlen(marker));
    g_free(raw);
    g_free(marker);
    if (!*agent || !*context) {
      g_free(agent);
      g_free(context);
      break;
    }
    char **parts = g_strsplit(context, SEP, -1);
    size_t count = g_strv_length(parts);
    for (size_t j = 0; j < count; j++) {
      char *p = strip(parts[j]);
      g_free(parts[j]);
      parts[j] = p;
    }
    if (count >= 2 && digit_text(parts[1])) {
      char *number = integer_text(parts[1]);
      if (number) {
        out = empty_object();
        Mut *root = yyjson_mut_doc_get_root(out);
        yyjson_mut_obj_add_strcpy(out, root, "agent", agent);
        yyjson_mut_obj_add_str(out, root, "event", events[i]);
        yyjson_mut_obj_add_strcpy(out, root, "workspace_label", parts[0]);
        yyjson_mut_obj_add_val(out, root, "workspace_number",
                               yyjson_mut_rawcpy(out, number));
        if (count > 2) {
          char *tab = g_strjoinv(SEP, parts + 2);
          yyjson_mut_obj_add_strcpy(out, root, "tab_label", tab);
          g_free(tab);
        } else
          yyjson_mut_obj_add_null(out, root, "tab_label");
        g_free(number);
      }
    }
    g_strfreev(parts);
    g_free(agent);
    g_free(context);
    break;
  }
done:
  g_free(s);
  g_free(b);
  g_free(a);
  g_free(l);
  g_free(text);
  return out;
}
static bool agent_matches(Val *record, Val *name) {
  char *wanted = lower(name), *agent = lower(field(record, "agent")),
       *display = lower(field(record, "display_agent"));
  bool yes = *wanted && (!strcmp(wanted, agent) || !strcmp(wanted, display));
  if (*wanted && *agent) {
    char *space = g_strconcat(agent, " ", NULL),
         *separator = g_strconcat(agent, SEP, NULL);
    yes = yes || g_str_has_prefix(wanted, space) ||
          g_str_has_prefix(wanted, separator);
    g_free(space);
    g_free(separator);
  }
  g_free(wanted);
  g_free(agent);
  g_free(display);
  return yes;
}
static bool status_matches(Val *agent, Val *event) {
  Val *status = field(agent, "agent_status");
  if (yyjson_is_arr(status) || yyjson_is_obj(status)) {
    focus_error = true;
    return false;
  }
  if (!yyjson_is_str(status) || !yyjson_is_str(event))
    return false;
  const char *s = yyjson_get_str(status), *e = yyjson_get_str(event);
  return !strcmp(e, "finished")
             ? (!strcmp(s, "done") || !strcmp(s, "idle"))
             : !strcmp(e, "needs attention") && !strcmp(s, "blocked");
}
static Val *pane(GPtrArray *agents, Val *event, size_t *matches) {
  *matches = 0;
  for (size_t i = 0; i < agents->len; i++)
    if (status_matches(agents->pdata[i], event))
      (*matches)++;
  Val *best = NULL;
  char *seq = NULL;
  for (size_t i = 0; i < agents->len; i++) {
    Val *a = agents->pdata[i];
    if (*matches && !status_matches(a, event))
      continue;
    char *n = integer(field(a, "state_change_seq"));
    if (!n)
      break;
    if (!best || integer_compare(n, seq) > 0) {
      best = a;
      g_free(seq);
      seq = n;
    } else
      g_free(n);
  }
  g_free(seq);
  return best;
}
Doc *resolve_target(Val *snapshot, Val *toast) {
  if (!truth(snapshot) || !truth(toast))
    return NULL;
  if (!object(snapshot) || !object(toast))
    return NULL;
  Val *workspaces = field(snapshot, "workspaces"),
      *tabs = field(snapshot, "tabs"), *agents = field(snapshot, "agents");
  if (!array(workspaces))
    return NULL;
  Val *ws = NULL, *only = NULL, *v;
  size_t count = 0, i, max;
  char *wanted = integer(field(toast, "workspace_number"));
  yyjson_arr_foreach(workspaces, i, max, v) {
    if (!object(v))
      break;
    if (!equal(field(v, "label"), field(toast, "workspace_label")))
      continue;
    only = v;
    count++;
    char *n = integer(field(v, "number"));
    if (n && wanted && !integer_compare(n, wanted) && !ws)
      ws = v;
    g_free(n);
  }
  g_free(wanted);
  if (!ws && count == 1)
    ws = only;
  if (!ws || focus_error)
    return NULL;
  Doc *out = empty_object();
  Mut *root = yyjson_mut_doc_get_root(out);
  Val *workspace = field(ws, "workspace_id");
  member(out, root, "workspace_id", workspace);
  Val *label = field(toast, "tab_label"), *tab = NULL;
  if (!array(tabs)) {
    yyjson_mut_doc_free(out);
    return NULL;
  }
  size_t tab_count = 0;
  yyjson_arr_foreach(tabs, i, max, v) {
    if (!object(v))
      break;
    if (equal(field(v, "workspace_id"), workspace) &&
        (!truth(label) || equal(field(v, "label"), label))) {
      tab = v;
      tab_count++;
    }
  }
  if (tab_count != 1)
    tab = NULL;
  if (truth(label) && !tab)
    return out;
  if (!array(agents)) {
    yyjson_mut_doc_free(out);
    return NULL;
  }
  GPtrArray *pool = g_ptr_array_new();
  yyjson_arr_foreach(agents, i, max, v) {
    if (!object(v))
      break;
    if (equal(field(v, "workspace_id"), workspace) &&
        agent_matches(v, field(toast, "agent")) &&
        (!tab || equal(field(v, "tab_id"), field(tab, "tab_id"))))
      g_ptr_array_add(pool, v);
  }
  size_t matching;
  Val *selected = pane(pool, field(toast, "event"), &matching);
  if (tab) {
    member(out, root, "tab_id", field(tab, "tab_id"));
    if (selected)
      member(out, root, "pane_id", field(selected, "pane_id"));
  } else if (selected && matching == 1) {
    member(out, root, "tab_id", field(selected, "tab_id"));
    member(out, root, "pane_id", field(selected, "pane_id"));
  }
  g_ptr_array_free(pool, true);
  if (focus_error) {
    yyjson_mut_doc_free(out);
    return NULL;
  }
  return out;
}
bool target_alive(Val *snapshot, Val *target) {
  if (!truth(snapshot) || !truth(target))
    return false;
  if (!object(snapshot) || !object(target))
    return false;
  const char *lists[] = {"agents", "panes", "tabs", "workspaces"},
             *keys[] = {"pane_id", "pane_id", "tab_id", "workspace_id"};
  Val *id = field(target, "pane_id");
  size_t start = 0, end = 2;
  if (!truth(id)) {
    id = field(target, "tab_id");
    start = 2;
    end = 3;
    if (!truth(id)) {
      id = field(target, "workspace_id");
      start = 3;
      end = 4;
    }
  }
  bool alive = false;
  for (size_t j = start; j < end; j++) {
    Val *items = field(snapshot, lists[j]), *v;
    size_t i, max;
    if (!array(items))
      return false;
    yyjson_arr_foreach(items, i, max, v) {
      if (!object(v))
        return false;
      Val *value = field(v, keys[j]);
      if (start == 0 && (yyjson_is_arr(value) || yyjson_is_obj(value) ||
                         yyjson_is_arr(id) || yyjson_is_obj(id))) {
        focus_error = true;
        return false;
      }
      if (equal(value, id)) {
        if (start)
          return true;
        alive = true;
      }
    }
  }
  return alive;
}
Doc *focus_payload(Val *target) {
  if (!truth(target))
    return NULL;
  const char *keys[] = {"pane_id", "tab_id", "workspace_id"},
             *methods[] = {"pane.focus", "tab.focus", "workspace.focus"};
  for (size_t i = 0; i < 3; i++) {
    Val *id = field(target, keys[i]);
    if (!truth(id))
      continue;
    Doc *out = empty_object();
    Mut *root = yyjson_mut_doc_get_root(out), *params = yyjson_mut_obj(out);
    yyjson_mut_obj_add_str(out, root, "method", methods[i]);
    member(out, params, keys[i], id);
    yyjson_mut_obj_add_val(out, root, "params", params);
    return out;
  }
  return NULL;
}
static bool ghostty(Val *client) {
  char *s = string(field(client, "class")), *l = g_utf8_strdown(s, -1);
  bool yes = strstr(l, "ghostty") && !strstr(l, "cliamp");
  g_free(s);
  g_free(l);
  return yes;
}
Val *choose_window(Val *clients, const char *pid, GPtrArray *ancestors,
                   const char *label) {
  if (!array(clients))
    return NULL;
  Val *v;
  size_t i, max;
  GPtrArray *attached = g_ptr_array_new(), *all = g_ptr_array_new(),
            *pool = g_ptr_array_new();
  Val *out = NULL;
  yyjson_arr_foreach(clients, i, max, v) {
    if (!object(v))
      break;
    char *n = integer(field(v, "pid"));
    if (!n)
      break;
    if (strcmp(pid, "0") && !integer_compare(n, pid)) {
      out = v;
      g_free(n);
      goto done;
    }
    bool found = false;
    for (size_t j = 0; ancestors && j < ancestors->len; j++)
      if (!integer_compare(n, ancestors->pdata[j]))
        found = true;
    if (found)
      g_ptr_array_add(attached, v);
    g_ptr_array_add(all, v);
    g_free(n);
  }
  GPtrArray *base = attached->len ? attached : all;
  for (size_t j = 0; j < base->len; j++)
    if (ghostty(base->pdata[j]))
      g_ptr_array_add(pool, base->pdata[j]);
  if (attached->len && !pool->len)
    for (size_t j = 0; j < attached->len; j++)
      g_ptr_array_add(pool, attached->pdata[j]);
  if (!attached->len && *label) {
    size_t titled = 0;
    Val *title_match = NULL;
    for (size_t j = 0; j < pool->len; j++) {
      char *title = string(field(pool->pdata[j], "title"));
      if (strstr(title, label)) {
        title_match = pool->pdata[j];
        titled++;
      }
      g_free(title);
    }
    if (titled == 1) {
      out = title_match;
      goto done;
    }
  }
  char *best = NULL;
  for (size_t j = 0; j < pool->len; j++) {
    Val *c = pool->pdata[j];
    char *history = integer(field(c, "focusHistoryID"));
    if (!history)
      break;
    if (!out || integer_compare(history, best) < 0) {
      out = c;
      g_free(best);
      best = history;
    } else
      g_free(history);
  }
  g_free(best);
done:
  g_ptr_array_free(attached, true);
  g_ptr_array_free(all, true);
  g_ptr_array_free(pool, true);
  return focus_error ? NULL : out;
}
