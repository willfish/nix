#include "token-report.h"
#include "token-template.h"
#include <stdio.h>
#include <string.h>
static char *escape(const char *s) {
  GString *out = g_string_new(NULL);
  for (const char *p = s ? s : "None"; *p; p++) {
    switch (*p) {
    case '&':
      g_string_append(out, "&amp;");
      break;
    case '<':
      g_string_append(out, "&lt;");
      break;
    case '>':
      g_string_append(out, "&gt;");
      break;
    case '"':
      g_string_append(out, "&quot;");
      break;
    default:
      g_string_append_c(out, *p);
    }
  }
  return g_string_free(out, FALSE);
}
static char *number(int64_t n) {
  char *s = g_strdup_printf("%" G_GINT64_FORMAT, n);
  GString *out = g_string_new(NULL);
  size_t start = *s == '-' ? 1 : 0, length = strlen(s);
  if (start)
    g_string_append_c(out, '-');
  for (size_t i = start; i < length; i++) {
    if (i > start && (length - i) % 3 == 0)
      g_string_append_c(out, ',');
    g_string_append_c(out, s[i]);
  }
  g_free(s);
  return g_string_free(out, FALSE);
}
static char *join(GPtrArray *array, const char *separator, const char *empty) {
  GString *out = g_string_new(NULL);
  for (size_t i = 0; i < array->len; i++) {
    if (i)
      g_string_append(out, separator);
    g_string_append(out, array->pdata[i]);
  }
  if (!out->len)
    g_string_append(out, empty);
  return g_string_free(out, FALSE);
}
static void td(GString *out, const char *value) {
  char *safe = escape(value);
  g_string_append_printf(out, "<td>%s</td>", safe);
  g_free(safe);
}
static void count(GString *out, int64_t n) {
  char *s = number(n);
  td(out, s);
  g_free(s);
}
static char *svg(GPtrArray *requests) {
  if (!requests->len)
    return g_strdup("<p>No LLM requests captured.</p>");
  size_t width = MAX(720u, 80 + requests->len * 70);
  const double top = 24, plot_h = 280;
  double max = 0;
  for (size_t i = 0; i < requests->len; i++) {
    Request *r = requests->pdata[i];
    max = fmax(max, (double)r->usage.value[0] + (double)r->usage.value[1]);
  }
  if (!max)
    max = 1;
  double gap = ((double)width - 88) / requests->len, bar_w = gap * 0.7;
  GString *out = g_string_new(NULL);
  g_string_append_printf(
      out,
      "<svg viewBox=\"0 0 %zu 360\" width=\"100%%\" role=\"img\" "
      "aria-label=\"Tokens per LLM request\"><rect width=\"%zu\" "
      "height=\"360\" fill=\"#0f1419\"/>",
      width, width);
  for (size_t i = 0; i < 5; i++) {
    double fraction = i * 0.25, y = top + plot_h * (1 - fraction);
    char *n = number((int64_t)(max * fraction));
    g_string_append_printf(
        out,
        "\n<line x1=\"64\" y1=\"%.1f\" x2=\"%zu\" y2=\"%.1f\" "
        "stroke=\"#243040\" stroke-width=\"1\"/>\n<text x=\"56\" y=\"%.1f\" "
        "text-anchor=\"end\" fill=\"#8aa0b4\" font-size=\"11\" "
        "font-family=\"ui-sans-serif,system-ui\">%s</text>",
        y, width - 24, y, y + 4, n);
    g_free(n);
  }
  for (size_t i = 0; i < requests->len; i++) {
    Request *r = requests->pdata[i];
    double input = (double)r->usage.value[0],
           cached = fmin((double)r->usage.value[3], input),
           output = (double)r->usage.value[1];
    double values[] = {fmax(input - cached, 0), cached, output};
    const char *colors[] = {"#3b82f6", "#22c55e", "#f59e0b"};
    double x = 64 + gap * i + (gap - bar_w) / 2, y = top + plot_h;
    for (size_t j = 0; j < 3; j++) {
      if (values[j] <= 0)
        continue;
      double height = plot_h * values[j] / max;
      y -= height;
      g_string_append_printf(out,
                             "\n<rect x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" "
                             "height=\"%.1f\" fill=\"%s\" rx=\"2\"/>",
                             x, y, bar_w, height, colors[j]);
    }
    char *total = number(r->usage.value[0] + r->usage.value[1]);
    g_string_append_printf(
        out,
        "\n<text x=\"%.1f\" y=\"332\" text-anchor=\"middle\" fill=\"#c5d4e3\" "
        "font-size=\"11\" "
        "font-family=\"ui-sans-serif,system-ui\">R%u</text>\n<text x=\"%.1f\" "
        "y=\"%.1f\" text-anchor=\"middle\" fill=\"#e8eef5\" font-size=\"10\" "
        "font-family=\"ui-sans-serif,system-ui\">%s</text>",
        x + bar_w / 2, r->index, x + bar_w / 2,
        top + plot_h - plot_h * (input + output) / max - 6, total);
    g_free(total);
  }
  g_string_append(
      out, "\n<text x=\"64\" y=\"352\" fill=\"#8aa0b4\" font-size=\"12\" "
           "font-family=\"ui-sans-serif,system-ui\">Blue: uncached input · "
           "Green: cached input · Amber: output</text>\n</svg>");
  return g_string_free(out, FALSE);
}
static gint tool_order(gconstpointer a, gconstpointer b) {
  const Tool *x = *(Tool *const *)a, *y = *(Tool *const *)b;
  if (x->unique != y->unique)
    return x->unique > y->unique ? -1 : 1;
  return strcmp(x->name, y->name);
}
static char *request_rows(GPtrArray *requests) {
  GString *out = g_string_new(NULL);
  for (size_t i = 0; i < requests->len; i++) {
    Request *r = requests->pdata[i];
    g_string_append_printf(out, "<tr><td>%u</td>", r->index);
    td(out, r->ts);
    count(out, r->usage.value[0]);
    count(out, r->usage.value[3]);
    count(out, r->usage.value[1]);
    count(out, r->usage.value[4]);
    count(out, r->chars);
    g_string_append_printf(
        out, "<td>%u</td><td>%" G_GINT64_FORMAT "</td><td>$%.4f</td>",
        r->advertised->len, r->messages, r->cost);
    char *calls = join(r->calls, ", ", "\342\200\224");
    td(out, calls);
    g_free(calls);
    g_string_append(out, "</tr>");
  }
  if (!out->len)
    g_string_append(out, "<tr><td colspan=\"11\">No requests.</td></tr>");
  return g_string_free(out, FALSE);
}
static char *tool_rows(Attribution *a) {
  GPtrArray *ordered = g_ptr_array_new();
  for (size_t i = 0; i < a->tools->len; i++)
    g_ptr_array_add(ordered, a->tools->pdata[i]);
  g_ptr_array_sort(ordered, tool_order);
  GString *out = g_string_new(NULL);
  for (size_t i = 0; i < ordered->len; i++) {
    Tool *t = ordered->pdata[i];
    const char *kind = "tool";
    char *note = g_strdup_printf("advertised on %" G_GINT64_FORMAT " requests",
                                 t->advertised);
    if (!strcmp(t->name, "skill_catalog")) {
      kind = "skill";
      g_free(note);
      note = join(a->queries, ", ", "catalogue lookup");
    } else if (!strcmp(t->name, "mcp")) {
      kind = "mcp";
      g_free(note);
      note = join(a->mcp, "; ", "status");
    }
    g_string_append(out, "<tr>");
    td(out, t->name);
    td(out, kind);
    g_string_append_printf(
        out, "<td>%" G_GINT64_FORMAT "</td><td>%" G_GINT64_FORMAT "</td>",
        t->unique, t->wire);
    td(out, note);
    g_string_append_printf(out, "<td>$%.4f</td></tr>", t->cost);
    g_free(note);
  }
  if (!out->len)
    g_string_append(out, "<tr><td colspan='6'>No skill, MCP, or tool "
                         "invocations parsed.</td></tr>");
  g_ptr_array_free(ordered, TRUE);
  return g_string_free(out, FALSE);
}
static char *rate(double value) {
  char *s = g_strdup_printf("%.15g", value);
  if (!strpbrk(s, ".eE")) {
    char *decimal = g_strconcat(s, ".0", NULL);
    g_free(s);
    return decimal;
  }
  return s;
}
char *token_html(Meta *meta, GPtrArray *requests, Attribution *attr,
                 Rates rates) {
  int64_t totals[5] = {0};
  double cost = 0;
  for (size_t i = 0; i < requests->len; i++) {
    Request *r = requests->pdata[i];
    for (size_t j = 0; j < 5; j++)
      totals[j] += r->usage.value[j];
    cost += r->cost;
  }
  GString *turns = g_string_new(NULL);
  for (size_t i = 0; i < 3; i++)
    g_string_append_printf(turns, "<li>%s</li>", token_turn_names[i]);
  const char *keys[] = {"CAPTURED",    "RUN",   "INPUT_RATE",   "CACHE_RATE",
                        "OUTPUT_RATE", "COUNT", "INPUT",        "CACHED",
                        "OUTPUT",      "COST",  "SVG",          "TURNS",
                        "RESULT",      "ROWS",  "SCHEMA_CHARS", "SCHEMA_COST",
                        "TOOLS"};
  char *values[] = {escape(meta->captured_at),
                    escape(meta->run),
                    rate(rates.input),
                    rate(rates.cached),
                    rate(rates.output),
                    g_strdup_printf("%u", requests->len),
                    number(totals[0]),
                    number(totals[3]),
                    number(totals[1]),
                    g_strdup_printf("%.4f", cost),
                    svg(requests),
                    g_string_free(turns, FALSE),
                    escape(meta->task_result),
                    request_rows(requests),
                    number(attr->schema_chars),
                    g_strdup_printf("%.4f", attr->schema_cost),
                    tool_rows(attr)};
  GString *out = g_string_new(NULL);
  for (const char *p = report_template; *p;) {
    if (*p == '@') {
      const char *end = strchr(p + 1, '@');
      bool found = false;
      if (end)
        for (size_t i = 0; i < G_N_ELEMENTS(keys); i++)
          if (strlen(keys[i]) == (size_t)(end - p - 1) &&
              !memcmp(p + 1, keys[i], (size_t)(end - p - 1))) {
            g_string_append(out, values[i]);
            p = end + 1;
            found = true;
            break;
          }
      if (found)
        continue;
    }
    g_string_append_c(out, *p++);
  }
  for (size_t i = 0; i < G_N_ELEMENTS(values); i++)
    g_free(values[i]);
  return g_string_free(out, FALSE);
}
